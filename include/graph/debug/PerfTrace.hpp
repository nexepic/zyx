/**
 * @file PerfTrace.hpp
 * @author Nexepic
 * @date 2026/3/26
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace graph::debug {

	class PerfTrace {
	public:
		struct Entry {
			uint64_t totalNs = 0;
			uint64_t calls = 0;
		};

		using Snapshot = std::unordered_map<std::string, Entry>;

		static void setEnabled(bool enabled);
		[[nodiscard]] static bool isEnabled();

		static void addDuration(std::string_view key, uint64_t durationNs);

		static void reset();
		[[nodiscard]] static Snapshot snapshotAndReset();
	};

} // namespace graph::debug

