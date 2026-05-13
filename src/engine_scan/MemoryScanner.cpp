#include "engine_scan/MemoryScanner.h"

#include "infra/Logger.h"

#include <algorithm>
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
    return {running_, fraction_.load(), results_.size(), status_};
}

std::vector<domain::ScanResult> ScanJob::results() const {
    std::scoped_lock lock(mutex_);
    return results_;
}

void ScanJob::scanFirst(domain::ScanMode mode, domain::ScanValue value) {
    const auto regions = session_.regions();
    const auto total = totalBytes(regions, options_);
    std::uint64_t visited{};
    const std::size_t valueSize = value.bytes.size();
    const std::size_t stride = mode == domain::ScanMode::UnknownInitial ? std::max<std::size_t>(1, domain::valueTypeSize(value.type)) : 1;

    for (const auto& region : regions) {
        if (cancel_) {
            break;
        }
        if (!eligibleRegion(region, options_)) {
            continue;
        }

        for (std::size_t offset = 0; offset < region.size && !cancel_; offset += chunkSize) {
            const std::size_t readSize = std::min(chunkSize + valueSize, region.size - offset);
            auto bytes = session_.readBytes(region.base + offset, readSize);
            visited += readSize;
            fraction_ = std::min(1.0, static_cast<double>(visited) / static_cast<double>(total));
            if (!bytes || bytes.value().size() < valueSize) {
                continue;
            }

            auto& buffer = bytes.value();
            std::vector<domain::ScanResult> batch;
            for (std::size_t i = 0; i + valueSize <= buffer.size() && !cancel_; i += stride) {
                std::vector<std::uint8_t> current(buffer.begin() + static_cast<std::ptrdiff_t>(i), buffer.begin() + static_cast<std::ptrdiff_t>(i + valueSize));
                bool matched = false;
                if (mode == domain::ScanMode::UnknownInitial) {
                    matched = true;
                } else if (mode == domain::ScanMode::Exact) {
                    matched = current == value.bytes;
                }
                if (matched) {
                    batch.push_back({region.base + offset + i, current, current});
                }
                if (batch.size() >= 4096) {
                    std::scoped_lock lock(mutex_);
                    results_.insert(results_.end(), batch.begin(), batch.end());
                    if (results_.size() > options_.maxResults) {
                        results_.resize(options_.maxResults);
                        cancel_ = true;
                    }
                    status_ = "Scanning: " + std::to_string(results_.size()) + " results";
                    batch.clear();
                }
            }
            if (!batch.empty()) {
                std::scoped_lock lock(mutex_);
                results_.insert(results_.end(), batch.begin(), batch.end());
                if (results_.size() > options_.maxResults) {
                    results_.resize(options_.maxResults);
                    cancel_ = true;
                }
                status_ = "Scanning: " + std::to_string(results_.size()) + " results";
            }
        }
    }

    {
        std::scoped_lock lock(mutex_);
        status_ = cancel_ ? "Scan cancelled or capped" : "Scan complete";
    }
    running_ = false;
}

void ScanJob::scanNext(domain::ScanMode mode, domain::ScanValue value, std::vector<domain::ScanResult> previous) {
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
        if (compareValues(mode, value.type, current.value(), oldBytes, value.bytes)) {
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

    {
        std::scoped_lock lock(mutex_);
        results_ = std::move(next);
        status_ = cancel_ ? "Next scan cancelled" : "Next scan complete";
    }
    running_ = false;
}

bool compareValues(domain::ScanMode mode, domain::ValueType type, const std::vector<std::uint8_t>& current, const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& exact) {
    switch (mode) {
    case domain::ScanMode::Exact:
        return current == exact;
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
