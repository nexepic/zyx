/**
 * @file DatabaseImpl.cpp
 * @author Nexepic
 * @date 2025/12/16
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

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include "graph/core/Database.hpp"
#include "graph/core/Transaction.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/log/Log.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/QueryPlan.hpp"
#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include "graph/query/api/QueryBuilder.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "zyx/zyx.hpp"
#include "DatabaseBulkInternal.hpp"

namespace zyx {

	// ========================================================================
	// 1. Helpers: Type Conversion
	// ========================================================================

	namespace {
		size_t skipSpaces(const std::string_view s, size_t i) {
			while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
				++i;
			}
			return i;
		}

		std::string toUpperCopy(const std::string_view s) {
			std::string out;
			out.reserve(s.size());
			for (char ch: s) {
				out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
			}
			return out;
		}

		std::string readToken(const std::string_view s, size_t &i) {
			i = skipSpaces(s, i);
			size_t start = i;
			while (i < s.size()) {
				const unsigned char ch = static_cast<unsigned char>(s[i]);
				if (std::isalnum(ch) || ch == '_' || ch == '.') {
					++i;
				} else {
					break;
				}
			}
			return std::string(s.substr(start, i - start));
		}

		enum class CypherTxnKind { NONE, TXN_BEGIN, TXN_COMMIT, TXN_ROLLBACK };

		CypherTxnKind detectCypherTxnStatement(const std::string_view query) {
			size_t pos = 0;
			std::string first = readToken(query, pos);
			std::string upper = toUpperCopy(first);
			if (upper == "BEGIN") {
				return CypherTxnKind::TXN_BEGIN;
			}
			if (upper == "COMMIT") {
				return CypherTxnKind::TXN_COMMIT;
			}
			if (upper == "ROLLBACK") {
				return CypherTxnKind::TXN_ROLLBACK;
			}
			return CypherTxnKind::NONE;
		}
	} // namespace

	// Forward declaration
	Value toPublicValue(const graph::query::ResultValue &v, const std::shared_ptr<graph::storage::DataManager> &dm);
	Value toPublicValue(const graph::PropertyValue &v);
	Value toDriverAbiValue(const graph::query::ResultValue &v, const std::shared_ptr<graph::storage::DataManager> &dm);
	Value toDriverAbiValue(const graph::PropertyValue &v, const std::shared_ptr<graph::storage::DataManager> &dm);
	Value toStringListValue(const std::vector<graph::PropertyValue> &values);

	// Convert Internal Node to Public Node Pointer (For shared_ptr usage in Value)
	std::shared_ptr<Node> toPublicNodePtr(const graph::Node &internalNode,
										  const std::shared_ptr<graph::storage::DataManager> &dm) {
		auto pubNode = std::make_shared<Node>();
		pubNode->id = internalNode.getId();

		// RESOLVE: All label IDs -> Strings using DataManager
		if (dm) {
			for (int64_t lid: internalNode.getLabelIds()) {
				if (lid != 0) {
					pubNode->labels.push_back(dm->resolveTokenName(lid));
				}
			}
			// Backward compat: first label
			pubNode->label = pubNode->labels.empty() ? "" : pubNode->labels[0];
		}

		for (const auto &[key, val]: internalNode.getProperties()) {
			pubNode->properties[key] = toPublicValue(val);
		}
		return pubNode;
	}

	// Convert Internal Edge to Public Edge Pointer
	std::shared_ptr<Edge> toPublicEdgePtr(const graph::Edge &internalEdge,
										  const std::shared_ptr<graph::storage::DataManager> &dm) {
		auto pubEdge = std::make_shared<Edge>();
		pubEdge->id = internalEdge.getId();
		pubEdge->sourceId = internalEdge.getSourceNodeId();
		pubEdge->targetId = internalEdge.getTargetNodeId();

		// RESOLVE: ID -> String using DataManager
		if (dm) {
			pubEdge->type = dm->resolveTokenName(internalEdge.getTypeId());
		}

		for (const auto &[key, val]: internalEdge.getProperties()) {
			pubEdge->properties[key] = toPublicValue(val);
		}
		return pubEdge;
	}

	// Convert Internal Node to Public Node (Value Copy for Algorithms)
	Node toPublicNode(const graph::Node &internalNode, const std::shared_ptr<graph::storage::DataManager> &dm) {
		auto ptr = toPublicNodePtr(internalNode, dm);
		return *ptr; // Copy the struct
	}

	// Convert internal PropertyValue to public Value variant
	Value toStringListValue(const std::vector<graph::PropertyValue> &values) {
		std::vector<std::string> strings;
		strings.reserve(values.size());
		for (const auto &val: values) {
			strings.push_back(val.toString());
		}
		return strings;
	}

	Value toPublicValue(const graph::PropertyValue &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						return std::monostate{};
					} else if constexpr (std::is_same_v<T, std::vector<graph::PropertyValue>>) {
						return toStringListValue(arg);
					} else if constexpr (std::is_same_v<T, graph::PropertyValue::MapType>) {
						graph::PropertyValue pv(arg);
						return pv.toString();
					} else if constexpr (std::is_same_v<T, graph::TemporalDate> ||
										 std::is_same_v<T, graph::TemporalDateTime> ||
										 std::is_same_v<T, graph::TemporalDuration>) {
						return arg.toISO();
					} else {
						// Primitives (int, double, bool, string) map directly
						return arg;
					}
				},
				v.getVariant());
	}

	Value toDriverAbiValue(const graph::PropertyValue &v, const std::shared_ptr<graph::storage::DataManager> &dm) {
		return std::visit(
				[&dm]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, std::monostate>) {
						return std::monostate{};
					} else if constexpr (std::is_same_v<T, std::vector<graph::PropertyValue>>) {
						auto list = std::make_shared<ValueList>();
						list->elements.reserve(arg.size());
						for (const auto &val: arg) {
							list->elements.push_back(toDriverAbiValue(val, dm));
						}
						return list;
					} else if constexpr (std::is_same_v<T, graph::PropertyValue::MapType>) {
						auto map = std::make_shared<ValueMap>();
						for (const auto &[key, val]: arg) {
							map->entries.emplace(key, toDriverAbiValue(val, dm));
						}
						return map;
					} else if constexpr (std::is_same_v<T, graph::TemporalDate> ||
										 std::is_same_v<T, graph::TemporalDateTime> ||
										 std::is_same_v<T, graph::TemporalDuration>) {
						return arg.toISO();
					} else {
						return arg;
					}
				},
				v.getVariant());
	}

	// Main conversion entry point for ResultValue
	Value toPublicValue(const graph::query::ResultValue &v, const std::shared_ptr<graph::storage::DataManager> &dm) {
		return std::visit(
				[&dm]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, graph::Node>) {
						return toPublicNodePtr(arg, dm);
					} else if constexpr (std::is_same_v<T, graph::Edge>) {
						return toPublicEdgePtr(arg, dm);
					} else if constexpr (std::is_same_v<T, graph::PropertyValue>) {
						return toPublicValue(arg);
					} else {
						return std::monostate{};
					}
				},
				v.getVariant());
	}

	Value toDriverAbiValue(const graph::query::ResultValue &v, const std::shared_ptr<graph::storage::DataManager> &dm) {
		return std::visit(
				[&dm]<typename T0>(T0 &&arg) -> Value {
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, graph::Node>) {
						return toPublicNodePtr(arg, dm);
					} else if constexpr (std::is_same_v<T, graph::Edge>) {
						return toPublicEdgePtr(arg, dm);
					} else if constexpr (std::is_same_v<T, graph::PropertyValue>) {
						return toDriverAbiValue(arg, dm);
					} else {
						return std::monostate{};
					}
				},
				v.getVariant());
	}

	// Convert public API Value to internal PropertyValue
	graph::PropertyValue toInternal(const Value &v) {
		return std::visit(
				[]<typename T0>(T0 &&arg) -> graph::PropertyValue {
					using T = std::decay_t<T0>;

					if constexpr (std::is_same_v<T, std::vector<float>>) {
						std::vector<graph::PropertyValue> vec;
						vec.reserve(arg.size());
						for (const auto value: arg) {
							vec.emplace_back(static_cast<double>(value));
						}
						return graph::PropertyValue(vec);
					} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
						std::vector<graph::PropertyValue> vec;
						vec.reserve(arg.size());
						for (const auto &s: arg) {
							// Try parsing as different types in order
							// First try int
							try {
								size_t pos = 0;
								int64_t val = std::stoll(s, &pos);
								if (pos == s.size()) {
									vec.push_back(graph::PropertyValue(val));
									continue;
								}
							} catch (...) {
								// Not an int, continue to next type
							}
							// Then try double
							try {
								size_t pos = 0;
								double val = std::stod(s, &pos);
								if (pos == s.size()) {
									vec.push_back(graph::PropertyValue(val));
									continue;
								}
							} catch (...) {
								// Not a double, continue to next type
							}
							// Otherwise store as string
							vec.push_back(graph::PropertyValue(s));
						}
						return graph::PropertyValue(vec);
					} else if constexpr (std::is_same_v<T, std::shared_ptr<ValueList>>) {
						if (!arg)
							return graph::PropertyValue();
						std::vector<graph::PropertyValue> vec;
						vec.reserve(arg->elements.size());
						for (const auto &elem: arg->elements) {
							vec.push_back(toInternal(elem));
						}
						return graph::PropertyValue(std::move(vec));
					} else if constexpr (std::is_same_v<T, std::shared_ptr<ValueMap>>) {
						if (!arg)
							return graph::PropertyValue();
						graph::PropertyValue::MapType map;
						for (const auto &[k, v]: arg->entries) {
							map.emplace(k, toInternal(v));
						}
						return graph::PropertyValue(std::move(map));
					} else if constexpr (std::is_same_v<T, std::shared_ptr<Node>> ||
										 std::is_same_v<T, std::shared_ptr<Edge>>) {
						return {};
					} else {
						return graph::PropertyValue(arg);
					}
				},
				v);
	}

	void validatePropertyColumns(const std::vector<PropertyColumn> &columns, size_t rowCount, std::string_view context) {
		std::unordered_set<std::string> seenNames;
		seenNames.reserve(columns.size());

		for (const auto &column: columns) {
			if (column.name.empty()) {
				throw std::invalid_argument(std::string(context) + " requires non-empty property column names");
			}
			if (column.values.size() != rowCount) {
				throw std::invalid_argument(std::string(context) + " property column '" + column.name +
											"' has " + std::to_string(column.values.size()) +
											" values for " + std::to_string(rowCount) + " rows");
			}
			auto [_, inserted] = seenNames.insert(column.name);
			if (!inserted) {
				throw std::invalid_argument(std::string(context) + " duplicate property column: " + column.name);
			}
		}
	}

	std::vector<graph::storage::BulkPropertyColumn>
	toInternalPropertyColumns(const std::vector<PropertyColumn> &columns) {
		std::vector<graph::storage::BulkPropertyColumn> internalColumns;
		internalColumns.reserve(columns.size());
		for (const auto &column: columns) {
			graph::storage::BulkPropertyColumn internalColumn;
			internalColumn.key = column.name;
			internalColumn.values.reserve(column.values.size());
			for (const auto &value: column.values) {
				internalColumn.values.push_back(toInternal(value));
			}
			internalColumns.push_back(std::move(internalColumn));
		}
		return internalColumns;
	}

	bool buildHomogeneousPropertyColumns(
			const std::vector<std::unordered_map<std::string, Value>> &rows,
			std::vector<graph::storage::BulkPropertyColumn> &columns) {
		columns.clear();
		if (rows.empty()) {
			return true;
		}

		std::vector<std::string> keys;
		keys.reserve(rows.front().size());
		for (const auto &entry: rows.front()) {
			const auto &key = entry.first;
			if (key.empty()) {
				return false;
			}
			keys.push_back(key);
		}
		std::sort(keys.begin(), keys.end());

		columns.reserve(keys.size());
		for (const auto &key: keys) {
			graph::storage::BulkPropertyColumn column;
			column.key = key;
			column.values.reserve(rows.size());
			columns.push_back(std::move(column));
		}

		for (const auto &row: rows) {
			if (row.size() != keys.size()) {
				columns.clear();
				return false;
			}
			for (size_t index = 0; index < keys.size(); ++index) {
				auto valueIt = row.find(keys[index]);
				if (valueIt == row.end()) {
					columns.clear();
					return false;
				}
				columns[index].values.push_back(toInternal(valueIt->second));
			}
		}
		return true;
	}

	bool buildHomogeneousEdgePropertyColumns(
			const std::vector<std::tuple<int64_t, int64_t, std::unordered_map<std::string, Value>>> &rows,
			std::vector<graph::storage::BulkPropertyColumn> &columns) {
		columns.clear();
		if (rows.empty()) {
			return true;
		}

		const auto &firstProps = std::get<2>(rows.front());
		std::vector<std::string> keys;
		keys.reserve(firstProps.size());
		for (const auto &entry: firstProps) {
			const auto &key = entry.first;
			if (key.empty()) {
				return false;
			}
			keys.push_back(key);
		}
		std::sort(keys.begin(), keys.end());

		columns.reserve(keys.size());
		for (const auto &key: keys) {
			graph::storage::BulkPropertyColumn column;
			column.key = key;
			column.values.reserve(rows.size());
			columns.push_back(std::move(column));
		}

		for (const auto &row: rows) {
			const auto &props = std::get<2>(row);
			if (props.size() != keys.size()) {
				columns.clear();
				return false;
			}
			for (size_t index = 0; index < keys.size(); ++index) {
				auto valueIt = props.find(keys[index]);
				if (valueIt == props.end()) {
					columns.clear();
					return false;
				}
				columns[index].values.push_back(toInternal(valueIt->second));
			}
		}
		return true;
	}

	// Convert public Value params to internal PropertyValue params
	graph::query::QueryContext toQueryContext(const std::unordered_map<std::string, Value> &params) {
		graph::query::QueryContext ctx;
		for (const auto &[key, val]: params) {
			ctx.parameters.emplace(key, toInternal(val));
		}
		return ctx;
	}

	// ========================================================================
	// 2. Result Implementation
	// ========================================================================

	class ResultImpl {
	public:
		explicit ResultImpl(graph::query::QueryResult internalRes, std::shared_ptr<graph::storage::DataManager> dm) :
			result_(std::move(internalRes)), dataManager_(std::move(dm)) {
			prepareMetadata();
		}

		graph::query::QueryResult result_;
		std::shared_ptr<graph::storage::DataManager> dataManager_;

		// Cursor State
		size_t cursor_ = 0;
		bool started_ = false;

		std::vector<std::string> columnNames_;
		int mode_ = 0; // 0=Rows (Default), 1=NodeStream, 2=EdgeStream
		std::string error_msg;

		void prepareMetadata() {
			// Priority: Explicit columns from Engine -> Implicit columns from Data
			const auto &executorCols = result_.getColumns();
			if (!executorCols.empty()) {
				columnNames_ = executorCols;
			} else if (!result_.getRows().empty()) {
				// Implicit columns: Extract from the first row and SORT for determinism
				for (const auto &firstRow = result_.getRows()[0]; const auto &k: firstRow | std::views::keys)
					columnNames_.push_back(k);
				std::ranges::sort(columnNames_);
			}

			// Determine mode based on content types of the first row
			// This preserves legacy "stream" behavior for single-entity queries
			if (columnNames_.size() == 1 && !result_.getRows().empty()) {
				const auto &firstVal = result_.getRows()[0].at(columnNames_[0]);
				if (firstVal.isNode()) {
					mode_ = 1; // Node Stream
				} else if (firstVal.isEdge()) {
					mode_ = 2; // Edge Stream
				}
			} else {
				mode_ = 0; // Standard Table Row
			}
		}

		double getDuration() const { return result_.getDuration(); }
	};

	Result::~Result() = default;
	Result::Result() = default;
	Result::Result(Result &&) noexcept = default;
	Result &Result::operator=(Result &&) noexcept = default;

	bool Result::hasNext() const {
		if (!impl_)
			return false;
		size_t total = impl_->result_.rowCount(); // Unified: everything is in rows now

		// If we haven't started, check if there is at least 1 item
		if (!impl_->started_)
			return total > 0;

		// If we have started, check if there is a next item
		return impl_->cursor_ + 1 < total;
	}

	void Result::next() const {
		if (impl_) {
			if (!impl_->started_) {
				impl_->started_ = true;
				impl_->cursor_ = 0;
			} else {
				impl_->cursor_++;
			}
		}
	}

	Value Result::get(const std::string &key) const {
		if (!impl_ || !impl_->started_)
			return std::monostate{};

		const auto &rows = impl_->result_.getRows();
		if (impl_->cursor_ >= rows.size())
			return std::monostate{};

		const auto &row = rows[impl_->cursor_];

		// --- MODE 0: ROWS (Standard) ---
		if (impl_->mode_ == 0) {
			// 1. Exact Match
			auto it = row.find(key);
			if (it != row.end())
				return toPublicValue(it->second, impl_->dataManager_);

			// 2. Fuzzy Match (User asks "name", DB has "n.name")
			for (const auto &[dbKey, dbVal]: row) {
				if (key.find('.') == std::string::npos && dbKey.size() > key.size()) {
					if (dbKey.ends_with("." + key))
						return toPublicValue(dbVal, impl_->dataManager_);
				}
				if (dbKey.find('.') == std::string::npos && key.size() > dbKey.size()) {
					if (key.ends_with("." + dbKey))
						return toPublicValue(dbVal, impl_->dataManager_);
				}
			}
			return std::monostate{};
		}

		// --- MODE 1 & 2: ENTITY STREAM (Legacy Compatibility) ---
		// In these modes, we assume there is exactly 1 column containing the entity.
		else if (impl_->mode_ == 1 || impl_->mode_ == 2) {
			const std::string &colName = impl_->columnNames_[0];
			const auto &entityVal = row.at(colName); // Safe because mode implies !empty

			// If requesting the entity itself (key empty or standard aliases)
			if (key.empty() || key == colName) {
				return toPublicValue(entityVal, impl_->dataManager_);
			}

			// If requesting a property inside the entity
			if (entityVal.isNode()) {
				const auto &props = entityVal.asNode().getProperties();
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			} else if (entityVal.isEdge()) {
				const auto &props = entityVal.asEdge().getProperties();
				auto it = props.find(key);
				if (it != props.end())
					return toPublicValue(it->second);
			}
		}

		return std::monostate{};
	}

	Value Result::get(int index) const {
		if (!impl_ || !impl_->started_ || index < 0 || index >= static_cast<int>(impl_->columnNames_.size()))
			return std::monostate{};

		// In the unified Row model, index maps directly to the sorted column name list
		return get(impl_->columnNames_[index]);
	}

	namespace detail {
		Value getTypedResultValue(const Result &result, int index) {
			if (!result.impl_ || !result.impl_->started_ || index < 0 ||
				index >= static_cast<int>(result.impl_->columnNames_.size())) {
				return std::monostate{};
			}

			const auto &rows = result.impl_->result_.getRows();
			if (result.impl_->cursor_ >= rows.size()) {
				return std::monostate{};
			}

			const auto &row = rows[result.impl_->cursor_];
			const auto &columnName = result.impl_->columnNames_[index];
			auto it = row.find(columnName);
			if (it == row.end()) {
				return std::monostate{};
			}
			return toDriverAbiValue(it->second, result.impl_->dataManager_);
		}
	} // namespace detail

	int Result::getColumnCount() const { return impl_ ? static_cast<int>(impl_->columnNames_.size()) : 0; }

	std::string Result::getColumnName(int index) const {
		return (impl_ && index >= 0 && index < static_cast<int>(impl_->columnNames_.size()))
					   ? impl_->columnNames_[index]
					   : "";
	}

	double Result::getDuration() const { return impl_ ? impl_->getDuration() : 0.0; }

	bool Result::isSuccess() const { return impl_ && impl_->error_msg.empty(); }

	std::string Result::getError() const {
		if (!impl_)
			return "Result not initialized";
		return impl_->error_msg;
	}

	// ========================================================================
	// 3. Database Implementation
	// ========================================================================

	class DatabaseImpl {
	public:
		explicit DatabaseImpl(const std::string &path) : db_(path) {}
		graph::Database db_;
		std::optional<graph::Transaction> cypherTxn_;

		[[nodiscard]] bool needsImplicitTransaction() const {
			return !db_.hasActiveTransaction() && !cypherTxn_.has_value();
		}

		void ensureOpen() {
			if (!db_.isOpen()) {
				db_.open();
			}
		}
	};

	// ========================================================================
	// 4. Transaction Implementation
	// ========================================================================

	class TransactionImpl {
	public:
		explicit TransactionImpl(graph::Transaction internalTxn, DatabaseImpl *dbImpl) :
			txn_(std::move(internalTxn)), dbImpl_(dbImpl) {}

		graph::Transaction txn_;
		DatabaseImpl *dbImpl_; // Non-owning reference to parent database
	};

	Transaction::Transaction() = default;
	Transaction::~Transaction() = default;
	Transaction::Transaction(Transaction &&) noexcept = default;
	Transaction &Transaction::operator=(Transaction &&) noexcept = default;

	Result Transaction::execute(const std::string &cypher) const { return execute(cypher, {}); }

	Result Transaction::execute(const std::string &cypher, const std::unordered_map<std::string, Value> &params) const {
		if (!impl_ || !impl_->txn_.isActive()) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult(), nullptr);
			res.impl_->error_msg = "Transaction is not active";
			return res;
		}

		try {
			auto start = std::chrono::high_resolution_clock::now();

			auto ctx = toQueryContext(params);
			// Propagate read-only mode from internal transaction
			if (impl_->txn_.isReadOnly()) {
				ctx.execMode = graph::query::ExecMode::EM_READ_ONLY;
			}
			auto internalRes = impl_->dbImpl_->db_.getQueryEngine()->execute(cypher, ctx);

			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> elapsed = end - start;
			internalRes.setDuration(elapsed.count());

			auto dm = impl_->dbImpl_->db_.getStorage()->getDataManager();

			Result res;
			res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes), std::move(dm));
			return res;
		} catch (const std::exception &e) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult(), nullptr);
			res.impl_->error_msg = e.what();
			return res;
		} catch (...) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult(), nullptr);
			res.impl_->error_msg = "Unknown error";
			return res;
		}
	}

	void Transaction::commit() {
		if (!impl_)
			throw std::runtime_error("Transaction not initialized");
		impl_->txn_.commit();
	}

	void Transaction::rollback() {
		if (!impl_)
			throw std::runtime_error("Transaction not initialized");
		impl_->txn_.rollback();
	}

	bool Transaction::isActive() const { return impl_ && impl_->txn_.isActive(); }

	bool Transaction::isReadOnly() const { return impl_ && impl_->txn_.isReadOnly(); }

	// ========================================================================
	// 5. Database API Methods
	// ========================================================================

	Database::Database(const std::string &path) : impl_(std::make_unique<DatabaseImpl>(path)) {}
	Database::~Database() = default;

	void Database::open() const { impl_->db_.open(); }
	bool Database::openIfExists() const { return impl_->db_.openIfExists(); }
	void Database::close() const { impl_->db_.close(); }
	void Database::save() const { impl_->db_.flush(); }

	Transaction Database::beginTransaction() {
		if (!impl_->db_.isOpen()) {
			impl_->db_.open();
		}

		auto internalTxn = impl_->db_.beginTransaction();

		Transaction txn;
		txn.impl_ = std::make_unique<TransactionImpl>(std::move(internalTxn), impl_.get());
		return txn;
	}

	Transaction Database::beginBulkTransaction() {
		if (!impl_->db_.isOpen()) {
			impl_->db_.open();
		}

		auto internalTxn = impl_->db_.beginBulkTransaction();

		Transaction txn;
		txn.impl_ = std::make_unique<TransactionImpl>(std::move(internalTxn), impl_.get());
		return txn;
	}

	Transaction Database::beginReadOnlyTransaction() {
		if (!impl_->db_.isOpen()) {
			impl_->db_.open();
		}

		auto internalTxn = impl_->db_.beginReadOnlyTransaction();

		Transaction txn;
		txn.impl_ = std::make_unique<TransactionImpl>(std::move(internalTxn), impl_.get());
		return txn;
	}

	bool Database::hasActiveTransaction() const {
		return impl_->db_.hasActiveTransaction() || impl_->cypherTxn_.has_value();
	}

	void Database::setThreadPoolSize(size_t poolSize) const { impl_->db_.setThreadPoolSize(poolSize); }

	Result Database::execute(const std::string &cypher) const { return execute(cypher, {}); }

	Result Database::execute(const std::string &cypher, const std::unordered_map<std::string, Value> &params) const {
		try {
			{
				graph::debug::ScopedPerfTimer timer("api.ensure_open");
				impl_->ensureOpen();
			}

			// Intercept transaction control statements (BEGIN/COMMIT/ROLLBACK)
			auto txnKind = CypherTxnKind::NONE;
			{
				graph::debug::ScopedPerfTimer timer("api.detect_txn_statement");
				txnKind = detectCypherTxnStatement(cypher);
			}
			if (txnKind != CypherTxnKind::NONE) {
				graph::query::QueryResult txnResult;
				auto start = std::chrono::high_resolution_clock::now();

				switch (txnKind) {
					case CypherTxnKind::TXN_BEGIN:
						if (impl_->cypherTxn_.has_value()) {
							throw std::runtime_error(
									"Nested transactions are not supported: transaction already active");
						}
						impl_->cypherTxn_.emplace(impl_->db_.beginTransaction());
						txnResult.addRow(
								{{"result", graph::query::ResultValue(graph::PropertyValue("Transaction started"))}});
						break;
					case CypherTxnKind::TXN_COMMIT:
						if (!impl_->cypherTxn_.has_value()) {
							throw std::runtime_error("No active transaction to commit");
						}
						impl_->cypherTxn_->commit();
						impl_->cypherTxn_.reset();
						txnResult.addRow(
								{{"result", graph::query::ResultValue(graph::PropertyValue("Transaction committed"))}});
						break;
					case CypherTxnKind::TXN_ROLLBACK:
						if (!impl_->cypherTxn_.has_value()) {
							throw std::runtime_error("No active transaction to rollback");
						}
						impl_->cypherTxn_.reset(); // destructor auto-rolls back
						txnResult.addRow({{"result", graph::query::ResultValue(
															 graph::PropertyValue("Transaction rolled back"))}});
						break;
					default: // ZYX_COV_EXCL_LINE: detectCypherTxnStatement returns only handled transaction kinds.
						break;
				}

				auto end = std::chrono::high_resolution_clock::now();
				std::chrono::duration<double, std::milli> elapsed = end - start;
				txnResult.setDuration(elapsed.count());

				Result res;
				res.impl_ = std::make_unique<ResultImpl>(std::move(txnResult), nullptr);
				return res;
			}

			// Build the plan first to determine if the query mutates data
			std::shared_ptr<graph::query::QueryEngine> queryEngine;
			{
				graph::debug::ScopedPerfTimer timer("api.get_query_engine");
				queryEngine = impl_->db_.getQueryEngine();
			}
			auto plan = [&]() {
				graph::debug::ScopedPerfTimer timer("api.build_plan");
				return queryEngine->buildPlan(cypher);
			}();
			bool isReadOnly = !plan.mutatesData && !plan.mutatesSchema;

			// Implicit transaction wrapping:
			// - If explicit/cypher transaction exists, execute within it.
			// - If no explicit transaction: wrap writes in implicit write txn,
			//   wrap reads in implicit read-only txn (for snapshot isolation).
			std::optional<graph::Transaction> implicitTxn;

			{
				graph::debug::ScopedPerfTimer timer("api.begin_implicit_txn");
				if (impl_->needsImplicitTransaction()) {
					if (isReadOnly) {
						implicitTxn.emplace(impl_->db_.beginReadOnlyTransaction());
					} else {
						implicitTxn.emplace(impl_->db_.beginTransaction());
					}
				}
			}

			auto start = std::chrono::high_resolution_clock::now();

			auto ctx = [&]() {
				graph::debug::ScopedPerfTimer timer("api.to_query_context");
				return toQueryContext(params);
			}();
			if (implicitTxn && implicitTxn->isReadOnly()) {
				ctx.execMode = graph::query::ExecMode::EM_READ_ONLY;
			}
			auto internalRes = [&]() {
				graph::debug::ScopedPerfTimer timer("api.execute_plan");
				return queryEngine->executePlan(std::move(plan), ctx);
			}();

			auto end = std::chrono::high_resolution_clock::now();
			std::chrono::duration<double, std::milli> elapsed = end - start;

			internalRes.setDuration(elapsed.count());

			// Slow query logging
			if (auto cm = impl_->db_.getConfigManager(); cm && cm->isSlowLogEnabled()) {
				if (elapsed.count() > static_cast<double>(cm->getSlowLogThresholdMs())) {
					graph::log::Log::warn("Slow query ({}ms): {}", static_cast<int64_t>(elapsed.count()), cypher);
				}
			}

			{
				graph::debug::ScopedPerfTimer timer("api.commit_implicit_txn");
				if (implicitTxn) {
					implicitTxn->commit();
				}
			}

			// We MUST inject DataManager into ResultImpl to allow label resolution later
			auto dm = impl_->db_.getStorage()->getDataManager();

			graph::debug::ScopedPerfTimer resultTimer("api.result_wrap");
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(std::move(internalRes), std::move(dm));
			return res;
		} catch (const std::exception &e) {
			// Implicit transaction auto-rolls back via destructor
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult(), nullptr);
			res.impl_->error_msg = e.what();
			return res;
		} catch (...) {
			Result res;
			res.impl_ = std::make_unique<ResultImpl>(graph::query::QueryResult(), nullptr);
			res.impl_->error_msg = "Unknown error";
			return res;
		}
	}

	int64_t Database::createNode(const std::string &label, const std::unordered_map<std::string, Value> &props) const {
		impl_->ensureOpen();
		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginTransaction());
		}

		auto dm = impl_->db_.getStorage()->getDataManager();

		int64_t labelId = dm->getOrCreateTokenId(label);
		graph::Node node(0, labelId);
		dm->addNode(node);
		int64_t newId = node.getId();

		if (!props.empty()) {
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			internalProps.reserve(props.size());
			for (const auto &[k, v]: props)
				internalProps.emplace(k, toInternal(v));
			dm->addNodeProperties(newId, internalProps);
		}

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return newId;
	}

	int64_t Database::createNode(const std::vector<std::string> &labels,
								 const std::unordered_map<std::string, Value> &props) const {
		impl_->ensureOpen();
		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginTransaction());
		}

		auto dm = impl_->db_.getStorage()->getDataManager();

		std::vector<int64_t> labelIds;
		for (const auto &lbl: labels) {
			labelIds.push_back(dm->getOrCreateTokenId(lbl));
		}

		int64_t firstLabelId = labelIds.empty() ? 0 : labelIds[0];
		graph::Node node(0, firstLabelId);
		for (size_t i = 1; i < labelIds.size(); ++i) {
			node.addLabelId(labelIds[i]);
		}
		dm->addNode(node);
		int64_t newId = node.getId();

		if (!props.empty()) {
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			internalProps.reserve(props.size());
			for (const auto &[k, v]: props)
				internalProps[k] = toInternal(v);
			dm->addNodeProperties(newId, internalProps);
		}

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return newId;
	}

	std::vector<int64_t>
	Database::createNodes(const std::string &label,
						  const std::vector<std::unordered_map<std::string, Value>> &propsList) const {
		if (propsList.empty())
			return {};

		impl_->ensureOpen();
		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginTransaction());
		}

		auto storage = impl_->db_.getStorage();
		auto dm = storage->getDataManager();

		int64_t labelId = dm->getOrCreateTokenId(label);

		std::vector<graph::storage::BulkPropertyColumn> columnarProperties;
		if (buildHomogeneousPropertyColumns(propsList, columnarProperties)) {
			auto ids = dm->addNodesColumnar(labelId, propsList.size(), columnarProperties);
			if (implicitTxn) {
				implicitTxn->commit();
			}
			return ids;
		}

		std::vector<graph::Node> nodes;
		nodes.reserve(propsList.size());

		for (const auto &props: propsList) {
			graph::Node n(0, labelId);
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			internalProps.reserve(props.size());
			for (const auto &[k, v]: props)
				internalProps.emplace(k, toInternal(v));
			n.setProperties(std::move(internalProps));
			nodes.push_back(std::move(n));
		}

		dm->addNodes(nodes);

		std::vector<int64_t> ids;
		ids.reserve(nodes.size());
		for (const auto &n: nodes) {
			ids.push_back(n.getId());
		}

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return ids;
	}

	std::vector<int64_t>
	Database::createNodesColumnar(const std::string &label,
								  size_t count,
								  const std::vector<PropertyColumn> &columns) const {
		validatePropertyColumns(columns, count, "createNodesColumnar");
		if (count == 0) {
			return {};
		}

		impl_->ensureOpen();
		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginTransaction());
		}

		auto storage = impl_->db_.getStorage();
		auto dm = storage->getDataManager();

		const int64_t labelId = dm->getOrCreateTokenId(label);
		auto ids = dm->addNodesColumnar(labelId, count, toInternalPropertyColumns(columns));

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return ids;
	}

	int64_t Database::createEdge(int64_t sourceId, int64_t targetId, const std::string &edgeType,
								 const std::unordered_map<std::string, Value> &props) const {
		impl_->ensureOpen();
		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginTransaction());
		}

		auto dm = impl_->db_.getStorage()->getDataManager();

		int64_t typeId = dm->getOrCreateTokenId(edgeType);
		graph::Edge edge(0, sourceId, targetId, typeId);
		dm->addEdge(edge);
		int64_t edgeId = edge.getId();

		if (!props.empty()) {
			std::unordered_map<std::string, graph::PropertyValue> internalProps;
			internalProps.reserve(props.size());
			for (const auto &[k, v]: props)
				internalProps.emplace(k, toInternal(v));
			dm->addEdgeProperties(edgeId, internalProps);
		}

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return edgeId;
	}

	std::vector<int64_t> Database::createEdges(
			const std::string &edgeType,
			const std::vector<std::tuple<int64_t, int64_t, std::unordered_map<std::string, Value>>> &edgesList) const {
		if (edgesList.empty())
			return {};

		impl_->ensureOpen();
		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginTransaction());
		}

		auto dm = impl_->db_.getStorage()->getDataManager();
		int64_t typeId = dm->getOrCreateTokenId(edgeType);

		std::vector<graph::storage::BulkPropertyColumn> columnarProperties;
		if (buildHomogeneousEdgePropertyColumns(edgesList, columnarProperties)) {
			std::vector<int64_t> sourceIds;
			std::vector<int64_t> targetIds;
			sourceIds.reserve(edgesList.size());
			targetIds.reserve(edgesList.size());
			for (const auto &edgeRow: edgesList) {
				sourceIds.push_back(std::get<0>(edgeRow));
				targetIds.push_back(std::get<1>(edgeRow));
			}

			auto ids = dm->addEdgesColumnar(typeId, sourceIds, targetIds, columnarProperties);
			if (implicitTxn) {
				implicitTxn->commit();
			}
			return ids;
		}

		std::vector<graph::Edge> edges;
		edges.reserve(edgesList.size());

		for (const auto &[srcId, dstId, props]: edgesList) {
			graph::Edge e(0, srcId, dstId, typeId);
			if (!props.empty()) {
				std::unordered_map<std::string, graph::PropertyValue> internalProps;
				internalProps.reserve(props.size());
				for (const auto &[k, v]: props)
					internalProps.emplace(k, toInternal(v));
				e.setProperties(std::move(internalProps));
			}
			edges.push_back(std::move(e));
		}

		dm->addEdges(edges);

		std::vector<int64_t> ids;
		ids.reserve(edges.size());
		for (const auto &e: edges) {
			ids.push_back(e.getId());
		}

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return ids;
	}

	std::vector<int64_t> Database::createEdgesColumnar(
			const std::string &edgeType,
			const std::vector<int64_t> &sourceIds,
			const std::vector<int64_t> &targetIds,
			const std::vector<PropertyColumn> &columns) const {
		if (sourceIds.size() != targetIds.size()) {
			throw std::invalid_argument("createEdgesColumnar requires matching source and target counts");
		}

		const size_t count = sourceIds.size();
		validatePropertyColumns(columns, count, "createEdgesColumnar");
		if (count == 0) {
			return {};
		}
		const auto internalColumns = toInternalPropertyColumns(columns);
		return detail::DatabaseBulkInternal::createEdgesColumnar(*this, edgeType, sourceIds, targetIds,
																 internalColumns);
	}

	std::vector<int64_t> detail::DatabaseBulkInternal::createEdgesColumnar(
			const Database &database, const std::string &edgeType, const std::vector<int64_t> &sourceIds,
			const std::vector<int64_t> &targetIds, const std::vector<graph::storage::BulkPropertyColumn> &columns) {
		if (sourceIds.size() != targetIds.size()) {
			throw std::invalid_argument("createEdgesColumnar requires matching source and target counts");
		}
		if (sourceIds.empty()) {
			return {};
		}

		database.impl_->ensureOpen();
		std::optional<graph::Transaction> implicitTxn;
		if (database.impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(database.impl_->db_.beginTransaction());
		}

		auto dm = database.impl_->db_.getStorage()->getDataManager();
		const int64_t typeId = dm->getOrCreateTokenId(edgeType);
		auto ids = dm->addEdgesColumnar(typeId, sourceIds, targetIds, columns);

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return ids;
	}

	bool Database::createNodePropertyIndexes(const std::string &label,
											 const std::vector<std::string> &properties) const {
		impl_->ensureOpen();
		if (label.empty()) {
			throw std::invalid_argument("createNodePropertyIndexes requires a non-empty label");
		}
		if (properties.empty()) {
			return true;
		}
		if (impl_->db_.hasActiveTransaction() || impl_->cypherTxn_.has_value()) {
			throw std::runtime_error("Schema operations cannot run inside an active transaction");
		}

		std::vector<graph::query::indexes::IndexManager::IndexCreateRequest> requests;
		requests.reserve(properties.size());
		for (const auto &property: properties) {
			if (property.empty()) {
				throw std::invalid_argument("createNodePropertyIndexes requires non-empty property names");
			}
			requests.push_back({"", "node", label, property});
		}

		auto queryEngine = impl_->db_.getQueryEngine();
		if (!queryEngine) {
			throw std::runtime_error("Query engine is not available");
		}
		if (auto storage = impl_->db_.getStorage()) {
			if (auto dm = storage->getDataManager(); dm && dm->hasUnsavedChanges()) {
				impl_->db_.flush();
			}
		}
		auto schemaTxn = impl_->db_.beginBulkTransaction();
		auto results = queryEngine->getIndexManager()->createIndexes(requests);
		const bool success = std::all_of(results.begin(), results.end(),
										 [](const auto &result) { return result.success; });
		if (success) {
			queryEngine->getPlanCache().clear();
			schemaTxn.commit();
		} else {
			schemaTxn.rollback();
		}
		return success;
	}

	std::vector<Node> Database::getShortestPath(int64_t startId, int64_t endId, int maxDepth) const {
		impl_->ensureOpen();

		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginReadOnlyTransaction());
		}

		auto storage = impl_->db_.getStorage();
		if (!storage) // ZYX_COV_EXCL_LINE: ensureOpen initializes storage before graph algorithms run.
			throw std::runtime_error("Storage not available");
		auto dm = storage->getDataManager();

		graph::query::algorithm::GraphAlgorithm algo(dm);
		auto internalPath = algo.findShortestPath(startId, endId, "out", maxDepth);

		std::vector<Node> publicPath;
		publicPath.reserve(internalPath.size());
		for (const auto &n: internalPath) {
			graph::Node fullNode = dm->getNode(n.getId());
			fullNode.setProperties(dm->getNodeProperties(n.getId()));
			publicPath.push_back(toPublicNode(fullNode, dm));
		}

		if (implicitTxn) {
			implicitTxn->commit();
		}
		return publicPath;
	}

	void Database::bfs(int64_t startNodeId, const std::function<bool(const Node &)> &visitor) const {
		impl_->ensureOpen();

		std::optional<graph::Transaction> implicitTxn;
		if (impl_->needsImplicitTransaction()) {
			implicitTxn.emplace(impl_->db_.beginReadOnlyTransaction());
		}

		auto storage = impl_->db_.getStorage();
		if (!storage) // ZYX_COV_EXCL_LINE: ensureOpen initializes storage before graph algorithms run.
			throw std::runtime_error("Storage not available");
		auto dm = storage->getDataManager();

		graph::query::algorithm::GraphAlgorithm algo(dm);
		auto internalVisitor = [&](int64_t nodeId, [[maybe_unused]] int depth) -> bool {
			graph::Node internalNode = dm->getNode(nodeId);
			internalNode.setProperties(dm->getNodeProperties(nodeId));
			return visitor(toPublicNode(internalNode, dm));
		};
		algo.breadthFirstTraversal(startNodeId, internalVisitor, "out");

		if (implicitTxn) {
			implicitTxn->commit();
		}
	}

} // namespace zyx
