#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "include/SimSys.h"

namespace JCore {
class SimSys;

namespace LogLevel {
    constexpr uint8_t CRITICAL = 1;
    constexpr uint8_t HIGH = 2;
    constexpr uint8_t MEDIUM = 3;
    constexpr uint8_t LOW = 4;
    constexpr uint8_t MINIMAL = 5;
}

class DebugLog;
struct LogParams {
    Unit unit;
    Stage stage;
    uint32_t blockId;
    uint64_t tileOpId;
    uint8_t level;
};
// Convenience Macros (must be defined after DebugLog class is declared)
// These macros now require the logger instance and unit as parameters
#define LOG_DEBUG(logger, unit, stage, id1, id2, level, ...) \
    do { \
        LogParams params = {unit, stage, id1, id2, level}; \
        (logger)->Debug(params, __VA_ARGS__); \
    } while (0)

#define LOG_DEBUG_STRUCT(logger, unit, stage, id1, id2, level, obj, ...) \
    do { \
        LogParams params = {unit, stage, id1, id2, level}; \
        (logger)->DebugStruct(params, obj, __VA_ARGS__); \
    } while (0)

// DebugLog Class Declaration
class DebugLog {
public:
    // Delete copy constructor and assignment operator
    DebugLog(const DebugLog&) = delete;
    DebugLog& operator=(const DebugLog&) = delete;

    // Constructor takes sim* pointer
    explicit DebugLog();

    // Initialize log file
    bool Init(const std::string& filename);

    // Close log file
    void Close();

    // Set the log level threshold. Only events with level >= threshold are printed.
    void SetLogLevelThreshold(uint8_t threshold);

    // Get the current log level threshold
    uint8_t GetLogLevelThreshold() const;

    uint64_t AllocDebugId();

    void Debug(const LogParams& params, const char* info)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Check level threshold
        if (params.level > logLevelThreshold_) {
            return;
        }
        if (!fileOpened_) {
            std::cerr << "[ERROR] Log file not open!" << std::endl;
            return;
        }
        if (!simPtr_) {
            std::cerr << "[ERROR] DebugLog: sim pointer is null! Cannot get cycle." << std::endl;
            return;
        }

        uint64_t currentCycle = simPtr_->getCycles();
        std::string formattedMsg = info;
        // std::string levelStr = std::to_string(level);
        std::string bidStr = std::to_string(params.blockId);
        std::string tileOpIdStr = std::to_string(params.tileOpId);
        if (params.blockId == std::numeric_limits<uint32_t>::max()) {
            bidStr = "-1";
        }
        if (params.tileOpId == std::numeric_limits<uint64_t>::max()) {
            tileOpIdStr = "-1";
        }

        std::stringstream s1;
        std::stringstream s2;
        std::stringstream s3;
        std::stringstream s4;
        s1 << "@" << std::dec << currentCycle << ",";
        s2 << "[" <<  EnumToString(params.unit) << "." << EnumToString(params.stage) << "],";
        s3 <<  "BlockId=" << bidStr << ",";
        s4 <<  "tileOpId=" << tileOpIdStr << ",";

        constexpr int lenTypeB = 10;
        constexpr int lenTypeC = 13;
        file_ << std::left << std::setw(lenTypeB) << s1.str()
              << std::setw(lenTypeC) << s2.str()
              << std::setw(lenTypeB) << s3.str()
              << std::setw(lenTypeB) << s4.str()
              << formattedMsg << "\n";
        file_.flush();
    }

    template<typename... Args>
    // void Debug(Unit unit, Stage stage, uint32_t id1, uint64_t id2,
    //                     int level, const char* format, Args... args) {
    void Debug(const LogParams& params, const char* format, Args... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Check level threshold
        if (params.level > logLevelThreshold_) {
            return;
        }
        if (!fileOpened_) {
            std::cerr << "[ERROR] Log file not open!" << std::endl;
            return;
        }
        if (!simPtr_) {
            std::cerr << "[ERROR] DebugLog: sim pointer is null! Cannot get cycle." << std::endl;
            return;
        }

        uint64_t currentCycle = simPtr_->getCycles();
        std::string formattedMsg = FormatString(format, args...);
        // std::string levelStr = std::to_string(level);
        std::string bidStr = std::to_string(params.blockId);
        std::string tileOpIdStr = std::to_string(params.tileOpId);
        if (params.blockId == std::numeric_limits<uint32_t>::max()) {
            bidStr = "-1";
        }
        if (params.tileOpId == std::numeric_limits<uint64_t>::max()) {
            tileOpIdStr = "-1";
        }

        std::stringstream s1;
        std::stringstream s2;
        std::stringstream s3;
        std::stringstream s4;
        s1 << "@" << std::dec << currentCycle << ",";
        s2 << "[" <<  EnumToString(params.unit) << "." << EnumToString(params.stage) << "],";
        s3 <<  "BlockId=" << bidStr << ",";
        s4 <<  "tileOpId=" << tileOpIdStr << ",";

        constexpr int lenTypeB = 10;
        constexpr int lenTypeC = 13;
        file_ << std::left << std::setw(lenTypeB) << s1.str()
              << std::setw(lenTypeC) << s2.str()
              << std::setw(lenTypeB) << s3.str()
              << std::setw(lenTypeB) << s4.str()
              << formattedMsg << "\n";
        file_.flush();
    }

    template<typename T, typename... Args>
    void DebugStruct(const LogParams& params, T obj, const char* format, Args... args)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Check level threshold
        if (params.level > logLevelThreshold_) {
            return;
        }
        if (!fileOpened_) {
            std::cerr << "[ERROR] Log file not open!" << std::endl;
            return;
        }
        if (!simPtr_) {
            std::cerr << "[ERROR] DebugLog: sim pointer is null! Cannot get cycle." << std::endl;
            return;
        }

        uint64_t currentCycle = simPtr_->getCycles();
        std::string formattedMsg = FormatString(format, args...);
        // std::string levelStr = std::to_string(level);
        std::string bidStr = std::to_string(params.blockId);
        std::string tileOpIdStr = std::to_string(params.tileOpId);
        if (params.blockId == std::numeric_limits<uint32_t>::max()) {
            bidStr = "-1";
        }
        if (params.tileOpId == std::numeric_limits<uint64_t>::max()) {
            tileOpIdStr = "-1";
        }

        std::stringstream s1;
        std::stringstream s2;
        std::stringstream s3;
        std::stringstream s4;
        s1 << "@" << std::dec << currentCycle << ",";
        s2 << "[" <<  EnumToString(params.unit) << "." << EnumToString(params.stage) << "],";
        s3 <<  "BlockId=" << bidStr << ",";
        s4 <<  "tileOpId=" << tileOpIdStr << ",";

        constexpr int lenTypeB = 10;
        constexpr int lenTypeC = 13;
        file_ << std::left << std::setw(lenTypeB) << s1.str()
              << std::setw(lenTypeC) << s2.str()
              << std::setw(lenTypeB) << s3.str()
              << std::setw(lenTypeB) << s4.str()
              << formattedMsg << " " << obj << "\n";
        file_.flush();
    }
public:
    SimSys *simPtr_ = nullptr;

private:
    // 递归终止
    static void PrintRecursive(std::string& oss) {}

    template<typename T, typename... Rest>
    static void PrintRecursive(std::string& oss, T first, Rest... rest)
    {
        oss += std::to_string(first) + " ";
        printRecursive(oss, rest...);
    }
    static void PrintRecursive(std::string& result, const char* str)
    {
        result += str;
        result += " ";
    }
    static void PrintRecursive(std::string& result, const std::string& str)
    {
        result += str;
        result += " ";
    }

    template<typename T>
    static const T& FormatArg(const T& arg)
    {
        return arg;
    }

    static const char* FormatArg(const std::string& arg)
    {
        return arg.c_str();
    }

    template<typename... Args>
    static std::string FormatString(const char* format, const Args&... args)
    {
        int size = snprintf(nullptr, 0, format, FormatArg(args)...) + 1; // +1 给 '\0'
        if (size <= 0) {
            return "[Format Error]";
        }

        std::vector<char> buf(size);
        snprintf(buf.data(), size, format, FormatArg(args)...);
        return std::string(buf.data());
    }

    // Fix compile error on higher version of g++
    static std::string FormatString(const char* info)
    {
        return std::string(info);
    }
    mutable std::mutex mutex_; // Mutex for thread safety
    std::ofstream file_; // Log file stream
    bool fileOpened_; // Flag indicating if file is open
    int logLevelThreshold_ = 3; // Log level threshold
    bool debugModeOn = false; // equal DebugVerboseON
    static uint64_t debugId; // if debugId equals UINT64_MAX, it indicates that it has not been used
};

} // namespace JCore

#endif // DEBUG_LOG_H
