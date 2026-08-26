// Tests for the patch registry.
//
// The registry's whole value is that a patch can be taken back, so the tests
// are mostly round-trips: write something over real bytes in a real target,
// then assert the originals come back byte for byte. The refusals matter just
// as much -- a registry that accepts an overlapping patch quietly records a
// wrong "original", and nobody finds out until they disable the two in the
// wrong order.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_patch/PatchRegistry.h"

#include <algorithm>
#include <numeric>

using namespace ire;
using testsupport::AttachedHelper;

namespace {

std::vector<std::uint8_t> read(domain::TargetSession& session, std::uintptr_t address, std::size_t size) {
    auto bytes = session.readBytes(address, size);
    REQUIRE(bytes.has_value());
    return bytes.value();
}

// Distinguishable filler, so a failed restore shows up as the wrong bytes
// rather than as a run of zeroes that could have come from anywhere.
std::vector<std::uint8_t> ramp(std::size_t size, std::uint8_t start) {
    std::vector<std::uint8_t> bytes(size);
    std::iota(bytes.begin(), bytes.end(), start);
    return bytes;
}

} // namespace

TEST_CASE("apply records the original bytes and writes the patch", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    REQUIRE(fixture.session.writeBytes(address, ramp(8, 0x10)).has_value());

    const auto id = registry.apply(address, ramp(8, 0xA0), "test patch", "original disassembly");
    REQUIRE(id.has_value());

    CHECK(read(fixture.session, address, 8) == ramp(8, 0xA0));

    const auto patch = registry.find(id.value());
    REQUIRE(patch.has_value());
    CHECK(patch->address == address);
    CHECK(patch->originalBytes == ramp(8, 0x10));
    CHECK(patch->patchedBytes == ramp(8, 0xA0));
    CHECK(patch->description == "test patch");
    CHECK(patch->originalText == "original disassembly");
    CHECK(patch->enabled);
    CHECK(patch->size() == 8);
    CHECK(patch->end() == address + 8);
}

TEST_CASE("disabling a patch puts the original bytes back", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    REQUIRE(fixture.session.writeBytes(address, ramp(4, 0x21)).has_value());
    const auto id = registry.apply(address, {0x90, 0x90, 0x90, 0x90}, "nop it");
    REQUIRE(id.has_value());

    REQUIRE(registry.setEnabled(id.value(), false).has_value());
    CHECK(read(fixture.session, address, 4) == ramp(4, 0x21));
    CHECK_FALSE(registry.find(id.value())->enabled);

    // And back on again. A one-way "undo" would be no better than the one-way
    // patch it replaced.
    REQUIRE(registry.setEnabled(id.value(), true).has_value());
    CHECK(read(fixture.session, address, 4) == std::vector<std::uint8_t>{0x90, 0x90, 0x90, 0x90});
    CHECK(registry.find(id.value())->enabled);

    // Setting the state it is already in writes nothing and is not an error.
    REQUIRE(registry.setEnabled(id.value(), true).has_value());
    CHECK(read(fixture.session, address, 4) == std::vector<std::uint8_t>{0x90, 0x90, 0x90, 0x90});
}

TEST_CASE("overlapping patches are refused", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    REQUIRE(registry.apply(address + 8, ramp(8, 0xB0), "first").has_value());

    // Exactly the same range, a range starting inside it, and a range ending
    // inside it. All three would capture the first patch's replacement bytes as
    // their "original".
    CHECK_FALSE(registry.apply(address + 8, ramp(8, 0xC0), "same range").has_value());
    CHECK_FALSE(registry.apply(address + 12, ramp(8, 0xC0), "starts inside").has_value());
    CHECK_FALSE(registry.apply(address + 4, ramp(8, 0xC0), "ends inside").has_value());
    CHECK_FALSE(registry.apply(address + 4, ramp(16, 0xC0), "encloses it").has_value());

    // Butting up against it on either side is fine: no byte is claimed twice.
    CHECK(registry.apply(address, ramp(8, 0xC0), "just before").has_value());
    CHECK(registry.apply(address + 16, ramp(8, 0xD0), "just after").has_value());
    CHECK(registry.patches().size() == 3);
}

TEST_CASE("remove restores the original bytes and forgets the patch", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    REQUIRE(fixture.session.writeBytes(address, ramp(6, 0x31)).has_value());
    const auto id = registry.apply(address, ramp(6, 0xE0), "temporary");
    REQUIRE(id.has_value());

    REQUIRE(registry.remove(id.value()).has_value());
    CHECK(read(fixture.session, address, 6) == ramp(6, 0x31));
    CHECK_FALSE(registry.find(id.value()).has_value());
    CHECK(registry.patches().empty());

    // Removing something that is not there says so rather than pretending.
    CHECK_FALSE(registry.remove(id.value()).has_value());
}

TEST_CASE("restoreAll disables every applied patch", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    REQUIRE(fixture.session.writeBytes(address, ramp(24, 0x41)).has_value());
    const auto first = registry.apply(address, ramp(8, 0xF0), "one");
    const auto second = registry.apply(address + 8, ramp(8, 0xF0), "two");
    const auto third = registry.apply(address + 16, ramp(8, 0xF0), "three");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    // Already off before the sweep: restoreAll must not mistake it for one that
    // needs writing, and must not report it as a failure either.
    REQUIRE(registry.setEnabled(second.value(), false).has_value());

    REQUIRE(registry.restoreAll().has_value());
    CHECK(read(fixture.session, address, 24) == ramp(24, 0x41));

    const auto patches = registry.patches();
    CHECK(patches.size() == 3);
    CHECK(std::none_of(patches.begin(), patches.end(), [](const engine_patch::Patch& p) { return p.enabled; }));
}

TEST_CASE("forgetAll drops the records without touching the target", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    REQUIRE(fixture.session.writeBytes(address, ramp(8, 0x51)).has_value());
    REQUIRE(registry.apply(address, ramp(8, 0x77), "left applied").has_value());

    registry.forgetAll();

    CHECK(registry.patches().empty());
    // The point of the distinction: the target keeps running patched.
    CHECK(read(fixture.session, address, 8) == ramp(8, 0x77));
}

TEST_CASE("covers reports only bytes an enabled patch owns", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    const auto id = registry.apply(address + 4, ramp(4, 0x60), "covered");
    REQUIRE(id.has_value());

    CHECK_FALSE(registry.covers(address + 3));
    CHECK(registry.covers(address + 4));
    CHECK(registry.covers(address + 7));
    CHECK_FALSE(registry.covers(address + 8));

    // A disabled patch owns nothing: the bytes there are the program's own
    // again, and marking them as modified would be a lie.
    REQUIRE(registry.setEnabled(id.value(), false).has_value());
    CHECK_FALSE(registry.covers(address + 4));
}

TEST_CASE("drift is detected when something else rewrites the bytes", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);
    const auto address = fixture.helper.scratch();

    REQUIRE(fixture.session.writeBytes(address, ramp(4, 0x71)).has_value());
    const auto id = registry.apply(address, ramp(4, 0x88), "watched");
    REQUIRE(id.has_value());
    CHECK_FALSE(registry.drifted(*registry.find(id.value())));

    // A write that goes around the registry, standing in for the target
    // rewriting its own code or an anti-tamper check putting it back.
    REQUIRE(fixture.session.writeBytes(address, ramp(4, 0x99)).has_value());
    CHECK(registry.drifted(*registry.find(id.value())));

    // Toggling writes over whatever is there, so the drift is gone afterwards
    // -- and the recorded original is what lands, not the interloper's bytes.
    REQUIRE(registry.setEnabled(id.value(), false).has_value());
    CHECK(read(fixture.session, address, 4) == ramp(4, 0x71));
    CHECK_FALSE(registry.drifted(*registry.find(id.value())));

    // A disabled patch drifts too: it is the original bytes that are expected
    // to be sitting there now.
    REQUIRE(fixture.session.writeBytes(address, ramp(4, 0x99)).has_value());
    CHECK(registry.drifted(*registry.find(id.value())));
}

TEST_CASE("a patch that cannot be applied records nothing", "[patch][integration]") {
    AttachedHelper fixture;
    engine_patch::PatchRegistry registry(fixture.session);

    CHECK_FALSE(registry.apply(fixture.helper.scratch(), {}, "no bytes").has_value());
    // A null page is never mapped, so this fails at the read.
    CHECK_FALSE(registry.apply(0x10, ramp(4, 0x01), "unmapped").has_value());
    CHECK(registry.patches().empty());
}

TEST_CASE("the registry refuses to work without a target", "[patch]") {
    platform_win32::Win32Platform platform;
    domain::TargetSession session(platform);
    engine_patch::PatchRegistry registry(session);

    const auto applied = registry.apply(0x1000, ramp(4, 0x01), "detached");
    REQUIRE_FALSE(applied.has_value());
    CHECK(applied.error().find("attached") != std::string::npos);
    CHECK(registry.patches().empty());
}
