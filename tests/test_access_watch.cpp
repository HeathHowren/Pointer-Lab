// Tests for "find out what accesses this address".
//
// Three separable concerns, tested separately:
//
//   1. precedingInstruction -- walking back from a trap address to the
//      instruction that actually did the access. x86 cannot be disassembled
//      backwards, so this is a heuristic and it needs to be pinned down.
//   2. AccessWatch aggregation -- counting, deduplicating and capping, driven
//      by synthetic contexts so the arithmetic is checked without a debugger.
//   3. The whole thing against a live target.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_debug/AccessWatch.h"
#include "engine_disasm/Disassembler.h"
#include "services/RuntimeServices.h"

#include <algorithm>
#include <chrono>
#include <thread>

using namespace ire;
using testsupport::AttachedHelper;

namespace {

domain::RegisterContext contextAt(std::uint64_t rip, std::uint32_t threadId = 1) {
    domain::RegisterContext context;
    context.rip = rip;
    context.threadId = threadId;
    context.captured = true;
    context.bitness = domain::Bitness::X64;
    return context;
}

const engine_debug::AccessSite* findSite(const std::vector<engine_debug::AccessSite>& sites,
                                         std::uintptr_t trapAddress) {
    const auto it = std::find_if(sites.begin(), sites.end(), [trapAddress](const engine_debug::AccessSite& site) {
        return site.trapAddress == trapAddress;
    });
    return it == sites.end() ? nullptr : &*it;
}

} // namespace

// ---------------------------------------------------------------------------
// precedingInstruction
// ---------------------------------------------------------------------------

TEST_CASE("precedingInstruction finds the instruction ending at an address", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    const auto scratch = fixture.helper.scratch();

    // A run of nops, then mov eax, 0x11223344 (5 bytes), then a nop.
    //
    // The leading nops matter: the lookback window has to contain known code,
    // or the walk starts in whatever the previous test left on the page and the
    // test is measuring luck. Real code has real instructions before it, which
    // is the condition this heuristic is designed for.
    std::vector<std::uint8_t> code(32, 0x90);
    const std::vector<std::uint8_t> mov{0xB8, 0x44, 0x33, 0x22, 0x11};
    code.insert(code.end(), mov.begin(), mov.end());
    code.push_back(0x90);
    REQUIRE(fixture.session.writeBytes(scratch, code).has_value());

    // The instruction ending here is the mov at scratch+32.
    const auto trap = scratch + 32 + mov.size();
    auto found = engine_disasm::precedingInstruction(disassembler, fixture.session, trap);
    REQUIRE(found.has_value());
    CHECK(found->address == scratch + 32);
    CHECK(found->bytes.size() == 5);
    CHECK(found->text.find("mov") != std::string::npos);
}

TEST_CASE("precedingInstruction handles a one-byte instruction", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    const auto scratch = fixture.helper.scratch();

    // A long run of nops, asked about well inside it so the whole lookback
    // window is known code. The instruction ending at scratch+32 is the nop at
    // scratch+31 -- the shortest possible answer, and the one a heuristic
    // biased toward long encodings would get wrong.
    const std::vector<std::uint8_t> code(48, 0x90);
    REQUIRE(fixture.session.writeBytes(scratch, code).has_value());

    auto found = engine_disasm::precedingInstruction(disassembler, fixture.session, scratch + 32);
    REQUIRE(found.has_value());
    CHECK(found->address == scratch + 31);
    CHECK(found->bytes.size() == 1);
}

TEST_CASE("precedingInstruction declines rather than guessing", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;

    // Nothing is mapped here, so there is no listing to walk and no honest
    // answer to give.
    CHECK_FALSE(engine_disasm::precedingInstruction(disassembler, fixture.session, 0x100).has_value());
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

TEST_CASE("AccessWatch aggregates hits by instruction", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    engine_debug::AccessWatch watch(fixture.session, disassembler);

    const auto scratch = fixture.helper.scratch();
    REQUIRE(fixture.session.writeBytes(scratch, std::vector<std::uint8_t>(32, 0x90)).has_value());

    watch.begin(fixture.helper.address(), 4, domain::BreakpointKind::HardwareWrite);

    // Two distinct trap addresses, hit different numbers of times.
    for (int i = 0; i < 5; ++i) {
        watch.record(contextAt(scratch + 8));
    }
    for (int i = 0; i < 2; ++i) {
        watch.record(contextAt(scratch + 12));
    }

    const auto sites = watch.sites();
    REQUIRE(sites.size() == 2);
    CHECK(watch.totalHits() == 7);
    CHECK_FALSE(watch.truncated());

    // Busiest first, because the instruction responsible is almost always the
    // one hitting most and the reader should not have to hunt for it.
    CHECK(sites[0].trapAddress == scratch + 8);
    CHECK(sites[0].hitCount == 5);
    CHECK(sites[1].hitCount == 2);
}

TEST_CASE("AccessWatch records nothing while stopped", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    engine_debug::AccessWatch watch(fixture.session, disassembler);

    // A hit arriving after the user pressed Stop must not appear, or the list
    // keeps growing after the watch is visibly off.
    watch.record(contextAt(fixture.helper.scratch()));
    CHECK(watch.sites().empty());
    CHECK(watch.totalHits() == 0);

    watch.begin(fixture.helper.address(), 4, domain::BreakpointKind::HardwareWrite);
    watch.record(contextAt(fixture.helper.scratch()));
    CHECK(watch.totalHits() == 1);

    watch.stop();
    watch.record(contextAt(fixture.helper.scratch()));
    CHECK(watch.totalHits() == 1);
}

TEST_CASE("AccessWatch caps distinct sites and says so", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    engine_debug::AccessWatch watch(fixture.session, disassembler);

    watch.begin(fixture.helper.address(), 4, domain::BreakpointKind::HardwareWrite);

    const auto scratch = fixture.helper.scratch();
    const auto overflow = engine_debug::AccessWatch::maxSites + 50;
    for (std::size_t i = 0; i < overflow; ++i) {
        watch.record(contextAt(scratch + i * 4));
    }

    CHECK(watch.sites().size() == engine_debug::AccessWatch::maxSites);
    CHECK(watch.totalHits() == overflow);
    // The cap must be visible. A silently truncated list reads as a complete
    // answer, which is worse than no answer.
    CHECK(watch.truncated());
}

TEST_CASE("begin clears whatever the last watch found", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    engine_debug::AccessWatch watch(fixture.session, disassembler);

    watch.begin(fixture.helper.address(), 4, domain::BreakpointKind::HardwareWrite);
    watch.record(contextAt(fixture.helper.scratch()));
    REQUIRE(watch.totalHits() == 1);

    watch.begin(fixture.helper.address() + 16, 4, domain::BreakpointKind::HardwareReadWrite);
    CHECK(watch.sites().empty());
    CHECK(watch.totalHits() == 0);
    CHECK(watch.watchedAddress() == fixture.helper.address() + 16);
    CHECK(watch.kind() == domain::BreakpointKind::HardwareReadWrite);
}

// ---------------------------------------------------------------------------
// Register interpretation
// ---------------------------------------------------------------------------

TEST_CASE("A register just below the watched address is called out as a struct base",
          "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    engine_debug::AccessWatch watch(fixture.session, disassembler);

    const auto watched = fixture.helper.address();
    watch.begin(watched, 4, domain::BreakpointKind::HardwareWrite);

    auto context = contextAt(fixture.helper.scratch());
    context.rdi = watched - 0xF8; // the classic "health is at EDI+0xF8"
    context.rbx = watched;        // the address itself
    context.rcx = 0;              // null
    context.rdx = 0x1234;         // just a number

    const auto meanings = watch.explain(context);
    REQUIRE(meanings.size() == domain::registerCount(domain::Bitness::X64));

    const auto by = [&meanings](const char* name) {
        const auto it = std::find_if(meanings.begin(), meanings.end(),
                                     [name](const engine_debug::RegisterMeaning& m) { return m.name == name; });
        REQUIRE(it != meanings.end());
        return *it;
    };

    // This is the whole point of the feature: turning a register dump into
    // "the value is at RDI+0xF8, so RDI is the object it belongs to".
    CHECK(by("RDI").interpretation.find("structure base") != std::string::npos);
    CHECK(by("RDI").interpretation.find("F8") != std::string::npos);
    CHECK(by("RBX").interpretation == "the watched address itself");
    CHECK(by("RCX").interpretation == "null");
    // Not a pointer into anything mapped, so nothing is claimed about it.
    CHECK(by("RDX").interpretation.empty());
}

TEST_CASE("A register pointing into a module is named as static", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    engine_debug::AccessWatch watch(fixture.session, disassembler);

    watch.begin(fixture.helper.address(), 4, domain::BreakpointKind::HardwareWrite);

    const auto modules = fixture.session.modules();
    REQUIRE_FALSE(modules.empty());

    auto context = contextAt(fixture.helper.scratch());
    context.rsi = modules.front().base + 0x40;

    const auto meanings = watch.explain(context);
    const auto it = std::find_if(meanings.begin(), meanings.end(),
                                 [](const engine_debug::RegisterMeaning& m) { return m.name == "RSI"; });
    REQUIRE(it != meanings.end());
    CHECK(it->interpretation.find("static") != std::string::npos);
}

TEST_CASE("Register interpretation follows the captured bitness", "[access][integration]") {
    AttachedHelper fixture;
    const engine_disasm::Disassembler disassembler;
    engine_debug::AccessWatch watch(fixture.session, disassembler);

    watch.begin(fixture.helper.address(), 4, domain::BreakpointKind::HardwareWrite);

    auto context = contextAt(fixture.helper.scratch());
    context.bitness = domain::Bitness::X86;

    // Nine registers, named EAX-style. Reporting sixteen would invent eight the
    // thread does not have.
    const auto meanings = watch.explain(context);
    CHECK(meanings.size() == 9);
    CHECK(meanings[0].name == "EIP");
    CHECK(std::none_of(meanings.begin(), meanings.end(),
                       [](const engine_debug::RegisterMeaning& m) { return m.name == "R8"; }));
}

// ---------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------

TEST_CASE("A write watch catches the instruction that changes a value", "[access][integration]") {
    AttachedHelper fixture;
    services::RuntimeServices services;
    REQUIRE(services.session().attach(fixture.helper.pid()).has_value());

    const auto target = fixture.helper.address();
    auto started = services.startAccessWatch(target, 4, true);
    INFO("startAccessWatch: " << started.error());
    REQUIRE(started.has_value());

    // The helper writes its value on command, so the access is deterministic
    // rather than something we wait and hope for.
    for (int i = 0; i < 5; ++i) {
        REQUIRE(fixture.helper.set(1000 + i));
    }

    // Hits arrive on the pump thread; give them a moment to land.
    auto& watch = services.accessWatch();
    for (int i = 0; i < 200 && watch.totalHits() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(watch.totalHits() > 0);
    const auto sites = watch.sites();
    REQUIRE_FALSE(sites.empty());

    // Whatever instruction it names has to be real code we can decode, not the
    // trap address passed through unresolved.
    CHECK(sites.front().instructionResolved);
    CHECK_FALSE(sites.front().text.empty());
    CHECK(sites.front().address != 0);

    services.stopAccessWatch();
    CHECK_FALSE(watch.active());

    // The target must survive having been watched and unwatched.
    CHECK(fixture.helper.ticks() >= 0);
    CHECK(fixture.helper.get() == 1004);
}

TEST_CASE("An unaligned or malformed watch is refused with a reason", "[access][integration]") {
    AttachedHelper fixture;
    services::RuntimeServices services;
    REQUIRE(services.session().attach(fixture.helper.pid()).has_value());

    // The processor requires a data breakpoint's address to be aligned to the
    // width it watches. Saying so beats SetThreadContext failing with a generic
    // invalid-parameter error.
    auto unaligned = services.startAccessWatch(fixture.helper.address() + 1, 4, true);
    REQUIRE_FALSE(unaligned.has_value());
    CHECK(unaligned.error().find("aligned") != std::string::npos);

    auto badWidth = services.startAccessWatch(fixture.helper.address(), 3, true);
    REQUIRE_FALSE(badWidth.has_value());
    CHECK(badWidth.error().find("1, 2, 4 or 8") != std::string::npos);
}

TEST_CASE("A watch with no process attached is refused", "[access]") {
    services::RuntimeServices services;
    auto result = services.startAccessWatch(0x1000, 4, true);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("Attach") != std::string::npos);
}
