#include "infra/Logger.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace ire::infra {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::initialize(const std::filesystem::path& logPath) {
    std::scoped_lock lock(mutex_);
    logPath_ = logPath;
    std::error_code ignored;
    std::filesystem::create_directories(logPath.parent_path(), ignored);
    std::ofstream(logPath_, std::ios::trunc) << "Pointer Lab log\n";
}

void Logger::log(LogLevel level, std::string message) {
    const auto now = std::chrono::system_clock::now();
    {
        std::scoped_lock lock(mutex_);
        records_.push_back({now, level, message});
        if (records_.size() > 5000) {
            records_.erase(records_.begin(), records_.begin() + 1000);
        }

        if (!logPath_.empty()) {
            std::ofstream out(logPath_, std::ios::app);
            const auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            localtime_s(&tm, &t);
            out << std::put_time(&tm, "%F %T") << " [" << levelName(level) << "] " << message << "\n";
        }
    }
}

std::vector<LogRecord> Logger::snapshot() const {
    std::scoped_lock lock(mutex_);
    return records_;
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

