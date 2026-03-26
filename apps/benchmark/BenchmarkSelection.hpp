/**
 * @file BenchmarkSelection.hpp
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

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace zyx::benchmark {

	enum class BenchmarkType { INSERT, QUERY, ALGO, VECTOR, CONCURRENCY, STORAGE, TRANSACTION };

	struct BenchmarkTypeInfo {
		BenchmarkType type;
		std::string_view token;
		std::string_view description;
	};

	struct BenchmarkSelector {
		std::set<BenchmarkType> selectedTypes;
		std::set<std::string> selectedIds;
	};

	inline constexpr std::array<BenchmarkTypeInfo, 7> BENCHMARK_TYPE_INFOS{{
			{BenchmarkType::INSERT, "insert", "Insert and bulk-write workloads"},
			{BenchmarkType::QUERY, "query", "Read/query workloads (index vs scan)"},
			{BenchmarkType::ALGO, "algo", "Graph algorithm workloads"},
			{BenchmarkType::VECTOR, "vector", "Vector insert/search workloads"},
			{BenchmarkType::CONCURRENCY, "concurrency", "Parallel execution workloads"},
			{BenchmarkType::STORAGE, "storage", "Storage footprint profiling workloads"},
			{BenchmarkType::TRANSACTION, "transaction", "Implicit vs explicit transaction workloads"},
	}};

	inline std::string toLowerCopy(std::string text) {
		std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	inline std::string trimCopy(std::string text) {
		const auto begin = std::find_if_not(text.begin(), text.end(), [](const unsigned char c) { return std::isspace(c); });
		const auto end = std::find_if_not(text.rbegin(), text.rend(), [](const unsigned char c) { return std::isspace(c); }).base();
		if (begin >= end) {
			return {};
		}
		return std::string(begin, end);
	}

	inline std::string normalizeToken(std::string token) { return toLowerCopy(trimCopy(std::move(token))); }

	inline std::optional<BenchmarkType> parseBenchmarkType(const std::string &token) {
		const std::string normalized = normalizeToken(token);
		if (normalized == "insert" || normalized == "write" || normalized == "writes") {
			return BenchmarkType::INSERT;
		}
		if (normalized == "query" || normalized == "read" || normalized == "reads") {
			return BenchmarkType::QUERY;
		}
		if (normalized == "algo" || normalized == "algorithm" || normalized == "algorithms") {
			return BenchmarkType::ALGO;
		}
		if (normalized == "vector" || normalized == "vec") {
			return BenchmarkType::VECTOR;
		}
		if (normalized == "concurrency" || normalized == "conc" || normalized == "parallel") {
			return BenchmarkType::CONCURRENCY;
		}
		if (normalized == "storage" || normalized == "footprint") {
			return BenchmarkType::STORAGE;
		}
		if (normalized == "transaction" || normalized == "txn") {
			return BenchmarkType::TRANSACTION;
		}
		return std::nullopt;
	}

	inline std::string benchmarkTypeToken(const BenchmarkType type) {
		for (const auto &info: BENCHMARK_TYPE_INFOS) {
			if (info.type == type) {
				return std::string(info.token);
			}
		}
		return "unknown";
	}

	inline bool matchesSelection(const BenchmarkSelector &selector, const BenchmarkType type, const std::string_view id,
								 const std::string_view displayName) {
		const std::string normalizedId = normalizeToken(std::string(id));
		(void) displayName;
		if (!selector.selectedTypes.empty() && !selector.selectedTypes.contains(type)) {
			return false;
		}
		if (!selector.selectedIds.empty() && !selector.selectedIds.contains(normalizedId)) {
			return false;
		}
		return true;
	}

} // namespace zyx::benchmark
