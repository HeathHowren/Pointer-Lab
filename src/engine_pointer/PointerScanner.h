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

} // namespace ire::engine_pointer

