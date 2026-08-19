// Pointer scanner and chain resolution against a live target.
//
// The helper reaches its value through a two-level chain rooted in a module
// global, so a scan should find a base that is a fixed point in the module
// rather than an address that means nothing once the process restarts.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_pointer/PointerScanner.h"
#include "services/RuntimeServices.h"

#include <chrono>
#include <cstring>
#include <cwctype>
#include <thread>

using namespace ire;
using testsupport::AttachedHelper;

namespace {

bool waitForScan(engine_pointer::PointerScanJob& job,
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

engine_pointer::PointerScanOptions optionsFor(std::uintptr_t target) {
    engine_pointer::PointerScanOptions options;
    options.target = target;
    options.maxDepth = 3;
    // The helper's chain is all zero offsets, so a small window is plenty and
    // keeps the scan quick.
    options.maxOffset = 0x40;
    options.maxResults = 5000;
    return options;
}

// The first chain that actually resolves back to the address it was scanned for.
std::optional<domain::PointerChain> workingChain(domain::TargetSession& session,
                                                 const std::vector<domain::PointerChain>& chains,
                                                 std::uintptr_t expected) {
    for (const auto& chain : chains) {
        auto resolved = engine_pointer::resolveChain(session, chain);
        if (resolved && resolved.value() == expected) {
            return chain;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("A pointer scan finds a module-anchored chain to a known value", "[pointer][integration]") {
    AttachedHelper fixture;
    engine_pointer::PointerScanJob job(fixture.session);

    job.start(optionsFor(fixture.helper.address()));
    REQUIRE(waitForScan(job));

    const auto chains = job.results();
    INFO("pointer scan returned " << chains.size() << " chains");
    REQUIRE_FALSE(chains.empty());

    // Every result must be anchored in a module: a chain based on a heap
    // address is worthless the moment the target restarts.
    for (const auto& chain : chains) {
        CHECK_FALSE(chain.moduleName.empty());
        CHECK_FALSE(chain.offsets.empty());
        CHECK(chain.valid());
    }

    const auto working = workingChain(fixture.session, chains, fixture.helper.address());
    REQUIRE(working.has_value());
    INFO("chain has " << working->offsets.size() << " level(s)");
    CHECK(working->offsets.size() <= 3);
}

// The whole reason pointer chains exist, and the thing the old code could not
// do because it threw the offsets away and stored an absolute base.
TEST_CASE("A pointer chain re-resolves after the target restarts", "[pointer][integration]") {
    domain::PointerChain saved;

    {
        AttachedHelper first;
        engine_pointer::PointerScanJob job(first.session);
        job.start(optionsFor(first.helper.address()));
        REQUIRE(waitForScan(job));

        const auto working = workingChain(first.session, job.results(), first.helper.address());
        REQUIRE(working.has_value());
        saved = *working;
    }
    // The first helper has now exited. Its value address is gone.

    AttachedHelper second;
    auto resolved = engine_pointer::resolveChain(second.session, saved);
    INFO("resolve: " << resolved.error());
    REQUIRE(resolved.has_value());
    CHECK(resolved.value() == second.helper.address());

    // Not just the right number: writing through the resolved address has to
    // change the value the new process actually reports.
    const auto replacement = domain::parseScanValue(domain::ValueType::Int32, "31337");
    REQUIRE(replacement.has_value());
    REQUIRE(second.session.writeBytes(resolved.value(), replacement->bytes).has_value());
    CHECK(second.helper.get() == 31337);
}

TEST_CASE("A chain-backed address list entry tracks a restart", "[pointer][integration]") {
    domain::PointerChain saved;
    {
        AttachedHelper first;
        engine_pointer::PointerScanJob job(first.session);
        job.start(optionsFor(first.helper.address()));
        REQUIRE(waitForScan(job));
        const auto working = workingChain(first.session, job.results(), first.helper.address());
        REQUIRE(working.has_value());
        saved = *working;
    }

    AttachedHelper second;
    services::AddressListService addresses(second.session);

    const auto id = addresses.addChain(saved, domain::ValueType::Int32, "needle", "Pointers");
    REQUIRE(id != 0);

    const auto entries = second.session.addressList().snapshot();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].chain.has_value());
    CHECK(entries[0].resolved);
    CHECK(entries[0].address == second.helper.address());
}

TEST_CASE("Resolution reports why a chain failed instead of guessing", "[pointer][integration]") {
    AttachedHelper fixture;

    SECTION("a chain with no module") {
        domain::PointerChain chain;
        chain.offsets = {0};
        auto resolved = engine_pointer::resolveChain(fixture.session, chain);
        CHECK_FALSE(resolved.has_value());
        CHECK_FALSE(resolved.error().empty());
    }
    SECTION("a module that is not loaded") {
        domain::PointerChain chain;
        chain.moduleName = L"definitely_not_loaded.dll";
        chain.offsets = {0};
        auto resolved = engine_pointer::resolveChain(fixture.session, chain);
        CHECK_FALSE(resolved.has_value());
        CHECK(resolved.error().find("not loaded") != std::string::npos);
    }
    SECTION("an offset that walks off into unmapped memory") {
        domain::PointerChain chain;
        chain.moduleName = L"pointerlab_test_helper.exe";
        chain.moduleOffset = 0x10;      // module header, not a pointer
        chain.offsets = {0, 0, 0, 0};
        auto resolved = engine_pointer::resolveChain(fixture.session, chain);
        CHECK_FALSE(resolved.has_value());
    }
    SECTION("a chain with no offsets is not a chain") {
        domain::PointerChain chain;
        chain.moduleName = L"pointerlab_test_helper.exe";
        CHECK_FALSE(chain.valid());
        CHECK_FALSE(engine_pointer::resolveChain(fixture.session, chain).has_value());
    }
}

TEST_CASE("Module names resolve case-insensitively", "[pointer][integration]") {
    AttachedHelper fixture;
    engine_pointer::PointerScanJob job(fixture.session);
    job.start(optionsFor(fixture.helper.address()));
    REQUIRE(waitForScan(job));

    auto working = workingChain(fixture.session, job.results(), fixture.helper.address());
    REQUIRE(working.has_value());

    // Windows module names are case-insensitive, and a hand-edited project file
    // will not necessarily match the loader's casing.
    auto shouted = *working;
    for (auto& ch : shouted.moduleName) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    auto resolved = engine_pointer::resolveChain(fixture.session, shouted);
    REQUIRE(resolved.has_value());
    CHECK(resolved.value() == fixture.helper.address());
}
