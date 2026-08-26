// The scan modes and value types added for parity with what the book teaches:
// value between, bigger/smaller than, increased/decreased by, same as first
// scan, and text search in both ASCII and UTF-16.
//
// The arithmetic is checked here without a target, and the two things that can
// only be wrong across a process boundary -- finding text in another process's
// memory, and carrying the first-scan value through a rescan -- are checked
// against the helper.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "domain/Domain.h"
#include "engine_scan/MemoryScanner.h"

#include <algorithm>
#include <cstring>

using namespace ire;
using testsupport::AttachedHelper;

namespace {

template <typename T>
std::vector<std::uint8_t> raw(T value) {
    std::vector<std::uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

domain::ScanValue numeric(domain::ValueType type, std::vector<std::uint8_t> bytes,
                          std::vector<std::uint8_t> second = {}) {
    domain::ScanValue value;
    value.type = type;
    value.bytes = std::move(bytes);
    value.bytes2 = std::move(second);
    return value;
}

bool compare(domain::ScanMode mode, const domain::ScanValue& value, const std::vector<std::uint8_t>& current,
             const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& first = {}) {
    return engine_scan::compareValues(mode, value, current, previous, first.empty() ? previous : first);
}

bool contains(const std::vector<domain::ScanResult>& results, std::uintptr_t address) {
    return std::any_of(results.begin(), results.end(),
                       [address](const domain::ScanResult& r) { return r.address == address; });
}

void waitForScan(engine_scan::ScanJob& job) {
    for (int i = 0; i < 600 && job.progress().running; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(job.progress().running);
}

} // namespace

// ---------------------------------------------------------------------------
// Mode classification
// ---------------------------------------------------------------------------

TEST_CASE("Modes are classified by what they need to run", "[scan]") {
    // Absolute filters test the value as it stands, so a first scan can use
    // them. That is the whole difference from the relative ones.
    CHECK(engine_scan::modeIsAbsolute(domain::ScanMode::ValueBetween));
    CHECK(engine_scan::modeIsAbsolute(domain::ScanMode::BiggerThan));
    CHECK(engine_scan::modeIsAbsolute(domain::ScanMode::SmallerThan));
    CHECK_FALSE(engine_scan::modeNeedsBaseline(domain::ScanMode::ValueBetween));
    CHECK_FALSE(engine_scan::modeNeedsBaseline(domain::ScanMode::BiggerThan));
    CHECK_FALSE(engine_scan::modeNeedsBaseline(domain::ScanMode::SmallerThan));

    CHECK(engine_scan::modeNeedsBaseline(domain::ScanMode::IncreasedBy));
    CHECK(engine_scan::modeNeedsBaseline(domain::ScanMode::DecreasedBy));
    CHECK(engine_scan::modeNeedsBaseline(domain::ScanMode::SameAsFirst));
    CHECK_FALSE(engine_scan::modeIsAbsolute(domain::ScanMode::IncreasedBy));

    // Only "value between" takes a second operand; the by-how-much modes carry
    // their delta in the first one.
    CHECK(domain::modeUsesSecondValue(domain::ScanMode::ValueBetween));
    CHECK_FALSE(domain::modeUsesSecondValue(domain::ScanMode::IncreasedBy));
    CHECK(domain::modeUsesValue(domain::ScanMode::IncreasedBy));
    CHECK_FALSE(domain::modeUsesValue(domain::ScanMode::SameAsFirst));
}

TEST_CASE("Ordering modes are refused for types that have no ordering", "[scan]") {
    for (const auto type : {domain::ValueType::Bytes, domain::ValueType::StringAscii,
                            domain::ValueType::StringUtf16}) {
        CHECK(engine_scan::modeSupportsType(domain::ScanMode::Exact, type));
        CHECK(engine_scan::modeSupportsType(domain::ScanMode::Changed, type));
        CHECK(engine_scan::modeSupportsType(domain::ScanMode::SameAsFirst, type));

        CHECK_FALSE(engine_scan::modeSupportsType(domain::ScanMode::BiggerThan, type));
        CHECK_FALSE(engine_scan::modeSupportsType(domain::ScanMode::ValueBetween, type));
        CHECK_FALSE(engine_scan::modeSupportsType(domain::ScanMode::IncreasedBy, type));
        // Excluded for a different reason: a baseline sweep needs a width, and
        // these types have none until a value is typed.
        CHECK_FALSE(engine_scan::modeSupportsType(domain::ScanMode::UnknownInitial, type));
    }

    // Every mode is available for a numeric type.
    for (const auto mode : domain::scanModes()) {
        CHECK(engine_scan::modeSupportsType(mode, domain::ValueType::Int32));
    }
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

TEST_CASE("Value between accepts the bounds in either order", "[scan]") {
    const auto value = numeric(domain::ValueType::Int32, raw<std::int32_t>(100), raw<std::int32_t>(200));
    const auto reversed = numeric(domain::ValueType::Int32, raw<std::int32_t>(200), raw<std::int32_t>(100));

    for (const auto& bounds : {value, reversed}) {
        CHECK(compare(domain::ScanMode::ValueBetween, bounds, raw<std::int32_t>(150), {}));
        // Inclusive at both ends, which is what "between 100 and 200" means to
        // anyone who has not read the source.
        CHECK(compare(domain::ScanMode::ValueBetween, bounds, raw<std::int32_t>(100), {}));
        CHECK(compare(domain::ScanMode::ValueBetween, bounds, raw<std::int32_t>(200), {}));
        CHECK_FALSE(compare(domain::ScanMode::ValueBetween, bounds, raw<std::int32_t>(99), {}));
        CHECK_FALSE(compare(domain::ScanMode::ValueBetween, bounds, raw<std::int32_t>(201), {}));
    }
}

TEST_CASE("Bigger and smaller than compare with the type's own signedness", "[scan]") {
    const auto signedZero = numeric(domain::ValueType::Int32, raw<std::int32_t>(0));
    CHECK(compare(domain::ScanMode::SmallerThan, signedZero, raw<std::int32_t>(-1), {}));
    CHECK_FALSE(compare(domain::ScanMode::BiggerThan, signedZero, raw<std::int32_t>(-1), {}));

    // The same bytes as an unsigned type are the largest value there is, and
    // reading them as signed is exactly the mistake this dispatch prevents.
    const auto unsignedZero = numeric(domain::ValueType::UInt32, raw<std::uint32_t>(0));
    CHECK(compare(domain::ScanMode::BiggerThan, unsignedZero, raw<std::uint32_t>(0xFFFFFFFFu), {}));
    CHECK_FALSE(compare(domain::ScanMode::SmallerThan, unsignedZero, raw<std::uint32_t>(0xFFFFFFFFu), {}));
}

TEST_CASE("Increased by and decreased by take an exact delta", "[scan]") {
    const auto seven = numeric(domain::ValueType::Int32, raw<std::int32_t>(7));
    const auto previous = raw<std::int32_t>(100);

    CHECK(compare(domain::ScanMode::IncreasedBy, seven, raw<std::int32_t>(107), previous));
    CHECK_FALSE(compare(domain::ScanMode::IncreasedBy, seven, raw<std::int32_t>(108), previous));
    // It really is exact: a value that went up, but not by seven, is out. That
    // selectivity is the entire point over plain "increased".
    CHECK_FALSE(compare(domain::ScanMode::IncreasedBy, seven, raw<std::int32_t>(200), previous));

    CHECK(compare(domain::ScanMode::DecreasedBy, seven, raw<std::int32_t>(93), previous));
    CHECK_FALSE(compare(domain::ScanMode::DecreasedBy, seven, raw<std::int32_t>(92), previous));
}

TEST_CASE("A delta wraps with its type", "[scan]") {
    // A u8 health value going 3 -> 253 really did decrease by 6. Rejecting that
    // would silently drop the very entry the user is hunting.
    const auto six = numeric(domain::ValueType::UInt8, raw<std::uint8_t>(6));
    CHECK(compare(domain::ScanMode::DecreasedBy, six, raw<std::uint8_t>(253), raw<std::uint8_t>(3)));
}

TEST_CASE("A float delta honours the tolerance", "[scan]") {
    const auto half = numeric(domain::ValueType::Float, raw<float>(0.5f));
    // Bit-exact equality after a subtraction finds nothing in practice, which is
    // the same reason exact float matching has a tolerance.
    CHECK(engine_scan::compareValues(domain::ScanMode::DecreasedBy, half, raw<float>(99.5000031f),
                                     raw<float>(100.0f), {}, 0.001));
    CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::DecreasedBy, half, raw<float>(98.0f),
                                           raw<float>(100.0f), {}, 0.001));
}

TEST_CASE("Same as first scan compares against the first value, not the previous one", "[scan]") {
    domain::ScanValue value;
    value.type = domain::ValueType::Int32;

    const auto first = raw<std::int32_t>(100);
    const auto previous = raw<std::int32_t>(50);
    const auto current = raw<std::int32_t>(100);

    // Back to where it started: unchanged from the first scan, changed from the
    // one before it. Those are different questions and this is the one mode
    // that can tell them apart.
    CHECK(engine_scan::compareValues(domain::ScanMode::SameAsFirst, value, current, previous, first));
    CHECK(engine_scan::compareValues(domain::ScanMode::Changed, value, current, previous, first));
    CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::SameAsFirst, value, raw<std::int32_t>(7), previous,
                                           first));
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

TEST_CASE("Text values pack as the type says and read back", "[scan]") {
    const auto ascii = domain::parseScanValue(domain::ValueType::StringAscii, "Player");
    REQUIRE(ascii.has_value());
    // No terminator: a name in a game's memory is usually a fixed buffer with
    // junk after the text, so searching for the NUL would miss most of them.
    CHECK(ascii->bytes.size() == 6);
    CHECK(domain::formatValue(domain::ValueType::StringAscii, ascii->bytes) == "Player");

    const auto wide = domain::parseScanValue(domain::ValueType::StringUtf16, "Player");
    REQUIRE(wide.has_value());
    CHECK(wide->bytes.size() == 12);
    CHECK(wide->bytes[1] == 0);
    CHECK(domain::formatValue(domain::ValueType::StringUtf16, wide->bytes) == "Player");

    // Variable length, like a byte pattern: the width comes from the text.
    CHECK(domain::valueTypeSize(domain::ValueType::StringAscii) == 0);
    CHECK(domain::isStringType(domain::ValueType::StringUtf16));
    CHECK_FALSE(domain::isStringType(domain::ValueType::Bytes));
}

TEST_CASE("Text matching folds case only when asked, and only for ASCII", "[scan]") {
    auto value = domain::parseScanValue(domain::ValueType::StringAscii, "player").value();
    const auto stored = domain::parseScanValue(domain::ValueType::StringAscii, "PLAYER").value().bytes;

    CHECK_FALSE(compare(domain::ScanMode::Exact, value, stored, {}));
    value.caseInsensitive = true;
    CHECK(compare(domain::ScanMode::Exact, value, stored, {}));

    auto wide = domain::parseScanValue(domain::ValueType::StringUtf16, "player").value();
    const auto storedWide = domain::parseScanValue(domain::ValueType::StringUtf16, "PlAyEr").value().bytes;
    CHECK_FALSE(compare(domain::ScanMode::Exact, wide, storedWide, {}));
    wide.caseInsensitive = true;
    CHECK(compare(domain::ScanMode::Exact, wide, storedWide, {}));
}

// ---------------------------------------------------------------------------
// Against a live target
// ---------------------------------------------------------------------------

TEST_CASE("A text scan finds a string in another process", "[scan][integration]") {
    AttachedHelper fixture;
    const auto scratch = fixture.helper.scratch();

    const std::string needle = "PointerLabTestString";
    std::vector<std::uint8_t> bytes(needle.begin(), needle.end());
    REQUIRE(fixture.session.writeBytes(scratch, bytes).has_value());

    engine_scan::ScanOptions options;
    options.writableOnly = true;
    engine_scan::ScanJob job(fixture.session, options);

    auto value = domain::parseScanValue(domain::ValueType::StringAscii, needle);
    REQUIRE(value.has_value());
    job.startFirst(domain::ScanMode::Exact, *value);
    waitForScan(job);

    CHECK(contains(job.results(), scratch));
}

TEST_CASE("A case-insensitive text scan finds text stored in another case", "[scan][integration]") {
    AttachedHelper fixture;
    const auto scratch = fixture.helper.scratch();

    const std::string stored = "MiXeDcAsEnEeDlE";
    std::vector<std::uint8_t> bytes(stored.begin(), stored.end());
    REQUIRE(fixture.session.writeBytes(scratch, bytes).has_value());

    engine_scan::ScanOptions options;
    options.writableOnly = true;
    engine_scan::ScanJob job(fixture.session, options);

    auto value = domain::parseScanValue(domain::ValueType::StringAscii, "mixedcaseneedle");
    REQUIRE(value.has_value());

    job.startFirst(domain::ScanMode::Exact, *value);
    waitForScan(job);
    CHECK_FALSE(contains(job.results(), scratch));

    value->caseInsensitive = true;
    job.startFirst(domain::ScanMode::Exact, *value);
    waitForScan(job);
    CHECK(contains(job.results(), scratch));
}

TEST_CASE("A first scan can filter with an absolute range", "[scan][integration]") {
    AttachedHelper fixture;
    REQUIRE(fixture.helper.set(4242));

    engine_scan::ScanOptions options;
    options.writableOnly = true;
    engine_scan::ScanJob job(fixture.session, options);

    // No baseline, no previous scan: "between 4240 and 4245" is answerable from
    // the bytes alone, which is what makes these modes usable on a first scan.
    domain::ScanValue value;
    value.type = domain::ValueType::Int32;
    value.bytes = raw<std::int32_t>(4240);
    value.bytes2 = raw<std::int32_t>(4245);
    job.startFirst(domain::ScanMode::ValueBetween, value);
    waitForScan(job);

    CHECK(contains(job.results(), fixture.helper.address()));
}

TEST_CASE("The first-scan value survives a rescan", "[scan][integration]") {
    AttachedHelper fixture;
    REQUIRE(fixture.helper.set(1000));

    engine_scan::ScanOptions options;
    options.writableOnly = true;
    engine_scan::ScanJob job(fixture.session, options);

    auto exact = domain::parseScanValue(domain::ValueType::Int32, "1000");
    REQUIRE(exact.has_value());
    job.startFirst(domain::ScanMode::Exact, *exact);
    waitForScan(job);
    REQUIRE(contains(job.results(), fixture.helper.address()));

    // Away and back again. After the middle step the previous value is 500, so
    // only a mode that kept the *first* value can still recognise 1000.
    REQUIRE(fixture.helper.set(500));
    domain::ScanValue changed;
    changed.type = domain::ValueType::Int32;
    changed.bytes = raw<std::int32_t>(0);
    job.startNext(domain::ScanMode::Changed, changed, job.results());
    waitForScan(job);
    REQUIRE(contains(job.results(), fixture.helper.address()));

    REQUIRE(fixture.helper.set(1000));
    job.startNext(domain::ScanMode::SameAsFirst, changed, job.results());
    waitForScan(job);
    CHECK(contains(job.results(), fixture.helper.address()));
}

TEST_CASE("Increased by finds an exact change in a live target", "[scan][integration]") {
    AttachedHelper fixture;
    REQUIRE(fixture.helper.set(100));

    engine_scan::ScanOptions options;
    options.writableOnly = true;
    engine_scan::ScanJob job(fixture.session, options);

    auto exact = domain::parseScanValue(domain::ValueType::Int32, "100");
    REQUIRE(exact.has_value());
    job.startFirst(domain::ScanMode::Exact, *exact);
    waitForScan(job);
    REQUIRE(contains(job.results(), fixture.helper.address()));

    REQUIRE(fixture.helper.set(107));
    domain::ScanValue delta;
    delta.type = domain::ValueType::Int32;
    delta.bytes = raw<std::int32_t>(7);
    job.startNext(domain::ScanMode::IncreasedBy, delta, job.results());
    waitForScan(job);
    CHECK(contains(job.results(), fixture.helper.address()));

    // And the wrong delta rules it out, which is the property that makes this
    // worth having over plain "increased".
    delta.bytes = raw<std::int32_t>(8);
    REQUIRE(fixture.helper.set(100));
    job.startFirst(domain::ScanMode::Exact, *exact);
    waitForScan(job);
    REQUIRE(fixture.helper.set(107));
    job.startNext(domain::ScanMode::IncreasedBy, delta, job.results());
    waitForScan(job);
    CHECK_FALSE(contains(job.results(), fixture.helper.address()));
}
