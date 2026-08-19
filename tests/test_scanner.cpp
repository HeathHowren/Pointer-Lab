#include <catch2/catch_test_macros.hpp>

#include "domain/Domain.h"
#include "engine_scan/MemoryScanner.h"

#include <cstring>

using namespace ire;

namespace {

template <typename T>
std::vector<std::uint8_t> raw(T value) {
    std::vector<std::uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

} // namespace

// Regression: scanFirst only ever handled Exact and UnknownInitial, so picking
// any relative mode produced zero results and still reported "Scan complete".
// These modes are now understood to require a baseline.
TEST_CASE("Relative scan modes are reported as needing a baseline", "[scan]") {
    CHECK(engine_scan::modeNeedsBaseline(domain::ScanMode::Changed));
    CHECK(engine_scan::modeNeedsBaseline(domain::ScanMode::Unchanged));
    CHECK(engine_scan::modeNeedsBaseline(domain::ScanMode::Increased));
    CHECK(engine_scan::modeNeedsBaseline(domain::ScanMode::Decreased));

    CHECK_FALSE(engine_scan::modeNeedsBaseline(domain::ScanMode::Exact));
    CHECK_FALSE(engine_scan::modeNeedsBaseline(domain::ScanMode::UnknownInitial));
}

TEST_CASE("compareValues implements each scan mode", "[scan]") {
    const auto previous = raw<std::int32_t>(100);
    const auto larger = raw<std::int32_t>(150);
    const auto smaller = raw<std::int32_t>(50);
    const auto same = raw<std::int32_t>(100);
    const auto type = domain::ValueType::Int32;
    const std::vector<std::uint8_t> noMask;

    SECTION("Exact compares against the searched value") {
        CHECK(engine_scan::compareValues(domain::ScanMode::Exact, type, same, previous, same, noMask));
        CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Exact, type, larger, previous, same, noMask));
    }
    SECTION("Changed and Unchanged compare against the previous value") {
        CHECK(engine_scan::compareValues(domain::ScanMode::Changed, type, larger, previous, {}, noMask));
        CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Changed, type, same, previous, {}, noMask));
        CHECK(engine_scan::compareValues(domain::ScanMode::Unchanged, type, same, previous, {}, noMask));
        CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Unchanged, type, larger, previous, {}, noMask));
    }
    SECTION("Increased and Decreased order numerically") {
        CHECK(engine_scan::compareValues(domain::ScanMode::Increased, type, larger, previous, {}, noMask));
        CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Increased, type, smaller, previous, {}, noMask));
        CHECK(engine_scan::compareValues(domain::ScanMode::Decreased, type, smaller, previous, {}, noMask));
        CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Decreased, type, larger, previous, {}, noMask));
    }
    SECTION("Signed comparison is not done on the raw bytes") {
        // As unsigned bytes, -1 (0xFFFFFFFF) looks larger than 1.
        const auto negative = raw<std::int32_t>(-1);
        const auto positive = raw<std::int32_t>(1);
        CHECK(engine_scan::compareValues(domain::ScanMode::Decreased, type, negative, positive, {}, noMask));
        CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Increased, type, negative, positive, {}, noMask));
    }
}

// Regression: exact float matching compared raw bytes, so a value the game
// displays as 100.0 almost never matched what the user typed.
TEST_CASE("Exact float matching honours a tolerance", "[scan]") {
    const auto stored = raw<float>(100.0000123f);
    const auto searched = raw<float>(100.0f);
    const std::vector<std::uint8_t> noMask;

    CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Exact, domain::ValueType::Float,
                                           stored, stored, searched, noMask, 0.0));
    CHECK(engine_scan::compareValues(domain::ScanMode::Exact, domain::ValueType::Float,
                                     stored, stored, searched, noMask, 0.001));

    const auto different = raw<float>(101.0f);
    CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Exact, domain::ValueType::Float,
                                           different, different, searched, noMask, 0.001));
}

// Regression: parseHexBytes discarded every non-hex character, so "90 ?? 90"
// silently became the two-byte pattern 90 90 and matched the wrong things.
TEST_CASE("Byte patterns support wildcards", "[scan]") {
    SECTION("a plain pattern has no wildcards") {
        const auto pattern = domain::parseHexPattern("48 8B 04 24");
        REQUIRE(pattern.has_value());
        CHECK(pattern->bytes == std::vector<std::uint8_t>{0x48, 0x8B, 0x04, 0x24});
        CHECK_FALSE(pattern->hasWildcards());
    }
    SECTION("?? and ? both mean one wildcard byte") {
        const auto doubled = domain::parseHexPattern("48 ?? 24");
        const auto single = domain::parseHexPattern("48 ? 24");
        REQUIRE(doubled.has_value());
        REQUIRE(single.has_value());
        CHECK(doubled->bytes.size() == 3);
        CHECK(doubled->mask == std::vector<std::uint8_t>{0xFF, 0x00, 0xFF});
        CHECK(doubled->mask == single->mask);
        CHECK(doubled->hasWildcards());
    }
    SECTION("spacing is irrelevant and case is ignored") {
        const auto spaced = domain::parseHexPattern("48 8b ?? 24");
        const auto packed = domain::parseHexPattern("488B??24");
        REQUIRE(spaced.has_value());
        REQUIRE(packed.has_value());
        CHECK(spaced->bytes == packed->bytes);
        CHECK(spaced->mask == packed->mask);
    }
    SECTION("malformed patterns are rejected rather than silently truncated") {
        CHECK_FALSE(domain::parseHexPattern("").has_value());
        CHECK_FALSE(domain::parseHexPattern("48 8").has_value());   // odd digit count
        CHECK_FALSE(domain::parseHexPattern("4? 8B").has_value());  // half-wildcard byte
        CHECK_FALSE(domain::parseHexPattern("48 ZZ").has_value());  // not hex
    }
}

TEST_CASE("A masked pattern ignores its wildcard positions", "[scan]") {
    const auto pattern = domain::parseHexPattern("48 ?? 24");
    REQUIRE(pattern.has_value());

    const std::vector<std::uint8_t> matching{0x48, 0x8B, 0x24};
    const std::vector<std::uint8_t> alsoMatching{0x48, 0xFF, 0x24}; // wildcard byte differs
    const std::vector<std::uint8_t> notMatching{0x49, 0x8B, 0x24};  // fixed byte differs

    CHECK(engine_scan::compareValues(domain::ScanMode::Exact, domain::ValueType::Bytes,
                                     matching, matching, pattern->bytes, pattern->mask));
    CHECK(engine_scan::compareValues(domain::ScanMode::Exact, domain::ValueType::Bytes,
                                     alsoMatching, alsoMatching, pattern->bytes, pattern->mask));
    CHECK_FALSE(engine_scan::compareValues(domain::ScanMode::Exact, domain::ValueType::Bytes,
                                           notMatching, notMatching, pattern->bytes, pattern->mask));
}

TEST_CASE("parseScanValue carries a wildcard mask for byte patterns", "[scan]") {
    const auto value = domain::parseScanValue(domain::ValueType::Bytes, "90 ?? 90");
    REQUIRE(value.has_value());
    // The wildcard must survive as a masked byte instead of being dropped.
    CHECK(value->bytes.size() == 3);
    CHECK(value->mask == std::vector<std::uint8_t>{0xFF, 0x00, 0xFF});

    CHECK_FALSE(domain::parseScanValue(domain::ValueType::Bytes, "nonsense").has_value());
}
