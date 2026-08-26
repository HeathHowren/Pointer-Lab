#pragma once

#include "domain/Domain.h"
#include "domain/TargetSession.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ire::engine_scan {

struct ScanProgress {
    bool running{};
    double fraction{};
    std::size_t results{};
    std::string status;
    // Set when the result limit stopped the scan early, so the UI can say so
    // instead of presenting a partial sweep as a complete one.
    bool truncated{};
};

struct ScanOptions {
    std::size_t maxResults{1000000};
    bool writableOnly{};
    bool executableOnly{};
    // Tolerance for exact float and double matches. Comparing floats by their
    // bytes almost never finds anything, because a displayed 100.0 is rarely
    // bit-identical to the stored value.
    double floatEpsilon{0.0001};
};

class ScanJob {
public:
    ScanJob(domain::TargetSession& session, ScanOptions options);
    ~ScanJob();

    ScanJob(const ScanJob&) = delete;
    ScanJob& operator=(const ScanJob&) = delete;

    void startFirst(domain::ScanMode mode, domain::ScanValue value);
    void startNext(domain::ScanMode mode, domain::ScanValue value, std::vector<domain::ScanResult> previous);
    void setOptions(ScanOptions options);
    void cancel();

    [[nodiscard]] ScanProgress progress() const;
    [[nodiscard]] std::vector<domain::ScanResult> results() const;
    [[nodiscard]] domain::ValueType valueType() const { return valueType_; }

private:
    void scanFirst(domain::ScanMode mode, domain::ScanValue value);
    void scanNext(domain::ScanMode mode, domain::ScanValue value, std::vector<domain::ScanResult> previous);

    domain::TargetSession& session_;
    ScanOptions options_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::atomic_bool cancel_{false};
    // Distinct from cancel_, which used to be reused for "hit the cap" and made
    // a truncated scan indistinguishable from one the user stopped.
    std::atomic_bool truncated_{false};
    std::atomic_bool running_{false};
    std::atomic<double> fraction_{0.0};
    std::string status_;
    std::vector<domain::ScanResult> results_;
    domain::ValueType valueType_{domain::ValueType::Int32};
};

// The preferred form. The ScanValue already carries the needle, the second
// operand, the wildcard mask and the case-folding flag, so the parameter list
// does not have to grow every time a mode takes another operand.
bool compareValues(domain::ScanMode mode, const domain::ScanValue& value,
                   const std::vector<std::uint8_t>& current, const std::vector<std::uint8_t>& previous,
                   const std::vector<std::uint8_t>& first, double floatEpsilon = 0.0);

// The flatter form, for the majority of modes that need neither a first-scan
// value nor a second operand.
bool compareValues(domain::ScanMode mode, domain::ValueType type, const std::vector<std::uint8_t>& current,
                   const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& exact,
                   const std::vector<std::uint8_t>& mask = {}, double floatEpsilon = 0.0);

// True when mode needs a previous value and therefore cannot filter on a first
// scan; such a scan captures a baseline instead.
[[nodiscard]] bool modeNeedsBaseline(domain::ScanMode mode);

// True when the mode tests the value as it stands, so a first scan can filter
// with it rather than only capturing a baseline. Exact, Value between, Bigger
// than and Smaller than are all of this kind.
[[nodiscard]] bool modeIsAbsolute(domain::ScanMode mode);

// Ordering has no meaning for a byte pattern or a string, so the modes that
// order values are not offered for them. Saying so up front is better than
// running a scan that can only ever return nothing.
[[nodiscard]] bool modeSupportsType(domain::ScanMode mode, domain::ValueType type);

// Every address in [start, start + size) where pattern matches, in order, up to
// maxResults.
//
// Unlike ScanJob this is synchronous and bounded, because its caller is an
// auto-assembler script resolving a single `aobscanmodule` over one module's
// image -- a few megabytes, not an address space, and nothing useful can happen
// until the answer arrives. Returning *all* the matches up to the cap rather
// than the first is deliberate: a pattern that matches twice is a pattern that
// is not specific enough to inject with, and the script must be able to say so.
[[nodiscard]] std::vector<std::uintptr_t> findPattern(const domain::TargetSession& session, std::uintptr_t start,
                                                      std::size_t size, const domain::HexPattern& pattern,
                                                      std::size_t maxResults = 64);

} // namespace ire::engine_scan
