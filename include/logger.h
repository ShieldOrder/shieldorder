#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>

namespace shieldorder {

enum class LogLevel { DEBUG, INFO, WARN, ERROR, FATAL };

class Logger {
public:
    static Logger& instance();

    void init(const std::string& log_file, LogLevel min_level = LogLevel::INFO);
    void log(LogLevel level, const std::string& component, const std::string& msg);

    void debug(const std::string& component, const std::string& msg);
    void info(const std::string& component, const std::string& msg);
    void warn(const std::string& component, const std::string& msg);
    void error(const std::string& component, const std::string& msg);

private:
    Logger() = default;
    std::ofstream file_;
    std::mutex mutex_;
    LogLevel min_level_ = LogLevel::INFO;
    bool initialized_ = false;

    std::string level_str(LogLevel l);
    std::string timestamp();
};

#define LOG_DEBUG(comp, msg) Logger::instance().debug(comp, msg)
#define LOG_INFO(comp, msg)  Logger::instance().info(comp, msg)
#define LOG_WARN(comp, msg)  Logger::instance().warn(comp, msg)
#define LOG_ERROR(comp, msg) Logger::instance().error(comp, msg)

} // namespace shieldorder
