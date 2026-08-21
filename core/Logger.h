#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace LoggerInternal {
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

inline void LogInfo(const std::string& message) {
	LoggerInternal::writeLine("INFO", message);
}
inline void LogError(const std::string& message) {
	LoggerInternal::writeLine("ERROR", message);
}
inline void LogFlush() {
	std::lock_guard<std::mutex> lock(LoggerInternal::mutexRef());
	std::ofstream out("czn_ripper.log", std::ios::app);
	if (out.is_open()) out.flush();
}
