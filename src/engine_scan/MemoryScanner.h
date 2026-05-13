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
};

struct ScanOptions {
    std::size_t maxResults{250000};
    bool writableOnly{};
    bool executableOnly{};
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
    std::atomic_bool running_{false};
    std::atomic<double> fraction_{0.0};
    std::string status_;
    std::vector<domain::ScanResult> results_;
    domain::ValueType valueType_{domain::ValueType::Int32};
};

bool compareValues(domain::ScanMode mode, domain::ValueType type, const std::vector<std::uint8_t>& current, const std::vector<std::uint8_t>& previous, const std::vector<std::uint8_t>& exact);

} // namespace ire::engine_scan
