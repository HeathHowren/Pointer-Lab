#include <catch2/catch_test_macros.hpp>

#include "domain/Domain.h"

using namespace ire;

TEST_CASE("valueTypeSize reports the expected widths", "[domain]") {
    CHECK(domain::valueTypeSize(domain::ValueType::Int8) == 1);
    CHECK(domain::valueTypeSize(domain::ValueType::UInt8) == 1);
    CHECK(domain::valueTypeSize(domain::ValueType::Int16) == 2);
    CHECK(domain::valueTypeSize(domain::ValueType::UInt16) == 2);
    CHECK(domain::valueTypeSize(domain::ValueType::Int32) == 4);
    CHECK(domain::valueTypeSize(domain::ValueType::UInt32) == 4);
    CHECK(domain::valueTypeSize(domain::ValueType::Int64) == 8);
    CHECK(domain::valueTypeSize(domain::ValueType::UInt64) == 8);
    CHECK(domain::valueTypeSize(domain::ValueType::Float) == 4);
    CHECK(domain::valueTypeSize(domain::ValueType::Double) == 8);
}

TEST_CASE("toHex renders an 0x-prefixed value", "[domain]") {
    CHECK(domain::toHex(0) == "0x0");
    CHECK(domain::toHex(0x140001000ull) == "0x140001000");
}

TEST_CASE("parseScanValue round-trips an integer through formatValue", "[domain]") {
    const auto parsed = domain::parseScanValue(domain::ValueType::Int32, "1234");
    REQUIRE(parsed.has_value());
    CHECK(parsed->bytes.size() == 4);
    CHECK(domain::formatValue(domain::ValueType::Int32, parsed->bytes) == "1234");
}

// Regression: addresses used to be parsed as decimal unless they contained a
// hex letter or an 0x prefix, so "00400000" silently resolved to address
// 400,000 decimal instead of 0x400000.
TEST_CASE("parseAddress always reads hexadecimal", "[domain]") {
    SECTION("digits without a prefix are hex, not decimal") {
        CHECK(domain::parseAddress("00400000") == 0x400000ull);
        CHECK(domain::parseAddress("140001000") == 0x140001000ull);
        CHECK(domain::parseAddress("10") == 0x10ull);
    }
    SECTION("an 0x prefix is accepted and equivalent") {
        CHECK(domain::parseAddress("0x140001000") == 0x140001000ull);
        CHECK(domain::parseAddress("0X140001000") == 0x140001000ull);
        CHECK(domain::parseAddress("0x140001000") == domain::parseAddress("140001000"));
    }
    SECTION("surrounding whitespace is ignored") {
        CHECK(domain::parseAddress("  7FF6DEADBEEF  ") == 0x7FF6DEADBEEFull);
    }
    SECTION("case does not matter") {
        CHECK(domain::parseAddress("deadbeef") == domain::parseAddress("DEADBEEF"));
    }
    SECTION("the full 64-bit range is supported") {
        CHECK(domain::parseAddress("FFFFFFFFFFFFFFFF") == 0xFFFFFFFFFFFFFFFFull);
    }
    SECTION("invalid input is rejected rather than partially parsed") {
        CHECK_FALSE(domain::parseAddress("").has_value());
        CHECK_FALSE(domain::parseAddress("   ").has_value());
        CHECK_FALSE(domain::parseAddress("0x").has_value());
        CHECK_FALSE(domain::parseAddress("12ghi").has_value());
        CHECK_FALSE(domain::parseAddress("hello").has_value());
        CHECK_FALSE(domain::parseAddress("-10").has_value());
        // Wider than 64 bits.
        CHECK_FALSE(domain::parseAddress("1FFFFFFFFFFFFFFFF").has_value());
    }
}
