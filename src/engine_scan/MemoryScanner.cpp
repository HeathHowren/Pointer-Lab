#include "engine_scan/MemoryScanner.h"

#include "infra/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>

namespace ire::engine_scan {

namespace {

constexpr std::size_t chunkSize = 1024 * 1024;

template <typename T>
T unpack(const std::vector<std::uint8_t>& bytes) {
    T value{};
    if (bytes.size() >= sizeof(T)) {
        std::memcpy(&value, bytes.data(), sizeof(T));
    }
    return value;
}

bool numericCompare(domain::ScanMode mode, domain::ValueType type, const std::vector<std::uint8_t>& current, const std::vector<std::uint8_t>& previous) {
    switch (type) {
    case domain::ValueType::Int8: {
        auto c = unpack<std::int8_t>(current); auto p = unpack<std::int8_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::UInt8: {
        auto c = unpack<std::uint8_t>(current); auto p = unpack<std::uint8_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::Int16: {
        auto c = unpack<std::int16_t>(current); auto p = unpack<std::int16_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::UInt16: {
        auto c = unpack<std::uint16_t>(current); auto p = unpack<std::uint16_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::Int32: {
        auto c = unpack<std::int32_t>(current); auto p = unpack<std::int32_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::UInt32: {
        auto c = unpack<std::uint32_t>(current); auto p = unpack<std::uint32_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::Int64: {
        auto c = unpack<std::int64_t>(current); auto p = unpack<std::int64_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::UInt64: {
        auto c = unpack<std::uint64_t>(current); auto p = unpack<std::uint64_t>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::Float: {
        auto c = unpack<float>(current); auto p = unpack<float>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::Double: {
        auto c = unpack<double>(current); auto p = unpack<double>(previous);
        return mode == domain::ScanMode::Increased ? c > p : c < p;
    }
    case domain::ValueType::Bytes:
        return false;
    }
    return false;
}

// Exact match against a raw buffer. Honours wildcard masks for byte patterns
// and an epsilon for floating point, and never allocates.
bool matchesExact(const std::uint8_t* candidate, const std::vector<std::uint8_t>& expected,
                  const std::vector<std::uint8_t>& mask, domain::ValueType type, double epsilon) {
    if (epsilon > 0.0 && type == domain::ValueType::Float && expected.size() == sizeof(float)) {
        float c{};
        float e{};
        std::memcpy(&c, candidate, sizeof(float));
        std::memcpy(&e, expected.data(), sizeof(float));
        return std::fabs(static_cast<double>(c) - static_cast<double>(e)) <= epsilon;
    }
    if (epsilon > 0.0 && type == domain::ValueType::Double && expected.size() == sizeof(double)) {
        double c{};
        double e{};
        std::memcpy(&c, candidate, sizeof(double));
        std::memcpy(&e, expected.data(), sizeof(double));
        return std::fabs(c - e) <= epsilon;
    }
    if (mask.size() == expected.size()) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (mask[i] != 0 && candidate[i] != expected[i]) {
                return false;
            }
        }
        return true;
    }
    return std::memcmp(candidate, expected.data(), expected.size()) == 0;
}

bool eligibleRegion(const domain::MemoryRegion& region, const ScanOptions& options) {
    if (!region.readable || region.size == 0) {
        return false;
    }
    if (options.writableOnly && !region.writable) {
        return false;
    }
    if (options.executableOnly && !region.executable) {
        return false;
    }
    return true;
}

std::uint64_t totalBytes(const std::vector<domain::MemoryRegion>& regions, const ScanOptions& options) {
    std::uint64_t total{};
    for (const auto& region : regions) {
        if (eligibleRegion(region, options)) {
            total += region.size;
        }
    }
    return total == 0 ? 1 : total;
}

} // namespace

ScanJob::ScanJob(domain::TargetSession& session, ScanOptions options) : session_(session), options_(options) {}

ScanJob::~ScanJob() {
    cancel();
}

void ScanJob::startFirst(domain::ScanMode mode, domain::ScanValue value) {
    cancel();
    cancel_ = false;
    truncated_ = false;
    running_ = true;
    fraction_ = 0.0;
    valueType_ = value.type;
    {
        std::scoped_lock lock(mutex_);
        results_.clear();
        status_ = "Starting first scan";
    }
    worker_ = std::jthread([this, mode, value = std::move(value)]() { scanFirst(mode, value); });
}

void ScanJob::startNext(domain::ScanMode mode, domain::ScanValue value, std::vector<domain::ScanResult> previous) {
    cancel();
    cancel_ = false;
    truncated_ = false;
    running_ = true;
    fraction_ = 0.0;
    valueType_ = value.type;
    {
        std::scoped_lock lock(mutex_);
        results_.clear();
        status_ = "Starting next scan";
    }
    worker_ = std::jthread([this, mode, value = std::move(value), previous = std::move(previous)]() mutable {
        scanNext(mode, value, std::move(previous));
    });
}

void ScanJob::setOptions(ScanOptions options) {
    cancel();
    options_ = options;
}

void ScanJob::cancel() {
    cancel_ = true;
    if (worker_.joinable()) {
        worker_.join();
    }
}

ScanProgress ScanJob::progress() const {
    std::scoped_lock lock(mutex_);
    return {running_, fraction_.load(), results_.size(), status_, truncated_.load()};
}

std::vector<domain::ScanResult> ScanJob::results() const {
    std::scoped_lock lock(mutex_);
    return results_;
}

void ScanJob::scanFirst(domain::ScanMode mode, domain::ScanValue value) {
    const auto started = std::chrono::steady_clock::now();
    const auto regions = session_.regions();
    const auto total = totalBytes(regions, options_);
    std::uint64_t visited{};
    const std::size_t valueSize = value.bytes.size();
    if (valueSize == 0) {
        infra::Logger::instance().warn("First scan rejected: the value is empty.");
        std::scoped_lock lock(mutex_);
        status_ = "Nothing to scan for: the value is empty.";
        running_ = false;
        return;
    }

    infra::Logger::instance().info(
        std::string("First scan: mode=") + domain::scanModeName(mode) + " type=" + domain::valueTypeName(value.type) +
        " regions=" + std::to_string(regions.size()) + " bytes=" + std::to_string(total) +
        " limit=" + std::to_string(options_.maxResults));

    // Changed/Unchanged/Increased/Decreased have nothing to compare against on
    // a first scan. They previously matched nothing at all and still reported
    // "Scan complete"; now they capture a baseline that Next scan filters.
    const bool baselineOnly = modeNeedsBaseline(mode);
    const bool snapshot = baselineOnly || mode == domain::ScanMode::UnknownInitial;
    const std::size_t stride = snapshot ? std::max<std::size_t>(1, domain::valueTypeSize(value.type)) : 1;

    bool stopped = false;
    for (const auto& region : regions) {
        if (cancel_ || stopped) {
            break;
        }
        if (!eligibleRegion(region, options_)) {
            continue;
        }

        for (std::size_t offset = 0; offset < region.size && !cancel_ && !stopped; offset += chunkSize) {
            const std::size_t span = std::min(chunkSize, region.size - offset);
            // Read a little past the chunk so a value straddling the boundary
            // is still found, but only emit matches that start inside the
            // chunk - otherwise every boundary produced duplicate results.
            const std::size_t readSize = std::min(span + valueSize - 1, region.size - offset);
            auto bytes = session_.readBytes(region.base + offset, readSize);

            visited += span; // span, not readSize: the overlap is not new ground
            fraction_ = std::min(1.0, static_cast<double>(visited) / static_cast<double>(total));
            if (!bytes || bytes.value().size() < valueSize) {
                continue;
            }

            const auto& buffer = bytes.value();
            const std::size_t limit = std::min(span, buffer.size() - valueSize + 1);
            std::vector<domain::ScanResult> batch;

            for (std::size_t i = 0; i < limit && !cancel_; i += stride) {
                // Compare in place; building a vector per candidate byte meant
                // one heap allocation for every slot in the address space.
                const std::uint8_t* candidate = buffer.data() + i;
                bool matched = snapshot;
                if (!matched) {
                    matched = matchesExact(candidate, value.bytes, value.mask, value.type, options_.floatEpsilon);
                }
                if (matched) {
                    std::vector<std::uint8_t> current(candidate, candidate + valueSize);
                    batch.push_back({region.base + offset + i, current, current});
                }
            }

            if (!batch.empty()) {
                std::scoped_lock lock(mutex_);
                results_.insert(results_.end(), batch.begin(), batch.end());
                if (results_.size() >= options_.maxResults) {
                    results_.resize(options_.maxResults);
                    truncated_ = true;
                    stopped = true;
                }
                status_ = "Scanning: " + std::to_string(results_.size()) + " results";
            }
        }
    }

    std::size_t found{};
    {
        std::scoped_lock lock(mutex_);
        found = results_.size();
        if (cancel_) {
            status_ = "Scan cancelled";
        } else if (truncated_) {
            status_ = "Stopped at the " + std::to_string(options_.maxResults) +
                      " result limit; narrow the scan or raise the limit";
        } else if (baselineOnly) {
            status_ = "Baseline captured (" + std::to_string(results_.size()) + " values). Run Next scan to filter by " +
                      domain::scanModeName(mode) + ".";
        } else {
            status_ = "Scan complete";
        }
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    infra::Logger::instance().info("First scan finished: " + std::to_string(found) + " results in " +
                                   std::to_string(elapsed) + " ms" + (cancel_ ? " (cancelled)" : "") +
                                   (truncated_ ? " (truncated at the result limit)" : ""));
    running_ = false;
}

void ScanJob::scanNext(domain::ScanMode mode, domain::ScanValue value, std::vector<domain::ScanResult> previous) {
    const auto started = std::chrono::steady_clock::now();
    infra::Logger::instance().info(std::string("Next scan: mode=") + domain::scanModeName(mode) +
                                   " type=" + domain::valueTypeName(value.type) +
                                   " candidates=" + std::to_string(previous.size()));
    const auto total = std::max<std::size_t>(1, previous.size());
    std::vector<domain::ScanResult> next;
    next.reserve(std::min(total, options_.maxResults));
    const std::size_t valueSize = value.bytes.size();

    for (std::size_t i = 0; i < previous.size() && !cancel_; ++i) {
        auto current = session_.readBytes(previous[i].address, valueSize);
        fraction_ = static_cast<double>(i + 1) / static_cast<double>(total);
        if (!current || current.value().size() != valueSize) {
            continue;
        }
        const auto& oldBytes = previous[i].current.empty() ? previous[i].previous : previous[i].current;
        if (compareValues(mode, value.type, current.value(), oldBytes, value.bytes, value.mask, options_.floatEpsilon)) {
            next.push_back({previous[i].address, oldBytes, current.value()});
        }
        if (next.size() >= options_.maxResults) {
            break;
        }
        if (i % 4096 == 0) {
            std::scoped_lock lock(mutex_);
            status_ = "Filtering: " + std::to_string(next.size()) + " results";
        }
    }

    std::size_t kept{};
    {
        std::scoped_lock lock(mutex_);
        results_ = std::move(next);
        kept = results_.size();
        status_ = cancel_ ? "Next scan cancelled" : "Next scan complete";
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    infra::Logger::instance().info("Next scan finished: " + std::to_string(kept) + " of " +
                                   std::to_string(previous.size()) + " survived in " + std::to_string(elapsed) + " ms" +
                                   (cancel_ ? " (cancelled)" : ""));
    running_ = false;
}

bool modeNeedsBaseline(domain::ScanMode mode) {
    switch (mode) {
    case domain::ScanMode::Changed:
    case domain::ScanMode::Unchanged:
    case domain::ScanMode::Increased:
    case domain::ScanMode::Decreased:
        return true;
    case domain::ScanMode::Exact:
    case domain::ScanMode::UnknownInitial:
        return false;
    }
    return false;
}

bool compareValues(domain::ScanMode mode, domain::ValueType type, const std::vector<std::uint8_t>& current,
                   const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& exact,
                   const std::vector<std::uint8_t>& mask, double floatEpsilon) {
    switch (mode) {
    case domain::ScanMode::Exact:
        if (current.size() < exact.size() || exact.empty()) {
            return false;
        }
        return matchesExact(current.data(), exact, mask, type, floatEpsilon);
    case domain::ScanMode::Changed:
        return current != previous;
    case domain::ScanMode::Unchanged:
        return current == previous;
    case domain::ScanMode::Increased:
    case domain::ScanMode::Decreased:
        return numericCompare(mode, type, current, previous);
    case domain::ScanMode::UnknownInitial:
        return true;
    }
    return false;
}

} // namespace ire::engine_scan
