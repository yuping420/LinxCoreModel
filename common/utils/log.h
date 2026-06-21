#pragma once
#ifndef LOG_H
#define LOG_H

#include <memory>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <cstdio>

#include <unordered_set>

#include "LogInfo.h"

namespace JCore {

class TTYCmd {
public:
    unsigned char code;

    explicit TTYCmd(int codeIn) : code(codeIn) {}
    std::string Str() const { return "\033[" + std::to_string(code) + "m"; }
};

#define TTY_COLOR(n, ...) TTYCmd(n), ##__VA_ARGS__, TTYCmd(0)
#define TTY_RED(...) TTY_COLOR(31, __VA_ARGS__)
#define TTY_GREEN(...) TTY_COLOR(32, __VA_ARGS__)
#define TTY_YELLOW(...) TTY_COLOR(33, __VA_ARGS__)
#define TTY_BLUE(...) TTY_COLOR(34, __VA_ARGS__)
#define TTY_MAGENTA(...) TTY_COLOR(35, __VA_ARGS__)
#define TTY_CYAN(...) TTY_COLOR(36, __VA_ARGS__)
#define TTY_WHITE(...) TTY_COLOR(37, __VA_ARGS__)

enum class LoggerLevel {
    DETAIL = 0,
    DEBUG,
    INFO,
    WARN,
    ERROR,
    FATAL,
    EVENT,
    NONE,
};

class StdLogger {
public:
    StdLogger &Log(TTYCmd &&cmd) {
        std::cout << cmd.Str();
        return *this;
    }
    template <typename T>
    StdLogger &Log(T &&t) {
        std::cout << (std::forward<T>(t));
        return *this;
    }

    StdLogger() = default;

private:
    StdLogger(const StdLogger &) = delete;
};

class FileLogger {
public:
    std::ofstream ofs;

    FileLogger(const std::string &filepath, bool append) {
        if (append) {
            ofs.open(filepath, std::ios_base::app);
        } else {
            ofs.open(filepath);
        }
    }

    FileLogger &Log([[maybe_unused]] TTYCmd &&cmd) { return *this; }

    template <typename T>
    FileLogger &Log(T &&t) {
        ofs << (std::forward<T>(t));
        return *this;
    }

private:
    FileLogger(const FileLogger &) = delete;
};

class LineLogger : public std::vector<std::string> {
public:
    LineLogger &Log([[maybe_unused]] TTYCmd &&cmd) { return *this; }
    LineLogger &Log(std::string &&t) {
        this->emplace_back(t);
        return *this;
    }
};

class LoggerManager {
public:
    std::mutex logMtx;
    LoggerLevel level{LoggerLevel::ERROR};
    bool stdEnabled{true};
    StdLogger stdLogger;
    std::unordered_map<std::string, std::unique_ptr<FileLogger>> fileLoggerDict;
    std::unordered_map<std::string, std::shared_ptr<LineLogger>> lineLoggerDict;

    bool unitFilter = false;
    std::unordered_set<Unit> unitFilterSet;

    uint64_t logCycles = 0;

    LoggerManager() = default;

    template <typename T>
    void Log(LoggerLevel l, T &&t, T &&tRich) {
        // if need multi-thread: std::lock_guard lock(logMtx);
        if (l >= level) {
            if (stdEnabled) {
                stdLogger.Log(std::forward<T>(tRich));
                fflush(stdout);
            }
        }
        for (auto &entry : fileLoggerDict) {
            entry.second->Log(std::forward<T>(t));
        }
        for (auto &entry : lineLoggerDict) {
            entry.second->Log(std::forward<T>(t));
        }
    }

    static void ResetLevel(LoggerLevel l) { GetManager().level = l; }
    static void SetCycles(uint64_t cyl) { GetManager().logCycles = cyl;}

    static void StdLoggerEnable(bool enabled) { GetManager().stdEnabled = enabled; }

    static void FileLoggerRegister(const std::string &filepath, bool append) {

        GetManager().fileLoggerDict.emplace(filepath, std::unique_ptr<FileLogger>(new FileLogger(filepath, append)));
    }

    static void EnableFilter() { GetManager().unitFilter = true; }
    static void DisEnableFilter() { GetManager().unitFilter = false; }
    static void InitFilterSet(const std::vector<std::string> &filterModule)
    {
        for (auto &module : filterModule) {
            GetManager().unitFilterSet.insert(StrToMachineType(module));
        }
    }
    static bool IsInFilterSet(Unit &unit)
    {
        return GetManager().unitFilterSet.find(unit) != GetManager().unitFilterSet.end();
    }

    static void FileLoggerUnregister(const std::string &filepath) { GetManager().fileLoggerDict.erase(filepath); }

    static void FileLoggerReplace(const std::string &oldfilepath, const std::string &newfilepath, bool append) {
        FileLoggerUnregister(oldfilepath);
        FileLoggerRegister(newfilepath, append);
    }

    static std::shared_ptr<LineLogger> LineLoggerRegister(const std::string &name) {
        auto logger = std::make_shared<LineLogger>();
        GetManager().lineLoggerDict[name] = logger;
        return logger;
    }

    static void LineLoggerUnregister(const std::string &name) { GetManager().lineLoggerDict.erase(name); }

    friend class Logger;
    static LoggerManager &GetManager() {
        static LoggerManager manager;
        return manager;
    }
};
constexpr uint32_t MAX_LOG_BUF_SIZE = 1024;
constexpr uint32_t CYCLE_WIDTH = 7;
constexpr uint32_t PREFIX_WIDTH = 10;
class Logger {
private:
    std::stringstream ss;
    std::stringstream ssRich;
    LoggerLevel level{LoggerLevel::ERROR};
    bool enableLog = false;
    [[maybe_unused]] Unit currentUnit{Unit::UNKNOW_CORE};  // 默认单元
    [[maybe_unused]] Stage currentStage{Stage::NA};

public:
    Logger(LoggerLevel levelIn, [[maybe_unused]] const std::string &func, [[maybe_unused]] int line) : level(levelIn) {
        enableLog = LoggerManager::GetManager().level <= level;
    }

    Logger(LoggerLevel levelIn, Unit unit, Stage stage, [[maybe_unused]] const std::string &func,
          [[maybe_unused]] int line)
        : level(levelIn), currentUnit(unit), currentStage(stage) {
        enableLog = LoggerManager::GetManager().level <= level;
        if (enableLog && LoggerManager::GetManager().unitFilter) {
            enableLog = LoggerManager::GetManager().IsInFilterSet(unit);
        }
        std::stringstream tmp;
        tmp << "[C:" << std::left << std::setw(CYCLE_WIDTH) << std::dec << LoggerManager::GetManager().logCycles << "]";
        tmp << "[" << std::setw(PREFIX_WIDTH) << (EnumToString(unit) + "." + EnumToString(stage)) << "]: ";
        ss << tmp.str();
        ssRich << tmp.str();
    }

    ~Logger() {
        if (enableLog) {
            Log("\n");

            LoggerManager::GetManager().Log(level, ss.str(), ssRich.str());
        }
    }

    Logger &Log(TTYCmd &&val) {
        ssRich << val.Str();
        return *this;
    }

    template <typename T>
    Logger &Log(T &&val) {
        ss << (std::forward<T>(val));
        ssRich << (std::forward<T>(val));
        return *this;
    }

    template <typename T>
    Logger &operator<<(T &&val) {
        if (enableLog) {
            return Log(std::forward<T>(val));
        } else {
            return *this;
        }
    }

    template <typename... Tys>
    Logger &operator()(Tys &&...vals) {
        if (enableLog) {
            LogHelper(std::forward<Tys>(vals)...);
        }
        return *this;
    }

private:
    // 空参数包的特化版本
    void LogHelper() {
    }
    // 非空参数包的版本
    template <typename T, typename... Rest>
    void LogHelper(T &&first, Rest &&...rest) {
        Log(std::forward<T>(first));
        LogHelper(std::forward<Rest>(rest)...);
    }
};
} // namespace JCore

#define LOG_LEVEL(lvl) if (LoggerManager::GetManager().level <= lvl) JCore::Logger(lvl, __func__, __LINE__)
#define LOG_DETAIL LOG_LEVEL(JCore::LoggerLevel::DETAIL)
#define LOG_DE LOG_LEVEL(JCore::LoggerLevel::DEBUG)
#define LOG_INFO LOG_LEVEL(JCore::LoggerLevel::INFO)
#define LOG_WARN LOG_LEVEL(JCore::LoggerLevel::WARN)
#define LOG_ERROR LOG_LEVEL(JCore::LoggerLevel::ERROR)
#define LOG_FATAL LOG_LEVEL(JCore::LoggerLevel::FATAL)
#define LOG_EVENT LOG_LEVEL(JCore::LoggerLevel::EVENT)

#define LOG_LEVEL_M(lvl, unit, stage) if (LoggerManager::GetManager().level <= lvl) \
    JCore::Logger(lvl, unit, stage, __func__, __LINE__)
#define LOG_DETAIL_M(unit, stage) LOG_LEVEL_M(JCore::LoggerLevel::DETAIL, unit, stage)
#define LOG_DEBUG_M(unit, stage) LOG_LEVEL_M(JCore::LoggerLevel::DEBUG, unit, stage)
#define LOG_INFO_M(unit, stage) LOG_LEVEL_M(JCore::LoggerLevel::INFO, unit, stage)
#define LOG_WARN_M(unit, stage) LOG_LEVEL_M(JCore::LoggerLevel::WARN, unit, stage)
#define LOG_ERROR_M(unit, stage) LOG_LEVEL_M(JCore::LoggerLevel::ERROR, unit, stage)
#define LOG_FATAL_M(unit, stage) LOG_LEVEL_M(JCore::LoggerLevel::FATAL, unit, stage)
#define LOG_EVENT_M(unit, stage) LOG_LEVEL_M(JCore::LoggerLevel::EVENT, unit, stage)

#endif
