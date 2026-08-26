#include "engine_scan/MemoryScanner.h"

#include "infra/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <type_traits>

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

// Calls `visit` with a value-initialised object of the C++ type that `type`
// names, so each comparison below is written once rather than once per type.
// The variable-length types have no arithmetic and answer false; every mode
// that reaches here has already been rejected for them by modeSupportsType.
template <typename Visitor>
bool visitNumeric(domain::ValueType type, Visitor&& visit) {
    switch (type) {
    case domain::ValueType::Int8: return visit(std::int8_t{});
    case domain::ValueType::UInt8: return visit(std::uint8_t{});
    case domain::ValueType::Int16: return visit(std::int16_t{});
    case domain::ValueType::UInt16: return visit(std::uint16_t{});
    case domain::ValueType::Int32: return visit(std::int32_t{});
    case domain::ValueType::UInt32: return visit(std::uint32_t{});
    case domain::ValueType::Int64: return visit(std::int64_t{});
    case domain::ValueType::UInt64: return visit(std::uint64_t{});
    case domain::ValueType::Float: return visit(float{});
    case domain::ValueType::Double: return visit(double{});
    case domain::ValueType::Bytes:
    case domain::ValueType::StringAscii:
    case domain::ValueType::StringUtf16:
        return false;
    }
    return false;
}

// Increased / Decreased / Increased by / Decreased by: everything that needs
// the previous value as well as the current one.
bool relativeCompare(domain::ScanMode mode, domain::ValueType type, const std::vector<std::uint8_t>& current,
                     const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& delta,
                     double epsilon) {
    return visitNumeric(type, [&](auto tag) {
        using T = decltype(tag);
        const auto c = unpack<T>(current);
        const auto p = unpack<T>(previous);
        switch (mode) {
        case domain::ScanMode::Increased: return c > p;
        case domain::ScanMode::Decreased: return c < p;
        case domain::ScanMode::IncreasedBy:
        case domain::ScanMode::DecreasedBy: {
            const auto d = unpack<T>(delta);
            // Wrapping is deliberate for the integer types: a health value
            // stored as u8 that goes from 3 to 253 really did decrease by 6.
            const auto expected =
                mode == domain::ScanMode::IncreasedBy ? static_cast<T>(p + d) : static_cast<T>(p - d);
            if constexpr (std::is_floating_point_v<T>) {
                // Exact equality would find nothing: the stored value is the
                // result of arithmetic that rarely lands on the typed decimal.
                return std::fabs(static_cast<double>(c) - static_cast<double>(expected)) <= epsilon;
            } else {
                return c == expected;
            }
        }
        default: return false;
        }
    });
}

// Bigger than / Smaller than / Value between: filters that test the value as it
// stands, so they need no previous scan.
bool absoluteCompare(domain::ScanMode mode, domain::ValueType type, const std::uint8_t* candidate,
                     std::size_t available, const std::vector<std::uint8_t>& low,
                     const std::vector<std::uint8_t>& high) {
    return visitNumeric(type, [&](auto tag) {
        using T = decltype(tag);
        if (available < sizeof(T)) {
            return false;
        }
        T c{};
        std::memcpy(&c, candidate, sizeof(T));
        const auto a = unpack<T>(low);
        switch (mode) {
        case domain::ScanMode::BiggerThan: return c > a;
        case domain::ScanMode::SmallerThan: return c < a;
        case domain::ScanMode::ValueBetween: {
            const auto b = unpack<T>(high);
            // Ordered here rather than rejected, because "between 200 and 100"
            // is a typo with an obvious meaning and no other one.
            return c >= std::min(a, b) && c <= std::max(a, b);
        }
        default: return false;
        }
    });
}

// ASCII case folding only, and deliberately so. Folding the rest correctly
// needs the Unicode case tables and a locale, and a scanner that folded some
// characters and not others without saying which would be worse than one that
// is clear about its limit.
std::uint8_t foldAscii(std::uint8_t byte) {
    return byte >= 'A' && byte <= 'Z' ? static_cast<std::uint8_t>(byte - 'A' + 'a') : byte;
}

bool stringMatches(const std::uint8_t* candidate, const std::vector<std::uint8_t>& expected,
                   domain::ValueType type, bool caseInsensitive) {
    if (!caseInsensitive) {
        return std::memcmp(candidate, expected.data(), expected.size()) == 0;
    }
    if (type == domain::ValueType::StringUtf16) {
        // Fold the low byte of each code unit and require the high byte to
        // match exactly: that is the ASCII range of UTF-16 and nothing else.
        for (std::size_t i = 0; i + 1 < expected.size(); i += 2) {
            if (candidate[i + 1] != expected[i + 1] || foldAscii(candidate[i]) != foldAscii(expected[i])) {
                return false;
            }
        }
        return true;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (foldAscii(candidate[i]) != foldAscii(expected[i])) {
            return false;
        }
    }
    return true;
}

// Exact match against a raw buffer. Honours wildcard masks for byte patterns,
// an epsilon for floating point and case folding for strings, and never
// allocates.
bool matchesExact(const std::uint8_t* candidate, const std::vector<std::uint8_t>& expected,
                  const std::vector<std::uint8_t>& mask, domain::ValueType type, double epsilon,
                  bool caseInsensitive = false) {
    if (domain::isStringType(type)) {
        return stringMatches(candidate, expected, type, caseInsensitive);
    }
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

// What a first scan can filter on: everything that tests the candidate as it
// stands. The modes that need a previous value capture a baseline instead.
bool firstScanMatches(domain::ScanMode mode, const domain::ScanValue& value, const std::uint8_t* candidate,
                      std::size_t available, double epsilon) {
    if (mode == domain::ScanMode::Exact) {
        return matchesExact(candidate, value.bytes, value.mask, value.type, epsilon, value.caseInsensitive);
    }
    return absoluteCompare(mode, value.type, candidate, available, value.bytes, value.bytes2);
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
                    matched = firstScanMatches(mode, value, candidate, buffer.size() - i, options_.floatEpsilon);
                }
                if (matched) {
                    std::vector<std::uint8_t> current(candidate, candidate + valueSize);
                    // previous, current and first are all the same on a first
                    // scan: this is the only value anyone has seen here yet.
                    batch.push_back({region.base + offset + i, current, current, current});
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
        const auto& oldBytes = previous[i].current.empty() ? previous[i].previous : previous[i].current;
        // A mode that compares against an earlier scan carries no value, and a
        // string or byte pattern has no width of its own, so the width comes
        // from what this result already matched. Reading a fixed 0 bytes here
        // made every string rescan return nothing.
        const std::size_t width = valueSize != 0 ? valueSize : oldBytes.size();
        if (width == 0) {
            continue;
        }
        auto current = session_.readBytes(previous[i].address, width);
        fraction_ = static_cast<double>(i + 1) / static_cast<double>(total);
        if (!current || current.value().size() != width) {
            continue;
        }
        // A table saved by an older build has no first-scan value; treating the
        // previous one as the first is the only honest fallback, and it makes
        // "same as first scan" behave as "unchanged" rather than as nothing.
        const auto& firstBytes = previous[i].first.empty() ? oldBytes : previous[i].first;
        if (compareValues(mode, value, current.value(), oldBytes, firstBytes, options_.floatEpsilon)) {
            next.push_back({previous[i].address, oldBytes, current.value(), firstBytes});
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
    case domain::ScanMode::IncreasedBy:
    case domain::ScanMode::DecreasedBy:
    case domain::ScanMode::SameAsFirst:
        return true;
    case domain::ScanMode::Exact:
    case domain::ScanMode::UnknownInitial:
    case domain::ScanMode::ValueBetween:
    case domain::ScanMode::BiggerThan:
    case domain::ScanMode::SmallerThan:
        return false;
    }
    return false;
}

bool modeIsAbsolute(domain::ScanMode mode) {
    switch (mode) {
    case domain::ScanMode::Exact:
    case domain::ScanMode::ValueBetween:
    case domain::ScanMode::BiggerThan:
    case domain::ScanMode::SmallerThan:
        return true;
    default:
        return false;
    }
}

bool modeSupportsType(domain::ScanMode mode, domain::ValueType type) {
    if (type != domain::ValueType::Bytes && !domain::isStringType(type)) {
        return true;
    }
    switch (mode) {
    case domain::ScanMode::Exact:
    case domain::ScanMode::Changed:
    case domain::ScanMode::Unchanged:
    case domain::ScanMode::SameAsFirst:
        // The last three work only as a Next scan, and legitimately so: they
        // take their width from the results an Exact scan already found.
        return true;
    default:
        // Increased, Value between, Increased by and the rest all order values,
        // and "bigger than" has no meaning for a name or a byte pattern.
        // Unknown initial is excluded for a different reason: a baseline sweep
        // needs a width, and these types have none until a value is typed.
        return false;
    }
}

std::vector<std::uintptr_t> findPattern(const domain::TargetSession& session, std::uintptr_t start,
                                        std::size_t size, const domain::HexPattern& pattern,
                                        std::size_t maxResults) {
    std::vector<std::uintptr_t> matches;
    if (pattern.bytes.empty() || size < pattern.bytes.size() || !session.attached()) {
        return matches;
    }

    const std::size_t needle = pattern.bytes.size();
    for (std::size_t offset = 0; offset + needle <= size && matches.size() < maxResults; offset += chunkSize) {
        const std::size_t span = std::min(chunkSize, size - offset);
        // Read past the chunk so a match straddling the boundary is still
        // found, but only emit matches starting inside it -- otherwise every
        // boundary produces the same match twice.
        const std::size_t readSize = std::min(span + needle - 1, size - offset);
        auto bytes = session.readBytes(start + offset, readSize);
        if (!bytes || bytes.value().size() < needle) {
            continue;
        }
        const auto& buffer = bytes.value();
        const std::size_t limit = std::min(span, buffer.size() - needle + 1);
        for (std::size_t i = 0; i < limit && matches.size() < maxResults; ++i) {
            if (matchesExact(buffer.data() + i, pattern.bytes, pattern.mask, domain::ValueType::Bytes, 0.0)) {
                matches.push_back(start + offset + i);
            }
        }
    }
    return matches;
}

bool compareValues(domain::ScanMode mode, const domain::ScanValue& value,
                   const std::vector<std::uint8_t>& current, const std::vector<std::uint8_t>& previous,
                   const std::vector<std::uint8_t>& first, double floatEpsilon) {
    switch (mode) {
    case domain::ScanMode::Exact:
        if (current.size() < value.bytes.size() || value.bytes.empty()) {
            return false;
        }
        return matchesExact(current.data(), value.bytes, value.mask, value.type, floatEpsilon,
                            value.caseInsensitive);
    case domain::ScanMode::ValueBetween:
    case domain::ScanMode::BiggerThan:
    case domain::ScanMode::SmallerThan:
        return absoluteCompare(mode, value.type, current.data(), current.size(), value.bytes, value.bytes2);
    case domain::ScanMode::Changed:
        return current != previous;
    case domain::ScanMode::Unchanged:
        return current == previous;
    case domain::ScanMode::SameAsFirst:
        return current == first;
    case domain::ScanMode::Increased:
    case domain::ScanMode::Decreased:
    case domain::ScanMode::IncreasedBy:
    case domain::ScanMode::DecreasedBy:
        return relativeCompare(mode, value.type, current, previous, value.bytes, floatEpsilon);
    case domain::ScanMode::UnknownInitial:
        return true;
    }
    return false;
}

bool compareValues(domain::ScanMode mode, domain::ValueType type, const std::vector<std::uint8_t>& current,
                   const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& exact,
                   const std::vector<std::uint8_t>& mask, double floatEpsilon) {
    domain::ScanValue value;
    value.type = type;
    value.bytes = exact;
    value.mask = mask;
    // No first-scan value to offer, so Same as first scan degrades to
    // Unchanged. Callers that mean it use the ScanValue form.
    return compareValues(mode, value, current, previous, previous, floatEpsilon);
}

} // namespace ire::engine_scan
