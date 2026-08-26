// Tests for the structure dissector.
//
// Two halves. The classification heuristics are pure functions over bytes and
// are tested as such -- they are the part most likely to be wrong, and they
// need no process at all. Everything above them is tested against a real
// target, because "is this number a pointer?" is a question only a real address
// space can answer, and the answer is the whole feature.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_struct/Dissector.h"

#include <cstring>

using namespace ire;
using testsupport::AttachedHelper;
using testsupport::HelperBitness;

namespace {

std::vector<std::uint8_t> floatBytes(float value) {
    std::vector<std::uint8_t> bytes(4);
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

std::vector<std::uint8_t> doubleBytes(double value) {
    std::vector<std::uint8_t> bytes(8);
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

std::vector<std::uint8_t> intBytes(std::int32_t value) {
    std::vector<std::uint8_t> bytes(4);
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

std::vector<std::uint8_t> pointerBytes(std::uint64_t value, std::size_t width) {
    std::vector<std::uint8_t> bytes(width);
    for (std::size_t i = 0; i < width; ++i) {
        bytes[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
    return bytes;
}

void append(std::vector<std::uint8_t>& into, const std::vector<std::uint8_t>& what) {
    into.insert(into.end(), what.begin(), what.end());
}

struct Fixture {
    AttachedHelper attached;
    engine_symbols::SymbolTable symbols;
    engine_struct::Dissector dissector{attached.session, symbols};

    explicit Fixture(HelperBitness bitness = HelperBitness::X64) : attached(bitness) {}

    [[nodiscard]] domain::TargetSession& session() { return attached.session; }
    // The helper's spare page, past the value it advertises. Writable, committed
    // and nothing else in the process cares what is in it.
    [[nodiscard]] std::uintptr_t scratch() const { return attached.helper.scratch(); }

    void write(std::uintptr_t address, const std::vector<std::uint8_t>& bytes) {
        REQUIRE(session().writeBytes(address, bytes).has_value());
    }
};

} // namespace

TEST_CASE("A small integer does not read as a float, and a float does", "[struct]") {
    using engine_struct::looksLikeFloat;

    // The reason the heuristic works: the two interpretations barely overlap.
    // 100 stored as an int reads as a denormal around 1e-43, which is not a
    // number anybody stores.
    CHECK_FALSE(looksLikeFloat(intBytes(100)));
    CHECK_FALSE(looksLikeFloat(intBytes(1)));
    CHECK_FALSE(looksLikeFloat(intBytes(65535)));
    CHECK_FALSE(looksLikeFloat(intBytes(-1)));

    CHECK(looksLikeFloat(floatBytes(100.0f)));
    CHECK(looksLikeFloat(floatBytes(-3.5f)));
    CHECK(looksLikeFloat(floatBytes(0.001f)));
    CHECK(looksLikeFloat(floatBytes(1234567.0f)));

    // Zero is refused, not because it is not a valid float but because it is
    // equally a valid anything: an int field that happens to be zero would be
    // called a float on no evidence.
    CHECK_FALSE(looksLikeFloat(floatBytes(0.0f)));
    CHECK_FALSE(looksLikeFloat(floatBytes(1e30f)));
    CHECK_FALSE(looksLikeFloat(floatBytes(1e-30f)));
    CHECK_FALSE(looksLikeFloat({0xFF, 0xFF, 0xFF, 0x7F})); // NaN
    CHECK_FALSE(looksLikeFloat({0x00, 0x00, 0x80, 0x7F})); // +inf
    // Wrong width is not a float, whatever the bytes say.
    CHECK_FALSE(looksLikeFloat(doubleBytes(1.0)));
}

TEST_CASE("The same rules apply to doubles at eight bytes", "[struct]") {
    using engine_struct::looksLikeDouble;

    CHECK(looksLikeDouble(doubleBytes(1234.5)));
    CHECK(looksLikeDouble(doubleBytes(-0.25)));
    CHECK_FALSE(looksLikeDouble(doubleBytes(0.0)));
    CHECK_FALSE(looksLikeDouble(doubleBytes(1e300)));
    CHECK_FALSE(looksLikeDouble(floatBytes(1.0f)));
    // Two unrelated 32-bit fields side by side. Reading them as one double is
    // exactly the mistake an unaligned eight-byte guess would make.
    std::vector<std::uint8_t> pair;
    append(pair, floatBytes(3.5f));
    append(pair, intBytes(100));
    CHECK_FALSE(looksLikeDouble(pair));
}

TEST_CASE("A structure knows its own size and which field owns a byte", "[struct]") {
    domain::Structure structure;
    structure.fields.push_back({0x00, domain::ValueType::UInt64, 0, "next"});
    structure.fields.push_back({0x10, domain::ValueType::Float, 0, "health"});

    CHECK(structure.sizeInBytes() == 0x14);
    REQUIRE(structure.fieldAt(0x10) != nullptr);
    CHECK(structure.fieldAt(0x10)->name == "health");
    // Not the same question: an eight-byte field at +0 owns +4 as well, and
    // nothing starts there.
    CHECK(structure.fieldAt(0x04) == nullptr);
    REQUIRE(structure.fieldCovering(0x04) != nullptr);
    CHECK(structure.fieldCovering(0x04)->name == "next");
    CHECK(structure.fieldCovering(0x08) == nullptr);

    CHECK(domain::defaultFieldName(0x2C) == "field_2c");
    // A structure's start is a guess. Finding the real object begins earlier
    // must not mean renumbering everything below it.
    CHECK(domain::defaultFieldName(-8) == "field_minus_8");
}

TEST_CASE("Fields may not overlap, and a variable-width field needs a length", "[struct][integration]") {
    Fixture fixture;
    const auto id = fixture.dissector.add("Player");

    REQUIRE(fixture.dissector.setField(id, {0x00, domain::ValueType::UInt64, 0, "next"}).has_value());
    // Adjacent is fine; that is what a structure is.
    REQUIRE(fixture.dissector.setField(id, {0x08, domain::ValueType::Int32, 0, "team"}).has_value());

    auto clash = fixture.dissector.setField(id, {0x04, domain::ValueType::Int32, 0, "inside next"});
    REQUIRE_FALSE(clash.has_value());
    CHECK(clash.error().find("overlap") != std::string::npos);

    // Replacing the field that already starts at an offset is not an overlap.
    REQUIRE(fixture.dissector.setField(id, {0x00, domain::ValueType::Double, 0, "position"}).has_value());
    CHECK(fixture.dissector.find(id)->fields.size() == 2);
    CHECK(fixture.dissector.find(id)->fieldAt(0)->name == "position");

    auto widthless = fixture.dissector.setField(id, {0x20, domain::ValueType::Bytes, 0, "blob"});
    REQUIRE_FALSE(widthless.has_value());
    CHECK(widthless.error().find("needs a length") != std::string::npos);
    REQUIRE(fixture.dissector.setField(id, {0x20, domain::ValueType::Bytes, 6, "blob"}).has_value());

    // Kept in offset order however they were added, because the panel reads top
    // to bottom and an object does not.
    const auto fields = fixture.dissector.find(id)->fields;
    CHECK(fields[0].offset == 0x00);
    CHECK(fields[1].offset == 0x08);
    CHECK(fields[2].offset == 0x20);

    REQUIRE(fixture.dissector.removeField(id, 0x08).has_value());
    CHECK(fixture.dissector.removeField(id, 0x08).has_value() == false);
}

TEST_CASE("Reading lays the layout over a real object", "[struct][integration]") {
    Fixture fixture;
    const auto base = fixture.scratch();

    std::vector<std::uint8_t> object;
    append(object, intBytes(1234));
    append(object, floatBytes(87.5f));
    fixture.write(base, object);

    const auto id = fixture.dissector.add("Player");
    REQUIRE(fixture.dissector.setField(id, {0x00, domain::ValueType::Int32, 0, "score"}).has_value());
    REQUIRE(fixture.dissector.setField(id, {0x04, domain::ValueType::Float, 0, "health"}).has_value());

    auto snapshot = fixture.dissector.read(id, {base});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().rows.size() == 2);
    CHECK(snapshot.value().unreadable.empty());
    CHECK(snapshot.value().rows[0].cells[0].read);
    CHECK(snapshot.value().rows[0].cells[0].text == "1234");
    CHECK(snapshot.value().rows[1].cells[0].text.rfind("87.5", 0) == 0);
    // One address, so "identical" is trivially true and means nothing; the flag
    // only earns its keep with a second column.
    CHECK(snapshot.value().rows[0].identical);
}

TEST_CASE("Two instances side by side show which fields describe an object", "[struct][integration]") {
    Fixture fixture;
    const auto first = fixture.scratch();
    const auto second = fixture.scratch() + 0x40;

    // Same team, different health. That difference is the entire point: the
    // fields that vary between two players are the ones that describe a player.
    std::vector<std::uint8_t> a;
    append(a, intBytes(7));
    append(a, floatBytes(100.0f));
    std::vector<std::uint8_t> b;
    append(b, intBytes(7));
    append(b, floatBytes(42.0f));
    fixture.write(first, a);
    fixture.write(second, b);

    const auto id = fixture.dissector.add("Player");
    REQUIRE(fixture.dissector.setField(id, {0x00, domain::ValueType::Int32, 0, "team"}).has_value());
    REQUIRE(fixture.dissector.setField(id, {0x04, domain::ValueType::Float, 0, "health"}).has_value());

    auto snapshot = fixture.dissector.read(id, {first, second});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().rows.size() == 2);
    CHECK(snapshot.value().rows[0].identical);
    CHECK_FALSE(snapshot.value().rows[1].identical);
    CHECK(snapshot.value().rows[1].cells[0].text.rfind("100", 0) == 0);
    CHECK(snapshot.value().rows[1].cells[1].text.rfind("42", 0) == 0);
}

TEST_CASE("An address that cannot be read is named, not filled in with zeroes",
          "[struct][integration]") {
    Fixture fixture;
    const auto good = fixture.scratch();
    // Nothing is mapped here, and reporting it as a column of zeroes would look
    // exactly like an object whose fields are all zero.
    const std::uintptr_t bad = 0x10;

    fixture.write(good, intBytes(99));
    const auto id = fixture.dissector.add("Player");
    REQUIRE(fixture.dissector.setField(id, {0x00, domain::ValueType::Int32, 0, "score"}).has_value());

    auto snapshot = fixture.dissector.read(id, {good, bad});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().unreadable == std::vector<std::size_t>{1});
    CHECK(snapshot.value().rows[0].cells[0].read);
    CHECK_FALSE(snapshot.value().rows[0].cells[1].read);
    // The instance that went away must not make the remaining ones look like
    // they disagree.
    CHECK(snapshot.value().rows[0].identical);
}

TEST_CASE("A field above the start of the object is read, not clipped", "[struct][integration]") {
    Fixture fixture;
    const auto base = fixture.scratch();

    fixture.write(base - 4, intBytes(-1));
    fixture.write(base, intBytes(1));

    const auto id = fixture.dissector.add("Player");
    REQUIRE(fixture.dissector.setField(id, {-4, domain::ValueType::Int32, 0, "header"}).has_value());
    REQUIRE(fixture.dissector.setField(id, {0, domain::ValueType::Int32, 0, "first"}).has_value());

    auto snapshot = fixture.dissector.read(id, {base});
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot.value().rows.size() == 2);
    CHECK(snapshot.value().rows[0].field.offset == -4);
    CHECK(snapshot.value().rows[0].cells[0].text == "-1");
    CHECK(snapshot.value().rows[1].cells[0].text == "1");
}

TEST_CASE("The guess names a pointer, a float and an int for what they are",
          "[struct][integration]") {
    Fixture fixture;
    const auto base = fixture.scratch();
    const auto pointerWidth = fixture.session().pointerSize();
    if (pointerWidth != 8) {
        SKIP("This layout is written for a 64-bit target.");
    }

    std::vector<std::uint8_t> object;
    // A real address in the target, which is what makes it a pointer rather
    // than a large number: the classifier checks that it lands in a committed
    // region, not that it looks round.
    append(object, pointerBytes(fixture.attached.helper.address(), 8));
    append(object, floatBytes(3.5f));
    append(object, intBytes(100));
    append(object, doubleBytes(1234.5));
    fixture.write(base, object);

    const auto id = fixture.dissector.add("Player");
    auto guessed = fixture.dissector.guess(id, {base}, object.size());
    REQUIRE(guessed.has_value());

    const auto fields = fixture.dissector.find(id)->fields;
    REQUIRE(fields.size() == 4);
    CHECK(fields[0].offset == 0x00);
    CHECK(fields[0].type == domain::ValueType::UInt64);
    CHECK(fields[1].offset == 0x08);
    CHECK(fields[1].type == domain::ValueType::Float);
    CHECK(fields[2].offset == 0x0C);
    CHECK(fields[2].type == domain::ValueType::Int32);
    CHECK(fields[3].offset == 0x10);
    CHECK(fields[3].type == domain::ValueType::Double);
    // Named by offset rather than by position, so inserting a field above one
    // never renames it.
    CHECK(fields[2].name == "field_c");

    // And the pointer is annotated with where it goes, which is what turns a
    // guessed layout into something you can follow.
    auto snapshot = fixture.dissector.read(id, {base});
    REQUIRE(snapshot.has_value());
    CHECK_FALSE(snapshot.value().rows[0].cells[0].annotation.empty());
    CHECK(snapshot.value().rows[1].cells[0].annotation.empty());
}

TEST_CASE("A run of zeroes is not called a pointer", "[struct][integration]") {
    Fixture fixture;
    const auto base = fixture.scratch();
    fixture.write(base, std::vector<std::uint8_t>(32, 0));

    const auto id = fixture.dissector.add("Empty");
    REQUIRE(fixture.dissector.guess(id, {base}, 32).has_value());

    // Every field integral, none of them a pointer or a float: otherwise every
    // padded gap in every object would be full of pointers to nothing.
    for (const auto& field : fixture.dissector.find(id)->fields) {
        CHECK(field.type == domain::ValueType::Int32);
    }
}

TEST_CASE("The guess finds the pointer the helper really has", "[struct][integration]") {
    Fixture fixture;
    if (fixture.session().pointerSize() != 8) {
        SKIP("Offsets below are written for a 64-bit target.");
    }

    // Not a value written by the test: `g_root` is a module global the helper
    // set to point at its own node, and finding it is the same thing the
    // feature does in a game.
    const auto id = fixture.dissector.add("Root");
    REQUIRE(fixture.dissector.guess(id, {fixture.attached.helper.root()}, 8).has_value());

    const auto fields = fixture.dissector.find(id)->fields;
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].type == domain::ValueType::UInt64);
}

TEST_CASE("The guess works the same way against a 32-bit target", "[struct][integration]") {
    if (!testsupport::helperAvailable(HelperBitness::X86)) {
        SKIP("The 32-bit test helper was not built (-DPOINTERLAB_BUILD_HELPER32=OFF).");
    }
    Fixture fixture(HelperBitness::X86);
    REQUIRE(fixture.session().pointerSize() == 4);

    // A pointer is four bytes here, so the same slot that would be one half of
    // a 64-bit pointer is a whole pointer of its own. Getting this wrong is
    // silent: the guess would call everything an int and nothing would fail.
    const auto id = fixture.dissector.add("Root");
    REQUIRE(fixture.dissector.guess(id, {fixture.attached.helper.root()}, 4).has_value());

    const auto fields = fixture.dissector.find(id)->fields;
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].type == domain::ValueType::UInt32);
}

TEST_CASE("The dissector refuses what it cannot show", "[struct][integration]") {
    Fixture fixture;
    const auto id = fixture.dissector.add("Player");

    auto none = fixture.dissector.read(id, {});
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().find("at least one address") != std::string::npos);

    std::vector<std::uintptr_t> many(engine_struct::Dissector::maxAddresses + 1, fixture.scratch());
    auto crowded = fixture.dissector.read(id, many);
    REQUIRE_FALSE(crowded.has_value());
    CHECK(crowded.error().find("side by side") != std::string::npos);

    auto huge = fixture.dissector.guess(id, {fixture.scratch()},
                                        engine_struct::Dissector::maxSize + 4);
    REQUIRE_FALSE(huge.has_value());

    auto unreadable = fixture.dissector.guess(id, {0x10}, 0x40);
    REQUIRE_FALSE(unreadable.has_value());
    CHECK(unreadable.error().find("does not start where you think") != std::string::npos);

    // A structure with no fields is not an error, it is a structure nobody has
    // filled in yet. It comes back with no rows and no complaint.
    auto empty = fixture.dissector.read(id, {fixture.scratch()});
    REQUIRE(empty.has_value());
    CHECK(empty.value().rows.empty());

    REQUIRE(fixture.dissector.remove(id).has_value());
    CHECK_FALSE(fixture.dissector.read(id, {fixture.scratch()}).has_value());
}
