/**
 * @file CApi.cpp
 * @author Nexepic
 * @date 2026/1/4
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

#include "metrix/metrix.hpp"
#include "metrix/metrix_c_api.h"
#include "metrix/value.hpp"

#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Error Handling (Thread-Local)
// ============================================================================

static thread_local std::string g_lastError;

void set_error(const std::string &msg) { g_lastError = msg; }

extern "C" const char *metrix_get_last_error() { return g_lastError.c_str(); }

// ============================================================================
// Internal Wrapper Structs
// ============================================================================

struct MetrixDB_T {
	std::unique_ptr<metrix::Database> cpp_db;
};

struct MetrixResult_T {
	metrix::Result cpp_result;

	// --- Memory Management ---
	// FFI clients (like Rust) expect 'const char*' to remain valid
	// until the next call to 'next()' or 'close()'.
	// We store strings here to ensure they don't dangle.
	std::vector<std::string> string_buffer;

	// Buffer for the JSON properties string
	std::string json_buffer;

	MetrixResult_T(metrix::Result res) : cpp_result(std::move(res)) {}
};

// ============================================================================
// Helpers
// ============================================================================

// Helper to escape special characters for JSON strings
std::string escape_json_string(const std::string &s) {
	std::ostringstream o;
	for (char c: s) {
		switch (c) {
			case '"':
				o << "\\\"";
				break;
			case '\\':
				o << "\\\\";
				break;
			case '\b':
				o << "\\b";
				break;
			case '\f':
				o << "\\f";
				break;
			case '\n':
				o << "\\n";
				break;
			case '\r':
				o << "\\r";
				break;
			case '\t':
				o << "\\t";
				break;
			default:
				if ('\x00' <= c && c <= '\x1f') {
					o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
				} else {
					o << c;
				}
		}
	}
	return o.str();
}

// Helper to serialize properties map to JSON string
std::string props_to_json(const std::unordered_map<std::string, metrix::Value> &props) {
	std::ostringstream oss;
	oss << "{";
	bool first = true;
	for (const auto &[k, v]: props) {
		if (!first)
			oss << ",";
		first = false;
		oss << "\"" << k << "\":";

		std::visit(
				[&]<typename T0>(T0 &&arg) {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						oss << "null";
					} else if constexpr (std::is_same_v<T, bool>) {
						oss << (arg ? "true" : "false");
					} else if constexpr (std::is_same_v<T, int64_t>) {
						oss << arg;
					} else if constexpr (std::is_same_v<T, double>) {
						oss << arg;
					} else if constexpr (std::is_same_v<T, std::string>) {
						oss << "\"" << escape_json_string(arg) << "\"";
					} else {
						oss << "\"<ComplexObject>\"";
					}
				},
				v);
	}
	oss << "}";
	return oss.str();
}

// Helper to stabilize a string pointer for C API return
const char *stabilize_string(MetrixResult_T *res, std::string s) {
	res->string_buffer.push_back(std::move(s));
	return res->string_buffer.back().c_str();
}

// ============================================================================
// C API Implementation
// ============================================================================

extern "C" {

// --- Lifecycle ---

MetrixDB_T *metrix_open(const char *path) {
	if (!path)
		return nullptr;
	try {
		auto db = std::make_unique<metrix::Database>(path);
		db->open();
		return new MetrixDB_T{std::move(db)};
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during metrix_open");
		return nullptr;
	}
}

MetrixDB_T *metrix_open_if_exists(const char *path) {
	if (!path) {
		set_error("Path pointer is null");
		return nullptr;
	}
	try {
		if (!std::filesystem::exists(path)) {
			set_error("Database file not found: " + std::string(path));
			return nullptr;
		}

		auto db = std::make_unique<metrix::Database>(path);
		// openIfExists calls open(), which calls validateAndReadHeader()
		if (db->openIfExists()) {
			return new MetrixDB_T{std::move(db)};
		}

		set_error("Failed to open existing database.");
		return nullptr;
	} catch (const std::exception &e) {
		// This will now catch "Invalid file format" or "Data corruption"
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("An unknown hard-fault occurred in the engine.");
		return nullptr;
	}
}

void metrix_close(const MetrixDB_T *db) {
	if (db) {
		try {
			db->cpp_db->close();
		} catch (...) {
			// Ignore errors on close
		}
		delete db;
	}
}

// --- Execution ---

MetrixResult_T *metrix_execute(const MetrixDB_T *db, const char *cypher) {
	if (!db || !cypher)
		return nullptr;
	try {
		auto res = db->cpp_db->execute(cypher);
		return new MetrixResult_T(std::move(res));
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during metrix_execute");
		return nullptr;
	}
}

void metrix_result_close(const MetrixResult_T *res) {
	if (res)
		delete res;
}

// --- Iteration ---

bool metrix_result_next(MetrixResult_T *res) {
	if (!res)
		return false;
	try {
		// Clear buffers from previous row to free memory and avoid staleness
		res->string_buffer.clear();
		res->json_buffer.clear();

		if (res->cpp_result.hasNext()) {
			res->cpp_result.next();
			return true;
		}
		return false;
	} catch (const std::exception &e) {
		set_error(e.what());
		return false;
	} catch (...) {
		set_error("Unknown Exception during metrix_result_next");
		return false;
	}
}

int metrix_result_column_count(const MetrixResult_T *res) {
	if (!res)
		return 0;
	try {
		return res->cpp_result.getColumnCount();
	} catch (...) {
		return 0;
	}
}

const char *metrix_result_column_name(MetrixResult_T *res, int index) {
	if (!res)
		return "";
	try {
		return stabilize_string(res, res->cpp_result.getColumnName(index));
	} catch (...) {
		return "";
	}
}

double metrix_result_get_duration(const MetrixResult_T *res) {
	if (!res)
		return 0.0;
	try {
		return res->cpp_result.getDuration();
	} catch (...) {
		return 0.0;
	}
}

// --- Data Access ---

MetrixValueType metrix_result_get_type(const MetrixResult_T *res, int col_index) {
	if (!res)
		return MX_NULL;
	try {
		auto val = res->cpp_result.get(col_index);
		return std::visit(
				[]<typename T0>([[maybe_unused]] T0 &&arg) -> MetrixValueType {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>)
						return MX_NULL;
					if constexpr (std::is_same_v<T, bool>)
						return MX_BOOL;
					if constexpr (std::is_same_v<T, int64_t>)
						return MX_INT;
					if constexpr (std::is_same_v<T, double>)
						return MX_DOUBLE;
					if constexpr (std::is_same_v<T, std::string>)
						return MX_STRING;
					if constexpr (std::is_same_v<T, std::shared_ptr<metrix::Node>>)
						return MX_NODE;
					if constexpr (std::is_same_v<T, std::shared_ptr<metrix::Edge>>)
						return MX_EDGE;
					return MX_NULL;
				},
				val);
	} catch (...) {
		return MX_NULL;
	}
}

int64_t metrix_result_get_int(const MetrixResult_T *res, int col_index) {
	if (!res)
		return 0;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *v = std::get_if<int64_t>(&val))
			return *v;
	} catch (...) {
	}
	return 0;
}

double metrix_result_get_double(const MetrixResult_T *res, int col_index) {
	if (!res)
		return 0.0;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *v = std::get_if<double>(&val))
			return *v;
	} catch (...) {
	}
	return 0.0;
}

bool metrix_result_get_bool(const MetrixResult_T *res, int col_index) {
	if (!res)
		return false;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *v = std::get_if<bool>(&val))
			return *v;
	} catch (...) {
	}
	return false;
}

const char *metrix_result_get_string(MetrixResult_T *res, int col_index) {
	if (!res)
		return "";
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *v = std::get_if<std::string>(&val)) {
			res->string_buffer.push_back(*v);
			return res->string_buffer.back().c_str();
		}
	} catch (...) {
	}
	return "";
}

// --- Complex Types (Node/Edge) ---

bool metrix_result_get_node(MetrixResult_T *res, int col_index, MetrixNode *out_node) {
	if (!res || !out_node)
		return false;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *nodePtr = std::get_if<std::shared_ptr<metrix::Node>>(&val)) {
			auto &node = **nodePtr;
			out_node->id = node.id;
			out_node->label = stabilize_string(res, node.label);
			return true;
		}
	} catch (...) {
	}
	return false;
}

bool metrix_result_get_edge(MetrixResult_T *res, int col_index, MetrixEdge *out_edge) {
	if (!res || !out_edge)
		return false;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *edgePtr = std::get_if<std::shared_ptr<metrix::Edge>>(&val)) {
			auto &edge = **edgePtr;
			out_edge->id = edge.id;
			out_edge->source_id = edge.sourceId;
			out_edge->target_id = edge.targetId;
			out_edge->type = stabilize_string(res, edge.label);
			return true;
		}
	} catch (...) {
	}
	return false;
}

const char *metrix_result_get_props_json(MetrixResult_T *res, int col_index) {
	if (!res)
		return "{}";
	try {
		auto val = res->cpp_result.get(col_index);
		std::string json;
		std::visit(
				[&]<typename T0>(T0 &&arg) {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::shared_ptr<metrix::Node>>) {
						json = props_to_json(arg->properties);
					} else if constexpr (std::is_same_v<T, std::shared_ptr<metrix::Edge>>) {
						json = props_to_json(arg->properties);
					} else {
						json = "{}";
					}
				},
				val);

		res->json_buffer = std::move(json);
		return res->json_buffer.c_str();
	} catch (...) {
		return "{}";
	}
}

bool metrix_result_is_success(const MetrixResult_T *res) {
	try {
		return res && res->cpp_result.isSuccess();
	} catch (...) {
		return false;
	}
}

const char *metrix_result_get_error(MetrixResult_T *res) {
	if (!res)
		return "Invalid Result Handle";
	try {
		return stabilize_string(res, res->cpp_result.getError());
	} catch (...) {
		return "Unknown Exception obtaining error message";
	}
}

} // extern "C"
