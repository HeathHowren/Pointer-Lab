#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace ire::infra {

enum class LogLevel { Trace, Info, Warning, Error };

struct LogRecord {
    std::chrono::system_clock::time_point time;
    LogLevel level;
    std::string message;
};

class Logger {
public:
    static Logger& instance();

    void initialize(const std::filesystem::path& logPath);
    void log(LogLevel level, std::string message);
    void trace(std::string message) { log(LogLevel::Trace, std::move(message)); }
    void info(std::string message) { log(LogLevel::Info, std::move(message)); }
    void warn(std::string message) { log(LogLevel::Warning, std::move(message)); }
    void error(std::string message) { log(LogLevel::Error, std::move(message)); }

    std::vector<LogRecord> snapshot() const;
    static const char* levelName(LogLevel level);

private:
    mutable std::mutex mutex_;
    std::filesystem::path logPath_;
    std::vector<LogRecord> records_;
};

} // namespace ire::infra

