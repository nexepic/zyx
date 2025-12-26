/**
 * @file SystemStateKeys.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

namespace graph::storage::state::keys {

	constexpr char SYS_CONFIG[] = "sys.config";

	// --- System Configuration Fields ---

	namespace Config {
		constexpr char LOG_LEVEL[] = "log.level";
	}

	// --- Base Keys for Entities ---
	namespace Node {
		constexpr char LABEL_ROOT[] = "node.index.label";
		constexpr char PROPERTY_PREFIX[] = "node.index.property";
	}

	namespace Edge {
		constexpr char LABEL_ROOT[] = "edge.index.label";
		constexpr char PROPERTY_PREFIX[] = "edge.index.property";
	}

	// --- Standard Suffixes for State Entries ---
	// These are appended to base keys to distinguish different storage areas
	constexpr char SUFFIX_CONFIG[] = ".config";        // For enabled flags
	constexpr char SUFFIX_STRING_ROOTS[] = ".string_roots";
	constexpr char SUFFIX_INT_ROOTS[] = ".int_roots";
	constexpr char SUFFIX_DOUBLE_ROOTS[] = ".double_roots";
	constexpr char SUFFIX_BOOL_ROOTS[] = ".bool_roots";
	constexpr char SUFFIX_KEY_TYPES[] = ".key_types";

	// --- Field Names within State Entities ---
	namespace Fields {
		constexpr char ROOT_ID[] = "rootId";
		constexpr char ENABLED[] = "enabled";
	}

	// Stores metadata map: IndexName -> DefinitionString
	// DefinitionString format: "TYPE|LABEL|PROPERTY"
	constexpr char SYS_INDEXES[] = "sys.indexes";

	// Key for controlling storage compaction
	// Value type: boolean (true/false)
	constexpr char STORAGE_COMPACTION_ENABLED[] = "storage.compaction.enabled";

} // namespace graph::storage::state::keys