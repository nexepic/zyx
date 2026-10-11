#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

namespace graph::query::test {

		struct ProcedureBackendState {
			storage::LogicalGraphVectorSearchResult search_result;
			std::unordered_map<uint64_t, storage::LogicalGraphNode> nodes;
			std::vector<storage::LogicalGraphEdge> edges;
			bool return_nonmonotonic_adjacency_page = false;
			bool return_null_search_snapshot = false;
			size_t cancellation_checks_before_throw = 0U;
			mutable size_t search_cancellation_checks = 0U;
			std::chrono::milliseconds search_cancellation_initial_delay{0};
			size_t training_cancellation_checks_before_throw = 0U;
			mutable size_t training_cancellation_checks = 0U;
			std::chrono::milliseconds training_prepare_delay{0};
			std::chrono::milliseconds training_preview_delay{0};
			std::chrono::milliseconds training_commit_delay{0};
			std::chrono::milliseconds projection_scan_delay{0};
			mutable size_t topology_scans = 0U;
			mutable size_t node_point_reads = 0U;
			mutable std::vector<storage::EntityReadShape> node_point_shapes;
			bool enable_existence_probes = true;
			int existence_result_adjustment = 0;
			size_t malformed_existence_call = 0U;
			mutable size_t node_batch_reads = 0U;
			mutable std::unordered_map<uint64_t, uintptr_t> last_node_body_addresses;
			mutable std::vector<size_t> node_batch_sizes;
			mutable std::vector<storage::EntityReadShape> node_batch_shapes;
			mutable size_t property_column_reads = 0U;
			mutable std::vector<std::vector<uint64_t>> property_column_ids;
			mutable std::vector<std::vector<std::string>> property_column_keys;
			bool malformed_property_columns = false;
			bool enable_property_admission = false;
			bool enable_property_presence_probes = false;
			bool enable_shaped_property_admission = false;
			mutable size_t shaped_admission_reads = 0U;
			std::function<void()> before_shaped_property_admission;
			mutable size_t admitted_property_copies = 0U;
			std::function<void()> before_property_admission;
			bool malformed_node_batch_cardinality = false;
			bool reverse_node_batch = false;
			mutable size_t batched_existence_scans = 0U;
			mutable std::vector<size_t> batched_existence_scan_sizes;
			mutable size_t full_adjacency_scans = 0U;
		};

		class ProcedureLogicalGraphSnapshot final : public storage::LogicalGraphVectorSearchSnapshot {
		public:
			explicit ProcedureLogicalGraphSnapshot(std::shared_ptr<const ProcedureBackendState> state) : state_(std::move(state)) {}

			[[nodiscard]] storage::LogicalGraphNode readNode(uint64_t nodeId) const override {
				const auto node = state_->nodes.find(nodeId);
				return node == state_->nodes.end() ? storage::LogicalGraphNode{} : node->second;
			}
			[[nodiscard]] storage::LogicalGraphNode readNodeShaped(
					uint64_t nodeId, const storage::EntityReadShape &shape) const override {
				++state_->node_point_reads;
				state_->node_point_shapes.push_back(shape);
				const auto found = state_->nodes.find(nodeId);
				if (found == state_->nodes.end()) return {};
				storage::LogicalGraphNode result;
				result.node_id = nodeId;
				if (shape.include_labels) result.labels = found->second.labels;
				if (shape.all_properties || !shape.property_tokens.empty()) result.properties = found->second.properties;
				return result;
			}
			[[nodiscard]] std::vector<storage::LogicalGraphNode> readNodesShapedSorted(
					const std::span<const uint64_t> nodeIds, const storage::EntityReadShape &shape) const override {
				++state_->node_batch_reads;
				state_->node_batch_sizes.push_back(nodeIds.size());
				state_->node_batch_shapes.push_back(shape);
				uint64_t previous = 0U;
				for (const uint64_t nodeId: nodeIds) {
					if (nodeId == 0U || nodeId <= previous) {
						throw std::invalid_argument("test Vector snapshot received an invalid sorted Node batch");
					}
					previous = nodeId;
				}
				std::vector<storage::LogicalGraphNode> result;
				result.reserve(nodeIds.size());
				for (const uint64_t nodeId: nodeIds) {
					const auto node = state_->nodes.find(nodeId);
					if (node == state_->nodes.end()) {
						throw std::runtime_error("test Vector snapshot lost a requested Node");
					}
					result.push_back(node->second);
					for (const auto &entry: result.back().properties) if (entry.key == "body") {
						if (const auto *body = std::get_if<std::string>(&entry.value.getVariant())) {
							state_->last_node_body_addresses[nodeId] = reinterpret_cast<uintptr_t>(body->data());
						}
					}
					if (!shape.include_labels) result.back().labels.clear();
					if (!shape.all_properties && shape.property_tokens.empty()) result.back().properties.clear();
				}
				if (state_->malformed_node_batch_cardinality && !result.empty()) result.pop_back();
				if (state_->reverse_node_batch) std::ranges::reverse(result);
				return result;
			}
			[[nodiscard]] bool supportsShapedPropertyReadAdmission() const override { return state_->enable_shaped_property_admission; }
			[[nodiscard]] std::vector<storage::LogicalGraphNode> readNodesShapedSortedWithAdmission(
					std::span<const uint64_t> nodeIds, const storage::EntityReadShape &shape,
					const std::function<void(size_t)> &admission) const override {
				++state_->shaped_admission_reads;
				if (state_->before_shaped_property_admission) state_->before_shaped_property_admission();
				for (uint64_t nodeId : nodeIds) {
					const auto &node = state_->nodes.at(nodeId);
					for (const auto &entry : node.properties) {
						admission(storage::CanonicalPropertyValueCodec::encodedSize(entry.value) + sizeof(uint32_t));
					}
				}
				// Copy complete Nodes only once the entire Property source was admitted.
				return readNodesShapedSorted(nodeIds, shape);
			}
			[[nodiscard]] std::vector<std::vector<PropertyValue>> readNodePropertyColumnsSorted(
					std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys) const override {
				return readPropertyColumns(nodeIds, keys, nullptr);
			}
			[[nodiscard]] bool supportsPropertyPresenceProbes() const override { return state_->enable_property_presence_probes; }
			[[nodiscard]] std::optional<bool> nodePropertyExists(uint64_t nodeId, std::string_view key) const override {
				if (!state_->enable_property_presence_probes) return LogicalGraphReadSnapshot::nodePropertyExists(nodeId, key);
				const auto &node = state_->nodes.at(nodeId);
				return std::ranges::any_of(node.properties, [&](const auto &entry) { return entry.key == key; });
			}
			[[nodiscard]] std::optional<bool> edgePropertyExists(uint64_t edgeId, std::string_view key) const override {
				if (!state_->enable_property_presence_probes) return LogicalGraphReadSnapshot::edgePropertyExists(edgeId, key);
				const auto edge = std::ranges::find(state_->edges, edgeId, &storage::LogicalGraphEdge::edge_id);
				return edge != state_->edges.end() &&
					std::ranges::any_of(edge->properties, [&](const auto &entry) { return entry.key == key; });
			}
			[[nodiscard]] bool supportsPropertyReadAdmission() const override { return state_->enable_property_admission; }
			[[nodiscard]] std::vector<std::vector<PropertyValue>> readNodePropertyColumnsSortedWithAdmission(
					std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys,
					const std::function<void(size_t)> &admission) const override {
				return readPropertyColumns(nodeIds, keys, &admission);
			}
			[[nodiscard]] std::vector<std::vector<PropertyValue>> readPropertyColumns(
					std::span<const uint64_t> nodeIds, std::span<const std::string_view> keys,
					const std::function<void(size_t)> *admission) const {
				++state_->property_column_reads;
				state_->property_column_ids.emplace_back(nodeIds.begin(), nodeIds.end());
				state_->property_column_keys.emplace_back(keys.begin(), keys.end());
				if (state_->malformed_property_columns) {
					return {};
				}
				std::vector<std::vector<PropertyValue>> columns(keys.size());
				uint64_t previous = 0U;
				for (const uint64_t id: nodeIds) {
					if (id == 0U || id <= previous || !state_->nodes.contains(id)) {
						throw std::runtime_error("test procedure received an invalid Property batch");
					}
					previous = id;
					const auto &node = state_->nodes.at(id);
					for (size_t column = 0U; column < keys.size(); ++column) {
						const auto entry = std::ranges::find(node.properties, keys[column],
								&storage::LogicalGraphPropertyEntry::key);
						if (entry != node.properties.end() && admission != nullptr) {
							if (state_->before_property_admission) state_->before_property_admission();
							(*admission)(storage::CanonicalPropertyValueCodec::encodedSize(entry->value) + sizeof(uint32_t));
							++state_->admitted_property_copies;
						}
						columns[column].push_back(entry == node.properties.end() ? PropertyValue() : entry->value);
					}
				}
				return columns;
			}
			[[nodiscard]] bool containsNode(uint64_t nodeId) const override {
				return state_->nodes.contains(nodeId);
			}
			[[nodiscard]] bool supportsNodeExistenceProbes() const override { return state_->enable_existence_probes; }
			[[nodiscard]] std::vector<bool> containsNodesSorted(
					const std::span<const uint64_t> nodeIds) const override {
				++state_->batched_existence_scans;
				state_->batched_existence_scan_sizes.push_back(nodeIds.size());
				std::vector<bool> result;
				result.reserve(nodeIds.size());
				uint64_t previous = 0U;
				for (const uint64_t nodeId: nodeIds) {
					if (nodeId == 0U || nodeId <= previous) {
						throw std::invalid_argument("test snapshot received an invalid sorted Node batch");
					}
					previous = nodeId;
					result.push_back(state_->nodes.contains(nodeId));
				}
				if (state_->batched_existence_scans == state_->malformed_existence_call && state_->existence_result_adjustment < 0 && !result.empty()) result.pop_back();
				if (state_->batched_existence_scans == state_->malformed_existence_call && state_->existence_result_adjustment > 0) result.push_back(true);
				return result;
			}
			[[nodiscard]] storage::LogicalGraphEdge readEdge(uint64_t edgeId) const override {
				const auto edge = std::ranges::find(state_->edges, edgeId, &storage::LogicalGraphEdge::edge_id);
				return edge == state_->edges.end() ? storage::LogicalGraphEdge{} : *edge;
			}
			void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {
				throw std::logic_error("scalar procedure test must not scan Nodes");
			}
			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(const uint64_t afterNodeId,
					const size_t maximumRecords, const size_t maximumMetadataPages) const override {
				if (maximumRecords == 0U || maximumMetadataPages == 0U) {
					throw std::invalid_argument("logical projection test received invalid Node scan bounds");
				}
				if (state_->projection_scan_delay.count() > 0) {
					std::this_thread::sleep_for(state_->projection_scan_delay);
				}
				std::vector<storage::LogicalGraphNode> nodes;
				nodes.reserve(state_->nodes.size());
				for (const auto &[_, node]: state_->nodes) {
					nodes.push_back(node);
				}
				std::ranges::sort(nodes, {}, &storage::LogicalGraphNode::node_id);
				storage::LogicalGraphNodePage page;
				for (const storage::LogicalGraphNode &node: nodes) {
					if (node.node_id <= afterNodeId) {
						continue;
					}
					if (page.nodes.size() == maximumRecords) {
						page.may_have_more = true;
						break;
					}
					page.nodes.push_back(node);
				}
				page.metadata_pages_visited = page.nodes.empty() ? 0U : 1U;
				return page;
			}
			[[nodiscard]] storage::LogicalGraphAdjacencyPage
			scanAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection,
							  std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override;
			[[nodiscard]] storage::LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(
					uint64_t nodeId, storage::LogicalGraphReadDirection direction,
					std::optional<storage::LogicalGraphAdjacencyContinuation> continuation,
					size_t maximumRecords) const override;
			void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("scalar procedure test must not scan relationships");
			}
			void visitAdjacentEdges(const uint64_t nodeId, const storage::LogicalGraphReadDirection direction,
							const size_t maximumRecords,
							const std::function<bool(const storage::LogicalGraphEdge &)> &visitor) const override {
				size_t visited = 0U;
				for (const storage::LogicalGraphEdge &edge: state_->edges) {
					const bool matches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
								 ? edge.source_node_id == nodeId
								 : edge.target_node_id == nodeId;
					if (!matches) {
						continue;
					}
					if (++visited > maximumRecords || !visitor(edge)) {
						return;
					}
				}
			}
			[[nodiscard]] std::optional<storage::LogicalGraphMutationFence> mutationFence() const override {
				return storage::LogicalGraphMutationFence{0x3456U, 0x789AU, {}};
			}
			[[nodiscard]] storage::LogicalGraphVectorSearchResult
			searchLogicalVector(const storage::LogicalGraphVectorSearchRequest &request) const override {
				if (state_->cancellation_checks_before_throw != 0U) {
					if (request.cancellation == nullptr) {
						throw std::logic_error("logical Vector procedure did not receive a cancellation checkpoint");
					}
					if (state_->search_cancellation_initial_delay.count() > 0) {
						std::this_thread::sleep_for(state_->search_cancellation_initial_delay);
					}
					for (size_t check = 0U; check < state_->cancellation_checks_before_throw; ++check) {
						++state_->search_cancellation_checks;
						request.cancellation->check();
					}
				}
				return state_->search_result;
			}

		private:
			std::shared_ptr<const ProcedureBackendState> state_;
		};

		inline storage::LogicalGraphAdjacencyPage ProcedureLogicalGraphSnapshot::scanAdjacentEdges(
				uint64_t nodeId, storage::LogicalGraphReadDirection direction,
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
			std::vector<storage::LogicalGraphEdge> edges;
			++state_->full_adjacency_scans;
			for (const storage::LogicalGraphEdge &edge: state_->edges) {
				const bool matches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
									 ? edge.source_node_id == nodeId
									 : edge.target_node_id == nodeId;
				if (matches) {
					edges.push_back(edge);
				}
			}
			std::ranges::sort(edges, {}, &storage::LogicalGraphEdge::edge_id);
			if (state_->return_nonmonotonic_adjacency_page && edges.size() > 1U) {
				std::swap(edges[0], edges[1]);
			}
			size_t begin = 0U;
			if (continuation.has_value()) {
				const auto next = std::ranges::find(edges, continuation->next_edge_id,
																		 &storage::LogicalGraphEdge::edge_id);
				if (next == edges.end()) {
					return {};
				}
				begin = static_cast<size_t>(std::distance(edges.begin(), next));
			}
			const size_t count = std::min(maximumRecords, edges.size() - begin);
			storage::LogicalGraphAdjacencyPage page;
			page.edges.insert(page.edges.end(), edges.begin() + static_cast<std::ptrdiff_t>(begin),
							  edges.begin() + static_cast<std::ptrdiff_t>(begin + count));
			if (begin + count < edges.size() && count != 0U) {
				page.continuation = {page.edges.back().edge_id, edges[begin + count].edge_id};
			}
			return page;
		}

		inline storage::LogicalGraphAdjacencyTopologyPage ProcedureLogicalGraphSnapshot::scanAdjacentEdgeTopologies(
				uint64_t nodeId, storage::LogicalGraphReadDirection direction,
				std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
			++state_->topology_scans;
			std::vector<storage::LogicalGraphEdgeTopology> edges;
			for (const storage::LogicalGraphEdge &edge: state_->edges) {
				const bool matches = direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING
						? edge.source_node_id == nodeId : edge.target_node_id == nodeId;
				if (matches) {
					edges.push_back({edge.edge_id, edge.source_node_id, edge.target_node_id, edge.type});
				}
			}
			std::ranges::sort(edges, {}, &storage::LogicalGraphEdgeTopology::edge_id);
			if (state_->return_nonmonotonic_adjacency_page && edges.size() > 1U) {
				std::swap(edges[0], edges[1]);
			}
			size_t begin = 0U;
			if (continuation.has_value()) {
				const auto next = std::ranges::find(edges, continuation->next_edge_id,
																 &storage::LogicalGraphEdgeTopology::edge_id);
				if (next == edges.end()) {
					return {};
				}
				begin = static_cast<size_t>(std::distance(edges.begin(), next));
			}
			const size_t count = std::min(maximumRecords, edges.size() - begin);
			storage::LogicalGraphAdjacencyTopologyPage page;
			page.edges.insert(page.edges.end(), edges.begin() + static_cast<std::ptrdiff_t>(begin),
					edges.begin() + static_cast<std::ptrdiff_t>(begin + count));
			if (begin + count < edges.size() && count != 0U) {
				page.continuation = {page.edges.back().edge_id, edges[begin + count].edge_id};
			}
			return page;
		}

		class RecordingVectorProcedureBackend final : public storage::LogicalGraphReadBackend,
																		 public storage::LogicalGraphVectorSearchBackend,
																		 public storage::LogicalGraphVectorMaintenanceBackend {
		public:
			class PreparedTraining final : public storage::PreparedLogicalGraphVectorTraining {
			public:
				PreparedTraining(RecordingVectorProcedureBackend &backend,
									const storage::LogicalGraphVectorTrainingRequest &request,
									storage::LogicalGraphVectorTrainingResult result) :
					backend_(backend), request_(request), result_(result), preview_{result.trained} {}

				[[nodiscard]] storage::LogicalGraphVectorTrainingPreview preview() const noexcept override {
					if (backend_.state->training_preview_delay.count() > 0) {
						std::this_thread::sleep_for(backend_.state->training_preview_delay);
					}
					return preview_;
				}

				[[nodiscard]] storage::LogicalGraphVectorTrainingResult commit() override {
					if (committed_) {
						throw std::logic_error("recording Vector training committed more than once");
					}
					committed_ = true;
					return backend_.publishTraining(request_, result_);
				}

			private:
				RecordingVectorProcedureBackend &backend_;
				storage::LogicalGraphVectorTrainingRequest request_;
				storage::LogicalGraphVectorTrainingResult result_;
				storage::LogicalGraphVectorTrainingPreview preview_;
				bool committed_ = false;
			};

			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				return std::make_unique<ProcedureLogicalGraphSnapshot>(state);
			}
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphVectorSearchSnapshot>
			openLogicalGraphVectorSearchSnapshot() const override {
				++search_snapshot_opens;
				if (state->return_null_search_snapshot) {
					return nullptr;
				}
				return std::make_unique<ProcedureLogicalGraphSnapshot>(state);
			}
			[[nodiscard]] std::unique_ptr<storage::PreparedLogicalGraphVectorTraining>
			prepareLogicalVectorTraining(const storage::LogicalGraphVectorTrainingRequest &request) override {
				return std::make_unique<PreparedTraining>(*this, request, prepareTraining(request));
			}

			[[nodiscard]] storage::LogicalGraphVectorTrainingResult
			prepareTraining(const storage::LogicalGraphVectorTrainingRequest &request) {
				++training_preparations;
				last_training_request = request;
				if (state->training_prepare_delay.count() > 0) {
					std::this_thread::sleep_for(state->training_prepare_delay);
				}
				if (state->training_cancellation_checks_before_throw != 0U) {
					if (request.cancellation == nullptr) {
						throw std::logic_error("logical Vector training did not receive a cancellation checkpoint");
					}
					for (size_t check = 0U; check < state->training_cancellation_checks_before_throw; ++check) {
						++state->training_cancellation_checks;
						request.cancellation->check();
					}
				}
				return training_result;
			}

			[[nodiscard]] storage::LogicalGraphVectorTrainingResult
			publishTraining(const storage::LogicalGraphVectorTrainingRequest &request,
								storage::LogicalGraphVectorTrainingResult result) {
				if (state->training_commit_delay.count() > 0) {
					std::this_thread::sleep_for(state->training_commit_delay);
				}
				++training_calls;
				last_training_request = request;
				return result;
			}

			mutable size_t search_snapshot_opens = 0U;
			size_t training_preparations = 0U;
			size_t training_calls = 0U;
			storage::LogicalGraphVectorTrainingRequest last_training_request;
			storage::LogicalGraphVectorTrainingResult training_result{1U, true, false};
			std::shared_ptr<ProcedureBackendState> state = std::make_shared<ProcedureBackendState>();
		};

} // namespace graph::query::test
