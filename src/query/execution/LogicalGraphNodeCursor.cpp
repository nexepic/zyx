/**
 * @file LogicalGraphNodeCursor.cpp
 * @brief Snapshot-pinned pull cursor over bounded logical Node pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/query/execution/LogicalGraphNodeCursor.hpp"

#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace graph::query::execution {

	namespace {

		void validateOptions(const LogicalGraphNodeCursorOptions &options) {
			if (options.page_records == 0U || options.maximum_metadata_pages_per_page == 0U) {
				throw std::invalid_argument("logical graph Node cursor requires positive page and metadata bounds");
			}
		}

		void validateNode(const storage::LogicalGraphNode &node, uint64_t afterNodeId) {
			if (node.node_id == 0U || node.node_id <= afterNodeId) {
				throw std::runtime_error("logical graph Node cursor received a non-monotonic Node ID");
			}
			std::unordered_set<std::string_view> labels;
			labels.reserve(node.labels.size());
			for (const std::string &label: node.labels) {
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical graph Node cursor received an invalid Node label set");
				}
			}
			std::unordered_set<std::string_view> propertyKeys;
			propertyKeys.reserve(node.properties.size());
			for (const storage::LogicalGraphPropertyEntry &property: node.properties) {
				if (property.key.empty() || !propertyKeys.insert(property.key).second) {
					throw std::runtime_error("logical graph Node cursor received an invalid owner-keyed Property set");
				}
			}
		}

	} // namespace

	LogicalGraphNodeCursor::LogicalGraphNodeCursor(const storage::LogicalGraphReadBackend &backend,
																					 LogicalGraphNodeCursorOptions options) :
		backend_(backend), options_(options) {
		validateOptions(options_);
	}

	void LogicalGraphNodeCursor::open() {
		if (isOpen()) {
			throw std::logic_error("logical graph Node cursor is already open");
		}
		snapshot_ = backend_.openLogicalGraphReadSnapshot();
		if (!snapshot_) {
			throw std::runtime_error("logical graph backend returned no read snapshot");
		}
		after_node_id_ = 0U;
		exhausted_ = false;
	}

	std::optional<storage::LogicalGraphNodePage> LogicalGraphNodeCursor::next() {
		if (!isOpen()) {
			throw std::logic_error("logical graph Node cursor is not open");
		}
		if (exhausted_) {
			return std::nullopt;
		}
		storage::LogicalGraphNodePage page =
				snapshot_->scanNodesAfter(after_node_id_, options_.page_records, options_.maximum_metadata_pages_per_page);
		if (page.nodes.size() > options_.page_records) {
			throw std::runtime_error("logical graph backend exceeded the Node cursor page bound");
		}
		uint64_t previousNodeId = after_node_id_;
		for (const storage::LogicalGraphNode &node: page.nodes) {
			validateNode(node, previousNodeId);
			previousNodeId = node.node_id;
		}
		if (page.nodes.empty()) {
			if (page.may_have_more) {
				throw std::runtime_error("logical graph backend returned an empty nonterminal Node page");
			}
			exhausted_ = true;
			return std::nullopt;
		}
		after_node_id_ = previousNodeId;
		if (!page.may_have_more) {
			exhausted_ = true;
		}
		return page;
	}

	void LogicalGraphNodeCursor::close() noexcept {
		snapshot_.reset();
		after_node_id_ = 0U;
		exhausted_ = false;
	}

} // namespace graph::query::execution
