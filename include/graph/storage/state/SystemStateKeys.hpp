/**
 * @file SystemStateKeys.hpp
 * @author Nexepic
 * @date 2025/12/15
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

namespace graph::storage::state::keys {

	constexpr char SYS_CONFIG[] = "sys.config";

	// --- System Configuration Fields ---

	namespace Config {
		constexpr char LOG_LEVEL[] = "log.level";
		// Key for controlling storage compaction
		// Value type: boolean (true/false)
		constexpr char STORAGE_COMPACTION_ENABLED[] = "storage.compaction.enabled";
		// Key for controlling thread pool size
		// Value type: int64_t (0 = auto-detect, 1 = single-threaded, >1 = N threads)
		constexpr char THREAD_POOL_SIZE[] = "thread.pool.size";
	} // namespace Config

	// --- Base Keys for Entities ---
	namespace Node {
		constexpr char LABEL_ROOT[] = "node.index.label";
		constexpr char PROPERTY_PREFIX[] = "node.index.property";
	} // namespace Node

	namespace Edge {
		constexpr char LABEL_ROOT[] = "edge.index.label";
		constexpr char PROPERTY_PREFIX[] = "edge.index.property";
	} // namespace Edge

	// --- Standard Suffixes for State Entries ---
	// These are appended to base keys to distinguish different storage areas
	constexpr char SUFFIX_CONFIG[] = ".config"; // For enabled flags
	constexpr char SUFFIX_STRING_ROOTS[] = ".string_roots";
	constexpr char SUFFIX_INT_ROOTS[] = ".int_roots";
	constexpr char SUFFIX_DOUBLE_ROOTS[] = ".double_roots";
	constexpr char SUFFIX_BOOL_ROOTS[] = ".bool_roots";
	constexpr char SUFFIX_KEY_TYPES[] = ".key_types";

	// --- Field Names within State Entities ---
	namespace Fields {
		constexpr char ROOT_ID[] = "rootId";
		constexpr char ENABLED[] = "enabled";
	} // namespace Fields

	// Stores metadata map: IndexName -> DefinitionString
	// DefinitionString format: "TYPE|LABEL|PROPERTY"
	constexpr char SYS_INDEXES[] = "sys.indexes";

} // namespace graph::storage::state::keys
