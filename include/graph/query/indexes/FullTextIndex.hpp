/**
 * @file FullTextIndex.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include <shared_mutex>

namespace graph::query::indexes {

	class FullTextIndex {
	public:
		FullTextIndex();

		// Add a text property to the index
		void addTextProperty(uint64_t nodeId, const std::string& key, const std::string& text);

		// Remove a text property from the index
		void removeTextProperty(uint64_t nodeId, const std::string& key);

		// Find nodes with text properties containing the search text
		[[nodiscard]] std::vector<int64_t> search(const std::string& key, const std::string& searchText) const;

		// Clear the index
		void clear();

	private:
		// Tokenize text into words
		static std::vector<std::string> tokenize(const std::string& text);

		// key -> token -> node IDs
		std::unordered_map<std::string, std::unordered_map<std::string, std::vector<int64_t>>> invertedIndex_;

		// nodeId -> key -> set of tokens
		std::unordered_map<uint64_t, std::unordered_map<std::string, std::set<std::string>>> nodeTokens_;

		// Thread safety
		mutable std::shared_mutex mutex_;
	};

} // namespace graph::query::indexes