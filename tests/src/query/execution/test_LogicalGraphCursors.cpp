/**
 * @file test_LogicalGraphCursors.cpp
 * @brief Protocol tests for bounded logical graph pull cursors.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/execution/LogicalGraphAdjacencyCursor.hpp"
#include "graph/query/execution/LogicalGraphNodeCursor.hpp"

namespace graph::query::execution {

	namespace {

		struct CursorScript {
			std::vector<storage::LogicalGraphNodePage> node_pages;
			std::vector<storage::LogicalGraphAdjacencyPage> adjacency_pages;
			bool return_null_snapshot = false;
		};

		class ScriptedCursorSnapshot final : public storage::LogicalGraphReadSnapshot {
		public:
			explicit ScriptedCursorSnapshot(CursorScript script) : script_(std::move(script)) {}

			[[nodiscard]] storage::LogicalGraphNode readNode(uint64_t) const override {
				throw std::logic_error("cursor test does not read Nodes directly");
			}

			[[nodiscard]] storage::LogicalGraphEdge readEdge(uint64_t) const override {
				throw std::logic_error("cursor test does not read Edges directly");
			}

			void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {
				throw std::logic_error("cursor test does not use Node visitors");
			}

			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
				if (node_page_index_ == script_.node_pages.size()) {
					return {};
				}
				return script_.node_pages[node_page_index_++];
			}

			[[nodiscard]] storage::LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
							  std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override {
				if (adjacency_page_index_ == script_.adjacency_pages.size()) {
					return {};
				}
				return script_.adjacency_pages[adjacency_page_index_++];
			}

			void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("cursor test does not use Edge visitors");
			}

			void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
										const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("cursor test does not use adjacency visitors");
			}

		private:
			CursorScript script_;
			mutable size_t node_page_index_ = 0U;
			mutable size_t adjacency_page_index_ = 0U;
		};

		class ScriptedCursorBackend final : public storage::LogicalGraphReadBackend {
		public:
			explicit ScriptedCursorBackend(CursorScript script) : script_(std::move(script)) {}

			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				++snapshot_opens_;
				if (script_.return_null_snapshot) {
					return nullptr;
				}
				return std::make_unique<ScriptedCursorSnapshot>(script_);
			}

			[[nodiscard]] size_t snapshotOpens() const { return snapshot_opens_; }

		private:
			CursorScript script_;
			mutable size_t snapshot_opens_ = 0U;
		};

		[[nodiscard]] storage::LogicalGraphNode node(uint64_t id, std::vector<std::string> labels = {"Label"},
																			 std::vector<storage::LogicalGraphPropertyEntry> properties = {}) {
			return {id, std::move(labels), std::move(properties)};
		}

		[[nodiscard]] storage::LogicalGraphEdge edge(uint64_t id, uint64_t source = 1U, uint64_t target = 2U,
																			 std::string type = "REL",
																			 std::vector<storage::LogicalGraphPropertyEntry> properties = {}) {
			return {id, source, target, std::move(type), std::move(properties)};
		}

		[[nodiscard]] LogicalGraphNodeCursorOptions nodeOptions() { return {1U, 1U}; }
		[[nodiscard]] LogicalGraphAdjacencyCursorOptions adjacencyOptions() { return {1U, 3U}; }

	} // namespace

	TEST(LogicalGraphNodeCursorTest, StreamsPagesRetainsOneSnapshotAndCanReopenAfterClose) {
		CursorScript script;
		script.node_pages = {{{node(1U)}, true, 1U}, {{node(2U)}, false, 1U}};
		ScriptedCursorBackend backend(std::move(script));
		LogicalGraphNodeCursor cursor(backend, nodeOptions());

		EXPECT_THROW(static_cast<void>(cursor.next()), std::logic_error);
		cursor.open();
		EXPECT_TRUE(cursor.isOpen());
		EXPECT_THROW(cursor.open(), std::logic_error);
		ASSERT_TRUE(cursor.next().has_value());
		EXPECT_EQ(cursor.next()->nodes, (std::vector<storage::LogicalGraphNode>{node(2U)}));
		EXPECT_FALSE(cursor.next().has_value());
		cursor.close();
		EXPECT_FALSE(cursor.isOpen());
		EXPECT_THROW(static_cast<void>(cursor.next()), std::logic_error);

		cursor.open();
		EXPECT_EQ(cursor.next()->nodes, (std::vector<storage::LogicalGraphNode>{node(1U)}));
		EXPECT_EQ(backend.snapshotOpens(), 2U);
	}

	TEST(LogicalGraphNodeCursorTest, RejectsInvalidOptionsSnapshotsPagesAndNodeMetadata) {
		CursorScript empty;
		ScriptedCursorBackend backend(empty);
		EXPECT_THROW(static_cast<void>(LogicalGraphNodeCursor(backend, {0U, 1U})), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphNodeCursor(backend, {1U, 0U})), std::invalid_argument);

		CursorScript nullScript;
		nullScript.return_null_snapshot = true;
		ScriptedCursorBackend nullBackend(std::move(nullScript));
		LogicalGraphNodeCursor nullCursor(nullBackend, nodeOptions());
		EXPECT_THROW(nullCursor.open(), std::runtime_error);

		const auto expectRejectedPage = [](storage::LogicalGraphNodePage page) {
			CursorScript script;
			script.node_pages.push_back(std::move(page));
			ScriptedCursorBackend pageBackend(std::move(script));
			LogicalGraphNodeCursor cursor(pageBackend, nodeOptions());
			cursor.open();
			EXPECT_THROW(static_cast<void>(cursor.next()), std::runtime_error);
		};
		expectRejectedPage({{node(1U), node(2U)}, false, 1U});
		expectRejectedPage({{}, true, 1U});
		expectRejectedPage({{node(0U)}, false, 1U});
		expectRejectedPage({{node(1U), node(1U)}, false, 1U});
		expectRejectedPage({{node(1U, {"", "Label"})}, false, 1U});
		expectRejectedPage({{node(1U, {"Label", "Label"})}, false, 1U});
		expectRejectedPage({{node(1U, {"Label"}, {{"", PropertyValue(1)}})}, false, 1U});

		CursorScript nonMonotonicScript;
		nonMonotonicScript.node_pages = {{{node(2U)}, true, 1U}, {{node(2U)}, false, 1U}};
		ScriptedCursorBackend nonMonotonicBackend(std::move(nonMonotonicScript));
		LogicalGraphNodeCursor nonMonotonicCursor(nonMonotonicBackend, nodeOptions());
		nonMonotonicCursor.open();
		ASSERT_TRUE(nonMonotonicCursor.next().has_value());
		EXPECT_THROW(static_cast<void>(nonMonotonicCursor.next()), std::runtime_error);

		CursorScript emptyTerminalScript;
		emptyTerminalScript.node_pages = {{{}, false, 0U}};
		ScriptedCursorBackend emptyTerminalBackend(std::move(emptyTerminalScript));
		LogicalGraphNodeCursor emptyTerminalCursor(emptyTerminalBackend, nodeOptions());
		emptyTerminalCursor.open();
		EXPECT_FALSE(emptyTerminalCursor.next().has_value());
	}

	TEST(LogicalGraphAdjacencyCursorTest, StreamsContinuationsEnforcesLifecycleAndSupportsBothDirections) {
		CursorScript script;
		script.adjacency_pages = {{{edge(10U)}, storage::LogicalGraphAdjacencyContinuation{10U, 11U}},
									  {{edge(11U)}, std::nullopt}};
		ScriptedCursorBackend backend(std::move(script));
		LogicalGraphAdjacencyCursor cursor(backend, 1U, storage::LogicalGraphReadDirection::LGRD_OUTGOING,
				adjacencyOptions());

		EXPECT_THROW(static_cast<void>(cursor.next()), std::logic_error);
		cursor.open();
		EXPECT_THROW(cursor.open(), std::logic_error);
		ASSERT_TRUE(cursor.next().has_value());
		EXPECT_EQ(cursor.next()->edges, (std::vector<storage::LogicalGraphEdge>{edge(11U)}));
		EXPECT_FALSE(cursor.next().has_value());
		cursor.close();
		EXPECT_THROW(static_cast<void>(cursor.next()), std::logic_error);

		CursorScript incomingScript;
		incomingScript.adjacency_pages = {{{edge(20U, 2U, 1U)}, std::nullopt}};
		ScriptedCursorBackend incomingBackend(std::move(incomingScript));
		LogicalGraphAdjacencyCursor incoming(incomingBackend, 1U, storage::LogicalGraphReadDirection::LGRD_INCOMING,
				adjacencyOptions());
		incoming.open();
		EXPECT_EQ(incoming.next()->edges, (std::vector<storage::LogicalGraphEdge>{edge(20U, 2U, 1U)}));
	}

	TEST(LogicalGraphAdjacencyCursorTest, RejectsInvalidOptionsAndSnapshotProtocols) {
		CursorScript script;
		ScriptedCursorBackend backend(script);
		EXPECT_THROW(static_cast<void>(LogicalGraphAdjacencyCursor(
								 backend, 0U, storage::LogicalGraphReadDirection::LGRD_OUTGOING, adjacencyOptions())),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAdjacencyCursor(
								 backend, 1U, storage::LogicalGraphReadDirection::LGRD_OUTGOING, {0U, 1U})),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAdjacencyCursor(
								 backend, 1U, storage::LogicalGraphReadDirection::LGRD_OUTGOING, {1U, 0U})),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAdjacencyCursor(
								 backend, 1U, storage::LogicalGraphReadDirection::LGRD_OUTGOING, {2U, 1U})),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(LogicalGraphAdjacencyCursor(
								 backend, 1U, static_cast<storage::LogicalGraphReadDirection>(99U), adjacencyOptions())),
				 std::invalid_argument);

		CursorScript nullScript;
		nullScript.return_null_snapshot = true;
		ScriptedCursorBackend nullBackend(std::move(nullScript));
		LogicalGraphAdjacencyCursor nullCursor(nullBackend, 1U, storage::LogicalGraphReadDirection::LGRD_OUTGOING,
				adjacencyOptions());
		EXPECT_THROW(nullCursor.open(), std::runtime_error);

		const auto expectRejectedPage = [](storage::LogicalGraphAdjacencyPage page,
																 LogicalGraphAdjacencyCursorOptions options = {1U, 3U}) {
			CursorScript script;
			script.adjacency_pages.push_back(std::move(page));
			ScriptedCursorBackend pageBackend(std::move(script));
			LogicalGraphAdjacencyCursor cursor(pageBackend, 1U, storage::LogicalGraphReadDirection::LGRD_OUTGOING,
					options);
			cursor.open();
			EXPECT_THROW(static_cast<void>(cursor.next()), std::runtime_error);
		};
		expectRejectedPage({{edge(10U), edge(11U)}, std::nullopt});
		expectRejectedPage({{}, storage::LogicalGraphAdjacencyContinuation{10U, 11U}});
		expectRejectedPage({{edge(0U)}, std::nullopt});
		expectRejectedPage({{edge(10U, 0U)}, std::nullopt});
		expectRejectedPage({{edge(10U, 1U, 0U)}, std::nullopt});
		expectRejectedPage({{edge(10U, 1U, 2U, "REL", {{"", PropertyValue(1)}})}, std::nullopt});
		expectRejectedPage({{edge(10U, 2U, 1U)}, std::nullopt});

		CursorScript untypedScript;
		untypedScript.adjacency_pages = {{{edge(10U, 1U, 2U, "")}, std::nullopt}};
		ScriptedCursorBackend untypedBackend(std::move(untypedScript));
		LogicalGraphAdjacencyCursor untypedCursor(untypedBackend, 1U,
				storage::LogicalGraphReadDirection::LGRD_OUTGOING, adjacencyOptions());
		untypedCursor.open();
		const auto untypedPage = untypedCursor.next();
		ASSERT_TRUE(untypedPage.has_value());
		ASSERT_EQ(untypedPage->edges.size(), 1U);
		EXPECT_TRUE(untypedPage->edges.front().type.empty());

		expectRejectedPage({{edge(10U)}, storage::LogicalGraphAdjacencyContinuation{9U, 11U}});
		expectRejectedPage({{edge(10U)}, storage::LogicalGraphAdjacencyContinuation{10U, 0U}});
		expectRejectedPage({{edge(10U)}, storage::LogicalGraphAdjacencyContinuation{10U, 10U}});
		expectRejectedPage({{edge(10U)}, storage::LogicalGraphAdjacencyContinuation{10U, 11U}}, {1U, 1U});

		CursorScript cumulativeBoundScript;
		cumulativeBoundScript.adjacency_pages = {{{edge(10U)}, storage::LogicalGraphAdjacencyContinuation{10U, 11U}},
																							{{edge(11U), edge(12U)}, std::nullopt}};
		ScriptedCursorBackend cumulativeBoundBackend(std::move(cumulativeBoundScript));
		LogicalGraphAdjacencyCursor cumulativeBound(cumulativeBoundBackend, 1U,
				storage::LogicalGraphReadDirection::LGRD_OUTGOING, {2U, 2U});
		cumulativeBound.open();
		ASSERT_TRUE(cumulativeBound.next().has_value());
		EXPECT_THROW(static_cast<void>(cumulativeBound.next()), std::runtime_error);

		CursorScript emptyTerminalScript;
		emptyTerminalScript.adjacency_pages = {{{}, std::nullopt}};
		ScriptedCursorBackend emptyTerminalBackend(std::move(emptyTerminalScript));
		LogicalGraphAdjacencyCursor emptyTerminal(emptyTerminalBackend, 1U,
				storage::LogicalGraphReadDirection::LGRD_OUTGOING, adjacencyOptions());
		emptyTerminal.open();
		EXPECT_FALSE(emptyTerminal.next().has_value());
	}

} // namespace graph::query::execution
