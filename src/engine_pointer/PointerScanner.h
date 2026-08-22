#pragma once

#include "domain/Domain.h"
#include "domain/TargetSession.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace ire::engine_pointer {

struct PointerScanProgress {
    bool running{};
    double fraction{};
    std::size_t results{};
    std::string status;
};

struct PointerScanOptions {
    std::uintptr_t target{};
    std::uint32_t maxDepth{3};
    std::uint32_t maxOffset{0x1000};
    std::size_t maxResults{10000};
    // Separate from maxResults: the frontier is the set of addresses the next
    // depth searches for, and capping it with the result limit meant raising
    // one silently changed how much of the address space got searched.
    std::size_t maxFrontier{200000};
};

class PointerScanJob {
public:
    explicit PointerScanJob(domain::TargetSession& session);
    ~PointerScanJob();

    PointerScanJob(const PointerScanJob&) = delete;
    PointerScanJob& operator=(const PointerScanJob&) = delete;

    void start(PointerScanOptions options);
    // Narrows the chains already found to the ones that still resolve to
    // newTarget. A first scan returns thousands of chains that happened to point
    // the right way once; the ones that survive being checked again -- normally
    // after the target has restarted and moved the value -- are the ones that
    // actually track it. This costs a few reads per chain rather than another
    // sweep of the address space, so it is the cheap half of the workflow.
    //
    // Refuses, leaving the chains untouched, when nothing is attached or there
    // is nothing to narrow. A cancelled filter also leaves them untouched: a
    // half-applied filter is not a result set.
    void filter(std::uintptr_t newTarget);
    void cancel();
    [[nodiscard]] PointerScanProgress progress() const;
    [[nodiscard]] std::vector<domain::PointerChain> results() const;

private:
    void run(PointerScanOptions options);
    void runFilter(std::uintptr_t newTarget);

    domain::TargetSession& session_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::atomic_bool cancel_{false};
    std::atomic_bool running_{false};
    std::atomic<double> fraction_{0.0};
    std::string status_;
    std::vector<domain::PointerChain> results_;
};

// Walks a chain in the live target and returns the address it currently points
// at. The module is looked up by name every time, so a chain found before a
// restart still resolves afterwards even though ASLR moved everything.
[[nodiscard]] infra::Result<std::uintptr_t> resolveChain(domain::TargetSession& session,
                                                         const domain::PointerChain& chain);

} // namespace ire::engine_pointer

