#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace ire::infra {

enum class LogLevel { Trace, Info, Warning, Error };

struct LogRecord {
    std::chrono::system_clock::time_point time;
    LogLevel level;
    std::string message;
    std::uint32_t threadId{};
};

class Logger {
public:
    static Logger& instance();

    // Rotates the previous run's log aside and opens the new one once, for the
    // lifetime of the process.
    void initialize(const std::filesystem::path& logPath);
    void log(LogLevel level, std::string message);
    void trace(std::string message) { log(LogLevel::Trace, std::move(message)); }
    void info(std::string message) { log(LogLevel::Info, std::move(message)); }
    void warn(std::string message) { log(LogLevel::Warning, std::move(message)); }
    void error(std::string message) { log(LogLevel::Error, std::move(message)); }

    [[nodiscard]] std::vector<LogRecord> snapshot() const;
    // Only records at or above this level are kept. Trace is very chatty during
    // a scan, so the default hides it.
    void setMinimumLevel(LogLevel level);
    [[nodiscard]] LogLevel minimumLevel() const;
    void clear();

    [[nodiscard]] std::filesystem::path path() const;
    static const char* levelName(LogLevel level);

private:
    mutable std::mutex mutex_;
    std::filesystem::path logPath_;
    // Held open rather than reopened per line: appending one line used to mean
    // an open, a seek and a close, which is thousands of file operations during
    // a scan that logs its progress.
    std::ofstream file_;
    std::vector<LogRecord> records_;
    LogLevel minimumLevel_{LogLevel::Info};
};

} // namespace ire::infra
