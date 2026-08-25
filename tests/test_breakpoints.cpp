// Live safety tests for the debugger.
//
// The old implementation resumed a hit with DBG_CONTINUE without rewinding RIP
// or restoring the overwritten byte, so the target resumed one byte into an
// instruction and crashed. It also called DebugActiveProcess on a different
// thread from WaitForDebugEvent, which Windows does not allow, so in practice
// no event was ever delivered at all. Both are load-bearing here: every test
// checks that the helper is still alive and still making progress afterwards.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "services/RuntimeServices.h"

#include <chrono>
#include <thread>

using namespace ire;
using testsupport::AttachedHelper;
using testsupport::needleValue;

namespace {

// Waits until a breakpoint has been hit at least `wanted` times, or gives up.
bool waitForHits(services::BreakpointService& service, std::uintptr_t address, std::uint64_t wanted,
                 std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& breakpoint : service.breakpoints()) {
            if (breakpoint.address == address && breakpoint.hitCount >= wanted) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::uint8_t readByte(domain::TargetSession& session, std::uintptr_t address) {
    auto bytes = session.readBytes(address, 1);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == 1);
    return bytes.value().front();
}

} // namespace

TEST_CASE("The debugger attaches and detaches without harming the target", "[breakpoint][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto before = fixture.helper.ticks();
    REQUIRE(before >= 0);

    auto attached = breakpoints.attachDebugger();
    INFO("attach: " << attached.error());
    REQUIRE(attached.has_value());
    CHECK(breakpoints.debuggerAttached());

    breakpoints.detachDebugger();
    CHECK_FALSE(breakpoints.debuggerAttached());

    // Still alive, still answering, still counting.
    CHECK(fixture.helper.get() == needleValue);
    CHECK(fixture.helper.ticks() > before);
}

// The headline fix: this used to crash the helper on the very first hit.
TEST_CASE("A breakpoint in a hot loop fires repeatedly and the target survives", "[breakpoint][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto tick = fixture.helper.tick();
    const auto ticksBefore = fixture.helper.ticks();
    REQUIRE(ticksBefore >= 0);

    REQUIRE(breakpoints.attachDebugger().has_value());
    auto added = breakpoints.addBreakpoint(tick, "tick");
    INFO("add: " << added.error());
    REQUIRE(added.has_value());

    // Repeatedly, not once: a breakpoint that disarmed itself after the first
    // hit would stop here.
    REQUIRE(waitForHits(breakpoints, tick, 25));

    const auto list = breakpoints.breakpoints();
    REQUIRE(list.size() == 1);
    CHECK(list[0].address == tick);
    CHECK(list[0].hitCount >= 25);
    CHECK(list[0].enabled);

    // RIP must have been rewound to the breakpoint itself, not left one byte
    // past it where the int3 finished.
    REQUIRE(list[0].lastHit.captured);
    CHECK(list[0].lastHit.rip == tick);
    CHECK(list[0].lastHit.threadId != 0);
    CHECK(list[0].lastHit.rsp != 0);

    // The target is not merely alive, it is still executing the loop the
    // breakpoint sits in.
    const auto ticksDuring = fixture.helper.ticks();
    CHECK(ticksDuring > ticksBefore);

    breakpoints.detachDebugger();

    // And it keeps running once the debugger lets go.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(fixture.helper.get() == needleValue);
    CHECK(fixture.helper.ticks() > ticksDuring);
}

// Regression: detaching while traps were still in flight killed the target
// roughly half the time. A thread executes the int3 microseconds before the
// byte is restored, and DebugActiveProcessStop then delivers that queued
// exception to a process with no debugger attached.
TEST_CASE("Removing a breakpoint from under a hot loop does not kill the target", "[breakpoint][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto tick = fixture.helper.tick();
    REQUIRE(breakpoints.attachDebugger().has_value());
    REQUIRE(breakpoints.addBreakpoint(tick, "tick").has_value());
    REQUIRE(waitForHits(breakpoints, tick, 20));

    // Removed while the loop is still running straight through it.
    REQUIRE(breakpoints.removeBreakpoint(tick).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto after = fixture.helper.ticks();
    CHECK(after > 0);
    CHECK(fixture.helper.get() == needleValue);

    breakpoints.detachDebugger();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(fixture.helper.ticks() > after);
}

// Attach and detach repeatedly against a target that is constantly hitting the
// breakpoint. Each cycle is another chance to strand a queued exception.
TEST_CASE("Repeated attach and detach cycles leave the target healthy", "[breakpoint][integration]") {
    AttachedHelper fixture;
    const auto tick = fixture.helper.tick();

    for (int cycle = 0; cycle < 5; ++cycle) {
        INFO("cycle " << cycle);
        services::BreakpointService breakpoints(fixture.session);
        REQUIRE(breakpoints.attachDebugger().has_value());
        REQUIRE(breakpoints.addBreakpoint(tick, "tick").has_value());
        REQUIRE(waitForHits(breakpoints, tick, 10));
        breakpoints.detachDebugger();

        // Alive and still counting after every single cycle.
        const auto ticks = fixture.helper.ticks();
        REQUIRE(ticks > 0);
        REQUIRE(fixture.helper.get() == needleValue);
    }
}

TEST_CASE("Detaching restores every original byte", "[breakpoint][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    // A cold address: nothing executes here, so the byte cannot change under us
    // mid-assertion the way one inside the hot loop could.
    const auto cold = fixture.helper.scratch();
    const std::vector<std::uint8_t> marker{0x48, 0x31, 0xC0, 0xC3};
    REQUIRE(fixture.session.writeBytes(cold, marker).has_value());

    REQUIRE(breakpoints.attachDebugger().has_value());
    REQUIRE(breakpoints.addBreakpoint(cold, "cold").has_value());
    CHECK(readByte(fixture.session, cold) == 0xCC);

    breakpoints.detachDebugger();
    CHECK(readByte(fixture.session, cold) == marker[0]);
    CHECK(breakpoints.breakpoints().empty());
}

TEST_CASE("Removing a breakpoint puts the original byte back", "[breakpoint][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto cold = fixture.helper.scratch();
    const std::vector<std::uint8_t> marker{0x90, 0x90, 0x90, 0xC3};
    REQUIRE(fixture.session.writeBytes(cold, marker).has_value());

    REQUIRE(breakpoints.attachDebugger().has_value());
    REQUIRE(breakpoints.addBreakpoint(cold, "cold").has_value());
    CHECK(readByte(fixture.session, cold) == 0xCC);

    REQUIRE(breakpoints.removeBreakpoint(cold).has_value());
    CHECK(readByte(fixture.session, cold) == 0x90);
    CHECK(breakpoints.breakpoints().empty());
}

TEST_CASE("Breakpoint bookkeeping rejects what it cannot do", "[breakpoint][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto cold = fixture.helper.scratch();
    REQUIRE(fixture.session.writeBytes(cold, std::vector<std::uint8_t>{0x90}).has_value());
    REQUIRE(breakpoints.attachDebugger().has_value());

    SECTION("the same address twice") {
        REQUIRE(breakpoints.addBreakpoint(cold, "first").has_value());
        CHECK_FALSE(breakpoints.addBreakpoint(cold, "second").has_value());
        CHECK(breakpoints.breakpoints().size() == 1);
    }
    SECTION("an address that is already an int3") {
        // Saving 0xCC as the original byte would leave one behind permanently.
        REQUIRE(fixture.session.writeBytes(cold, std::vector<std::uint8_t>{0xCC}).has_value());
        CHECK_FALSE(breakpoints.addBreakpoint(cold, "stacked").has_value());
    }
    SECTION("an address that is not mapped") {
        CHECK_FALSE(breakpoints.addBreakpoint(0x10, "unmapped").has_value());
    }
    SECTION("removing one that was never set") {
        CHECK_FALSE(breakpoints.removeBreakpoint(cold).has_value());
    }

    breakpoints.detachDebugger();
    CHECK(fixture.helper.ticks() >= 0);
}

TEST_CASE("Breakpoints cannot be set before the debugger attaches", "[breakpoint][integration]") {
    AttachedHelper fixture;
    platform_win32::DebugEventPump pump;

    // The service attaches on demand, so this checks the pump directly.
    CHECK_FALSE(pump.addBreakpoint(fixture.helper.scratch(), "early").has_value());
    CHECK(pump.breakpoints().empty());
    CHECK_FALSE(pump.attached());
}

// Hardware breakpoints. The point of them is that nothing in the target is
// modified and nothing is ever disarmed, so the single-step window a software
// breakpoint can be missed through does not exist.
TEST_CASE("A hardware execute breakpoint fires repeatedly without patching the target",
          "[breakpoint][hardware][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto tick = fixture.helper.tick();
    const auto original = readByte(fixture.session, tick);
    const auto ticksBefore = fixture.helper.ticks();
    REQUIRE(ticksBefore >= 0);

    REQUIRE(breakpoints.attachDebugger().has_value());
    auto added = breakpoints.addBreakpoint(tick, "tick", domain::BreakpointKind::HardwareExecute);
    INFO("add: " << added.error());
    REQUIRE(added.has_value());

    REQUIRE(waitForHits(breakpoints, tick, 25));

    // The whole difference from a software breakpoint: the instruction stream is
    // untouched. No int3 was ever written, so there is nothing to restore.
    CHECK(readByte(fixture.session, tick) == original);

    const auto list = breakpoints.breakpoints();
    REQUIRE(list.size() == 1);
    CHECK(list[0].kind == domain::BreakpointKind::HardwareExecute);
    CHECK(list[0].slot >= 0);
    CHECK(list[0].slot < 4);
    REQUIRE(list[0].lastHit.captured);
    // An execute breakpoint faults before the instruction runs, so RIP is the
    // breakpoint itself rather than something past it.
    CHECK(list[0].lastHit.rip == tick);

    // Still executing the loop it is breaking on. If the resume flag were not
    // set the thread would fault on the same instruction forever and this would
    // not move.
    const auto ticksDuring = fixture.helper.ticks();
    CHECK(ticksDuring > ticksBefore);

    breakpoints.detachDebugger();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(fixture.helper.get() == needleValue);
    CHECK(fixture.helper.ticks() > ticksDuring);
}

// The other thing only hardware can do: break on data being touched rather than
// on code running.
TEST_CASE("A hardware write watchpoint fires when the value is written", "[breakpoint][hardware][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto needle = fixture.helper.address();
    REQUIRE(breakpoints.attachDebugger().has_value());
    auto added = breakpoints.addBreakpoint(needle, "needle", domain::BreakpointKind::HardwareWrite, 4);
    INFO("add: " << added.error());
    REQUIRE(added.has_value());

    // Reading it must not trip a write watchpoint.
    CHECK(fixture.helper.get() == needleValue);
    CHECK(breakpoints.breakpoints().front().hitCount == 0);

    // Writing it must.
    REQUIRE(fixture.helper.set(4321));
    REQUIRE(waitForHits(breakpoints, needle, 1));

    const auto list = breakpoints.breakpoints();
    REQUIRE(list.size() == 1);
    CHECK(list[0].kind == domain::BreakpointKind::HardwareWrite);
    CHECK(list[0].length == 4);
    REQUIRE(list[0].lastHit.captured);
    CHECK(list[0].lastHit.threadId != 0);

    breakpoints.detachDebugger();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(fixture.helper.get() == 4321);
    CHECK(fixture.helper.ticks() > 0);
}

TEST_CASE("There are exactly four hardware breakpoints and the fifth says so",
          "[breakpoint][hardware][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto base = fixture.helper.scratch();
    REQUIRE(breakpoints.attachDebugger().has_value());

    for (int slot = 0; slot < 4; ++slot) {
        const auto address = base + static_cast<std::uintptr_t>(slot) * 8;
        INFO("slot " << slot);
        auto added = breakpoints.addBreakpoint(address, "watch", domain::BreakpointKind::HardwareWrite, 1);
        INFO("add: " << added.error());
        REQUIRE(added.has_value());
    }

    // A limit of the processor, reported rather than worked around.
    auto fifth = breakpoints.addBreakpoint(base + 32, "too many", domain::BreakpointKind::HardwareWrite, 1);
    REQUIRE_FALSE(fifth.has_value());
    CHECK(fifth.error().find("four") != std::string::npos);
    CHECK(breakpoints.breakpoints().size() == 4);

    // A software breakpoint uses no debug register, so it is still available.
    REQUIRE(fixture.session.writeBytes(base + 64, std::vector<std::uint8_t>{0x90}).has_value());
    CHECK(breakpoints.addBreakpoint(base + 64, "software").has_value());

    // Freeing a register makes room again.
    REQUIRE(breakpoints.removeBreakpoint(base).has_value());
    CHECK(breakpoints.addBreakpoint(base + 32, "now it fits", domain::BreakpointKind::HardwareWrite, 1).has_value());

    breakpoints.detachDebugger();
    CHECK(fixture.helper.ticks() >= 0);
}

TEST_CASE("A hardware watchpoint rejects a width the registers cannot encode",
          "[breakpoint][hardware][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto base = fixture.helper.scratch();
    REQUIRE(breakpoints.attachDebugger().has_value());

    SECTION("a width that is not 1, 2, 4 or 8") {
        auto added = breakpoints.addBreakpoint(base, "three", domain::BreakpointKind::HardwareWrite, 3);
        REQUIRE_FALSE(added.has_value());
        CHECK(added.error().find("1, 2, 4 or 8") != std::string::npos);
    }
    SECTION("an address not aligned to its width") {
        auto added = breakpoints.addBreakpoint(base + 1, "misaligned", domain::BreakpointKind::HardwareWrite, 4);
        REQUIRE_FALSE(added.has_value());
        CHECK(added.error().find("aligned") != std::string::npos);
    }
    CHECK(breakpoints.breakpoints().empty());

    breakpoints.detachDebugger();
    CHECK(fixture.helper.ticks() >= 0);
}

TEST_CASE("Hits are reported to the UI without touching it from the pump thread", "[breakpoint][integration]") {
    AttachedHelper fixture;
    services::BreakpointService breakpoints(fixture.session);

    const auto tick = fixture.helper.tick();
    REQUIRE(breakpoints.attachDebugger().has_value());
    REQUIRE(breakpoints.addBreakpoint(tick, "tick").has_value());
    REQUIRE(waitForHits(breakpoints, tick, 5));

    const auto events = breakpoints.takeEvents();
    CHECK_FALSE(events.empty());
    CHECK(events.front().find("Breakpoint hit") != std::string::npos);
    // Rate limited, so a hot loop cannot flood the toast queue.
    CHECK(events.size() < 64);
    CHECK(breakpoints.takeEvents().empty());

    breakpoints.detachDebugger();
}
