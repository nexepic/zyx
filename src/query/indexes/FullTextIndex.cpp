/**
 * @file FullTextIndex.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/indexes/FullTextIndex.hpp"
#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>

namespace graph::query::indexes {

	FullTextIndex::FullTextIndex() = default;

	void FullTextIndex::addTextProperty(uint64_t nodeId, const std::string &key, const std::string &text) {
		std::vector<std::string> tokens = tokenize(text);

		std::unique_lock lock(mutex_);

		// Remove old tokens if they exist
		auto nodeIt = nodeTokens_.find(nodeId);
		if (nodeIt != nodeTokens_.end()) {
			auto keyIt = nodeIt->second.find(key);
			if (keyIt != nodeIt->second.end()) {
				// Remove old tokens from inverted index
				for (const auto &token: keyIt->second) {
					auto &nodeIds = invertedIndex_[key][token];
					nodeIds.erase(std::remove(nodeIds.begin(), nodeIds.end(), nodeId), nodeIds.end());

					// Clean up empty entries
					if (nodeIds.empty()) {
						invertedIndex_[key].erase(token);
						if (invertedIndex_[key].empty()) {
							invertedIndex_.erase(key);
						}
					}
				}

				// Clear old tokens
				keyIt->second.clear();
			}
		}

		// Add new tokens
		std::set<std::string> uniqueTokens(tokens.begin(), tokens.end());
		for (const auto &token: uniqueTokens) {
			invertedIndex_[key][token].push_back(nodeId);
		}

		// Update node tokens mapping
		nodeTokens_[nodeId][key] = uniqueTokens;
	}

	void FullTextIndex::removeTextProperty(uint64_t nodeId, const std::string &key) {
		std::unique_lock lock(mutex_);

		auto nodeIt = nodeTokens_.find(nodeId);
		if (nodeIt == nodeTokens_.end()) {
			return;
		}

		auto keyIt = nodeIt->second.find(key);
		if (keyIt == nodeIt->second.end()) {
			return;
		}

		// Remove tokens from inverted index
		for (const auto &token: keyIt->second) {
			auto &nodeIds = invertedIndex_[key][token];
			nodeIds.erase(std::remove(nodeIds.begin(), nodeIds.end(), nodeId), nodeIds.end());

			// Clean up empty entries
			if (nodeIds.empty()) {
				invertedIndex_[key].erase(token);
				if (invertedIndex_[key].empty()) {
					invertedIndex_.erase(key);
				}
			}
		}

		// Remove key from node tokens
		nodeIt->second.erase(key);
		if (nodeIt->second.empty()) {
			nodeTokens_.erase(nodeId);
		}
	}

	std::vector<int64_t> FullTextIndex::search(const std::string &key, const std::string &searchText) const {
		std::vector<std::string> searchTokens = tokenize(searchText);
		if (searchTokens.empty()) {
			return {};
		}

		std::shared_lock lock(mutex_);

		// Check if we have anything indexed for this key
		auto keyIt = invertedIndex_.find(key);
		if (keyIt == invertedIndex_.end()) {
			return {};
		}

		// Start with nodes matching the first token
		const auto &firstToken = searchTokens[0];
		auto tokenIt = keyIt->second.find(firstToken);
		if (tokenIt == keyIt->second.end()) {
			return {};
		}

		std::vector<int64_t> result = tokenIt->second;

		// Intersect with nodes matching subsequent tokens
		for (size_t i = 1; i < searchTokens.size(); ++i) {
			const auto &token = searchTokens[i];
			tokenIt = keyIt->second.find(token);
			if (tokenIt == keyIt->second.end()) {
				return {};
			}

			const auto &nodeIds = tokenIt->second;
			std::vector<int64_t> intersection;

			// Find intersection (assuming vectors are unsorted)
			for (int64_t nodeId: result) {
				if (std::find(nodeIds.begin(), nodeIds.end(), nodeId) != nodeIds.end()) {
					intersection.push_back(nodeId);
				}
			}

			result = intersection;
			if (result.empty()) {
				return {};
			}
		}

		return result;
	}

	void FullTextIndex::clear() {
		std::unique_lock lock(mutex_);
		invertedIndex_.clear();
		nodeTokens_.clear();
	}

	std::vector<std::string> FullTextIndex::tokenize(const std::string &text) {
		std::vector<std::string> tokens;
		std::stringstream ss(text);
		std::string token;

		// Simple tokenization by whitespace
		while (ss >> token) {
			// Convert to lowercase and remove punctuation
			std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return std::tolower(c); });

			token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::ispunct(c); }),
						token.end());

			if (!token.empty()) {
				tokens.push_back(token);
			}
		}

		return tokens;
	}

} // namespace graph::query::indexes
