/**
 * @file Log.hpp
 * @author Nexepic
 * @date 2025/12/3
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <format>
#include <iostream>
#include <mutex>
#include <string_view>

namespace graph::log {

	class Log {
	public:
		// Configuration
		static void setDebug(bool enable) { debugEnabled_ = enable; }

		static bool isDebugEnabled() { return debugEnabled_; }

		// Standard Info (Always printed, usually to stdout)
		template<typename... Args>
		static void info(std::string_view fmt, Args &&...args) {
			std::lock_guard<std::mutex> lock(mutex_);
			std::cout << "[INFO] " << std::vformat(fmt, std::make_format_args(args...)) << std::endl;
		}

		// Warning Logging (Always printed, to stdout, Magenta color)
		template<typename... Args>
		static void warn(std::string_view fmt, Args &&...args) {
			std::lock_guard<std::mutex> lock(mutex_);
			// ANSI Magenta color
			std::cout << "\033[1;35m[WARN] " << std::vformat(fmt, std::make_format_args(args...)) << "\033[0m"
					  << std::endl;
		}

		// Error Logging (Always printed, to stderr, Red color)
		template<typename... Args>
		static void error(std::string_view fmt, Args &&...args) {
			std::lock_guard<std::mutex> lock(mutex_);
			// ANSI Red color
			std::cerr << "\033[1;31m[ERROR] " << std::vformat(fmt, std::make_format_args(args...)) << "\033[0m"
					  << std::endl;
		}

		// Debug Logging (Only printed if debugEnabled_ is true, usually Yellow/Grey)
		template<typename... Args>
		static void debug(std::string_view fmt, Args &&...args) {
			if (!debugEnabled_)
				return;

			std::lock_guard<std::mutex> lock(mutex_);
			// ANSI Yellow color
			std::cout << "\033[1;33m[DEBUG] " << std::vformat(fmt, std::make_format_args(args...)) << "\033[0m"
					  << std::endl;
		}

	private:
		static inline bool debugEnabled_ = false;
		static inline std::mutex mutex_;
	};

} // namespace graph::log
