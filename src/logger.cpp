#include "logger.h"
#include <iostream>
#include <iomanip>
#include <ctime>

namespace shieldorder {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const std::string& log_file, LogLevel min_level) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.open(log_file, std::ios::app);
    min_level_ = min_level;
    initialized_ = true;

    if (!file_.is_open()) {
        std::cerr << "WARNING: Could not open log file: " << log_file << std::endl;
    }
}

void Logger::log(LogLevel level, const std::string& component, const std::string& msg) {
    if (level < min_level_) return;

    std::lock_guard<std::mutex> lock(mutex_);
    std::string entry = timestamp() + " [" + level_str(level) + "] [" + component + "] " + msg;

    // Always output ERROR and above to stderr
    if (level >= LogLevel::ERROR) {
        std::cerr << entry << std::endl;
    }

    if (initialized_ && file_.is_open()) {
        file_ << entry << std::endl;
        file_.flush();
    } else {
        std::cout << entry << std::endl;
    }
}

void Logger::debug(const std::string& component, const std::string& msg) {
    log(LogLevel::DEBUG, component, msg);
}

void Logger::info(const std::string& component, const std::string& msg) {
    log(LogLevel::INFO, component, msg);
}

void Logger::warn(const std::string& component, const std::string& msg) {
    log(LogLevel::WARN, component, msg);
}

void Logger::error(const std::string& component, const std::string& msg) {
    log(LogLevel::ERROR, component, msg);
}

std::string Logger::level_str(LogLevel l) {
    switch (l) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::FATAL: return "FATAL";
    }
    return "?????";
}

std::string Logger::timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    std::tm tm_buf;
    gmtime_r(&time_t, &tm_buf);

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

} // namespace shieldorder
