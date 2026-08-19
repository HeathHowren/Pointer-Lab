// End-to-end tests that drive the real scanner against a real second process.
//
// The helper is told over stdin exactly when to change its value, so every scan
// mode can be exercised against live memory deterministically rather than
// against whatever the test process happens to be doing.

#include <catch2/catch_test_macros.hpp>

#include "HelperProcess.h"

#include "engine_scan/MemoryScanner.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

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
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool contains(const std::vector<domain::ScanResult>& results, std::uintptr_t address) {
    return std::any_of(results.begin(), results.end(),
                       [address](const domain::ScanResult& result) { return result.address == address; });
}

engine_scan::ScanOptions testOptions() {
    engine_scan::ScanOptions options;
    options.writableOnly = true;
    options.maxResults = 20000000;
    return options;
}

domain::ScanValue int32Value(std::int32_t value) {
    const auto parsed = domain::parseScanValue(domain::ValueType::Int32, std::to_string(value));
    REQUIRE(parsed.has_value());
    return *parsed;
}

} // namespace

TEST_CASE("An exact scan finds a known value in a live process", "[scan][integration]") {
    AttachedHelper fixture;

    // Prove the read path before trusting anything the scan says.
    auto readBack = fixture.session.readBytes(fixture.helper.address(), sizeof(std::int32_t));
    REQUIRE(readBack.has_value());
    REQUIRE(readBack.value().size() == sizeof(std::int32_t));
    std::int32_t observed{};
    std::memcpy(&observed, readBack.value().data(), sizeof(observed));
    REQUIRE(observed == needleValue);

    engine_scan::ScanJob job(fixture.session, testOptions());
    job.startFirst(domain::ScanMode::Exact, int32Value(needleValue));
    REQUIRE(waitForScan(job));

    const auto results = job.results();
    INFO("exact scan returned " << results.size() << " results");
    CHECK_FALSE(job.progress().truncated);
    CHECK(contains(results, fixture.helper.address()));
}

TEST_CASE("Exact then Next narrows as the value changes", "[scan][integration]") {
    AttachedHelper fixture;
    engine_scan::ScanJob job(fixture.session, testOptions());

    job.startFirst(domain::ScanMode::Exact, int32Value(needleValue));
    REQUIRE(waitForScan(job));
    auto results = job.results();
    REQUIRE(contains(results, fixture.helper.address()));

    SECTION("Increased keeps a value that went up") {
        REQUIRE(fixture.helper.set(needleValue + 1000));
        job.startNext(domain::ScanMode::Increased, int32Value(0), std::move(results));
        REQUIRE(waitForScan(job));
        CHECK(contains(job.results(), fixture.helper.address()));
    }
    SECTION("Decreased drops a value that went up") {
        REQUIRE(fixture.helper.set(needleValue + 1000));
        job.startNext(domain::ScanMode::Decreased, int32Value(0), std::move(results));
        REQUIRE(waitForScan(job));
        CHECK_FALSE(contains(job.results(), fixture.helper.address()));
    }
    SECTION("Decreased keeps a value that went down") {
        REQUIRE(fixture.helper.set(needleValue - 1000));
        job.startNext(domain::ScanMode::Decreased, int32Value(0), std::move(results));
        REQUIRE(waitForScan(job));
        CHECK(contains(job.results(), fixture.helper.address()));
    }
    SECTION("Changed keeps a value that moved and Unchanged does not") {
        REQUIRE(fixture.helper.set(42));
        auto forChanged = results;
        job.startNext(domain::ScanMode::Changed, int32Value(0), std::move(forChanged));
        REQUIRE(waitForScan(job));
        CHECK(contains(job.results(), fixture.helper.address()));

        job.startNext(domain::ScanMode::Unchanged, int32Value(0), std::move(results));
        REQUIRE(waitForScan(job));
        CHECK_FALSE(contains(job.results(), fixture.helper.address()));
    }
    SECTION("Unchanged keeps a value that stayed put") {
        job.startNext(domain::ScanMode::Unchanged, int32Value(0), std::move(results));
        REQUIRE(waitForScan(job));
        CHECK(contains(job.results(), fixture.helper.address()));
    }
    SECTION("Exact re-filters on the new value") {
        REQUIRE(fixture.helper.set(777));
        job.startNext(domain::ScanMode::Exact, int32Value(777), std::move(results));
        REQUIRE(waitForScan(job));
        CHECK(contains(job.results(), fixture.helper.address()));
    }
}

// Regression: a first scan in any relative mode matched nothing and still
// reported "Scan complete", so the whole unknown-value workflow silently
// produced an empty result set.
TEST_CASE("A relative first scan captures a baseline instead of nothing", "[scan][integration]") {
    AttachedHelper fixture;
    engine_scan::ScanJob job(fixture.session, testOptions());

    job.startFirst(domain::ScanMode::Changed, int32Value(0));
    REQUIRE(waitForScan(job));

    auto baseline = job.results();
    REQUIRE_FALSE(baseline.empty());
    REQUIRE(contains(baseline, fixture.helper.address()));
    // The status has to explain what happened rather than claim completion.
    CHECK(job.progress().status.find("Baseline") != std::string::npos);

    REQUIRE(fixture.helper.set(needleValue + 7));
    job.startNext(domain::ScanMode::Changed, int32Value(0), std::move(baseline));
    REQUIRE(waitForScan(job));
    CHECK(contains(job.results(), fixture.helper.address()));
}

TEST_CASE("An unknown-initial scan snapshots memory for later filtering", "[scan][integration]") {
    AttachedHelper fixture;
    engine_scan::ScanJob job(fixture.session, testOptions());

    job.startFirst(domain::ScanMode::UnknownInitial, int32Value(0));
    REQUIRE(waitForScan(job));

    auto snapshot = job.results();
    INFO("snapshot captured " << snapshot.size() << " slots");
    REQUIRE_FALSE(snapshot.empty());
    REQUIRE(contains(snapshot, fixture.helper.address()));

    REQUIRE(fixture.helper.set(needleValue + 5000));
    job.startNext(domain::ScanMode::Increased, int32Value(0), std::move(snapshot));
    REQUIRE(waitForScan(job));
    CHECK(contains(job.results(), fixture.helper.address()));
}

// Regression: parseHexBytes dropped '?' characters, so a wildcard pattern
// silently became a shorter literal pattern and matched the wrong addresses.
TEST_CASE("A byte pattern with a wildcard matches in a live process", "[scan][integration]") {
    AttachedHelper fixture;
    engine_scan::ScanJob job(fixture.session, testOptions());

    // 0x5AFE1234 little-endian is 34 12 FE 5A; the wildcard covers byte 1.
    const auto pattern = domain::parseScanValue(domain::ValueType::Bytes, "34 ?? FE 5A");
    REQUIRE(pattern.has_value());

    job.startFirst(domain::ScanMode::Exact, *pattern);
    REQUIRE(waitForScan(job));
    CHECK(contains(job.results(), fixture.helper.address()));
}

TEST_CASE("Writing through the session changes the target value", "[scan][integration]") {
    AttachedHelper fixture;

    const auto replacement = int32Value(999);
    REQUIRE(fixture.session.writeBytes(fixture.helper.address(), replacement.bytes).has_value());
    // Read it back through the helper itself rather than through our own handle,
    // so this proves the write really landed in the other process.
    CHECK(fixture.helper.get() == 999);
}

TEST_CASE("Cancelling a scan stops it and says so", "[scan][integration]") {
    AttachedHelper fixture;
    engine_scan::ScanJob job(fixture.session, testOptions());

    job.startFirst(domain::ScanMode::UnknownInitial, int32Value(0));
    job.cancel();
    CHECK_FALSE(job.progress().running);
}

TEST_CASE("The result limit is reported as truncation, not completion", "[scan][integration]") {
    AttachedHelper fixture;
    engine_scan::ScanOptions options = testOptions();
    options.maxResults = 16;

    engine_scan::ScanJob job(fixture.session, options);
    job.startFirst(domain::ScanMode::UnknownInitial, int32Value(0));
    REQUIRE(waitForScan(job));

    CHECK(job.results().size() == 16);
    CHECK(job.progress().truncated);
    CHECK(job.progress().status.find("limit") != std::string::npos);
}
