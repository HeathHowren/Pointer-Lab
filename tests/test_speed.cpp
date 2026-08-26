// Tests for the speed hack.
//
// The only assertion that means anything here is the one against a real
// process: inject the payload, ask the target what time it thinks it is, wait a
// known amount of real time, and ask again. Everything else -- that the module
// loaded, that the exports resolved, that a number was written -- can be true
// while the clock runs at exactly the speed it always did.
//
// tests/helper/main.cpp answers TIME by calling GetTickCount64 and
// QueryPerformanceCounter the ordinary way, so both go through its own import
// table, which is what the payload redirects.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_inject/Injector.h"
#include "engine_speed/SpeedController.h"

#include <chrono>
#include <thread>

using namespace ire;
using testsupport::AttachedHelper;

namespace {

struct Fixture {
    AttachedHelper attached;
    engine_inject::Injector injector{attached.session};
    engine_speed::SpeedController speed{attached.session, injector};
};

void pause(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// The payload's worker thread does the hooking, and it starts after
// LoadLibraryW has already returned. Polling is the honest way to wait for it;
// a fixed sleep would be a race dressed up as a delay.
bool waitForRunning(engine_speed::SpeedController& speed, int timeoutMs = 4000) {
    for (int waited = 0; waited < timeoutMs; waited += 25) {
        if (speed.status().running) {
            return true;
        }
        pause(25);
    }
    return false;
}

bool waitForScale(engine_speed::SpeedController& speed, double expected, int timeoutMs = 4000) {
    for (int waited = 0; waited < timeoutMs; waited += 25) {
        if (speed.status().applied == expected) {
            return true;
        }
        pause(25);
    }
    return false;
}

// How much the target thinks passed while `realMs` really did.
std::uint64_t observedElapsed(testsupport::HelperProcess& helper, int realMs) {
    // Asked first, so a target that died under the injection fails as "the
    // helper stopped answering" rather than as "the clock read zero".
    REQUIRE(helper.ticks() >= 0);
    const auto before = helper.times();
    pause(realMs);
    const auto after = helper.times();
    REQUIRE(before.milliseconds != 0);
    REQUIRE(after.milliseconds != 0);
    REQUIRE(after.milliseconds >= before.milliseconds);
    return after.milliseconds - before.milliseconds;
}

} // namespace

TEST_CASE("The payload redirects the target's clocks and speeds them up", "[speed][integration]") {
    Fixture fixture;

    REQUIRE(fixture.speed.load().has_value());
    REQUIRE(waitForRunning(fixture.speed));

    const auto loaded = fixture.speed.status();
    CHECK(loaded.loaded);
    // The number that separates "the payload is in there" from "the payload is
    // doing something". Zero here would mean the helper does not call the
    // timing functions through its import table, and every timing assertion
    // below would be measuring nothing.
    CHECK(loaded.hookedImports > 0);
    CHECK(loaded.applied == 1.0);

    // Unscaled first, as the control. A machine under load makes this longer
    // than 200ms but never shorter, so the bound is one-sided.
    const auto normal = observedElapsed(fixture.attached.helper, 200);
    CHECK(normal >= 150);
    CHECK(normal < 600);

    REQUIRE(fixture.speed.setScale(4.0).has_value());
    REQUIRE(waitForScale(fixture.speed, 4.0));

    const auto fast = observedElapsed(fixture.attached.helper, 200);
    // Deliberately loose. The assertion is "the clock is running much faster",
    // not "the clock is running at exactly four times", because the sampling
    // itself takes time and a shared build machine is not a metronome.
    CHECK(fast > normal * 2);

    REQUIRE(fixture.speed.setScale(0.25).has_value());
    REQUIRE(waitForScale(fixture.speed, 0.25));

    const auto slow = observedElapsed(fixture.attached.helper, 400);
    CHECK(slow < 300);
}

TEST_CASE("Time never jumps or runs backwards when the rate changes", "[speed][integration]") {
    Fixture fixture;
    REQUIRE(fixture.speed.load().has_value());
    REQUIRE(waitForRunning(fixture.speed));

    // The reason the payload rebases rather than multiplying. `real * scale`
    // would make this sequence jump forward by hours on the first change and
    // backwards by hours on the second, and a game that sees time go backwards
    // does not slow down -- it divides by a negative delta and detonates.
    std::uint64_t previous = fixture.attached.helper.times().milliseconds;
    REQUIRE(previous != 0);

    for (const double scale : {3.0, 0.5, 8.0, 1.0, 0.1}) {
        REQUIRE(fixture.speed.setScale(scale).has_value());
        REQUIRE(waitForScale(fixture.speed, scale));
        const auto now = fixture.attached.helper.times().milliseconds;
        REQUIRE(now != 0);
        CHECK(now >= previous);
        // A rebase moves the clock by whatever really elapsed across the poll,
        // which is milliseconds. A multiply would move it by the whole uptime
        // times the change in scale, which on any machine that has been on for
        // an hour is millions.
        CHECK(now - previous < 60000);
        previous = now;
    }
}

TEST_CASE("Removing the hook puts every redirected import back", "[speed][integration]") {
    Fixture fixture;
    REQUIRE(fixture.speed.load().has_value());
    REQUIRE(waitForRunning(fixture.speed));
    REQUIRE(fixture.speed.status().hookedImports > 0);

    REQUIRE(fixture.speed.setScale(6.0).has_value());
    REQUIRE(waitForScale(fixture.speed, 6.0));
    CHECK(observedElapsed(fixture.attached.helper, 200) > 400);

    REQUIRE(fixture.speed.reset().has_value());
    for (int waited = 0; waited < 4000 && fixture.speed.status().running; waited += 25) {
        pause(25);
    }
    CHECK_FALSE(fixture.speed.status().running);
    CHECK(fixture.speed.status().hookedImports == 0);

    // Back to real time, which is what "removing the hook" has to mean: the
    // import entries hold the addresses they held before anything was injected.
    const auto restored = observedElapsed(fixture.attached.helper, 200);
    CHECK(restored >= 150);
    CHECK(restored < 600);
}

TEST_CASE("A speed outside the supported range is refused, not clamped", "[speed][integration]") {
    Fixture fixture;
    REQUIRE(fixture.speed.load().has_value());

    // Clamping would leave a request for 1000x looking exactly like a hook that
    // is installed and doing nothing.
    for (const double bad : {0.0, -1.0, 0.001, 1000.0}) {
        auto refused = fixture.speed.setScale(bad);
        INFO(bad);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().find("Speed must be between") != std::string::npos);
    }
    CHECK(fixture.speed.setScale(engine_speed::SpeedController::minScale).has_value());
    CHECK(fixture.speed.setScale(engine_speed::SpeedController::maxScale).has_value());
}

TEST_CASE("Loading twice finds the payload already there", "[speed][integration]") {
    Fixture fixture;
    REQUIRE(fixture.speed.load().has_value());
    REQUIRE(waitForRunning(fixture.speed));
    const auto first = fixture.speed.status().hookedImports;

    // Not loaded a second time -- LoadLibraryW would simply bump a refcount,
    // but the payload's worker would not run again and the hook count would be
    // the tell if it had.
    REQUIRE(fixture.speed.load().has_value());
    pause(100);
    CHECK(fixture.speed.status().hookedImports == first);
}

TEST_CASE("Nothing can be sped up without a target", "[speed]") {
    platform_win32::Win32Platform platform;
    domain::TargetSession session(platform);
    engine_inject::Injector injector(session);
    engine_speed::SpeedController speed(session, injector);

    auto loaded = speed.load();
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find("Attach to a process") != std::string::npos);

    // reset() is called on every detach whether or not the feature was used, so
    // it has to be a no-op rather than an error when nothing was injected.
    CHECK(speed.reset().has_value());
    CHECK_FALSE(speed.status().loaded);
}
