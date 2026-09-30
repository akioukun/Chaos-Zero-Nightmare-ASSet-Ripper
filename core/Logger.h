#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace Logger {
    namespace Internal {
        inline std::mutex& mutexRef() {
            static std::mutex m; return m;
        }
        inline std::string nowTimestamp() {
            using namespace std::chrono;
            auto now = system_clock::now();
            auto t = system_clock::to_time_t(now);
            auto tm = *std::localtime(&t);
            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
            return oss.str();
        }
        inline void writeLine(const char* level, const std::string& msg) {
            std::lock_guard<std::mutex> lock(mutexRef());
            std::ofstream out("czn_ripper.log", std::ios::app);
            if (!out.is_open()) return;
            out << nowTimestamp() << " [" << level << "] " << msg << "\n";
            out.flush();
        }
    }

    inline void Info(const std::string& message) {
        Internal::writeLine("INFO", message);
    }
    inline void Error(const std::string& message) {
        Internal::writeLine("ERROR", message);
    }
    inline void Flush() {
        std::lock_guard lock(Internal::mutexRef());
        if (std::ofstream out("czn_ripper.log", std::ios::app); out.is_open()) out.flush();
    }
}

inline void LogInfo(const std::string& message) {
    Logger::Info(message);
}
inline void LogError(const std::string& message) {
    Logger::Error(message);
}
inline void LogFlush() {
    Logger::Flush();
}
