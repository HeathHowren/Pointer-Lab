#include "infra/Logger.h"

#include <Windows.h>

#include <iomanip>
#include <sstream>

namespace ire::infra {

namespace {

constexpr std::size_t maxRecords = 5000;
constexpr std::size_t trimTo = 4000;
// Roughly a few thousand lines. Past that the previous run is rotated away.
constexpr std::uintmax_t maxLogBytes = 2 * 1024 * 1024;

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::initialize(const std::filesystem::path& logPath) {
    std::scoped_lock lock(mutex_);

    logPath_ = logPath;
    std::error_code ignored;
    std::filesystem::create_directories(logPath.parent_path(), ignored);

    // Keep one previous run rather than truncating it. A crash report is worth
    // very little when launching the application again to collect it is what
    // destroyed the evidence.
    if (std::filesystem::exists(logPath_, ignored)) {
        const auto size = std::filesystem::file_size(logPath_, ignored);
        if (!ignored && size > maxLogBytes) {
            auto previous = logPath_;
            previous += ".1";
            std::filesystem::remove(previous, ignored);
            std::filesystem::rename(logPath_, previous, ignored);
        }
    }

    file_.open(logPath_, std::ios::app);
    if (file_) {
        file_ << "\n=== Pointer Lab started ===\n";
        file_.flush();
    }
}

void Logger::log(LogLevel level, std::string message) {
    const auto now = std::chrono::system_clock::now();
    const auto threadId = static_cast<std::uint32_t>(GetCurrentThreadId());

    std::scoped_lock lock(mutex_);
    if (level < minimumLevel_) {
        return;
    }

    records_.push_back({now, level, message, threadId});
    if (records_.size() > maxRecords) {
        records_.erase(records_.begin(), records_.begin() + (records_.size() - trimTo));
    }

    if (file_) {
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        file_ << std::put_time(&tm, "%F %T") << " [" << std::setw(5) << threadId << "] ["
              << levelName(level) << "] " << message << "\n";
        // Flushed every line on purpose: the last line before a crash is
        // usually the one that matters, and it is worthless sitting in a
        // buffer that never gets written.
        file_.flush();
    }
}

std::vector<LogRecord> Logger::snapshot() const {
    std::scoped_lock lock(mutex_);
    return records_;
}

void Logger::setMinimumLevel(LogLevel level) {
    std::scoped_lock lock(mutex_);
    minimumLevel_ = level;
}

LogLevel Logger::minimumLevel() const {
    std::scoped_lock lock(mutex_);
    return minimumLevel_;
}

void Logger::clear() {
    std::scoped_lock lock(mutex_);
    records_.clear();
}

std::filesystem::path Logger::path() const {
    std::scoped_lock lock(mutex_);
    return logPath_;
}

const char* Logger::levelName(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Info: return "info";
    case LogLevel::Warning: return "warn";
    case LogLevel::Error: return "error";
    }
    return "unknown";
}

} // namespace ire::infra
