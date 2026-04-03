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

#include "zyx/zyx.hpp"
#include "zyx/zyx_c_api.h"
#include "zyx/value.hpp"

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

extern "C" const char *zyx_get_last_error() { return g_lastError.c_str(); }

// ============================================================================
// Internal Wrapper Structs
// ============================================================================

struct ZYXDB_T {
	std::unique_ptr<zyx::Database> cpp_db;
};

struct ZYXResult_T {
	zyx::Result cpp_result;

	// --- Memory Management ---
	// FFI clients (like Rust) expect 'const char*' to remain valid
	// until the next call to 'next()' or 'close()'.
	// We store strings here to ensure they don't dangle.
	std::vector<std::string> string_buffer;

	// Buffer for the JSON properties string
	std::string json_buffer;

	ZYXResult_T(zyx::Result res) : cpp_result(std::move(res)) {}
};

struct ZYXTxn_T {
	zyx::Transaction cpp_txn;
	ZYXTxn_T(zyx::Transaction txn) : cpp_txn(std::move(txn)) {}
};

struct ZYXParams_T {
	std::unordered_map<std::string, zyx::Value> params;
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
std::string props_to_json(const std::unordered_map<std::string, zyx::Value> &props) {
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

// Helper to serialize a single Value to JSON
void value_to_json(std::ostringstream &oss, const zyx::Value &v) {
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
				} else if constexpr (std::is_same_v<T, std::vector<float>>) {
					oss << "[";
					for (size_t i = 0; i < arg.size(); ++i) {
						if (i > 0) oss << ",";
						oss << arg[i];
					}
					oss << "]";
				} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
					oss << "[";
					for (size_t i = 0; i < arg.size(); ++i) {
						if (i > 0) oss << ",";
						oss << "\"" << escape_json_string(arg[i]) << "\"";
					}
					oss << "]";
				} else {
					oss << "\"<ComplexObject>\"";
				}
			},
			v);
}

// Helper to stabilize a string pointer for C API return
const char *stabilize_string(ZYXResult_T *res, std::string s) {
	res->string_buffer.push_back(std::move(s));
	return res->string_buffer.back().c_str();
}

// Convert ZYXParams_T to std::unordered_map<std::string, zyx::Value>
std::unordered_map<std::string, zyx::Value> params_to_map(const ZYXParams_T *params) {
	if (!params) return {};
	return params->params;
}

// ============================================================================
// C API Implementation
// ============================================================================

extern "C" {

// --- Lifecycle ---

ZYXDB_T *zyx_open(const char *path) {
	if (!path)
		return nullptr;
	try {
		auto db = std::make_unique<zyx::Database>(path);
		db->open();
		return new ZYXDB_T{std::move(db)};
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during zyx_open");
		return nullptr;
	}
}

ZYXDB_T *zyx_open_if_exists(const char *path) {
	if (!path) {
		set_error("Path pointer is null");
		return nullptr;
	}
	try {
		if (!std::filesystem::exists(path)) {
			set_error("Database file not found: " + std::string(path));
			return nullptr;
		}

		auto db = std::make_unique<zyx::Database>(path);
		// openIfExists calls open(), which calls validateAndReadHeader()
		if (db->openIfExists()) {
			return new ZYXDB_T{std::move(db)};
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

void zyx_close(const ZYXDB_T *db) {
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

ZYXResult_T *zyx_execute(const ZYXDB_T *db, const char *cypher) {
	if (!db || !cypher)
		return nullptr;
	try {
		auto res = db->cpp_db->execute(cypher);
		return new ZYXResult_T(std::move(res));
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during zyx_execute");
		return nullptr;
	}
}

ZYXResult_T *zyx_execute_params(const ZYXDB_T *db, const char *cypher, const ZYXParams_T *params) {
	if (!db || !cypher)
		return nullptr;
	try {
		auto res = db->cpp_db->execute(cypher, params_to_map(params));
		return new ZYXResult_T(std::move(res));
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during zyx_execute_params");
		return nullptr;
	}
}

void zyx_result_close(const ZYXResult_T *res) {
	if (res)
		delete res;
}

// --- Transaction Control ---

ZYXTxn_T *zyx_begin_transaction(ZYXDB_T *db) {
	if (!db)
		return nullptr;
	try {
		auto txn = db->cpp_db->beginTransaction();
		return new ZYXTxn_T(std::move(txn));
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during zyx_begin_transaction");
		return nullptr;
	}
}

ZYXResult_T *zyx_txn_execute(ZYXTxn_T *txn, const char *cypher) {
	if (!txn || !cypher)
		return nullptr;
	try {
		auto res = txn->cpp_txn.execute(cypher);
		return new ZYXResult_T(std::move(res));
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during zyx_txn_execute");
		return nullptr;
	}
}

ZYXResult_T *zyx_txn_execute_params(ZYXTxn_T *txn, const char *cypher, const ZYXParams_T *params) {
	if (!txn || !cypher)
		return nullptr;
	try {
		auto res = txn->cpp_txn.execute(cypher, params_to_map(params));
		return new ZYXResult_T(std::move(res));
	} catch (const std::exception &e) {
		set_error(e.what());
		return nullptr;
	} catch (...) {
		set_error("Unknown Exception during zyx_txn_execute_params");
		return nullptr;
	}
}

bool zyx_txn_commit(ZYXTxn_T *txn) {
	if (!txn)
		return false;
	try {
		txn->cpp_txn.commit();
		return true;
	} catch (const std::exception &e) {
		set_error(e.what());
		return false;
	} catch (...) {
		set_error("Unknown Exception during zyx_txn_commit");
		return false;
	}
}

void zyx_txn_rollback(ZYXTxn_T *txn) {
	if (!txn)
		return;
	try {
		txn->cpp_txn.rollback();
	} catch (...) {
		// Ignore errors during rollback
	}
}

void zyx_txn_close(ZYXTxn_T *txn) {
	if (txn)
		delete txn; // Transaction destructor auto-rolls back if active
}

// --- Parameters ---

ZYXParams_T *zyx_params_create() {
	return new ZYXParams_T{};
}

void zyx_params_set_int(ZYXParams_T *p, const char *key, int64_t val) {
	if (p && key)
		p->params[key] = val;
}

void zyx_params_set_double(ZYXParams_T *p, const char *key, double val) {
	if (p && key)
		p->params[key] = val;
}

void zyx_params_set_string(ZYXParams_T *p, const char *key, const char *val) {
	if (p && key && val)
		p->params[key] = std::string(val);
}

void zyx_params_set_bool(ZYXParams_T *p, const char *key, bool val) {
	if (p && key)
		p->params[key] = val;
}

void zyx_params_set_null(ZYXParams_T *p, const char *key) {
	if (p && key)
		p->params[key] = std::monostate{};
}

void zyx_params_close(ZYXParams_T *p) {
	delete p;
}

// --- Batch Operations ---

bool zyx_create_node(ZYXDB_T *db, const char *label, const ZYXParams_T *props) {
	if (!db || !label)
		return false;
	try {
		db->cpp_db->createNode(label, params_to_map(props));
		return true;
	} catch (const std::exception &e) {
		set_error(e.what());
		return false;
	} catch (...) {
		set_error("Unknown Exception during zyx_create_node");
		return false;
	}
}

int64_t zyx_create_node_ret_id(ZYXDB_T *db, const char *label, const ZYXParams_T *props) {
	if (!db || !label)
		return -1;
	try {
		return db->cpp_db->createNodeRetId(label, params_to_map(props));
	} catch (const std::exception &e) {
		set_error(e.what());
		return -1;
	} catch (...) {
		set_error("Unknown Exception during zyx_create_node_ret_id");
		return -1;
	}
}

bool zyx_create_edge_by_id(ZYXDB_T *db, int64_t source_id, int64_t target_id,
                            const char *label, const ZYXParams_T *props) {
	if (!db || !label)
		return false;
	try {
		db->cpp_db->createEdgeById(source_id, target_id, label, params_to_map(props));
		return true;
	} catch (const std::exception &e) {
		set_error(e.what());
		return false;
	} catch (...) {
		set_error("Unknown Exception during zyx_create_edge_by_id");
		return false;
	}
}

// --- Iteration ---

bool zyx_result_next(ZYXResult_T *res) {
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
		set_error("Unknown Exception during zyx_result_next");
		return false;
	}
}

int zyx_result_column_count(const ZYXResult_T *res) {
	if (!res)
		return 0;
	try {
		return res->cpp_result.getColumnCount();
	} catch (...) {
		return 0;
	}
}

const char *zyx_result_column_name(ZYXResult_T *res, int index) {
	if (!res)
		return "";
	try {
		return stabilize_string(res, res->cpp_result.getColumnName(index));
	} catch (...) {
		return "";
	}
}

double zyx_result_get_duration(const ZYXResult_T *res) {
	if (!res)
		return 0.0;
	try {
		return res->cpp_result.getDuration();
	} catch (...) {
		return 0.0;
	}
}

// --- Data Access ---

ZYXValueType zyx_result_get_type(const ZYXResult_T *res, int col_index) {
	if (!res)
		return ZYX_NULL;
	try {
		auto val = res->cpp_result.get(col_index);
		return std::visit(
				[]<typename T0>([[maybe_unused]] T0 &&arg) -> ZYXValueType {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>)
						return ZYX_NULL;
					if constexpr (std::is_same_v<T, bool>)
						return ZYX_BOOL;
					if constexpr (std::is_same_v<T, int64_t>)
						return ZYX_INT;
					if constexpr (std::is_same_v<T, double>)
						return ZYX_DOUBLE;
					if constexpr (std::is_same_v<T, std::string>)
						return ZYX_STRING;
					if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Node>>)
						return ZYX_NODE;
					if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Edge>>)
						return ZYX_EDGE;
					if constexpr (std::is_same_v<T, std::vector<float>> ||
					              std::is_same_v<T, std::vector<std::string>>)
						return ZYX_LIST;
					return ZYX_NULL;
				},
				val);
	} catch (...) {
		return ZYX_NULL;
	}
}

int64_t zyx_result_get_int(const ZYXResult_T *res, int col_index) {
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

double zyx_result_get_double(const ZYXResult_T *res, int col_index) {
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

bool zyx_result_get_bool(const ZYXResult_T *res, int col_index) {
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

const char *zyx_result_get_string(ZYXResult_T *res, int col_index) {
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

bool zyx_result_get_node(ZYXResult_T *res, int col_index, ZYXNode *out_node) {
	if (!res || !out_node)
		return false;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *nodePtr = std::get_if<std::shared_ptr<zyx::Node>>(&val)) {
			auto &node = **nodePtr;
			out_node->id = node.id;
			out_node->label = stabilize_string(res, node.label);
			return true;
		}
	} catch (...) {
	}
	return false;
}

bool zyx_result_get_edge(ZYXResult_T *res, int col_index, ZYXEdge *out_edge) {
	if (!res || !out_edge)
		return false;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *edgePtr = std::get_if<std::shared_ptr<zyx::Edge>>(&val)) {
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

const char *zyx_result_get_props_json(ZYXResult_T *res, int col_index) {
	if (!res)
		return "{}";
	try {
		auto val = res->cpp_result.get(col_index);
		std::string json;
		std::visit(
				[&]<typename T0>(T0 &&arg) {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Node>>) {
						json = props_to_json(arg->properties);
					} else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Edge>>) {
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

// --- List Access ---

int zyx_result_list_size(const ZYXResult_T *res, int col_index) {
	if (!res)
		return 0;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *v = std::get_if<std::vector<std::string>>(&val))
			return static_cast<int>(v->size());
	} catch (...) {
	}
	return 0;
}

ZYXValueType zyx_result_list_get_type(const ZYXResult_T *res, int col_index, [[maybe_unused]] int list_index) {
	if (!res)
		return ZYX_NULL;
	try {
		auto val = res->cpp_result.get(col_index);
		if (std::get_if<std::vector<std::string>>(&val))
			return ZYX_STRING;
	} catch (...) {
	}
	return ZYX_NULL;
}

int64_t zyx_result_list_get_int(const ZYXResult_T *res, int col_index, int list_index) {
	if (!res)
		return 0;
	try {
		[[maybe_unused]] auto val = res->cpp_result.get(col_index);
		[[maybe_unused]] int idx = list_index;
	} catch (...) {
	}
	return 0;
}

double zyx_result_list_get_double(const ZYXResult_T *res, int col_index, int list_index) {
	if (!res)
		return 0.0;
	try {
		[[maybe_unused]] auto val = res->cpp_result.get(col_index);
		[[maybe_unused]] int idx = list_index;
	} catch (...) {
	}
	return 0.0;
}

bool zyx_result_list_get_bool(const ZYXResult_T *res, int col_index, int list_index) {
	if (!res)
		return false;
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *v = std::get_if<std::vector<std::string>>(&val)) {
			if (list_index >= 0 && list_index < static_cast<int>(v->size()))
				return (*v)[list_index] == "true";
		}
	} catch (...) {
	}
	return false;
}

const char *zyx_result_list_get_string(ZYXResult_T *res, int col_index, int list_index) {
	if (!res)
		return "";
	try {
		auto val = res->cpp_result.get(col_index);
		if (auto *v = std::get_if<std::vector<std::string>>(&val)) {
			if (list_index >= 0 && list_index < static_cast<int>(v->size()))
				return stabilize_string(res, (*v)[list_index]);
		}
	} catch (...) {
	}
	return "";
}

// --- Map Access ---

const char *zyx_result_get_map_json(ZYXResult_T *res, int col_index) {
	if (!res)
		return "{}";
	try {
		auto val = res->cpp_result.get(col_index);
		std::ostringstream oss;
		value_to_json(oss, val);
		res->json_buffer = oss.str();
		return res->json_buffer.c_str();
	} catch (...) {
		return "{}";
	}
}

// --- Status ---

bool zyx_result_is_success(const ZYXResult_T *res) {
	try {
		return res && res->cpp_result.isSuccess();
	} catch (...) {
		return false;
	}
}

const char *zyx_result_get_error(ZYXResult_T *res) {
	if (!res)
		return "Invalid Result Handle";
	try {
		return stabilize_string(res, res->cpp_result.getError());
	} catch (...) {
		return "Unknown Exception obtaining error message";
	}
}

} // extern "C"
