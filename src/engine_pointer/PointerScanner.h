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
    void cancel();
    [[nodiscard]] PointerScanProgress progress() const;
    [[nodiscard]] std::vector<domain::PointerChain> results() const;

private:
    void run(PointerScanOptions options);

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

