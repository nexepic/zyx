/**
 * @file test_LogicalGraphTransactionOverlay.cpp
 * @brief Unit tests for transaction-local logical graph read overlays.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/LogicalGraphReadExecutor.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"

#include "storage/LogicalGraphTransactionOverlayTestFixture.hpp"

namespace graph::storage {

	namespace {
	using namespace transaction_overlay_test;

	TEST(LogicalGraphTransactionOverlayTest, AdjacencyAdmissionForwardsAndCoversCreatedContinuation) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.adjacency_property_admission = true;
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsAdjacencyPropertyReadAdmission());
		const std::function<void(size_t)> reject = [](size_t) { throw std::length_error("rejected"); };
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
			LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, reject)), std::length_error);
		size_t admitted = 0U;
		const std::function<void(size_t)> accept = [&](size_t bytes) { admitted += bytes; };
		const auto page = snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
			LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, accept);
		ASSERT_EQ(page.edges.size(), 1U);
		EXPECT_EQ(page.edges.front().edge_id, 10U);
		EXPECT_EQ(admitted, 17U);

		const PropertyValue payload(std::string(256U * 1024U, 's'));
		const auto created = overlay.createLogicalEdges({{1U, 2U, "STAGED", {{"payload", payload}}}}).front();
		ASSERT_TRUE(snapshot->supportsAdjacencyPropertyReadAdmission());
		const auto first = snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
			LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, accept);
		ASSERT_TRUE(first.continuation);
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
			LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U, reject)), std::length_error);
		size_t checkpoints = 0U;
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
			LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U,
			[&](size_t bytes) { if (bytes == 0U && ++checkpoints == 7U) throw std::runtime_error("cancel created Edge copy"); })), std::runtime_error);
		EXPECT_EQ(checkpoints, 7U);
		const size_t before = admitted;
		const auto staged = snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
			LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U, accept);
		ASSERT_EQ(staged.edges.size(), 1U);
		EXPECT_EQ(staged.edges.front().edge_id, created);
		EXPECT_EQ(staged.edges.front().properties.front().value, payload);
		EXPECT_GT(admitted, before);
		EXPECT_FALSE(staged.continuation);
	}

	TEST(LogicalGraphTransactionOverlayTest, UnmodifiedAdjacencyForwardsBothAdmissionCallbacks) {
		for (bool vectorView : {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.adjacency_read_admission = true;
			LogicalGraphTransactionOverlay overlay(transaction);
			auto snapshot = vectorView ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot()) : overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->supportsAdjacencyReadAdmission());
			const auto accept = [](size_t) {};
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, accept,
				[](size_t) { throw std::length_error("buffer rejected"); })), std::length_error);
			for (int retry = 0; retry < 2; ++retry) {
				size_t properties = 0U, buffers = 0U;
				const auto page = snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U,
					[&](size_t bytes) { properties += bytes; }, [&](size_t bytes) { buffers = bytes; });
				ASSERT_EQ(page.edges.size(), 1U);
				EXPECT_EQ(page.edges.front().edge_id, 10U);
				EXPECT_EQ(properties, 17U);
				EXPECT_EQ(buffers, 29U);
			}
			overlay.replaceLogicalEdgeProperties({{10U, {{"payload", PropertyValue(int64_t{7})}}}});
			EXPECT_TRUE(snapshot->supportsAdjacencyReadAdmission());
			EXPECT_NO_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, accept, accept)));
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, ModifiedCombinedAdjacencyRejectsBuffersAndRetriesCreatedContinuation) {
		for (bool vectorView : {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.adjacency_read_admission = true;
			LogicalGraphTransactionOverlay overlay(transaction);
			const PropertyValue payload(std::string(4096U, 'p'));
			overlay.replaceLogicalEdgeProperties({{10U, {{"payload", payload}}}});
			const auto created = overlay.createLogicalEdges({{1U, 2U, "CREATED", {{"payload", payload}}}}).front();
			auto snapshot = vectorView ? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot()) : overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->supportsAdjacencyReadAdmission());
			const auto accept = [](size_t) {};
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, accept,
				[](size_t bytes) { if (bytes > 29U) throw std::length_error("merge buffer rejected"); })), std::length_error);
			const auto first = snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, accept, accept);
			ASSERT_EQ(first.edges.size(), 1U);
			EXPECT_EQ(first.edges.front().properties.front().value, payload);
			ASSERT_TRUE(first.continuation);
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U, accept,
				[](size_t bytes) { if (bytes > 1U) throw std::length_error("created slots rejected"); })), std::length_error);
			for (int retry = 0; retry < 2; ++retry) {
				size_t retained = 0U, peak = 0U;
				auto page = snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U, accept,
					[&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); });
				ASSERT_EQ(page.edges.size(), 1U);
				EXPECT_EQ(page.edges.front().edge_id, created);
				EXPECT_EQ(page.edges.front().properties.front().value, payload);
				EXPECT_GE(peak, retained);
				EXPECT_GT(retained, sizeof(LogicalGraphEdge));
				EXPECT_FALSE(page.continuation);
				page.edges.front().properties.front().value = PropertyValue("changed output");
			}
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, CombinedAdjacencyRetainsMetadataAcrossArrayGrowth) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.adjacency_read_admission = true;
		LogicalGraphTransactionOverlay overlay(transaction);
		for (size_t i = 0; i < 40U; ++i)
			overlay.createLogicalEdges({{1U, 2U, std::string(80U, 't'), {{std::string(90U, 'k'), PropertyValue(int64_t{7})}}}});
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		const auto accept = [](size_t) {};
		for (int retry = 0; retry < 2; ++retry) {
			size_t retained = 0U, peak = 0U;
			const auto page = snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 64U, accept,
				[&](size_t bytes) { retained = bytes; peak = std::max(peak, bytes); });
			ASSERT_GT(page.edges.size(), 40U);
			size_t expected = page.edges.capacity() * sizeof(LogicalGraphEdge);
			for (const auto &edge : page.edges) {
				expected += edge.properties.capacity() * sizeof(LogicalGraphPropertyEntry);
				if (!edge.type.empty()) expected += edge.type.capacity();
				for (const auto &entry : edge.properties) expected += entry.key.capacity();
			}
			EXPECT_EQ(retained, expected);
			EXPECT_GE(peak, retained);
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 64U, accept,
				[&](size_t bytes) { if (bytes >= expected) throw std::length_error("retained metadata rejected"); })), std::length_error);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, ModifiedAdjacencyAdmitsStagedPayloadBeforeCopyAndRetries) {
		for (bool vectorView : {false, true}) {
			SCOPED_TRACE(vectorView);
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.adjacency_property_admission = true;
			const PropertyValue original("base");
			base.edges.at(10U).properties = {{"payload", original}};
			LogicalGraphTransactionOverlay overlay(transaction);
			const PropertyValue payload(std::vector<PropertyValue>(4096U, PropertyValue(int64_t{7})));
			overlay.replaceLogicalEdgeProperties({{10U, {{"payload", payload}}}});
			auto snapshot = vectorView
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->supportsAdjacencyPropertyReadAdmission());
			size_t calls = 0U;
			const std::function<void(size_t)> rejectStaged = [&](size_t bytes) {
				++calls;
				if (bytes > 17U) throw std::length_error("staged copy rejected");
			};
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
				LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, rejectStaged)), std::length_error);
			EXPECT_EQ(calls, 2U);
			size_t checkpoints = 0U;
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgesWithPropertyAdmission(1U, LogicalGraphReadDirection::LGRD_OUTGOING,
				{}, 1U, [&](size_t bytes) { if (bytes == 0U && ++checkpoints == 8U) throw std::runtime_error("cancel during staged list copy"); })), std::runtime_error);
			EXPECT_EQ(checkpoints, 8U);
			for (int retry = 0; retry < 2; ++retry) {
				size_t admitted = 0U;
				const std::function<void(size_t)> accept = [&](size_t bytes) { admitted += bytes; };
				auto page = snapshot->scanAdjacentEdgesWithPropertyAdmission(1U,
					LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U, accept);
				ASSERT_EQ(page.edges.size(), 1U);
				EXPECT_EQ(page.edges.front().edge_id, 10U);
				EXPECT_EQ(page.edges.front().properties.front().value, payload);
				EXPECT_EQ(admitted, 17U + CanonicalPropertyValueCodec::encodedSize(payload) + sizeof(uint32_t));
				page.edges.front().properties.front().value = PropertyValue("changed output");
				EXPECT_EQ(base.edges.at(10U).properties.front().value, original);
			}
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, UnsupportedBaseAdmissionFailsBeforeReadingOrInvokingCallback) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_FALSE(snapshot->supportsPropertyReadAdmission());
		EXPECT_FALSE(snapshot->supportsShapedPropertyReadAdmission());
		const std::array<uint64_t, 1U> ids{1U};
		const std::array<std::string_view, 1U> keys{"payload"};
		size_t calls = 0U;
		const std::function<void(size_t)> admission = [&](size_t) { ++calls; };
		EXPECT_THROW(static_cast<void>(snapshot->readNodePropertyColumnsSortedWithAdmission(ids, keys, admission)), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSortedWithAdmission(ids, EntityReadShape{.all_properties = true}, admission)), std::logic_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSortedWithAdmission(ids, EntityReadShape{.all_properties = true}, admission)), std::logic_error);
		EXPECT_EQ(calls, 0U);
	}

	TEST(LogicalGraphTransactionOverlayTest, StagedSelectedColumnsCancelMidCopyAndRetry) {
		for (bool edge : {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.selected_property_admission = true;
			base.shaped_property_admission = true;
			LogicalGraphTransactionOverlay overlay(transaction);
			const PropertyValue payload(std::string(256U * 1024U, 'p'));
			if (edge) overlay.replaceLogicalEdgeProperties({{10U, {{"payload", payload}}}});
			else overlay.replaceLogicalNodeProperties({{1U, {{"payload", payload}}}});
			auto snapshot = overlay.openLogicalGraphReadSnapshot();
			const std::array<uint64_t, 1U> ids{edge ? 10U : 1U};
			const std::array<std::string_view, 1U> keys{"payload"};
			const auto read = [&](const std::function<void(size_t)> &admission) {
				return edge ? snapshot->readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)
					: snapshot->readNodePropertyColumnsSortedWithAdmission(ids, keys, admission);
			};
			size_t checkpoints = 0U;
			EXPECT_THROW(static_cast<void>(read([&](size_t bytes) {
				if (bytes == 0U && ++checkpoints == 4U) throw std::runtime_error("cancel selected value copy");
			})), std::runtime_error);
			EXPECT_EQ(checkpoints, 4U);
			const EntityReadShape shape{.property_tokens = {5U}};
			const auto readShaped = [&](const std::function<void(size_t)> &admission) {
				return edge ? snapshot->readEdgesShapedSortedWithAdmission(ids, shape, admission).front().properties
					: snapshot->readNodesShapedSortedWithAdmission(ids, shape, admission).front().properties;
			};
			checkpoints = 0U;
			EXPECT_THROW(static_cast<void>(readShaped([&](size_t bytes) {
				if (bytes == 0U && ++checkpoints == 7U) throw std::runtime_error("cancel selected shape copy");
			})), std::runtime_error);
			EXPECT_EQ(checkpoints, 7U);
			const auto shaped = readShaped([](size_t) {});
			ASSERT_EQ(shaped.size(), 1U);
			EXPECT_EQ(shaped.front().value, payload);
			for (int retry = 0; retry < 2; ++retry) {
				auto columns = read([](size_t) {});
				ASSERT_EQ(columns.size(), 1U);
				ASSERT_EQ(columns.front().size(), 1U);
				EXPECT_EQ(columns.front().front(), payload);
				columns.front().front() = PropertyValue("modified output");
			}
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, SelectedPropertyAdmissionCoversBaseAndStagedEdges) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.selected_property_admission = true;
		base.edges.at(10U).properties = {{"payload", PropertyValue(std::string(32U, 'b'))}};
		LogicalGraphTransactionOverlay overlay(transaction);
		const PropertyValue stagedPayload(std::string(48U, 's'));
		const uint64_t stagedEdge = overlay.createLogicalEdges(
				{{1U, 2U, "STAGED", {{"payload", stagedPayload}}}}).front();
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		ASSERT_TRUE(snapshot->supportsPropertyReadAdmission());
		const std::array<uint64_t, 2U> edgeIds{10U, stagedEdge};
		const std::array<std::string_view, 1U> keys{"payload"};
		size_t admittedBytes = 0U;
		const std::function<void(size_t)> admission = [&](const size_t bytes) { admittedBytes += bytes; };

		const auto columns = snapshot->readEdgePropertyColumnsSortedWithAdmission(edgeIds, keys, admission);

		ASSERT_EQ(columns.size(), 1U);
		ASSERT_EQ(columns.front().size(), 2U);
		EXPECT_EQ(columns.front()[0], PropertyValue(std::string(32U, 'b')));
		EXPECT_EQ(columns.front()[1], PropertyValue(std::string(48U, 's')));
		EXPECT_EQ(admittedBytes,
				CanonicalPropertyValueCodec::encodedSize(base.edges.at(10U).properties.front().value) + sizeof(uint32_t) +
				CanonicalPropertyValueCodec::encodedSize(stagedPayload) + sizeof(uint32_t));
		EXPECT_EQ(base.read_edge_property_column_calls, 1U);
	}

	TEST(LogicalGraphTransactionOverlayTest, ShapedAdmissionMergesBaseAndStagedPayloadsAcrossSnapshotRevisions) {
		for (bool vector : {false, true}) {
			SCOPED_TRACE(vector);
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.shaped_property_admission = true;
			base.edges.at(10U).properties = {{"payload", PropertyValue(std::string(512U, 'b'))}};
			LogicalGraphTransactionOverlay overlay(transaction);
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vector
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->supportsShapedPropertyReadAdmission());
			size_t calls = 0U;
			const std::function<void(size_t)> reject = [&](size_t) { ++calls; throw std::length_error("shaped source exhausted"); };
			EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSortedWithAdmission(std::array<uint64_t, 1U>{1U}, EntityReadShape{.all_properties = true}, reject)), std::length_error);
			EXPECT_EQ(base.read_node_batch_calls, 0U);
			const PropertyValue staged(std::string(1024U, 's'));
			const uint64_t nodeId = overlay.createLogicalNodes({{{"Added"}, {{"payload", staged}, {"rank", PropertyValue()}}}}).front();
			const uint64_t edgeId = overlay.createLogicalEdges({{1U, nodeId, "ADDED", {{"payload", staged}, {"rank", PropertyValue()}}}}).front();
			ASSERT_TRUE(snapshot->supportsShapedPropertyReadAdmission());
			const EntityReadShape complete{.all_properties = true};
			for (bool edge : {false, true}) {
				SCOPED_TRACE(edge);
				const std::array<uint64_t, 2U> ids = edge ? std::array<uint64_t, 2U>{10U, edgeId} : std::array<uint64_t, 2U>{1U, nodeId};
				const auto read = [&](std::span<const uint64_t> selected, const EntityReadShape &shape, const std::function<void(size_t)> &admission) {
					return edge ? snapshot->readEdgesShapedSortedWithAdmission(selected, shape, admission).back().properties
						: snapshot->readNodesShapedSortedWithAdmission(selected, shape, admission).back().properties;
				};
				EXPECT_THROW(static_cast<void>(read(ids, complete, reject)), std::length_error);
				EXPECT_THROW(static_cast<void>(read(std::span(ids).last(1U), complete, reject)), std::length_error);
				for (int retry = 0; retry < 2; ++retry) {
					size_t bytes = 0U;
					const std::function<void(size_t)> accept = [&](size_t valueBytes) { bytes += valueBytes; };
					auto result = read(ids, complete, accept);
					ASSERT_EQ(result.size(), 2U);
					EXPECT_EQ(result[0].value, staged);
					EXPECT_EQ(result[1].value.getType(), graph::PropertyType::NULL_TYPE);
					const auto &baseValue = edge ? base.edges.at(10U).properties[0].value : base.nodes.at(1U).properties[0].value;
					EXPECT_EQ(bytes, CanonicalPropertyValueCodec::encodedSize(baseValue) + CanonicalPropertyValueCodec::encodedSize(staged) + 6U + 3U * sizeof(uint32_t));
					result[0].value = PropertyValue("mutated output");
					const EntityReadShape selected{.property_tokens = {2U}};
					const size_t beforeSelection = bytes;
					const auto nullOnly = read(std::span(ids).last(1U), selected, accept);
					EXPECT_EQ(bytes - beforeSelection, 10U);
					ASSERT_EQ(nullOnly.size(), 1U);
					EXPECT_EQ(nullOnly[0].key, "rank");
					const size_t beforeMissing = bytes;
					EXPECT_TRUE(read(std::span(ids).last(1U), EntityReadShape{.property_tokens = {99U}}, accept).empty());
					EXPECT_EQ(bytes, beforeMissing);
					EXPECT_EQ(nullOnly[0].value.getType(), graph::PropertyType::NULL_TYPE);
				}
				const size_t beforeInvalid = calls;
				EXPECT_THROW(static_cast<void>(read(std::array<uint64_t, 2U>{ids[0], 0U}, complete, reject)), std::invalid_argument);
				EXPECT_EQ(calls, beforeInvalid);
				EXPECT_THROW(static_cast<void>(read(ids, complete, {})), std::invalid_argument);
			}
			overlay.deleteLogicalNodes({{nodeId, true, 1U}});
			const size_t beforeDeleted = calls;
			EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSortedWithAdmission(std::array<uint64_t, 2U>{1U, nodeId}, EntityReadShape{.all_properties = true}, reject)), std::runtime_error);
			EXPECT_EQ(calls, beforeDeleted);
			transaction.rollback();
			EXPECT_THROW(static_cast<void>(snapshot->supportsShapedPropertyReadAdmission()), std::logic_error);
			EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSortedWithAdmission({}, EntityReadShape{.all_properties = true}, reject)), std::logic_error);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, NativeEdgeReferencesMergeStagedTopologyAndPreserveKnownTypes) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.native_edge_refs = true;
			base.native_topology_pages = true;
			base.edges.emplace(300U, LogicalGraphEdge{300U, 1U, 2U, "BASE_LINK"});
			LogicalGraphTransactionOverlay overlay(transaction);
			overlay.replaceLogicalEdgeProperties({{10U, {{"payload", PropertyValue(std::string(8192U, 'x'))}}}});
			ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "BASE_LINK", {}}, {1U, 2U, "BASE_LINK", {}}}),
				(std::vector<uint64_t>{200U, 201U}));
			overlay.deleteLogicalEdges({{201U}, {300U}});
			auto snapshot = vectorSnapshot
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->supportsNativeEdgeRefScans());
			ASSERT_TRUE(snapshot->supportsNativeRelationshipTypeTokens());
			const size_t tokenLookupsBeforeScans = base.relationship_type_token_lookup_calls;
			const size_t reads = base.read_edge_calls;
			for (size_t repeat = 0U; repeat < 2U; ++repeat) {
				std::vector<EntityRef> refs;
				snapshot->visitEdgeRefs(2U, [&](const EntityRef &ref) { refs.push_back(ref); return true; });
				EXPECT_EQ(refs, (std::vector<EntityRef>{{10U, true, 1U, 2U, 5U}, {200U, true, 1U, 2U, 5U}}));
				EXPECT_EQ(base.edge_ref_bounds.back(), 3U);
			}
			EXPECT_EQ(base.relationship_type_token_lookup_calls, tokenLookupsBeforeScans);
			EXPECT_THROW(snapshot->visitEdgeRefs(1U, [](const EntityRef &) { return true; }), std::runtime_error);
			std::vector<uint64_t> prefix;
			EXPECT_NO_THROW(snapshot->visitEdgeRefs(2U, [&](const EntityRef &ref) {
				prefix.push_back(ref.entity_id); return prefix.size() < 2U;
			}));
			EXPECT_EQ(prefix, (std::vector<uint64_t>{10U, 200U}));
			EXPECT_EQ(base.read_edge_calls, reads);
			EXPECT_THROW(snapshot->visitEdgeRefs(0U, [](const EntityRef &) { return true; }), std::invalid_argument);
			ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "NEW_TYPE", {}}}).size(), 1U);
			EXPECT_FALSE(snapshot->supportsNativeRelationshipTypeTokens());
			EXPECT_TRUE(snapshot->supportsNativeRelationshipTypeTokens("BASE_LINK"));
			EXPECT_FALSE(snapshot->supportsNativeRelationshipTypeTokens("NEW_TYPE"));
			const auto baseTypeToken = snapshot->findRelationshipTypeToken("BASE_LINK");
			ASSERT_EQ(baseTypeToken, std::optional<uint64_t>{5U});
			std::vector<uint64_t> knownTypeIds;
			snapshot->visitEdgeRefs(3U, [&](const EntityRef &reference) {
				if (reference.type_token == *baseTypeToken) knownTypeIds.push_back(reference.entity_id);
				return true;
			});
			EXPECT_EQ(knownTypeIds, (std::vector<uint64_t>{10U, 200U}));
			base.edges.emplace(301U, LogicalGraphEdge{301U, 1U, 2U, "BASE_LINK"});
			base.edges.emplace(302U, LogicalGraphEdge{302U, 1U, 2U, "BASE_LINK"});
			const size_t lookupsBeforeAdjacency = base.relationship_type_token_lookup_calls;
			const auto adjacentRefs = snapshot->scanAdjacentEdgeRefs(
					1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 5U);
			EXPECT_EQ(adjacentRefs.edges, (std::vector<EntityRef>{
				{10U, true, 1U, 2U, 5U}, {301U, true, 1U, 2U, 5U}, {302U, true, 1U, 2U, 5U},
				{200U, true, 1U, 2U, 5U}, {202U, true, 1U, 2U, 0U}}));
			EXPECT_EQ(base.relationship_type_token_lookup_calls, lookupsBeforeAdjacency + 1U);
			EXPECT_TRUE(snapshot->supportsNativeEdgeRefScans());
			base.edges.erase(301U);
			base.edges.erase(302U);
			overlay.deleteLogicalNodes({{2U, true, 3U}});
			size_t visited = 0U;
			snapshot->visitEdgeRefs(1U, [&](const EntityRef &) { ++visited; return true; });
			EXPECT_EQ(visited, 0U);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, NativeEdgeReferencesRejectMalformedTopologyAndIgnoredStop) {
		for (const std::vector<EntityRef> &refs: {
			std::vector<EntityRef>{{0U, true, 1U, 2U, 5U}},
			std::vector<EntityRef>{{10U, false, 1U, 2U, 5U}},
			std::vector<EntityRef>{{10U, true, 0U, 2U, 5U}},
			std::vector<EntityRef>{{10U, true, 1U, 0U, 5U}},
			std::vector<EntityRef>{{10U, true, 1U, 2U, 5U}, {10U, true, 1U, 2U, 5U}},
			std::vector<EntityRef>{{200U, true, 1U, 2U, 5U}}}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.native_edge_refs = true;
			base.forced_edge_refs = refs;
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "BASE_LINK", {}}}).size(), 1U);
			auto snapshot = overlay.openLogicalGraphReadSnapshot();
			EXPECT_THROW(snapshot->visitEdgeRefs(8U, [](const EntityRef &) { return true; }), std::runtime_error);
		}
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.native_edge_refs = true;
		base.ignore_edge_ref_stop = true;
		base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "BASE_LINK"});
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(snapshot->visitEdgeRefs(2U, [](const EntityRef &) { return false; }), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, NativeNodeReferencesMergeStagedStateWithoutPayloadReads) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.native_node_refs = true;
			base.edges.clear();
			base.nodes.emplace(150U, LogicalGraphNode{150U, {"Base"}, {}});
			LogicalGraphTransactionOverlay overlay(transaction);
			overlay.replaceLogicalNodeProperties({{1U, {{"payload", PropertyValue(std::string(8192U, 'x'))}}}});
			overlay.deleteLogicalNodes({{2U, false, 0U}});
			ASSERT_EQ(overlay.createLogicalNodes({{{"NewLabel"}, {}}, {{"NewLabel"}, {}}}),
				(std::vector<uint64_t>{100U, 101U}));
			overlay.deleteLogicalNodes({{101U, false, 0U}});
			auto snapshot = vectorSnapshot
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			ASSERT_TRUE(snapshot->supportsNativeNodeRefScans());
			const size_t reads = base.read_node_calls;
			for (size_t repeat = 0U; repeat < 2U; ++repeat) {
				std::vector<uint64_t> ids;
				snapshot->visitNodeRefs(3U, [&](const EntityRef &ref) { ids.push_back(ref.entity_id); return true; });
				EXPECT_EQ(ids, (std::vector<uint64_t>{1U, 100U, 150U}));
				EXPECT_EQ(base.node_ref_bounds.back(), 4U);
			}
			std::vector<std::pair<uint64_t, bool>> matches;
			snapshot->visitNodeRefsWithLabelMatches(3U, {"NewLabel"}, [&](const EntityRef &ref, bool match) {
				matches.emplace_back(ref.entity_id, match); return true;
			});
			EXPECT_EQ(matches, (std::vector<std::pair<uint64_t, bool>>{{1U, false}, {100U, true}, {150U, false}}));
			EXPECT_THROW(snapshot->visitNodeRefs(2U, [](const EntityRef &) { return true; }), std::runtime_error);
			size_t visited = 0U;
			EXPECT_NO_THROW(snapshot->visitNodeRefs(1U, [&](const EntityRef &) { ++visited; return false; }));
			EXPECT_EQ(visited, 1U);
			std::vector<uint64_t> prefix;
			EXPECT_NO_THROW(snapshot->visitNodeRefs(2U, [&](const EntityRef &ref) {
				prefix.push_back(ref.entity_id); return prefix.size() < 2U;
			}));
			EXPECT_EQ(prefix, (std::vector<uint64_t>{1U, 100U}));
			EXPECT_EQ(base.read_node_calls, reads);
			EXPECT_THROW(snapshot->visitNodeRefs(0U, [](const EntityRef &) { return true; }), std::invalid_argument);
			EXPECT_THROW(snapshot->visitNodeRefsWithLabelMatches(3U, {""}, [](const EntityRef &, bool) { return true; }), std::invalid_argument);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, NativeNodeReferencesRejectMalformedSourcesAndIgnoredStop) {
		for (const std::vector<EntityRef> &refs: {
			std::vector<EntityRef>{{0U, false, 0U, 0U, 0U}},
			std::vector<EntityRef>{{1U, true, 0U, 0U, 0U}},
			std::vector<EntityRef>{{2U, false, 0U, 0U, 0U}, {1U, false, 0U, 0U, 0U}},
			std::vector<EntityRef>{{1U, false, 0U, 0U, 0U}, {1U, false, 0U, 0U, 0U}},
			std::vector<EntityRef>{{100U, false, 0U, 0U, 0U}}}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.native_node_refs = true;
			base.forced_node_refs = refs;
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalNodes({{{"Created"}, {}}}), (std::vector<uint64_t>{100U}));
			auto snapshot = overlay.openLogicalGraphReadSnapshot();
			EXPECT_THROW(snapshot->visitNodeRefs(8U, [](const EntityRef &) { return true; }), std::runtime_error);
		}
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.native_node_refs = true;
		base.ignore_node_ref_stop = true;
		LogicalGraphTransactionOverlay overlay(transaction);
		auto snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(snapshot->visitNodeRefs(2U, [](const EntityRef &) { return false; }), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, NonNullProbesRespectStagedValuesAndTransactionLifetime) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			LogicalGraphTransactionOverlay overlay(transaction);
			overlay.replaceLogicalNodeProperties({{1U, {{"value", PropertyValue()}}}});
			overlay.replaceLogicalEdgeProperties({{10U, {{"value", PropertyValue()}}}});
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			EXPECT_EQ(snapshot->nodePropertyIsNonNull(1U, "value"), std::optional<bool>{false});
			EXPECT_EQ(snapshot->edgePropertyIsNonNull(10U, "value"), std::optional<bool>{false});
			EXPECT_EQ(snapshot->nodePropertyIsNonNull(1U, "missing"), std::optional<bool>{false});
			EXPECT_EQ(snapshot->edgePropertyIsNonNull(10U, "missing"), std::optional<bool>{false});
			overlay.replaceLogicalNodeProperties({{1U, {{"value", PropertyValue(0)}}}});
			overlay.replaceLogicalEdgeProperties({{10U, {{"value", PropertyValue(false)}}}});
			EXPECT_EQ(snapshot->nodePropertyIsNonNull(1U, "value"), std::optional<bool>{true});
			EXPECT_EQ(snapshot->edgePropertyIsNonNull(10U, "value"), std::optional<bool>{true});
			overlay.deleteLogicalEdges({{10U}});
			EXPECT_THROW(static_cast<void>(snapshot->edgePropertyIsNonNull(10U, "value")), std::runtime_error);
			transaction.rollback();
			EXPECT_THROW(static_cast<void>(snapshot->nodePropertyIsNonNull(1U, "value")), std::logic_error);
			EXPECT_THROW(static_cast<void>(snapshot->edgePropertyIsNonNull(10U, "value")), std::logic_error);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, OverlaysPropertiesCreatedNodesAndResumableEdges) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.replaceLogicalNodeProperties({{1U, {{"name", PropertyValue("Grace")},
																				  {"rank", PropertyValue(int64_t(7))}}}});
		const std::vector<uint64_t> created = overlay.createLogicalNodes({{{"Added"}, {}}});
		ASSERT_EQ(created, (std::vector<uint64_t>{100U}));
		const std::vector<uint64_t> edges = overlay.createLogicalEdges(
				{{1U, created[0], "ADDED", {}}, {1U, created[0], "ADDED", {}}, {1U, created[0], "ADDED", {}}});
		ASSERT_EQ(edges, (std::vector<uint64_t>{200U, 201U, 202U}));

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode updated = snapshot->readNode(1U);
		EXPECT_EQ(updated.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Grace")},
																		 {"rank", PropertyValue(int64_t(7))}}));
		const LogicalGraphNodePage firstNodes = snapshot->scanNodesAfter(0U, 2U, 1U);
		ASSERT_EQ(firstNodes.nodes.size(), 2U);
		ASSERT_TRUE(firstNodes.may_have_more);
		const LogicalGraphNodePage secondNodes = snapshot->scanNodesAfter(2U, 2U, 1U);
		ASSERT_EQ(secondNodes.nodes, (std::vector<LogicalGraphNode>{{100U, {"Added"}}}));
		const LogicalGraphAdjacencyPage firstEdges = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U);
		ASSERT_EQ(firstEdges.edges.size(), 2U);
		ASSERT_TRUE(firstEdges.continuation.has_value());
		const LogicalGraphAdjacencyPage secondEdges = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, firstEdges.continuation, 2U);
		ASSERT_EQ(secondEdges.edges.size(), 2U);
		EXPECT_FALSE(secondEdges.continuation.has_value());
		EXPECT_EQ(transaction.readEdgeCalls(), 0U);

		overlay.deleteLogicalEdges({{200U}});
		snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(200U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, ReusesCompleteRecordsFromBasePages) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();

		const LogicalGraphNodePage nodes = snapshot->scanNodesAfter(0U, 2U, 1U);
		ASSERT_EQ(nodes.nodes.size(), 2U);
		EXPECT_EQ(transaction.readNodeCalls(), 0U);
		snapshot->visitEdges(1U, [](const LogicalGraphEdge &) { return false; });
		EXPECT_EQ(transaction.readEdgeCalls(), 0U);
	}

	TEST(LogicalGraphTransactionOverlayTest, AppliesReadShapesToStagedEntitiesAndSortedBatches) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const std::vector<uint64_t> createdNodes =
				overlay.createLogicalNodes({{{"Added"}, {{"name", PropertyValue("Grace")},
																	 {"rank", PropertyValue(int64_t(7))}}}});
		ASSERT_EQ(createdNodes, (std::vector<uint64_t>{100U}));
		const std::vector<uint64_t> createdEdges =
				overlay.createLogicalEdges({{1U, createdNodes.front(), "ADDED", {{"weight", PropertyValue(int64_t(3))}}}});
		ASSERT_EQ(createdEdges, (std::vector<uint64_t>{200U}));

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode identity = snapshot->readNodeShaped(createdNodes.front(), EntityReadShape{});
		EXPECT_EQ(identity, (LogicalGraphNode{createdNodes.front(), {}, {}}));
		const LogicalGraphNode selectedNode = snapshot->readNodeShaped(
				createdNodes.front(), EntityReadShape{.include_labels = true, .property_tokens = {2U}});
		EXPECT_EQ(selectedNode,
				  (LogicalGraphNode{createdNodes.front(), {"Added"}, {{"rank", PropertyValue(int64_t(7))}}}));
		const LogicalGraphEdge selectedEdge = snapshot->readEdgeShaped(
				createdEdges.front(), EntityReadShape{.include_type = true, .property_tokens = {3U}});
		EXPECT_EQ(selectedEdge,
				  (LogicalGraphEdge{createdEdges.front(), 1U, createdNodes.front(), "ADDED",
																	{{"weight", PropertyValue(int64_t(3))}}}));

		const auto nodes = snapshot->readNodesShapedSorted(
				std::array<uint64_t, 2U>{1U, createdNodes.front()}, EntityReadShape{.include_labels = true});
		ASSERT_EQ(nodes.size(), 2U);
		EXPECT_EQ(nodes.front().labels, (std::vector<std::string>{"Base"}));
		EXPECT_EQ(nodes.back().labels, (std::vector<std::string>{"Added"}));
		EXPECT_EQ(transaction.readNodeBatchCalls(), 1U);
		const auto edges = snapshot->readEdgesShapedSorted(
				std::array<uint64_t, 2U>{10U, createdEdges.front()}, EntityReadShape{.include_type = true});
		ASSERT_EQ(edges.size(), 2U);
		EXPECT_EQ(edges.front().type, "BASE_LINK");
		EXPECT_EQ(edges.back().type, "ADDED");
		EXPECT_EQ(transaction.readEdgeBatchCalls(), 1U);
		const auto nodeNames = snapshot->readNodePropertiesSorted(
				std::array<uint64_t, 2U>{1U, createdNodes.front()}, "name");
		EXPECT_EQ(nodeNames, (std::vector<PropertyValue>{PropertyValue("Ada"), PropertyValue("Grace")}));
		EXPECT_EQ(transaction.readNodePropertyBatchCalls(), 1U);
		EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSorted(
				std::array<uint64_t, 2U>{createdNodes.front(), 1U}, EntityReadShape{})), std::invalid_argument);
	}

	TEST(LogicalGraphTransactionOverlayTest, CreationIdentityRangesAreBoundedAndNeverWrap) {
		const LogicalGraphCreationIdentityRange range{10U, 3U};
		EXPECT_FALSE(range.contains(9U));
		EXPECT_TRUE(range.contains(10U));
		EXPECT_TRUE(range.contains(11U));
		EXPECT_TRUE(range.contains(12U));
		EXPECT_FALSE(range.contains(13U));

		const LogicalGraphCreationIdentityRange zeroFirstId{0U, 3U};
		const LogicalGraphCreationIdentityRange emptyRange{10U, 0U};
		EXPECT_FALSE(zeroFirstId.contains(1U));
		EXPECT_FALSE(emptyRange.contains(10U));
		const LogicalGraphCreationIdentityRange nearLimit{std::numeric_limits<uint64_t>::max() - 1U, 2U};
		EXPECT_TRUE(nearLimit.contains(std::numeric_limits<uint64_t>::max() - 1U));
		EXPECT_TRUE(nearLimit.contains(std::numeric_limits<uint64_t>::max()));
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsOversizedAndNonIncreasingBaseNodePages) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalNodes({{{"Added"}, {}}}), (std::vector<uint64_t>{100U}));
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
					? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
					: overlay.openLogicalGraphReadSnapshot();
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.nodes.emplace(3U, LogicalGraphNode{3U, {"Base"}, {}});
			for (const auto &ids: {std::vector<uint64_t>{2U, 3U, 4U}, std::vector<uint64_t>{2U, 2U},
					std::vector<uint64_t>{3U, 2U}, std::vector<uint64_t>{1U}, std::vector<uint64_t>{0U}}) {
				SCOPED_TRACE(::testing::PrintToString(ids));
				LogicalGraphNodePage malformed;
				malformed.metadata_pages_visited = 1U;
				for (const uint64_t id: ids) malformed.nodes.push_back({id, {}, {}});
				base.forced_node_page = malformed;
				EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(1U, 2U, 4U, EntityReadShape{})), std::runtime_error);
				EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(1U, 2U, 4U)), std::runtime_error);
			}
			base.forced_node_page.reset();
			const auto page = snapshot->scanNodesAfter(1U, 2U, 4U, EntityReadShape{});
			EXPECT_EQ(page.nodes, (std::vector<LogicalGraphNode>{{2U, {}, {}}, {3U, {}, {}}}));
			EXPECT_TRUE(page.may_have_more);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, SharesTheBaseMetadataPageBudgetAcrossItsNodeOverlay) {
		// A relocatable base scan needs at least a directory root and leaf. The overlay
		// must forward that budget instead of constraining every probe to one page.
		InMemoryLogicalTransaction transaction(false, 2U);
		LogicalGraphTransactionOverlay overlay(transaction);
		ASSERT_EQ(overlay.createLogicalNodes({{{"Added"}, {}}}), (std::vector<uint64_t>{100U}));

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNodePage page = snapshot->scanNodesAfter(0U, 3U, 2U);
		EXPECT_EQ(page.nodes,
				  (std::vector<LogicalGraphNode>{{1U, {"Base"}, {{"name", PropertyValue("Ada")}}},
																						 {2U, {"Base"}}, {100U, {"Added"}}}));
		EXPECT_FALSE(page.may_have_more);
		EXPECT_EQ(page.metadata_pages_visited, 2U);
	}

	TEST(LogicalGraphTransactionOverlayTest, MergesCompleteExactLookupWithTheTransactionFinalState) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.clear();
		base.exact_node_lookup = [&base](const std::vector<std::string> &labels,
											 const std::vector<std::pair<std::string, PropertyValue>> &properties,
											 size_t maximumNodes, size_t maximumPropertyEntries)
				-> std::optional<std::vector<LogicalGraphNode>> {
			if (maximumNodes == 0U || maximumPropertyEntries == 0U) {
				throw std::invalid_argument("test exact lookup requires positive bounds");
			}
			if (labels != std::vector<std::string>{"Base"} || properties.size() != 1U ||
				properties.front().first != "name") {
				return std::nullopt;
			}
			if (properties.front().second == PropertyValue("Ada")) {
				return std::vector<LogicalGraphNode>{base.nodes.at(1U)};
			}
			return std::vector<LogicalGraphNode>{};
		};
	LogicalGraphTransactionOverlay overlay(transaction);
	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
	EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
				 {"Base"}, {{"name", PropertyValue("Ada")}}, 0U, 1U)),
			 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(snapshot->findNodesByExactProperties(
				 {"Base"}, {{"name", PropertyValue("Ada")}}, 1U, 0U)),
			 std::invalid_argument);
	const auto findByName = [&snapshot](std::string_view name) {
		return snapshot->findNodesByExactProperties({"Base"}, {{"name", PropertyValue(std::string(name))}}, 2U, 1U);
	};

		ASSERT_TRUE(findByName("Ada").has_value());
		EXPECT_EQ(findByName("Ada")->front().node_id, 1U);

		// A base support-tree match must be filtered through the final transaction
		// state before it reaches the runtime.
		overlay.replaceLogicalNodeProperties({{1U, {{"name", PropertyValue("Grace")}}}});
		EXPECT_EQ(*findByName("Ada"), (std::vector<LogicalGraphNode>{}));
		ASSERT_TRUE(findByName("Grace").has_value());
		EXPECT_EQ(findByName("Grace")->front().node_id, 1U);

		overlay.deleteLogicalNodes({{1U, false, 0U}});
		EXPECT_EQ(*findByName("Grace"), (std::vector<LogicalGraphNode>{}));
		const uint64_t created = overlay.createLogicalNodes({{{"Base"}, {{"name", PropertyValue("Ada")}}}}).front();
		ASSERT_TRUE(findByName("Ada").has_value());
		EXPECT_EQ(findByName("Ada")->front().node_id, created);
	}

	TEST(LogicalGraphTransactionOverlayTest, PreservesTheOptionalExactLookupFallback) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();

		// A backend without a complete index lookup must leave the runtime on its
		// bounded scan path rather than returning an incomplete overlay result.
		EXPECT_EQ(snapshot->findNodesByExactProperties(
					  {"Base"}, {{"name", PropertyValue("Ada")}}, 1U, 1U),
				  std::nullopt);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsMismatchedFenceAndReadOnlyStaging) {
		InMemoryLogicalTransaction writable;
		LogicalGraphTransactionOverlay overlay(writable);
		LogicalGraphMutationFence wrongFence{9U, 9U, {}};
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"Blocked"}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(batch, wrongFence)),
				 LogicalGraphMutationConflict);

		InMemoryLogicalTransaction readOnly(true);
		LogicalGraphTransactionOverlay readOnlyOverlay(readOnly);
		EXPECT_THROW(static_cast<void>(readOnlyOverlay.executeLogicalGraphMutationBatch(batch)), std::logic_error);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = readOnlyOverlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readNode(1U).node_id, 1U);
	}

	TEST(LogicalGraphTransactionOverlayTest, DetachDeleteDoesNotChargeAlreadyDeletedBaseOrCreatedEdges) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "STAGED", {}}}), (std::vector<uint64_t>{200U}));
		overlay.deleteLogicalEdges({{10U}, {200U}});

		// The explicit deletions are already final-state invisible, so a detach
		// delete with one permitted Edge must not count them a second time.
		EXPECT_NO_THROW(overlay.deleteLogicalNodes({{1U, true, 1U}}));
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readNode(1U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(10U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(200U)), std::runtime_error);
		EXPECT_EQ(snapshot->readNode(2U).node_id, 2U);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsNonDetachDeleteWithAVisibleIncidentEdgeAtStaging) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		EXPECT_THROW(overlay.deleteLogicalNodes({{1U, false, 0U}}), std::runtime_error);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readNode(1U).node_id, 1U);
		EXPECT_EQ(snapshot->readEdge(10U).edge_id, 10U);
	}

	TEST(LogicalGraphTransactionOverlayTest, AppliesFinalStateBatchForCreatedEntitiesAndProperties) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const LogicalGraphMutationEntityReference firstCreatedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U};
		const LogicalGraphMutationEntityReference secondCreatedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U};
		LogicalGraphMutationBatch batch;
		batch.node_creates = {{{"Original"}, {{"name", PropertyValue("Ada")}}}, {{"Target"}, {}}};
		batch.node_label_replacements = {{firstCreatedNode, {"Final", "Extra"}}};
		batch.edge_creates = {{firstCreatedNode, secondCreatedNode, "REL", {{"rank", PropertyValue(int64_t(1))}}},
							  {firstCreatedNode, secondCreatedNode, "REL", {}},
							  {firstCreatedNode, secondCreatedNode, "REL", {}}};
		const LogicalGraphMutationBatchResult result = overlay.executeLogicalGraphMutationBatch(batch);
		ASSERT_EQ(result.created_node_ids, (std::vector<uint64_t>{100U, 101U}));
		ASSERT_EQ(result.created_edge_ids, (std::vector<uint64_t>{200U, 201U, 202U}));

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode created = snapshot->readNode(100U);
		EXPECT_EQ(created.labels, (std::vector<std::string>{"Extra", "Final"}));
		EXPECT_EQ(created.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Ada")}}));
		const LogicalGraphEdge edge = snapshot->readEdge(200U);
		EXPECT_EQ(edge.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"rank", PropertyValue(int64_t(1))}}));

		const LogicalGraphAdjacencyPage first = snapshot->scanAdjacentEdges(
				100U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U);
		ASSERT_EQ(first.edges.size(), 2U);
		ASSERT_TRUE(first.continuation.has_value());
		const LogicalGraphAdjacencyPage second = snapshot->scanAdjacentEdges(
				100U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 2U);
		ASSERT_EQ(second.edges.size(), 1U);
		EXPECT_FALSE(second.continuation.has_value());

		std::vector<uint64_t> nodeIds;
		snapshot->visitNodes(4U, [&nodeIds](const LogicalGraphNode &node) {
			nodeIds.push_back(node.node_id);
			return true;
		});
		EXPECT_EQ(nodeIds, (std::vector<uint64_t>{1U, 2U, 100U, 101U}));
		std::vector<uint64_t> edgeIds;
		snapshot->visitEdges(4U, [&edgeIds](const LogicalGraphEdge &value) {
			edgeIds.push_back(value.edge_id);
			return true;
		});
		EXPECT_EQ(edgeIds, (std::vector<uint64_t>{10U, 200U, 201U, 202U}));

		overlay.replaceLogicalNodeProperties({{100U, {}}});
		overlay.replaceLogicalEdgeProperties({{200U, {}}});
		snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_TRUE(snapshot->readNode(100U).properties.empty());
		EXPECT_TRUE(snapshot->readEdge(200U).properties.empty());
	}

	TEST(LogicalGraphTransactionOverlayTest, ReplacesPersistedStateAndRejectsMalformedReadPages) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		const LogicalGraphMutationEntityReference persistedEdge{
				LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 10U};
		LogicalGraphMutationBatch batch;
		batch.node_label_replacements = {{persistedNode, {"Updated", "Base"}}};
		batch.property_replacements = {{persistedNode, {{"name", PropertyValue("Grace")},
																 {"rank", PropertyValue(int64_t(7))}}},
										 {persistedEdge, {{"weight", PropertyValue(int64_t(3))}}}};
		EXPECT_TRUE(overlay.executeLogicalGraphMutationBatch(batch).created_node_ids.empty());

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode node = snapshot->readNode(1U);
		EXPECT_EQ(node.labels, (std::vector<std::string>{"Base", "Updated"}));
		EXPECT_EQ(node.properties.size(), 2U);
		const LogicalGraphEdge edge = snapshot->readEdge(10U);
		ASSERT_EQ(edge.properties.size(), 1U);
		EXPECT_EQ(edge.properties.front().key, "weight");
		EXPECT_THROW(snapshot->visitNodes(0U, [](const LogicalGraphNode &) { return true; }), std::invalid_argument);
		EXPECT_THROW(snapshot->visitEdges(0U, [](const LogicalGraphEdge &) { return true; }), std::invalid_argument);
		EXPECT_THROW(snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 0U,
															 [](const LogicalGraphEdge &) { return true; }),
				 std::invalid_argument);

		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.return_empty_nonterminal_node_page = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);
		base.return_zero_cost_nonterminal_node_page = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);
		base.return_zero_cost_nonterminal_node_page = false;
		base.return_oversized_node_metadata_cost = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);
		base.return_empty_nonterminal_node_page = false;
		base.return_oversized_node_metadata_cost = false;
		base.return_empty_nonterminal_adjacency_page = true;
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)),
				 std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsInvalidMutationsAndInvalidBaseFenceWithoutLeakingState) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		LogicalGraphMutationBatch duplicateProperties;
		duplicateProperties.node_creates = {{{"Created"}, {{"key", PropertyValue(int64_t(1))},
																					 {"key", PropertyValue(int64_t(2))}}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(duplicateProperties)),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(overlay.createLogicalNodes({{{""}, {}}})), std::invalid_argument);
		LogicalGraphMutationBatch untypedEdge;
		untypedEdge.edge_creates = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"", {}}};
		EXPECT_EQ(overlay.executeLogicalGraphMutationBatch(untypedEdge).created_edge_ids, (std::vector<uint64_t>{200U}));
		std::unique_ptr<LogicalGraphReadSnapshot> untypedSnapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_TRUE(untypedSnapshot->readEdge(200U).type.empty());

		LogicalGraphMutationBatch invalidReference;
		invalidReference.edge_creates = {{
				{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U},
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(invalidReference)),
				 std::invalid_argument);
		EXPECT_THROW(overlay.deleteLogicalEdges({{0U}}), std::invalid_argument);

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readNode(100U)), std::out_of_range);
		EXPECT_EQ(snapshot->readNode(1U).labels, (std::vector<std::string>{"Base"}));
		static_cast<void>(transaction.commit());
		EXPECT_THROW(static_cast<void>(overlay.openLogicalGraphReadSnapshot()), std::logic_error);
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch({})), std::logic_error);

		InMemoryLogicalTransaction invalidFenceTransaction;
		static_cast<InMemoryLogicalSnapshot &>(invalidFenceTransaction.readSnapshot()).fence = {};
		EXPECT_THROW(static_cast<void>(LogicalGraphTransactionOverlay(invalidFenceTransaction)), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RollsBackOverlayStateWhenTransactionIdentityResultsAreInconsistent) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		transaction.setIncompletePreview(true);
		EXPECT_THROW(static_cast<void>(overlay.createLogicalNodes({{{"Incomplete"}, {}}})), std::logic_error);
		transaction.setIncompletePreview(false);
		transaction.setMismatchedStageResult(true);
		EXPECT_THROW(static_cast<void>(overlay.createLogicalNodes({{{"Mismatch"}, {}}})), std::logic_error);
		transaction.setMismatchedStageResult(false);

		InMemoryLogicalTransaction duplicateNodesTransaction;
		LogicalGraphTransactionOverlay duplicateNodes(duplicateNodesTransaction);
		duplicateNodesTransaction.setDuplicateNodeIds(true);
		EXPECT_THROW(static_cast<void>(duplicateNodes.createLogicalNodes({{{"One"}, {}}, {{"Two"}, {}}})),
				 std::logic_error);
		InMemoryLogicalTransaction duplicateEdgesTransaction;
		LogicalGraphTransactionOverlay duplicateEdges(duplicateEdgesTransaction);
		duplicateEdgesTransaction.setDuplicateEdgeIds(true);
		EXPECT_THROW(static_cast<void>(duplicateEdges.createLogicalEdges(
								  {{1U, 2U, "REL", {}}, {1U, 2U, "REL", {}}})),
				 std::logic_error);

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readNode(100U)), std::out_of_range);

		// A failed stage after a direct-property replacement must restore both the
		// entity image and its old fixed-record bridge visibility.
		InMemoryLogicalTransaction propertyTransaction;
		LogicalGraphTransactionOverlay propertyOverlay(propertyTransaction);
		propertyTransaction.setMismatchedStageResult(true);
		LogicalGraphMutationBatch mismatchedPropertyBatch;
		mismatchedPropertyBatch.node_creates = {{{"Mismatch"}, {}}};
		mismatchedPropertyBatch.property_replacements = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				{{"name", PropertyValue("Grace")}}}};
		EXPECT_THROW(static_cast<void>(propertyOverlay.executeLogicalGraphMutationBatch(mismatchedPropertyBatch)),
				 std::logic_error);
		propertyTransaction.setMismatchedStageResult(false);
		std::unique_ptr<LogicalGraphReadSnapshot> propertySnapshot = propertyOverlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNode unchanged = propertySnapshot->readNode(1U);
		EXPECT_EQ(unchanged.properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"name", PropertyValue("Ada")}}));
	}

	TEST(LogicalGraphTransactionOverlayTest, VisitorsBoundPageMaterializationAcrossBatchesAndEarlyStop) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.nodes.clear();
			base.edges.clear();
			std::vector<uint64_t> expectedNodes, expectedEdges;
			const PropertyValue payload(std::string(1024U, 'x'));
			for (uint64_t id = 1U; id <= 1025U; ++id) {
				base.nodes.emplace(id, LogicalGraphNode{id, {"N"}, {{"payload", payload}}});
				base.edges.emplace(id + 9U, LogicalGraphEdge{id + 9U, 1U, 2U, "L", {{"payload", payload}}});
				if (id != 3U) expectedNodes.push_back(id);
				if (id + 9U != 12U) expectedEdges.push_back(id + 9U);
			}
			LogicalGraphTransactionOverlay overlay(transaction);
			overlay.deleteLogicalEdges({{12U}});
			overlay.deleteLogicalNodes({{3U, false}});
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
					? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
					: overlay.openLogicalGraphReadSnapshot();
			const auto checkBounds = [](const std::vector<size_t> &bounds) {
				ASSERT_FALSE(bounds.empty());
				EXPECT_TRUE(std::ranges::all_of(bounds, [](size_t bound) { return bound > 0U && bound <= 512U; }));
			};
			std::vector<uint64_t> nodes;
			snapshot->visitNodes(expectedNodes.size(), [&](const LogicalGraphNode &node) {
				nodes.push_back(node.node_id);
				EXPECT_EQ(node.properties, (std::vector<LogicalGraphPropertyEntry>{{"payload", payload}}));
				return true;
			});
			EXPECT_EQ(nodes, expectedNodes);
			checkBounds(base.node_scan_bounds);
			EXPECT_GE(base.node_scan_bounds.size(), 2U);
			base.node_scan_bounds.clear();
			nodes.clear();
			snapshot->visitNodes(expectedNodes.size(), [&](const LogicalGraphNode &node) {
				nodes.push_back(node.node_id);
				return false;
			});
			EXPECT_EQ(nodes, (std::vector<uint64_t>{1U}));
			checkBounds(base.node_scan_bounds);
			// Filtering a deleted record can refill the same bounded output page.
			EXPECT_LE(base.node_scan_bounds.size(), 2U);
			nodes.clear();
			EXPECT_THROW(snapshot->visitNodes(513U, [&](const LogicalGraphNode &node) {
				nodes.push_back(node.node_id);
				return true;
			}), std::runtime_error);
			EXPECT_EQ(nodes, (std::vector<uint64_t>(expectedNodes.begin(), expectedNodes.begin() + 513U)));

			std::vector<uint64_t> edges;
			snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, expectedEdges.size(),
					[&](const LogicalGraphEdge &edge) {
					edges.push_back(edge.edge_id);
					EXPECT_EQ(edge.properties, (std::vector<LogicalGraphPropertyEntry>{{"payload", payload}}));
					return true;
				});
			EXPECT_EQ(edges, expectedEdges);
			checkBounds(base.adjacency_scan_bounds);
			EXPECT_GE(base.adjacency_scan_bounds.size(), 2U);
			base.adjacency_scan_bounds.clear();
			edges.clear();
			snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, expectedEdges.size(),
					[&](const LogicalGraphEdge &edge) {
					edges.push_back(edge.edge_id);
					return false;
				});
			EXPECT_EQ(edges, (std::vector<uint64_t>{10U}));
			checkBounds(base.adjacency_scan_bounds);
			EXPECT_LE(base.adjacency_scan_bounds.size(), 2U);
			edges.clear();
			EXPECT_THROW(snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 513U,
					[&](const LogicalGraphEdge &edge) {
					edges.push_back(edge.edge_id);
					return true;
				}), std::runtime_error);
			EXPECT_EQ(edges, (std::vector<uint64_t>(expectedEdges.begin(), expectedEdges.begin() + 513U)));
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, PreservesBaseContinuationsAndEnforcesVisitBounds) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();

		const LogicalGraphAdjacencyPage first = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		ASSERT_EQ(first.edges, (std::vector<LogicalGraphEdge>{base.edges.at(10U)}));
		ASSERT_TRUE(first.continuation.has_value());
		const LogicalGraphAdjacencyPage second = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U);
		EXPECT_EQ(second.edges, (std::vector<LogicalGraphEdge>{base.edges.at(11U)}));
		EXPECT_FALSE(second.continuation.has_value());

		std::vector<uint64_t> visited;
		snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
													[&visited](const LogicalGraphEdge &edge) {
														visited.push_back(edge.edge_id);
														return false;
													});
		EXPECT_EQ(visited, (std::vector<uint64_t>{10U}));
		EXPECT_THROW(snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 1U,
															 [](const LogicalGraphEdge &) { return true; }),
				 std::runtime_error);

		overlay.deleteLogicalEdges({{10U}});
		snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(10U)), std::runtime_error);
		std::vector<uint64_t> visibleEdges;
		snapshot->visitEdges(2U, [&visibleEdges](const LogicalGraphEdge &edge) {
			visibleEdges.push_back(edge.edge_id);
			return true;
		});
		EXPECT_EQ(visibleEdges, (std::vector<uint64_t>{11U}));
	}

	TEST(LogicalGraphTransactionOverlayTest, SparseAdjacencyDoesNotAllocateItsMaximumRecordBound) {
		for (const bool vectorSnapshot: {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			const std::string type(4096U, 't');
			base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, type, {}});
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, type, {}}}), (std::vector<uint64_t>{200U}));
			auto snapshot = vectorSnapshot
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			const auto bound = std::numeric_limits<size_t>::max();
			const auto topology = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, bound);
			ASSERT_EQ(topology.edges.size(), 3U);
			EXPECT_LE(topology.edges.capacity(), 16U);
			EXPECT_FALSE(topology.continuation.has_value());
			EXPECT_EQ(topology.edges[1].edge_id, 11U);
			EXPECT_EQ(topology.edges[1].type, type);
			EXPECT_EQ(topology.edges[2].edge_id, 200U);
			EXPECT_EQ(topology.edges[2].type, type);
			const auto complete = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, bound);
			ASSERT_EQ(complete.edges.size(), 3U);
			EXPECT_LE(complete.edges.capacity(), 16U);
			EXPECT_EQ(complete.edges[1].type, type);
			EXPECT_EQ(base.edges.at(11U).type, type);
			for (uint64_t id = 12U; id < 30U; ++id)
				base.edges.emplace(id, LogicalGraphEdge{id, 1U, 2U, type, {}});
			const auto grown = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 21U);
			ASSERT_EQ(grown.edges.size(), 21U);
			EXPECT_LE(grown.edges.capacity(), 21U);
			EXPECT_FALSE(grown.continuation.has_value());
			ASSERT_NO_THROW(overlay.deleteLogicalNodes({{2U, true, 21U}}));
			const auto empty = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, bound);
			EXPECT_TRUE(empty.edges.empty());
			EXPECT_EQ(empty.edges.capacity(), 0U);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, ScansTopologyWithoutMaterializingEdgesAcrossCreatedContinuation) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND", {{"payload", PropertyValue("large")}}});
		LogicalGraphTransactionOverlay overlay(transaction);
		ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "CREATED_A", {}}, {1U, 2U, "CREATED_B", {}}}),
				  (std::vector<uint64_t>{200U, 201U}));
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();

		std::vector<uint64_t> ids;
		std::optional<LogicalGraphAdjacencyContinuation> continuation;
		for (;;) {
			const LogicalGraphAdjacencyTopologyPage page = snapshot->scanAdjacentEdgeTopologies(
					1U, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 1U);
			for (const auto &edge: page.edges) {
				ids.push_back(edge.edge_id);
				EXPECT_TRUE(edge.type == "BASE_LINK" || edge.type == "SECOND" || edge.type == "CREATED_A" ||
								edge.type == "CREATED_B");
			}
			if (!page.continuation.has_value()) {
				break;
			}
			continuation = page.continuation;
		}
		EXPECT_EQ(ids, (std::vector<uint64_t>{10U, 11U, 200U, 201U}));
		EXPECT_GT(transaction.topologyScanCalls(), 0U);
		EXPECT_EQ(transaction.readEdgeCalls(), 0U);

		InMemoryLogicalTransaction deletedEndpointTransaction;
		LogicalGraphTransactionOverlay deletedEndpointOverlay(deletedEndpointTransaction);
		ASSERT_NO_THROW(deletedEndpointOverlay.deleteLogicalNodes({{2U, true, 1U}}));
		const auto deletedSnapshot = deletedEndpointOverlay.openLogicalGraphReadSnapshot();
		EXPECT_TRUE(deletedSnapshot->scanAdjacentEdgeTopologies(
					1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 4U).edges.empty());
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsMalformedCompleteAdjacencyBeforeOverlayFiltering) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalNodes({{{"Added"}, {}}}), (std::vector<uint64_t>{100U}));
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
					? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
					: overlay.openLogicalGraphReadSnapshot();
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			const auto reject = [&](LogicalGraphAdjacencyPage page,
					std::optional<LogicalGraphAdjacencyContinuation> continuation = std::nullopt) {
				base.forced_adjacency_page = std::move(page);
				EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
						1U, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 2U)), std::runtime_error);
			};
			reject({{{10U, 1U, 2U, "L"}, {11U, 1U, 2U, "L"}, {12U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{0U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{10U, 1U, 2U, "L"}, {10U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{11U, 1U, 2U, "L"}, {10U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{10U, 1U, 0U, "L"}}, std::nullopt});
			reject({{{10U, 2U, 1U, "L"}}, std::nullopt});
			reject({{{10U, 1U, 2U, "L"}}, LogicalGraphAdjacencyContinuation{9U, 11U}});
			reject({{{10U, 1U, 2U, "L"}}, LogicalGraphAdjacencyContinuation{10U, 0U}});
			reject({{{10U, 1U, 2U, "L"}}, LogicalGraphAdjacencyContinuation{10U, 10U}});
			reject({{}, LogicalGraphAdjacencyContinuation{10U, 11U}});
			reject({{{11U, 1U, 2U, "L"}}, std::nullopt}, LogicalGraphAdjacencyContinuation{9U, 10U});
			base.forced_adjacency_page.reset();
			EXPECT_EQ(snapshot->scanAdjacentEdges(
					1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U).edges.size(), 1U);
			overlay.deleteLogicalEdges({{10U}});
			const size_t readsAfterDeletion = transaction.readEdgeCalls();
			reject({{{10U, 1U, 2U, "L"}, {10U, 1U, 2U, "L"}}, std::nullopt});
			base.forced_adjacency_page.reset();
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
					100U, static_cast<LogicalGraphReadDirection>(255U), std::nullopt, 2U)), std::invalid_argument);
			EXPECT_EQ(transaction.readEdgeCalls(), readsAfterDeletion);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsMalformedBaseTopologyPagesBeforeOverlayFiltering) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalNodes({{{"Added"}, {}}}), (std::vector<uint64_t>{100U}));
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
					? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
					: overlay.openLogicalGraphReadSnapshot();
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			const auto reject = [&](LogicalGraphAdjacencyTopologyPage page,
					std::optional<LogicalGraphAdjacencyContinuation> continuation = std::nullopt) {
				base.forced_topology_page = std::move(page);
				EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgeTopologies(
						1U, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 2U)), std::runtime_error);
			};
			reject({{{10U, 1U, 2U, "L"}, {11U, 1U, 2U, "L"}, {12U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{0U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{10U, 1U, 2U, "L"}, {10U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{11U, 1U, 2U, "L"}, {10U, 1U, 2U, "L"}}, std::nullopt});
			reject({{{10U, 1U, 0U, "L"}}, std::nullopt});
			reject({{{10U, 2U, 1U, "L"}}, std::nullopt});
			reject({{{10U, 1U, 2U, "L"}}, LogicalGraphAdjacencyContinuation{9U, 11U}});
			reject({{{10U, 1U, 2U, "L"}}, LogicalGraphAdjacencyContinuation{10U, 0U}});
			reject({{{10U, 1U, 2U, "L"}}, LogicalGraphAdjacencyContinuation{10U, 10U}});
			reject({{{11U, 1U, 2U, "L"}}, std::nullopt}, LogicalGraphAdjacencyContinuation{9U, 10U});
			base.forced_topology_page.reset();
			EXPECT_EQ(snapshot->scanAdjacentEdgeTopologies(
					1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 2U).edges.size(), 1U);
			EXPECT_EQ(transaction.readEdgeCalls(), 0U);
			overlay.deleteLogicalEdges({{10U}});
			const size_t readsAfterDeletion = transaction.readEdgeCalls();
			reject({{{10U, 1U, 2U, "L"}, {10U, 1U, 2U, "L"}}, std::nullopt});
			base.forced_topology_page.reset();
			EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgeTopologies(
					100U, static_cast<LogicalGraphReadDirection>(255U), std::nullopt, 2U)), std::invalid_argument);
			EXPECT_EQ(transaction.readEdgeCalls(), readsAfterDeletion);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, TopologyContinuationsReplayAcrossDeletedGapsAndDifferentPageSizes) {
		for (const bool vectorSnapshot: {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.native_topology_pages = true;
			const std::string type(4096U, 't');
			for (uint64_t id = 11U; id <= 14U; ++id)
				base.edges.emplace(id, LogicalGraphEdge{id, 1U, 2U, type, {}});
			LogicalGraphTransactionOverlay overlay(transaction);
			const auto created = overlay.createLogicalEdges({{1U, 2U, type, {}}, {1U, 2U, type, {}}});
			ASSERT_EQ(created, (std::vector<uint64_t>{200U, 201U}));
			overlay.deleteLogicalEdges({{11U}, {13U}});
			const size_t readsBeforeScan = transaction.readEdgeCalls();
			auto snapshot = vectorSnapshot
				? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
				: overlay.openLogicalGraphReadSnapshot();
			const auto first = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1U);
			ASSERT_EQ(first.edges.size(), 1U);
			ASSERT_TRUE(first.continuation.has_value());
			EXPECT_EQ(first.continuation->next_edge_id, 12U);
			const auto middle = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 3U);
			ASSERT_EQ(middle.edges.size(), 3U);
			EXPECT_EQ(middle.edges[0].edge_id, 12U);
			EXPECT_EQ(middle.edges[1].edge_id, 14U);
			EXPECT_EQ(middle.edges[2].edge_id, 200U);
			ASSERT_TRUE(middle.continuation.has_value());
			const auto last = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, middle.continuation, 1U);
			ASSERT_EQ(last.edges.size(), 1U);
			EXPECT_EQ(last.edges[0].edge_id, 201U);
			EXPECT_FALSE(last.continuation.has_value());
			const auto replay = snapshot->scanAdjacentEdgeTopologies(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 4U);
			ASSERT_EQ(replay.edges.size(), 4U);
			EXPECT_FALSE(replay.continuation.has_value());
			for (size_t index = 0U; index < middle.edges.size(); ++index)
				EXPECT_EQ(replay.edges[index], middle.edges[index]);
			EXPECT_EQ(replay.edges.back(), last.edges[0]);
			for (const auto &edge: replay.edges) EXPECT_EQ(edge.type, type);
			EXPECT_EQ(transaction.readEdgeCalls(), readsBeforeScan);
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, TopologyContinuationsPointToVisibleRowsAcrossDeletedGaps) {
		for (const bool vectorSnapshot: {false, true}) {
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.native_topology_pages = true;
			for (uint64_t id = 11U; id <= 13U; ++id) base.edges.emplace(id, LogicalGraphEdge{id, 1U, 2U, "BASE", {}});
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "CREATED_A", {}}, {1U, 2U, "CREATED_B", {}}}),
					(std::vector<uint64_t>{200U, 201U}));
			overlay.deleteLogicalEdges({{11U}, {12U}, {200U}});
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
					? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
					: overlay.openLogicalGraphReadSnapshot();
			EXPECT_TRUE(snapshot->supportsNativeAdjacentEdgeTopologyPages());
			EXPECT_TRUE(snapshot->supportsNativeAdjacentEdgeRefScans());
			const size_t readsBeforeScan = transaction.readEdgeCalls();
			std::vector<uint64_t> ids;
			std::optional<LogicalGraphAdjacencyContinuation> continuation;
			for (size_t probe = 0U; probe < 5U; ++probe) {
				const auto page = snapshot->scanAdjacentEdgeTopologies(
						1U, LogicalGraphReadDirection::LGRD_OUTGOING, continuation, 1U);
				ASSERT_EQ(page.edges.size(), 1U);
				if (continuation.has_value()) EXPECT_EQ(page.edges.front().edge_id, continuation->next_edge_id);
				ids.push_back(page.edges.front().edge_id);
				continuation = page.continuation;
				if (!continuation.has_value()) break;
				EXPECT_EQ(continuation->previous_edge_id, ids.back());
			}
			EXPECT_FALSE(continuation.has_value());
			EXPECT_EQ(ids, (std::vector<uint64_t>{10U, 13U, 201U}));
			const auto refs = snapshot->scanAdjacentEdgeRefs(
					2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 2U);
			ASSERT_EQ(refs.edges.size(), 2U);
			ASSERT_TRUE(refs.continuation.has_value());
			EXPECT_EQ(refs.continuation->next_edge_id, 201U);
			const auto last = snapshot->scanAdjacentEdgeRefs(
					2U, LogicalGraphReadDirection::LGRD_INCOMING, refs.continuation, 2U);
			ASSERT_EQ(last.edges.size(), 1U);
			EXPECT_EQ(last.edges.front().entity_id, 201U);
			EXPECT_FALSE(last.continuation.has_value());
			EXPECT_EQ(transaction.readEdgeCalls(), readsBeforeScan);
			if (!vectorSnapshot) {
				graph::query::execution::LogicalGraphReadExecutor executor(overlay);
				graph::query::execution::LogicalGraphReadRequest request;
				request.expansion = graph::query::execution::LogicalGraphExpansion{};
				request.bounds = {2U, 8U, 8U, 8U};
				std::vector<uint64_t> executedIds;
				static_cast<void>(executor.execute(request, [&](const auto &row) {
					executedIds.push_back(row.edge->edge_id);
				}));
				EXPECT_EQ(executedIds, (std::vector<uint64_t>{10U, 13U, 201U}));
				EXPECT_EQ(transaction.readEdgeBatchCalls(), 1U);
				EXPECT_EQ(transaction.readNodeBatchCalls(), 1U);
			}
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, ResumesStagedAdjacencyByIdWithoutRescanningItsPrefix) {
		for (const bool topologyOnly: {false, true}) {
			for (const auto direction: {LogicalGraphReadDirection::LGRD_OUTGOING, LogicalGraphReadDirection::LGRD_INCOMING}) {
				InMemoryLogicalTransaction transaction;
				LogicalGraphTransactionOverlay overlay(transaction);
				const uint64_t root = overlay.createLogicalNodes({{{"Added"}, {}}}).front();
				std::vector<LogicalGraphEdgeCreateRequest> requests;
				for (size_t index = 0U; index < 64U; ++index) {
					requests.push_back({index % 2U == 0U ? root : 1U, index % 2U == 0U ? 1U : root,
							"LINK", {{"payload", PropertyValue(std::string(256U, 'x'))}}});
				}
				const auto created = overlay.createLogicalEdges(requests);
				std::vector<LogicalGraphEdgeDeleteRequest> deletions;
				std::vector<uint64_t> expected;
				for (size_t index = 0U; index < created.size(); ++index) {
					if (index % 8U == 0U) deletions.push_back({created[index]});
					else if ((index % 2U == 0U) == (direction == LogicalGraphReadDirection::LGRD_OUTGOING))
						expected.push_back(created[index]);
				}
				overlay.deleteLogicalEdges(deletions);
				auto snapshot = overlay.openLogicalGraphReadSnapshot();
				const bool valuesWereEnabled = graph::debug::PerfTrace::areValuesEnabled();
				graph::debug::PerfTrace::setValueCollectionEnabled(true);
				graph::debug::PerfTrace::reset();
				std::vector<uint64_t> actual;
				std::optional<LogicalGraphAdjacencyContinuation> continuation;
				for (size_t probe = 0U; probe < 64U; ++probe) {
					if (topologyOnly) {
						const auto page = snapshot->scanAdjacentEdgeTopologies(root, direction, continuation, 3U);
						for (const auto &edge: page.edges) actual.push_back(edge.edge_id);
						continuation = page.continuation;
					} else {
						const auto page = snapshot->scanAdjacentEdges(root, direction, continuation, 3U);
						for (const auto &edge: page.edges) {
							actual.push_back(edge.edge_id);
							EXPECT_EQ(edge.properties.size(), 1U);
						}
						continuation = page.continuation;
					}
					if (!continuation.has_value()) break;
				}
				const auto trace = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setValueCollectionEnabled(valuesWereEnabled);
				EXPECT_FALSE(continuation.has_value());
				EXPECT_EQ(actual, expected);
				ASSERT_TRUE(trace.contains("native_graph.read.overlay_created_adjacency_candidates"));
				EXPECT_LE(trace.at("native_graph.read.overlay_created_adjacency_candidates").totalValue, 3 * 64);
			}
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, AppliesDetachDeletionToBaseAndCreatedIncidentEdges) {
		InMemoryLogicalTransaction baseTransaction;
		LogicalGraphTransactionOverlay baseOverlay(baseTransaction);
		EXPECT_NO_THROW(baseOverlay.deleteLogicalNodes({{1U, true, 1U}}));
		std::unique_ptr<LogicalGraphReadSnapshot> baseSnapshot = baseOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(baseSnapshot->readNode(1U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(baseSnapshot->readEdge(10U)), std::runtime_error);

		InMemoryLogicalTransaction createdTransaction;
		LogicalGraphTransactionOverlay createdOverlay(createdTransaction);
		const uint64_t created = createdOverlay.createLogicalNodes({{{"Created"}, {}}}).front();
		const uint64_t edge = createdOverlay.createLogicalEdges({{1U, created, "REL", {}}}).front();
		EXPECT_THROW(createdOverlay.deleteLogicalNodes({{created, false, 0U}}), std::runtime_error);
		EXPECT_NO_THROW(createdOverlay.deleteLogicalNodes({{created, true, 1U}}));
		std::unique_ptr<LogicalGraphReadSnapshot> createdSnapshot = createdOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(createdSnapshot->readNode(created)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(createdSnapshot->readEdge(edge)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(createdOverlay.createLogicalEdges({{created, 1U, "REL", {}}})),
				 std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, CoversResumableNodeAndAdjacencyBoundaryFailures) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		ASSERT_EQ(overlay.createLogicalNodes({{{"Created"}, {}}, {{"Created"}, {}}}).size(), 2U);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 0U, 1U)), std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 0U)), std::invalid_argument);
		EXPECT_THROW(snapshot->visitNodes(2U, [](const LogicalGraphNode &) { return true; }), std::runtime_error);
		size_t visited = 0U;
		snapshot->visitNodes(4U, [&visited](const LogicalGraphNode &) {
			++visited;
			return false;
		});
		EXPECT_EQ(visited, 1U);

		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 100U, LogicalGraphReadDirection::LGRD_OUTGOING,
								 LogicalGraphAdjacencyContinuation{1U, 2U}, 1U)),
				 std::runtime_error);
		EXPECT_TRUE(overlay.executeLogicalGraphMutationBatch({}).created_node_ids.empty());
	}

	TEST(LogicalGraphTransactionOverlayTest, FiltersDeletedBasePagesAndRetainsCreatedContinuationValidation) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.deleteLogicalEdges({{10U}});
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const LogicalGraphAdjacencyPage visible = snapshot->scanAdjacentEdges(
				1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		EXPECT_EQ(visible.edges, (std::vector<LogicalGraphEdge>{base.edges.at(11U)}));

		const uint64_t node = overlay.createLogicalNodes({{{"Created"}, {}}}).front();
		ASSERT_EQ(overlay.createLogicalEdges({{node, node, "REL", {}}, {node, node, "REL", {}}}).size(), 2U);
		const LogicalGraphAdjacencyPage first = snapshot->scanAdjacentEdges(
				node, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		ASSERT_TRUE(first.continuation.has_value());
		overlay.deleteLogicalEdges({{200U}, {201U}});
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 node, LogicalGraphReadDirection::LGRD_OUTGOING, first.continuation, 1U)),
				 std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsInvalidReferenceKindsAndDeletedEntityMutations) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		const LogicalGraphMutationEntityReference persistedEdge{
				LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 10U};

		LogicalGraphMutationBatch wrongLabelKind;
		wrongLabelKind.node_label_replacements = {{persistedEdge, {"Wrong"}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(wrongLabelKind)), std::invalid_argument);
		LogicalGraphMutationBatch unknownPropertyKind;
		unknownPropertyKind.property_replacements = {{{static_cast<LogicalGraphMutationEntityKind>(99U),
																						  LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(unknownPropertyKind)),
				 std::invalid_argument);
		LogicalGraphMutationBatch invalidCreatedNode;
		invalidCreatedNode.node_creates = {{{"Created"}, {}}};
		invalidCreatedNode.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
																						 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 1U}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(invalidCreatedNode)),
				 std::invalid_argument);
		LogicalGraphMutationBatch invalidReferenceKind;
		invalidReferenceKind.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
																						  static_cast<LogicalGraphMutationReferenceKind>(99U), 1U}, {}}};
		EXPECT_THROW(static_cast<void>(overlay.executeLogicalGraphMutationBatch(invalidReferenceKind)),
				 std::invalid_argument);
		LogicalGraphMutationBatch createdEdgeProperty;
		createdEdgeProperty.edge_creates = {{persistedNode,
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		createdEdgeProperty.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_EDGE,
																						  LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U},
																						 {{"rank", PropertyValue(int64_t(2))}}}};
		EXPECT_EQ(overlay.executeLogicalGraphMutationBatch(createdEdgeProperty).created_edge_ids,
				  (std::vector<uint64_t>{200U}));
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(snapshot->readEdge(200U).properties,
				  (std::vector<LogicalGraphPropertyEntry>{{"rank", PropertyValue(int64_t(2))}}));

		overlay.deleteLogicalEdges({{10U}, {200U}});
		EXPECT_THROW(overlay.replaceLogicalEdgeProperties({{10U, {{"rank", PropertyValue(int64_t(1))}}}}),
				 std::runtime_error);
		overlay.deleteLogicalNodes({{1U, false, 0U}});
		EXPECT_THROW(overlay.replaceLogicalNodeProperties({{1U, {{"rank", PropertyValue(int64_t(1))}}}}),
				 std::runtime_error);
		EXPECT_THROW(overlay.replaceLogicalNodeProperties({{0U, {}}}), std::invalid_argument);

	}

	TEST(LogicalGraphTransactionOverlayTest, DefensivelyHidesBaseEdgesWhoseEndpointWasDeleted) {
		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		// A lower layer must normally expose matching adjacency and direct Edge
		// reads. Keep the overlay's endpoint visibility check tested even if a
		// malformed lower snapshot violates that invariant.
		base.hide_edges_from_adjacency = true;
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.deleteLogicalNodes({{1U, false, 0U}});

		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(10U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, EnforcesNodeScanBudgetAndTerminalTraversalPaths) {
		InMemoryLogicalTransaction terminalTransaction;
		LogicalGraphTransactionOverlay terminalOverlay(terminalTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> terminal = terminalOverlay.openLogicalGraphReadSnapshot();
		std::vector<uint64_t> terminalNodes;
		terminal->visitNodes(3U, [&terminalNodes](const LogicalGraphNode &node) {
			terminalNodes.push_back(node.node_id);
			return true;
		});
		EXPECT_EQ(terminalNodes, (std::vector<uint64_t>{1U, 2U}));

		InMemoryLogicalTransaction transaction;
		auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
		base.edges.clear();
		LogicalGraphTransactionOverlay overlay(transaction);
		overlay.deleteLogicalNodes({{1U, false, 0U}});
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::runtime_error);

		ASSERT_EQ(overlay.createLogicalNodes({{{"Created"}, {}}, {{"Created"}, {}}}).size(), 2U);
		const LogicalGraphNodePage createdPage = snapshot->scanNodesAfter(2U, 1U, 1U);
		ASSERT_EQ(createdPage.nodes.size(), 1U);
		EXPECT_TRUE(createdPage.may_have_more);
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 0U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdges(
								 1U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 0U)),
				 std::invalid_argument);
	}

	TEST(LogicalGraphTransactionOverlayTest, DefensivelyEnforcesVisitorBoundsAndStopsCreatedVisitors) {
		InMemoryLogicalTransaction malformedVisitorTransaction;
		auto &malformedBase = static_cast<InMemoryLogicalSnapshot &>(malformedVisitorTransaction.readSnapshot());
		malformedBase.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		malformedBase.ignore_edge_visit_bound = true;
		LogicalGraphTransactionOverlay malformedVisitorOverlay(malformedVisitorTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> malformedSnapshot =
				malformedVisitorOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(malformedSnapshot->visitEdges(1U, [](const LogicalGraphEdge &) { return true; }), std::runtime_error);

		InMemoryLogicalTransaction createdVisitorTransaction;
		LogicalGraphTransactionOverlay createdVisitorOverlay(createdVisitorTransaction);
		ASSERT_EQ(createdVisitorOverlay.createLogicalEdges({{1U, 2U, "CREATED", {}}}).size(), 1U);
		std::unique_ptr<LogicalGraphReadSnapshot> createdSnapshot = createdVisitorOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(createdSnapshot->visitEdges(1U, [](const LogicalGraphEdge &) { return true; }), std::runtime_error);

		createdVisitorOverlay.deleteLogicalEdges({{10U}});
		createdSnapshot = createdVisitorOverlay.openLogicalGraphReadSnapshot();
		size_t createdVisits = 0U;
		createdSnapshot->visitEdges(2U, [&createdVisits](const LogicalGraphEdge &) {
			++createdVisits;
			return false;
		});
		EXPECT_EQ(createdVisits, 1U);

		InMemoryLogicalTransaction adjacencyTransaction;
		LogicalGraphTransactionOverlay adjacencyOverlay(adjacencyTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> adjacency = adjacencyOverlay.openLogicalGraphReadSnapshot();
		std::vector<uint64_t> adjacentIds;
		adjacency->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 2U,
																				[&adjacentIds](const LogicalGraphEdge &edge) {
																					adjacentIds.push_back(edge.edge_id);
																					return true;
																				});
		EXPECT_EQ(adjacentIds, (std::vector<uint64_t>{10U}));
	}

	TEST(LogicalGraphTransactionOverlayTest, StopsEdgeVisitorsBeforeCrossingFromBaseToStagedRows) {
		for (const bool vectorSnapshot: {false, true}) {
			SCOPED_TRACE(vectorSnapshot);
			InMemoryLogicalTransaction transaction;
			auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
			base.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND", {}});
			LogicalGraphTransactionOverlay overlay(transaction);
			ASSERT_EQ(overlay.createLogicalEdges({{1U, 2U, "ADDED_A", {}}, {1U, 2U, "ADDED_B", {}}}),
					(std::vector<uint64_t>{200U, 201U}));
			std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
					? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
					: overlay.openLogicalGraphReadSnapshot();
			for (const size_t stopAt: {1U, 2U}) {
				for (const size_t bound: {stopAt, size_t(4U)}) {
					std::vector<uint64_t> ids;
					EXPECT_NO_THROW(snapshot->visitEdges(bound, [&](const LogicalGraphEdge &edge) {
						ids.push_back(edge.edge_id);
						return ids.size() < stopAt;
					}));
					EXPECT_EQ(ids, stopAt == 1U ? (std::vector<uint64_t>{10U}) : (std::vector<uint64_t>{10U, 11U}));
				}
			}
			std::vector<uint64_t> allIds;
			snapshot->visitEdges(4U, [&](const LogicalGraphEdge &edge) {
				allIds.push_back(edge.edge_id);
				return true;
			});
			EXPECT_EQ(allIds, (std::vector<uint64_t>{10U, 11U, 200U, 201U}));
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, BatchesExistenceProbesAcrossStagedAndBaseNodesWithoutPayloadReads) {
		for (const bool vectorSnapshot: {false, true}) {
			for (const bool nativeProbes: {false, true}) {
				SCOPED_TRACE(vectorSnapshot);
				SCOPED_TRACE(nativeProbes);
				InMemoryLogicalTransaction transaction;
				auto &base = static_cast<InMemoryLogicalSnapshot &>(transaction.readSnapshot());
				base.native_existence_probes = nativeProbes;
				LogicalGraphTransactionOverlay overlay(transaction);
				std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
						? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
						: overlay.openLogicalGraphReadSnapshot();
				const std::array<uint64_t, 4> ids{1U, 2U, 100U, 101U};
				EXPECT_EQ(snapshot->supportsNodeExistenceProbes(), nativeProbes);
				EXPECT_EQ(snapshot->containsNodesSorted(ids), (std::vector<bool>{true, true, false, false}));
				ASSERT_EQ(base.existence_batches.size(), 1U);
				EXPECT_EQ(base.existence_batches.back(), (std::vector<uint64_t>{1U, 2U, 100U, 101U}));
				ASSERT_EQ(overlay.createLogicalNodes({{{"Added"}, {{"payload", PropertyValue(std::string(8192U, 'x'))}}}}),
						(std::vector<uint64_t>{100U}));
				overlay.deleteLogicalNodes({{2U, true, 1U}});
				base.existence_batches.clear();
				const size_t pointReads = base.read_node_calls;
				EXPECT_EQ(snapshot->supportsNodeExistenceProbes(), nativeProbes);
				EXPECT_EQ(snapshot->containsNodesSorted(ids), (std::vector<bool>{true, false, true, false}));
				ASSERT_EQ(base.existence_batches.size(), 1U);
				EXPECT_EQ(base.existence_batches.back(), (std::vector<uint64_t>{1U, 101U}));
				EXPECT_EQ(base.read_node_calls, pointReads);
				base.existence_batches.clear();
				const std::array<uint64_t, 2> staged{2U, 100U};
				EXPECT_EQ(snapshot->containsNodesSorted(staged), (std::vector<bool>{false, true}));
				EXPECT_TRUE(snapshot->containsNodesSorted({}).empty());
				EXPECT_TRUE(base.existence_batches.empty());
				for (const auto &invalid: {std::vector<uint64_t>{0U}, {1U, 1U}, {2U, 1U}}) {
					EXPECT_THROW(static_cast<void>(snapshot->containsNodesSorted(invalid)), std::invalid_argument);
				}
				for (const auto &invalid: {std::vector<bool>{}, {true}, {true, false, true}}) {
					base.forced_existence_batch = invalid;
					EXPECT_THROW(static_cast<void>(snapshot->containsNodesSorted(ids)), std::runtime_error);
				}
				base.forced_existence_batch.reset();
				EXPECT_EQ(snapshot->containsNodesSorted(ids), (std::vector<bool>{true, false, true, false}));
				transaction.rollback();
				EXPECT_THROW(static_cast<void>(snapshot->containsNodesSorted({})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->supportsNodeExistenceProbes()), std::logic_error);
			}
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsAllStagedAndEmptyReadsAfterTransactionEnds) {
		for (const bool commitTransaction: {false, true}) {
			for (const bool vectorSnapshot: {false, true}) {
				SCOPED_TRACE(commitTransaction);
				SCOPED_TRACE(vectorSnapshot);
				InMemoryLogicalTransaction transaction;
				LogicalGraphTransactionOverlay overlay(transaction);
				ASSERT_EQ(overlay.createLogicalNodes({{{"Added"}, {{"name", PropertyValue("staged")}}}}),
						(std::vector<uint64_t>{100U}));
				ASSERT_EQ(overlay.createLogicalEdges({{100U, 2U, "ADDED", {{"weight", PropertyValue(7)}}}}),
						(std::vector<uint64_t>{200U}));
				std::unique_ptr<LogicalGraphReadSnapshot> snapshot = vectorSnapshot
						? std::unique_ptr<LogicalGraphReadSnapshot>(overlay.openLogicalGraphVectorSearchSnapshot())
						: overlay.openLogicalGraphReadSnapshot();
				const std::array<uint64_t, 1> nodeIds{100U}, edgeIds{200U};
				const std::array<std::string_view, 1> keys{"name"};
				EXPECT_TRUE(snapshot->containsNode(100U));
				EXPECT_EQ(snapshot->readNode(100U).node_id, 100U);
				EXPECT_EQ(snapshot->readEdge(200U).edge_id, 200U);
				if (commitTransaction) static_cast<void>(transaction.commit());
				else transaction.rollback();
				EXPECT_THROW(static_cast<void>(snapshot->containsNode(100U)), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->nodePropertyIsNonNull(100U, "name")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->supportsPropertyNonNullProbes()), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->supportsNativeRelationshipTypeTokens()), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->findRelationshipTypeToken("BASE_LINK")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->supportsNativeNodeRefScans()), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->supportsNativeEdgeRefScans()), std::logic_error);
				EXPECT_THROW(snapshot->visitNodeRefs(1U, [](const EntityRef &) { return false; }), std::logic_error);
				EXPECT_THROW(snapshot->visitEdgeRefs(1U, [](const EntityRef &) { return false; }), std::logic_error);
				EXPECT_THROW(snapshot->visitNodeRefsWithLabelMatches(1U, {}, [](const EntityRef &, bool) { return false; }), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->edgePropertyIsNonNull(200U, "name")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNode(100U)), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdge(200U)), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNodeShaped(100U, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdgeShaped(200U, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSorted(nodeIds, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSorted(edgeIds, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNodesShapedSorted({}, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdgesShapedSorted({}, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNodePropertiesSorted(nodeIds, "name")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertiesSorted(edgeIds, "weight")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNodePropertiesSorted({}, "name")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertiesSorted({}, "weight")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNodePropertyColumnsSorted(nodeIds, keys)), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSorted(edgeIds, keys)), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readNodePropertyColumnsSorted({}, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->readEdgePropertyColumnsSorted({}, {})), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->mutationFence()), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->findPropertyToken("name")), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->scanAdjacentEdgeTopologies(
						100U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)), std::logic_error);
				EXPECT_THROW(static_cast<void>(snapshot->scanNodesAfter(0U, 1U, 1U)), std::logic_error);
			}
		}
	}

	TEST(LogicalGraphTransactionOverlayTest, RejectsMalformedCreatedReferencesAndDetachBoundViolations) {
		InMemoryLogicalTransaction completedTransaction;
		static_cast<void>(completedTransaction.commit());
		EXPECT_THROW(static_cast<void>(LogicalGraphTransactionOverlay(completedTransaction)), std::logic_error);

		InMemoryLogicalTransaction referenceTransaction;
		LogicalGraphTransactionOverlay referenceOverlay(referenceTransaction);
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		LogicalGraphMutationBatch invalidCreatedEdgeReference;
		invalidCreatedEdgeReference.edge_creates = {{
				persistedNode,
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		invalidCreatedEdgeReference.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_EDGE,
																									 LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 1U}, {}}};
		EXPECT_THROW(static_cast<void>(referenceOverlay.executeLogicalGraphMutationBatch(invalidCreatedEdgeReference)),
				 std::invalid_argument);

		InMemoryLogicalTransaction malformedAdjacencyTransaction;
		auto &malformedBase = static_cast<InMemoryLogicalSnapshot &>(malformedAdjacencyTransaction.readSnapshot());
		malformedBase.edges.emplace(11U, LogicalGraphEdge{11U, 1U, 2U, "SECOND"});
		malformedBase.ignore_adjacency_visit_bound = true;
		LogicalGraphTransactionOverlay malformedAdjacencyOverlay(malformedAdjacencyTransaction);
		EXPECT_THROW(malformedAdjacencyOverlay.deleteLogicalNodes({{1U, true, 1U}}), std::runtime_error);

		InMemoryLogicalTransaction createdDetachTransaction;
		auto &createdBase = static_cast<InMemoryLogicalSnapshot &>(createdDetachTransaction.readSnapshot());
		createdBase.edges.clear();
		LogicalGraphTransactionOverlay createdDetachOverlay(createdDetachTransaction);
		const uint64_t createdNode = createdDetachOverlay.createLogicalNodes({{{"Created"}, {}}}).front();
		const std::vector<uint64_t> createdEdges = createdDetachOverlay.createLogicalEdges(
				{{createdNode, createdNode, "SELF", {}}, {createdNode, createdNode, "SELF", {}}});
		ASSERT_EQ(createdEdges.size(), 2U);
		EXPECT_THROW(createdDetachOverlay.deleteLogicalNodes({{createdNode, true, 1U}}), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, ValidatesVisitorsDirectionsAndEmptyDirectoryMetadata) {
		InMemoryLogicalTransaction transaction;
		LogicalGraphTransactionOverlay overlay(transaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = overlay.openLogicalGraphReadSnapshot();
		const std::function<bool(const LogicalGraphNode &)> noNodeVisitor;
		const std::function<bool(const LogicalGraphEdge &)> noEdgeVisitor;
		EXPECT_THROW(snapshot->visitNodes(1U, noNodeVisitor), std::invalid_argument);
		EXPECT_THROW(snapshot->visitEdges(1U, noEdgeVisitor), std::invalid_argument);
		EXPECT_THROW(snapshot->visitAdjacentEdges(1U, LogicalGraphReadDirection::LGRD_OUTGOING, 1U, noEdgeVisitor),
				 std::invalid_argument);

		const LogicalGraphAdjacencyPage emptyOutgoing = snapshot->scanAdjacentEdges(
				2U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U);
		EXPECT_TRUE(emptyOutgoing.edges.empty());
		EXPECT_FALSE(emptyOutgoing.continuation.has_value());
		const LogicalGraphAdjacencyPage incoming = snapshot->scanAdjacentEdges(
				2U, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U);
		EXPECT_EQ(incoming.edges, (std::vector<LogicalGraphEdge>{LogicalGraphEdge{10U, 1U, 2U, "BASE_LINK"}}));

		InMemoryLogicalTransaction emptyDirectoryTransaction;
		auto &emptyBase = static_cast<InMemoryLogicalSnapshot &>(emptyDirectoryTransaction.readSnapshot());
		emptyBase.nodes.clear();
		emptyBase.edges.clear();
		emptyBase.return_zero_cost_terminal_node_page = true;
		LogicalGraphTransactionOverlay emptyDirectoryOverlay(emptyDirectoryTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> emptyDirectory = emptyDirectoryOverlay.openLogicalGraphReadSnapshot();
		const LogicalGraphNodePage emptyNodes = emptyDirectory->scanNodesAfter(0U, 1U, 1U);
		EXPECT_TRUE(emptyNodes.nodes.empty());
		EXPECT_FALSE(emptyNodes.may_have_more);
		EXPECT_EQ(emptyNodes.metadata_pages_visited, 0U);

		InMemoryLogicalTransaction noFenceTransaction;
		static_cast<InMemoryLogicalSnapshot &>(noFenceTransaction.readSnapshot()).return_no_mutation_fence = true;
		EXPECT_THROW(static_cast<void>(LogicalGraphTransactionOverlay(noFenceTransaction)), std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, ValidatesShortCircuitingMutationInputsAndReusableProperties) {
		InMemoryLogicalTransaction invalidFenceTransaction;
		LogicalGraphTransactionOverlay invalidFenceOverlay(invalidFenceTransaction);
		LogicalGraphMutationBatch createBatch;
		createBatch.node_creates = {{{"Created"}, {}}};
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.executeLogicalGraphMutationBatch(
								 createBatch, LogicalGraphMutationFence{})),
				 LogicalGraphMutationConflict);
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.createLogicalNodes(
								 {{{"Created"}, {{"", PropertyValue(int64_t(1))}}}})),
				 std::invalid_argument);
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.createLogicalNodes({{{"Duplicate", "Duplicate"}, {}}})),
				 std::invalid_argument);

		LogicalGraphMutationBatch invalidTarget;
		invalidTarget.edge_creates = {{
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U},
				{LogicalGraphMutationEntityKind::LGMK_EDGE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		EXPECT_THROW(static_cast<void>(invalidFenceOverlay.executeLogicalGraphMutationBatch(invalidTarget)),
				 std::invalid_argument);

		InMemoryLogicalTransaction deletedTargetTransaction;
		auto &deletedTargetBase = static_cast<InMemoryLogicalSnapshot &>(deletedTargetTransaction.readSnapshot());
		deletedTargetBase.edges.clear();
		LogicalGraphTransactionOverlay deletedTargetOverlay(deletedTargetTransaction);
		deletedTargetOverlay.deleteLogicalNodes({{2U, false, 0U}});
		EXPECT_THROW(static_cast<void>(deletedTargetOverlay.createLogicalEdges({{1U, 2U, "REL", {}}})),
				 std::runtime_error);

		InMemoryLogicalTransaction propertyTransaction;
		LogicalGraphTransactionOverlay propertyOverlay(propertyTransaction);
		propertyOverlay.replaceLogicalEdgeProperties({{10U, {{"first", PropertyValue(int64_t(1))}}}});
		propertyOverlay.replaceLogicalEdgeProperties({{10U, {{"second", PropertyValue(int64_t(2))}}}});
		std::unique_ptr<LogicalGraphReadSnapshot> propertySnapshot = propertyOverlay.openLogicalGraphReadSnapshot();
		const LogicalGraphEdge updated = propertySnapshot->readEdge(10U);
		ASSERT_EQ(updated.properties.size(), 1U);
		EXPECT_EQ(updated.properties.front().key, "second");

		InMemoryLogicalTransaction incompleteEdgeTransaction;
		LogicalGraphTransactionOverlay incompleteEdgeOverlay(incompleteEdgeTransaction);
		incompleteEdgeTransaction.setIncompletePreview(true);
		EXPECT_THROW(static_cast<void>(incompleteEdgeOverlay.createLogicalEdges({{1U, 2U, "REL", {}}})),
				 std::logic_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, ValidatesTypedCreatedReferencesAndFiltersCreatedDirections) {
		const LogicalGraphMutationEntityReference persistedNode{
				LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1U};
		InMemoryLogicalTransaction wrongCreatedNodeTransaction;
		LogicalGraphTransactionOverlay wrongCreatedNodeOverlay(wrongCreatedNodeTransaction);
		LogicalGraphMutationBatch wrongCreatedNode;
		wrongCreatedNode.node_creates = {{{"Created"}, {}}};
		wrongCreatedNode.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_EDGE,
																									 LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE, 0U}, {}}};
		EXPECT_THROW(static_cast<void>(wrongCreatedNodeOverlay.executeLogicalGraphMutationBatch(wrongCreatedNode)),
				 std::invalid_argument);

		InMemoryLogicalTransaction wrongCreatedEdgeTransaction;
		LogicalGraphTransactionOverlay wrongCreatedEdgeOverlay(wrongCreatedEdgeTransaction);
		LogicalGraphMutationBatch wrongCreatedEdge;
		wrongCreatedEdge.edge_creates = {{
				persistedNode,
				{LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 2U},
				"REL", {}}};
		wrongCreatedEdge.property_replacements = {{{LogicalGraphMutationEntityKind::LGMK_NODE,
																									 LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE, 0U}, {}}};
		EXPECT_THROW(static_cast<void>(wrongCreatedEdgeOverlay.executeLogicalGraphMutationBatch(wrongCreatedEdge)),
				 std::invalid_argument);

		InMemoryLogicalTransaction directionTransaction;
		auto &directionBase = static_cast<InMemoryLogicalSnapshot &>(directionTransaction.readSnapshot());
		directionBase.edges.clear();
		LogicalGraphTransactionOverlay directionOverlay(directionTransaction);
		const uint64_t created = directionOverlay.createLogicalNodes({{{"Created"}, {}}}).front();
		ASSERT_EQ(directionOverlay.createLogicalEdges({{1U, created, "REL", {}}}), (std::vector<uint64_t>{200U}));
		std::unique_ptr<LogicalGraphReadSnapshot> directionSnapshot = directionOverlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(directionSnapshot->scanAdjacentEdges(
							  created, LogicalGraphReadDirection::LGRD_INCOMING, std::nullopt, 1U)
					  .edges,
				  (std::vector<LogicalGraphEdge>{{200U, 1U, created, "REL"}}));
		EXPECT_TRUE(directionSnapshot->scanAdjacentEdges(
							  2U, LogicalGraphReadDirection::LGRD_OUTGOING, std::nullopt, 1U)
					  .edges
					  .empty());
	}

	TEST(LogicalGraphTransactionOverlayTest, FiltersDeletedNodesAcrossScansAndDirectEdgeReads) {
		InMemoryLogicalTransaction nodeScanTransaction;
		auto &nodeScanBase = static_cast<InMemoryLogicalSnapshot &>(nodeScanTransaction.readSnapshot());
		nodeScanBase.edges.clear();
		LogicalGraphTransactionOverlay nodeScanOverlay(nodeScanTransaction);
		nodeScanOverlay.replaceLogicalNodeProperties({{1U, {{"state", PropertyValue("updated")}}}});
		ASSERT_EQ(nodeScanOverlay.createLogicalNodes({{{"Created"}, {}}, {{"Created"}, {}}}).size(), 2U);
		nodeScanOverlay.deleteLogicalNodes({{100U, false, 0U}});
		std::unique_ptr<LogicalGraphReadSnapshot> nodeScan = nodeScanOverlay.openLogicalGraphReadSnapshot();
		EXPECT_EQ(nodeScan->scanNodesAfter(100U, 1U, 1U).nodes,
				  (std::vector<LogicalGraphNode>{{101U, {"Created"}}}));
		EXPECT_TRUE(nodeScan->scanNodesAfter(101U, 1U, 1U).nodes.empty());

		InMemoryLogicalTransaction endpointTransaction;
		auto &endpointBase = static_cast<InMemoryLogicalSnapshot &>(endpointTransaction.readSnapshot());
		endpointBase.hide_edges_from_adjacency = true;
		LogicalGraphTransactionOverlay endpointOverlay(endpointTransaction);
		endpointOverlay.deleteLogicalNodes({{2U, false, 0U}});
		std::unique_ptr<LogicalGraphReadSnapshot> hiddenBaseSnapshot = endpointOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(hiddenBaseSnapshot->readEdgeShaped(
				10U, EntityReadShape{.include_type = true})), std::runtime_error);
		EXPECT_THROW(static_cast<void>(hiddenBaseSnapshot->edgePropertyIsNonNull(10U, "value")), std::runtime_error);
		// Create an overlay Edge state without marking it deleted. The direct read
		// must still reject it because its target Node is transaction-invisible.
		endpointOverlay.replaceLogicalEdgeProperties({{10U, {{"state", PropertyValue("updated")}}}});
		std::unique_ptr<LogicalGraphReadSnapshot> endpointSnapshot = endpointOverlay.openLogicalGraphReadSnapshot();
		EXPECT_THROW(static_cast<void>(endpointSnapshot->readEdge(10U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, EnforcesCreatedTargetDetachAndHiddenPropertyLookup) {
		InMemoryLogicalTransaction targetDetachTransaction;
		auto &targetDetachBase = static_cast<InMemoryLogicalSnapshot &>(targetDetachTransaction.readSnapshot());
		targetDetachBase.edges.clear();
		LogicalGraphTransactionOverlay targetDetachOverlay(targetDetachTransaction);
		ASSERT_EQ(targetDetachOverlay.createLogicalEdges({{1U, 2U, "REL", {}}}), (std::vector<uint64_t>{200U}));
		EXPECT_THROW(targetDetachOverlay.deleteLogicalNodes({{2U, false, 0U}}), std::runtime_error);
		EXPECT_NO_THROW(targetDetachOverlay.deleteLogicalNodes({{2U, true, 1U}}));

		InMemoryLogicalTransaction propertyTransaction;
		auto &propertyBase = static_cast<InMemoryLogicalSnapshot &>(propertyTransaction.readSnapshot());
		propertyBase.edges.clear();
		LogicalGraphTransactionOverlay propertyOverlay(propertyTransaction);
		std::unique_ptr<LogicalGraphReadSnapshot> snapshot = propertyOverlay.openLogicalGraphReadSnapshot();
		propertyOverlay.deleteLogicalNodes({{1U, false, 0U}});
		EXPECT_THROW(static_cast<void>(snapshot->readNode(1U)), std::runtime_error);
	}

	TEST(LogicalGraphTransactionOverlayTest, RetainsReservationOnlyBatchesAndChecksBothDetachEdgeSources) {
		InMemoryLogicalTransaction reservationTransaction;
		LogicalGraphTransactionOverlay reservationOverlay(reservationTransaction);
		LogicalGraphMutationBatch cancelledReservation;
		cancelledReservation.creation_identity_plan =
				LogicalGraphCreationIdentityPlan{{{100U, 1U}}, {}, {{200U, 1U}}, {}};
		EXPECT_EQ(reservationOverlay.executeLogicalGraphMutationBatch(cancelledReservation),
				  (LogicalGraphMutationBatchResult{}));

		InMemoryLogicalTransaction stagedBaseEdgeTransaction;
		LogicalGraphTransactionOverlay stagedBaseEdgeOverlay(stagedBaseEdgeTransaction);
		stagedBaseEdgeOverlay.replaceLogicalEdgeProperties({{10U, {{"state", PropertyValue("updated")}}}});
		EXPECT_NO_THROW(stagedBaseEdgeOverlay.deleteLogicalNodes({{1U, true, 1U}}));

		InMemoryLogicalTransaction stagedSourceEdgeTransaction;
		auto &stagedSourceBase = static_cast<InMemoryLogicalSnapshot &>(stagedSourceEdgeTransaction.readSnapshot());
		stagedSourceBase.edges.clear();
		LogicalGraphTransactionOverlay stagedSourceEdgeOverlay(stagedSourceEdgeTransaction);
		ASSERT_EQ(stagedSourceEdgeOverlay.createLogicalEdges({{1U, 2U, "REL", {}}}), (std::vector<uint64_t>{200U}));
		EXPECT_THROW(stagedSourceEdgeOverlay.deleteLogicalNodes({{1U, false, 0U}}), std::runtime_error);
	}

	} // namespace

} // namespace graph::storage
