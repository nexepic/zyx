/**
 * @file Log.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/3
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <iostream>
#include <mutex>

namespace graph::utils {

	class Log {
	public:
		// Configuration
		static void setDebug(bool enable) {
			debugEnabled_ = enable;
		}

		static bool isDebugEnabled() {
			return debugEnabled_;
		}

		// Standard Info (Always printed, usually to stdout)
		template<typename... Args>
		static void info(Args&&... args) {
			std::lock_guard<std::mutex> lock(mutex_);
			std::cout << "[INFO] ";
			(std::cout << ... << args);
			std::cout << std::endl;
		}

		// Error Logging (Always printed, to stderr, Red color)
		template<typename... Args>
		static void error(Args&&... args) {
			std::lock_guard<std::mutex> lock(mutex_);
			// ANSI Red color
			std::cerr << "\033[1;31m[ERROR] ";
			(std::cerr << ... << args);
			std::cerr << "\033[0m" << std::endl; // Reset color
		}

		// Debug Logging (Only printed if debugEnabled_ is true, usually Yellow/Grey)
		template<typename... Args>
		static void debug(Args&&... args) {
			if (!debugEnabled_) return;

			std::lock_guard<std::mutex> lock(mutex_);
			// ANSI Yellow color
			std::cout << "\033[1;33m[DEBUG] ";
			(std::cout << ... << args);
			std::cout << "\033[0m" << std::endl;
		}

	private:
		static inline bool debugEnabled_ = false;
		static inline std::mutex mutex_;
	};

} // namespace graph::utils