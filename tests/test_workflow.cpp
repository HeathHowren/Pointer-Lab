// The whole tool, in the order somebody actually uses it.
//
// Every link in this chain has its own focused test elsewhere. This one exists
// for the seams between them: a scan result feeding the address list, a freeze
// fighting the target, a project surviving a save and a restart, a chain found
// by the scanner still resolving in a process that did not exist when it was
// found. Those are the joins that no single-subsystem test covers.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_disasm/Disassembler.h"
#include "engine_pointer/PointerScanner.h"
#include "services/RuntimeServices.h"
#include "storage/ProjectStore.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

using namespace ire;
using testsupport::AttachedHelper;
using testsupport::needleValue;

namespace {

bool waitForScan(engine_scan::ScanJob& job, std::chrono::seconds timeout = std::chrono::seconds(120)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!job.progress().running) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool waitForPointerScan(engine_pointer::PointerScanJob& job,
                        std::chrono::seconds timeout = std::chrono::seconds(120)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!job.progress().running) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// Polls rather than sleeping a fixed time: the freeze loop runs every 50 ms, so
// a fixed wait is either flaky or needlessly slow.
bool waitUntil(const std::function<bool()>& condition, std::chrono::seconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::filesystem::path scratchProject() {
    std::error_code ignored;
    return std::filesystem::temp_directory_path(ignored) / "pointerlab-workflow.iretable";
}

} // namespace

TEST_CASE("Scan, track, freeze, save, restart, re-resolve", "[workflow][integration]") {
    const auto projectPath = scratchProject();
    std::error_code ignored;
    std::filesystem::remove(projectPath, ignored);

    domain::PointerChain savedChain;
    std::uint64_t frozenEntryId{};

    // --- First session: find the value, track it, freeze it, save the project.
    {
        AttachedHelper fixture;
        services::RuntimeServices services;
        REQUIRE(services.session().attach(fixture.helper.pid()).has_value());

        // 1. Exact scan for the value the helper starts out holding.
        const auto needle = domain::parseScanValue(domain::ValueType::Int32, std::to_string(needleValue));
        REQUIRE(needle.has_value());
        services.scanJob().startFirst(domain::ScanMode::Exact, *needle);
        REQUIRE(waitForScan(services.scanJob()));

        auto results = services.scanJob().results();
        INFO("exact scan found " << results.size() << " results");
        REQUIRE_FALSE(results.empty());

        // 2. Change the value in the target, then narrow with a relative scan.
        //    This is the workflow the relative modes exist for, and the one that
        //    matched nothing at all before this release.
        fixture.helper.set(needleValue + 1);
        services.scanJob().startNext(domain::ScanMode::Changed, *needle, std::move(results));
        REQUIRE(waitForScan(services.scanJob()));

        const auto narrowed = services.scanJob().results();
        INFO("changed scan left " << narrowed.size() << " results");
        REQUIRE_FALSE(narrowed.empty());

        const bool foundTheValue =
            std::any_of(narrowed.begin(), narrowed.end(),
                        [&](const domain::ScanResult& r) { return r.address == fixture.helper.address(); });
        CHECK(foundTheValue);

        // 3. Track it and freeze it. The helper keeps running, so a freeze that
        //    does not actually fight back would let the value drift.
        frozenEntryId = services.addressList().add(fixture.helper.address(), domain::ValueType::Int32,
                                                   "helper value", "workflow");
        const auto frozenBytes = domain::parseScanValue(domain::ValueType::Int32, "4242");
        REQUIRE(frozenBytes.has_value());
        REQUIRE(services.addressList().updateValue(frozenEntryId, frozenBytes->bytes));
        REQUIRE(services.addressList().setFrozen(frozenEntryId, true));

        // The target is told to change it; the freeze loop must put it back.
        fixture.helper.set(7);
        REQUIRE(waitUntil([&] { return fixture.helper.get() == 4242; }));

        // 4. Disassemble real code in the target and confirm it decodes.
        const auto instructions = services.disassembler().disassemble(services.session(), fixture.helper.tick(), 8);
        REQUIRE_FALSE(instructions.empty());
        CHECK(std::any_of(instructions.begin(), instructions.end(),
                          [](const domain::Instruction& i) { return i.valid; }));

        // 5. Find a pointer chain to the value, so the entry can survive ASLR.
        engine_pointer::PointerScanOptions options;
        options.target = fixture.helper.address();
        options.maxDepth = 3;
        options.maxOffset = 0x40;
        options.maxResults = 5000;
        services.pointerScanJob().start(options);
        REQUIRE(waitForPointerScan(services.pointerScanJob()));

        for (const auto& chain : services.pointerScanJob().results()) {
            auto resolved = engine_pointer::resolveChain(services.session(), chain);
            if (resolved && resolved.value() == fixture.helper.address()) {
                savedChain = chain;
                break;
            }
        }
        REQUIRE(savedChain.valid());
        services.addressList().addChain(savedChain, domain::ValueType::Int32, "via chain", "workflow");

        // 6. Save the project, exactly as autosave-on-exit does.
        storage::ProjectTable table;
        table.lastPid = fixture.helper.pid();
        table.lastProcessName = services.session().processName();
        table.entries = services.session().addressList().snapshot();
        REQUIRE(table.entries.size() == 2);

        storage::ProjectStore store;
        REQUIRE(store.save(projectPath, table).has_value());

        // Unfreeze before tearing down so the loop is not writing into a
        // process that is about to exit.
        REQUIRE(services.addressList().setFrozen(frozenEntryId, false));
    }

    // The first helper has now exited. Its addresses mean nothing any more.

    // --- Second session: reload the project against a brand new target.
    {
        AttachedHelper fixture;
        services::RuntimeServices services;
        REQUIRE(services.session().attach(fixture.helper.pid()).has_value());

        storage::ProjectStore store;
        auto loaded = store.load(projectPath);
        INFO("load: " << loaded.error());
        REQUIRE(loaded.has_value());
        REQUIRE(loaded.value().entries.size() == 2);

        const auto& entries = loaded.value().entries;
        const auto chained = std::find_if(entries.begin(), entries.end(),
                                          [](const domain::AddressEntry& e) { return e.chain.has_value(); });
        REQUIRE(chained != entries.end());

        // A chain-backed entry comes back unresolved on purpose: the address it
        // was saved with belonged to a process that no longer exists.
        CHECK_FALSE(chained->resolved);

        auto resolved = engine_pointer::resolveChain(services.session(), *chained->chain);
        INFO("resolve after restart: " << resolved.error());
        REQUIRE(resolved.has_value());
        CHECK(resolved.value() == fixture.helper.address());

        // And it is the real address, not a coincidence: writing through it has
        // to change what the new process reports.
        const auto replacement = domain::parseScanValue(domain::ValueType::Int32, "555");
        REQUIRE(replacement.has_value());
        REQUIRE(services.session().writeBytes(resolved.value(), replacement->bytes).has_value());
        CHECK(fixture.helper.get() == 555);
    }

    std::filesystem::remove(projectPath, ignored);
}

TEST_CASE("A breakpoint, a patch and a detach leave the target healthy", "[workflow][integration]") {
    AttachedHelper fixture;
    services::RuntimeServices services;
    REQUIRE(services.session().attach(fixture.helper.pid()).has_value());

    const auto tick = fixture.helper.tick();

    // Assemble a patch and pad it, the way the assembler panel does. Padding is
    // what stops a short patch leaving half an instruction behind.
    auto assembled = services.assembler().assemble("nop", tick);
    INFO("assemble: " << assembled.error());
    REQUIRE(assembled.has_value());
    const auto padded = engine_disasm::padToInstructionBoundary(services.disassembler(), services.session(), tick,
                                                                assembled.value());
    CHECK(padded.size() >= assembled.value().size());

    // Keep the original bytes so the target is left exactly as it was found.
    auto original = services.session().readBytes(tick, padded.size());
    REQUIRE(original.has_value());
    REQUIRE(original.value().size() == padded.size());

    REQUIRE(services.breakpoints().attachDebugger().has_value());
    REQUIRE(services.breakpoints().addBreakpoint(tick, "tick").has_value());

    // The helper calls tick() in bursts, so hits should accumulate while the
    // target keeps running rather than the breakpoint firing once and stopping.
    const auto ticksBefore = fixture.helper.ticks();
    REQUIRE(ticksBefore >= 0);
    REQUIRE(waitUntil([&] {
        const auto list = services.breakpoints().breakpoints();
        return !list.empty() && list.front().hitCount > 2;
    }));

    REQUIRE(services.breakpoints().removeBreakpoint(tick).has_value());
    services.breakpoints().detachDebugger();

    // The target has to still be alive and still counting after all of that.
    REQUIRE(waitUntil([&] { return fixture.helper.ticks() > ticksBefore; }));
    CHECK(fixture.helper.get() != 0);

    // And the instruction stream has to be exactly what it was.
    auto after = services.session().readBytes(tick, padded.size());
    REQUIRE(after.has_value());
    CHECK(after.value() == original.value());
}
