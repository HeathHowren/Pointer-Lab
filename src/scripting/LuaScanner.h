#pragma once

#include "domain/Domain.h"
#include "domain/TargetSession.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ire::scripting {

struct LuaScanOptions {
    domain::ValueType type{domain::ValueType::Int32};
    std::string script;
    std::size_t stride{};
    std::size_t maxResults{50000};
    bool writableOnly{};
    bool executableOnly{};
};

struct LuaScanProgress {
    bool running{};
    double fraction{};
    std::size_t results{};
    std::string status;
    std::string error;
};

class LuaScanJob {
public:
    explicit LuaScanJob(domain::TargetSession& session);
    ~LuaScanJob();

    LuaScanJob(const LuaScanJob&) = delete;
    LuaScanJob& operator=(const LuaScanJob&) = delete;

    void start(LuaScanOptions options);
    void cancel();

    [[nodiscard]] LuaScanProgress progress() const;
    [[nodiscard]] std::vector<domain::ScanResult> results() const;
    [[nodiscard]] domain::ValueType valueType() const { return valueType_; }

private:
    void run(LuaScanOptions options);

    domain::TargetSession& session_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::atomic_bool cancel_{false};
    std::atomic_bool running_{false};
    std::atomic<double> fraction_{0.0};
    std::string status_;
    std::string error_;
    std::vector<domain::ScanResult> results_;
    domain::ValueType valueType_{domain::ValueType::Int32};
};

} // namespace ire::scripting

