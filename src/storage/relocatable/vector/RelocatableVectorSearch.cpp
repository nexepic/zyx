/**
 * @file RelocatableVectorSearch.cpp
 * @brief Generation-checked exact and derived Vector search for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/RelocatableVectorSearch.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyMapping.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorDeltaMapping.hpp"
#include "graph/storage/relocatable/vector/VectorNodeMapping.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorPqMapping.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/common/VectorMath.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void checkCancellation(const LogicalGraphVectorSearchRequest &request) {
			if (request.cancellation != nullptr) {
				request.cancellation->check();
			}
		}

		[[nodiscard]] size_t expectedRawRecordBytes(uint32_t dimensions) {
			if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
				if (static_cast<uint64_t>(dimensions) * sizeof(uint16_t) + sizeof(uint64_t) >
					std::numeric_limits<size_t>::max()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "relocatable logical Vector search dimensions exceed a host-addressable raw record");
				}
			}
			return sizeof(uint64_t) + static_cast<size_t>(dimensions) * sizeof(uint16_t);
		}

		void requireRoot(const MetadataSnapshotPin &snapshot, RelocatableMetadataStore &metadata, uint64_t indexId,
						 PageId expectedPageId, Generation expectedGeneration, const char *message) {
			const std::optional<IndexRootLocation> root = metadata.findIndexRootOptional(snapshot, indexId);
			if (!root.has_value() || root->root_page_id != expectedPageId ||
				root->root_generation != expectedGeneration) {
				fail(FormatError::FERR_INVALID_STATE, message);
			}
		}

		void validateSemanticManifest(const MetadataSnapshotPin &snapshot, const IndexDefinition &definition,
									  const VectorManifest &manifest, RelocatableMetadataStore &metadata) {
			const bool hasDelta = manifest.delta_mapping_index_id != 0U;
			const uint8_t invalid =
					static_cast<uint8_t>(manifest.index_id != definition.index_id) |
					static_cast<uint8_t>(definition.vector_dimensions == 0U) |
					static_cast<uint8_t>(manifest.dimensions == 0U) |
					static_cast<uint8_t>(manifest.dimensions != definition.vector_dimensions) |
					static_cast<uint8_t>(manifest.metric != VectorSemanticCodec::metricFor(definition)) |
					static_cast<uint8_t>(!manifest.node_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(!manifest.node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(!VectorNodeMapping::isMappingIndexId(manifest.vector_node_mapping_index_id)) |
					static_cast<uint8_t>(!manifest.vector_node_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(!manifest.vector_node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(hasDelta != manifest.delta_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(hasDelta != manifest.delta_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(hasDelta != (manifest.delta_entry_count != 0U)) |
					static_cast<uint8_t>(hasDelta &&
										 !VectorDeltaMapping::isMappingIndexId(manifest.delta_mapping_index_id));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search found an invalid semantic Manifest");
			}
			requireRoot(snapshot, metadata, definition.index_id, manifest.node_mapping_root_page_id,
						manifest.node_mapping_root_generation,
						"relocatable logical Vector search found a stale Node-to-Vector mapping root");
			requireRoot(snapshot, metadata, manifest.vector_node_mapping_index_id,
						manifest.vector_node_mapping_root_page_id, manifest.vector_node_mapping_root_generation,
						"relocatable logical Vector search found a stale Vector-to-Node mapping root");
			if (hasDelta) {
				requireRoot(snapshot, metadata, manifest.delta_mapping_index_id, manifest.delta_mapping_root_page_id,
							manifest.delta_mapping_root_generation,
							"relocatable logical Vector search found a stale Vector delta mapping root");
			}
		}

		struct VectorDeltaOverlay {
			std::unordered_set<uint64_t> erased;
			std::vector<uint64_t> inserted;

			[[nodiscard]] bool suppresses(uint64_t vectorId) const { return erased.contains(vectorId); }
			[[nodiscard]] bool empty() const { return entryCount() == 0U; }
			[[nodiscard]] size_t entryCount() const { return erased.size() + inserted.size(); }
		};

		void validateDeltaSemanticMembership(const MetadataSnapshotPin &snapshot, const IndexDefinition &definition,
											 const VectorManifest &manifest, const VectorDeltaEntry &entry,
											 BTreePageStore &trees, RelocatableMetadataStore &metadata) {
			const std::optional<uint64_t> nodeId = VectorNodeMapping::findNode(
					snapshot, trees, metadata, manifest.vector_node_mapping_index_id, entry.vector_id);
			const bool hasRawLocation = metadata.findVectorLocationOptional(snapshot, entry.vector_id).has_value();
			VectorDeltaMapping::validateCurrentRawMembership(entry, hasRawLocation);
			if (entry.operation == VectorDeltaOperation::VDO_ERASE) {
				const std::optional<uint64_t> forward =
						BTreeIndex(definition.index_id, trees, metadata)
								.find(snapshot, VectorSemanticCodec::nodeMappingKey(entry.node_id));
				// A tombstone suppresses a physically retained base record. The old
				// Vector location may already be absent from the current directory, and
				// an update may legitimately point the same Node at a new Vector ID.
				// Only an old reverse mapping or an exact old forward mapping makes this
				// tombstone semantically reachable. If a replacement forward mapping is
				// present, validate its complete replacement identity instead.
				bool invalidReplacement = false;
				if (forward.has_value()) {
					if (*forward == 0U || *forward == entry.vector_id) {
						invalidReplacement = true;
					} else {
						const std::optional<uint64_t> replacementNode = VectorNodeMapping::findNode(
								snapshot, trees, metadata, manifest.vector_node_mapping_index_id, *forward);
						invalidReplacement = !replacementNode.has_value() || *replacementNode != entry.node_id ||
								!metadata.findVectorLocationOptional(snapshot, *forward).has_value();
					}
				}
				if (nodeId.has_value() || invalidReplacement) {
					fail(FormatError::FERR_INVALID_STATE, "relocatable logical Vector search found an erase overlay "
																 "entry with semantic reachability or an invalid replacement mapping");
				}
				return;
			}
			// VectorDeltaMapping already established that the remaining operation is
			// an INSERT with a nonzero Node ID and a current Raw location. This
			// layer owns the independent reverse semantic mapping check.
			if (!nodeId.has_value() || *nodeId != entry.node_id) {
				fail(FormatError::FERR_INVALID_STATE, "relocatable logical Vector search found an insert overlay entry "
													  "without a semantic Vector record");
			}
			const std::optional<uint64_t> forward =
					BTreeIndex(definition.index_id, trees, metadata)
							.find(snapshot, VectorSemanticCodec::nodeMappingKey(*nodeId));
			if (!forward.has_value() || *forward != entry.vector_id) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search found non-bijective insert overlay mappings");
			}
		}

		[[nodiscard]] VectorDeltaOverlay loadDeltaOverlay(const MetadataSnapshotPin &snapshot,
														  const IndexDefinition &definition,
														  const VectorManifest &manifest,
														  const LogicalGraphVectorSearchRequest &request,
														  BTreePageStore &trees, RelocatableMetadataStore &metadata) {
			VectorDeltaOverlay overlay;
			if (manifest.delta_mapping_index_id == 0U) {
				return overlay;
			}
			if (manifest.delta_entry_count > request.maximum_mapping_entries) {
				throw std::runtime_error(
						"relocatable logical Vector search delta overlay exceeds its caller-supplied mapping bound");
			}
			const std::vector<VectorDeltaEntry> entries = VectorDeltaMapping::scan(
					snapshot, trees, metadata, manifest.delta_mapping_index_id, manifest.delta_entry_count, request.cancellation);
			if (entries.size() != manifest.delta_entry_count) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search delta overlay count disagrees with its Manifest");
			}
			overlay.erased.reserve(entries.size());
			overlay.inserted.reserve(entries.size());
			for (const VectorDeltaEntry &entry: entries) {
				checkCancellation(request);
				validateDeltaSemanticMembership(snapshot, definition, manifest, entry, trees, metadata);
				if (entry.operation == VectorDeltaOperation::VDO_ERASE) {
					// VectorDeltaMapping::scan establishes unique, strictly ordered Vector IDs.
					overlay.erased.insert(entry.vector_id);
				} else if (entry.operation == VectorDeltaOperation::VDO_INSERT) {
					overlay.inserted.push_back(entry.vector_id);
				} else {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable logical Vector search delta overlay has an invalid operation");
				}
			}
			return overlay;
		}

		[[nodiscard]] bool hasAnyAdjacencyIdentity(const VectorManifest &manifest) {
			return manifest.adjacency_generation.isValid() || manifest.adjacency_mapping_index_id != 0U ||
				   manifest.adjacency_mapping_root_page_id.isValid() ||
				   manifest.adjacency_mapping_root_generation.isValid() || manifest.entry_point_vector_id != 0U;
		}

		[[nodiscard]] bool validateAdjacencyIdentity(const MetadataSnapshotPin &snapshot,
													 const VectorManifest &manifest,
													 RelocatableMetadataStore &metadata) {
			if (!hasAnyAdjacencyIdentity(manifest)) {
				return false;
			}
			const uint8_t invalid = static_cast<uint8_t>(!manifest.adjacency_generation.isValid()) |
									static_cast<uint8_t>(manifest.entry_point_vector_id == 0U) |
									static_cast<uint8_t>(!VectorAdjacencyMapping::isMappingIndexId(
											manifest.adjacency_mapping_index_id)) |
									static_cast<uint8_t>(!manifest.adjacency_mapping_root_page_id.isValid()) |
									static_cast<uint8_t>(!manifest.adjacency_mapping_root_generation.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search found a partial or incompatible adjacency generation");
			}
			requireRoot(snapshot, metadata, manifest.adjacency_mapping_index_id,
						manifest.adjacency_mapping_root_page_id, manifest.adjacency_mapping_root_generation,
						"relocatable logical Vector search found a stale adjacency mapping root");
			return true;
		}

		[[nodiscard]] bool hasAnyPqIdentity(const VectorManifest &manifest) {
			return manifest.codebook_id != 0U || manifest.codebook_generation.isValid() ||
				   manifest.pq_mapping_index_id != 0U || manifest.pq_mapping_root_page_id.isValid() ||
				   manifest.pq_mapping_root_generation.isValid();
		}

		[[nodiscard]] bool validatePqIdentity(const MetadataSnapshotPin &snapshot, const VectorManifest &manifest,
											  RelocatableMetadataStore &metadata) {
			if (!hasAnyPqIdentity(manifest)) {
				return false;
			}
			const uint8_t invalid =
					static_cast<uint8_t>(manifest.codebook_id == 0U) |
					static_cast<uint8_t>(!manifest.codebook_generation.isValid()) |
					static_cast<uint8_t>(!VectorPqMapping::isMappingIndexId(manifest.pq_mapping_index_id)) |
					static_cast<uint8_t>(!manifest.pq_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(!manifest.pq_mapping_root_generation.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search found a partial PQ generation");
			}
			requireRoot(snapshot, metadata, manifest.pq_mapping_index_id, manifest.pq_mapping_root_page_id,
						manifest.pq_mapping_root_generation,
						"relocatable logical Vector search found a stale PQ mapping root");
			return true;
		}

		[[nodiscard]] float checkedFloatScore(double score, const char *message) {
			if (!std::isfinite(score) || score > static_cast<double>(std::numeric_limits<float>::max()) ||
				score < -static_cast<double>(std::numeric_limits<float>::max())) {
				throw std::overflow_error(message);
			}
			return static_cast<float>(score);
		}

		struct ExactCandidate {
			uint64_t vector_id = 0U;
			uint64_t node_id = 0U;
			float score = 0.0F;
		};

		[[nodiscard]] ExactCandidate
		readExactCandidate(const MetadataSnapshotPin &snapshot, const IndexDefinition &definition,
						   const VectorManifest &manifest, const LogicalGraphVectorSearchRequest &request,
						   uint64_t vectorId, std::optional<uint64_t> expectedNodeId, BTreePageStore &trees,
						   VectorPageStore &vectors, RelocatableMetadataStore &metadata,
							   LogicalGraphVectorSearchStats &stats) {
			checkCancellation(request);
			const std::optional<uint64_t> nodeId = VectorNodeMapping::findNode(
					snapshot, trees, metadata, manifest.vector_node_mapping_index_id, vectorId);
			++stats.semantic_mapping_lookups;
			if (!nodeId.has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search could not resolve a candidate Node");
			}
			if (expectedNodeId.has_value()) {
				if (*nodeId != *expectedNodeId) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable logical Vector search found inconsistent semantic mapping directions");
				}
			} else {
				const std::optional<uint64_t> forward =
						BTreeIndex(definition.index_id, trees, metadata)
								.find(snapshot, VectorSemanticCodec::nodeMappingKey(*nodeId));
				++stats.semantic_mapping_lookups;
				if (!forward.has_value() || *forward != vectorId) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable logical Vector search found non-bijective semantic Vector mappings");
				}
			}
			const VectorLocation location = metadata.findVectorLocation(snapshot, vectorId);
			const VectorPage page = vectors.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_NORMAL);
			const VectorPageHeader header = page.header();
			const size_t expectedBytes = expectedRawRecordBytes(definition.vector_dimensions);
			const uint8_t invalidPage = static_cast<uint8_t>(header.kind != VectorStorageKind::VSK_RAW) |
										static_cast<uint8_t>(header.index_id != definition.index_id) |
										static_cast<uint8_t>(header.page_id != location.page_id) |
										static_cast<uint8_t>(header.record_bytes != expectedBytes);
			if (invalidPage != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search resolved an invalid raw Vector page");
			}
			const std::vector<std::byte> raw = page.read(location.slot_id, location.slot_generation);
			++stats.raw_vector_reads;
			if (raw.size() != expectedBytes || LittleEndianCodec::readU64(raw, 0U) != vectorId) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search resolved a stale raw Vector record");
			}

			double score = 0.0;
			double querySquaredNorm = 0.0;
			double candidateSquaredNorm = 0.0;
			for (uint32_t dimension = 0U; dimension < definition.vector_dimensions; ++dimension) {
				common::BFloat16 encoded;
				encoded.data = LittleEndianCodec::readU16(raw, sizeof(uint64_t) + static_cast<size_t>(dimension) *
																						  sizeof(uint16_t));
				const float candidate = encoded.toFloat();
				if (!std::isfinite(candidate)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable logical Vector search decoded a non-finite raw coordinate");
				}
				if (manifest.metric == VectorDistanceMetric::VDM_L2) {
					const double difference = static_cast<double>(request.query_vector[dimension]) - candidate;
					score += difference * difference;
				} else {
					const double query = static_cast<double>(request.query_vector[dimension]);
					score += query * candidate;
					querySquaredNorm += query * query;
					candidateSquaredNorm += static_cast<double>(candidate) * candidate;
				}
			}
			if (manifest.metric == VectorDistanceMetric::VDM_INNER_PRODUCT) {
				score = -score;
			} else if (manifest.metric == VectorDistanceMetric::VDM_COSINE) {
				if (candidateSquaredNorm == 0.0) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable logical Vector search found a zero-norm cosine Vector record");
				}
				score = 1.0 - std::clamp(score / std::sqrt(querySquaredNorm * candidateSquaredNorm), -1.0, 1.0);
			}
			return {vectorId, *nodeId,
					checkedFloatScore(score, "relocatable logical Vector search produced a non-finite exact distance")};
		}

		[[nodiscard]] bool exactHitLess(const LogicalGraphVectorSearchHit &left,
										const LogicalGraphVectorSearchHit &right) {
			return left.score == right.score ? left.node_id < right.node_id : left.score < right.score;
		}

		[[nodiscard]] std::vector<LogicalGraphVectorSearchHit> finishExact(std::vector<ExactCandidate> candidates,
																		   size_t topK) {
			std::vector<LogicalGraphVectorSearchHit> hits;
			hits.reserve(candidates.size());
			for (const ExactCandidate &candidate: candidates) {
				hits.push_back({candidate.node_id, candidate.score});
			}
			std::ranges::sort(hits, exactHitLess);
			if (hits.size() > topK) {
				hits.resize(topK);
			}
			return hits;
		}

		class PqDistanceTable final {
		public:
			PqDistanceTable(const VectorCodebook &codebook, const VectorManifest &manifest,
							std::span<const float> query) {
				const VectorCodebookHeader &header = codebook.header;
				metric_ = manifest.metric;
				subspace_count_ = header.subspace_count;
				centroid_count_ = header.centroid_count;
				const uint32_t subspaceDimensions = header.dimensions / header.subspace_count;
				const size_t tableValues = static_cast<size_t>(header.subspace_count) * header.centroid_count;
				primary_scores_.resize(tableValues);
				centroid_squared_norms_.resize(tableValues);
				for (const float value: query) {
					query_squared_norm_ += static_cast<double>(value) * value;
				}
				for (uint32_t subspace = 0U; subspace < header.subspace_count; ++subspace) {
					const size_t queryOffset = static_cast<size_t>(subspace) * subspaceDimensions;
					for (uint32_t centroid = 0U; centroid < header.centroid_count; ++centroid) {
						const size_t centroidOffset =
								(static_cast<size_t>(subspace) * header.centroid_count + centroid) * subspaceDimensions;
						double primaryScore = 0.0;
						double centroidSquaredNorm = 0.0;
						for (uint32_t coordinate = 0U; coordinate < subspaceDimensions; ++coordinate) {
							const uint32_t bits = LittleEndianCodec::readU32(
									codebook.payload, (centroidOffset + coordinate) * sizeof(float));
							const float value = std::bit_cast<float>(bits);
							const double queryValue = static_cast<double>(query[queryOffset + coordinate]);
							if (metric_ == VectorDistanceMetric::VDM_L2) {
								const double delta = queryValue - value;
								primaryScore += delta * delta;
							} else {
								primaryScore += queryValue * value;
								centroidSquaredNorm += static_cast<double>(value) * value;
							}
						}
						const size_t ordinal = static_cast<size_t>(subspace) * header.centroid_count + centroid;
						primary_scores_[ordinal] = primaryScore;
						centroid_squared_norms_[ordinal] = centroidSquaredNorm;
					}
				}
			}

			[[nodiscard]] size_t recordBytes() const { return sizeof(uint64_t) + subspace_count_; }

			[[nodiscard]] float score(std::span<const std::byte> record) const {
				double result = 0.0;
				double candidateSquaredNorm = 0.0;
				for (uint32_t subspace = 0U; subspace < subspace_count_; ++subspace) {
					const uint8_t centroid = std::to_integer<uint8_t>(record[sizeof(uint64_t) + subspace]);
					if (centroid >= centroid_count_) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "relocatable logical Vector search found an out-of-range PQ centroid code");
					}
					const size_t ordinal = static_cast<size_t>(subspace) * centroid_count_ + centroid;
					result += primary_scores_[ordinal];
					candidateSquaredNorm += centroid_squared_norms_[ordinal];
				}
				if (metric_ == VectorDistanceMetric::VDM_INNER_PRODUCT) {
					result = -result;
				} else if (metric_ == VectorDistanceMetric::VDM_COSINE) {
					if (candidateSquaredNorm == 0.0) {
						fail(FormatError::FERR_INVALID_STATE,
							 "relocatable logical Vector search found a zero-norm reconstructed cosine PQ vector");
					}
					result =
							1.0 - std::clamp(result / std::sqrt(query_squared_norm_ * candidateSquaredNorm), -1.0, 1.0);
				}
				return checkedFloatScore(
						result, "relocatable logical Vector search produced a non-finite approximate distance");
			}

		private:
			VectorDistanceMetric metric_ = VectorDistanceMetric::VDM_L2;
			uint32_t subspace_count_ = 0U;
			uint32_t centroid_count_ = 0U;
			double query_squared_norm_ = 0.0;
			std::vector<double> primary_scores_;
			std::vector<double> centroid_squared_norms_;
		};

		struct ApproximateCandidate {
			uint64_t vector_id = 0U;
			float score = 0.0F;
		};

		struct ApproximateLater {
			[[nodiscard]] bool operator()(const ApproximateCandidate &left, const ApproximateCandidate &right) const {
				return left.score == right.score ? left.vector_id > right.vector_id : left.score > right.score;
			}
		};

		[[nodiscard]] bool approximateLess(const ApproximateCandidate &left, const ApproximateCandidate &right) {
			return left.score == right.score ? left.vector_id < right.vector_id : left.score < right.score;
		}

		[[nodiscard]] LogicalGraphVectorSearchResult
		searchExact(const MetadataSnapshotPin &snapshot, const IndexDefinition &definition,
					const VectorManifest &manifest, const LogicalGraphVectorSearchRequest &request,
					BTreePageStore &trees, VectorPageStore &vectors, RelocatableMetadataStore &metadata) {
			const utils::OwnedStdVector<BTreePageEntry> mappings =
					BTreeIndex(definition.index_id, trees, metadata)
							.scan(snapshot, std::nullopt, request.maximum_mapping_entries + 1U,
								  BufferPoolReadHint::BPRH_SEQUENTIAL, request.cancellation);
			if (mappings.size() > request.maximum_mapping_entries ||
				mappings.size() > request.maximum_raw_vector_reads) {
				throw std::runtime_error("relocatable logical Vector exact search exceeded its caller-supplied bound");
			}
			LogicalGraphVectorSearchStats stats;
			stats.scanned_mapping_entries = mappings.size();
			std::vector<ExactCandidate> candidates;
			candidates.reserve(mappings.size());
			for (const BTreePageEntry &mapping: mappings) {
				checkCancellation(request);
				if (mapping.key.size() !=
							sizeof(uint64_t) || // ZYX_COV_EXCL_LINE: typed Node-to-Vector mapping builders emit
												// canonical 8-byte keys; a forged variable-width leaf also fails the
												// slotted-page free-space index validation before BTreeIndex::scan can
												// return it. Keep this defense for alternate page stores.
					mapping.value_or_child == 0U) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable logical Vector search found a malformed Node-to-Vector mapping");
				}
				uint64_t nodeId = 0U;
				for (const std::byte byte: mapping.key) {
					nodeId = (nodeId << 8U) | std::to_integer<uint8_t>(byte);
				}
				if (nodeId == 0U) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable logical Vector search found a noncanonical Node mapping key");
				}
				ExactCandidate candidate =
						readExactCandidate(snapshot, definition, manifest, request, mapping.value_or_child, nodeId,
										   trees, vectors, metadata, stats);
				candidates.push_back(candidate);
			}
			return {finishExact(std::move(candidates), request.top_k), stats};
		}

		[[nodiscard]] VectorAdjacencyRecord readAdjacency(const MetadataSnapshotPin &snapshot,
														  const VectorManifest &manifest, uint64_t vectorId,
														  BTreePageStore &trees, VectorAdjacencyPageStore &adjacencies,
														  RelocatableMetadataStore &metadata,
														  LogicalGraphVectorSearchStats &stats) {
			const std::optional<PageId> pageId = VectorAdjacencyMapping::findPage(
					snapshot, trees, metadata, manifest.adjacency_mapping_index_id, vectorId);
			++stats.derived_mapping_lookups;
			if (!pageId.has_value()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search found no adjacency record for a candidate");
			}
			const VectorAdjacencyRecord record = adjacencies.readRecord(snapshot, *pageId, vectorId);
			++stats.adjacency_record_reads;
			return record;
		}

		[[nodiscard]] LogicalGraphVectorSearchResult
		searchRawAdjacency(const MetadataSnapshotPin &snapshot, const IndexDefinition &definition,
						   const VectorManifest &manifest, const LogicalGraphVectorSearchRequest &request,
						   BTreePageStore &trees, VectorPageStore &vectors, VectorAdjacencyPageStore &adjacencies,
						   RelocatableMetadataStore &metadata, const VectorDeltaOverlay &overlay) {
			if (overlay.suppresses(manifest.entry_point_vector_id)) {
				LogicalGraphVectorSearchResult exact =
						searchExact(snapshot, definition, manifest, request, trees, vectors, metadata);
				exact.stats.scanned_delta_entries = overlay.entryCount();
				exact.stats.used_delta = true;
				return exact;
			}
			if (overlay.inserted.size() > request.maximum_raw_vector_reads) {
				throw std::runtime_error(
						"relocatable logical Vector delta inserts exceed the caller-supplied exact-read bound");
			}
			const size_t maximumBaseCandidates = request.maximum_raw_vector_reads - overlay.inserted.size();
			std::unordered_set<uint64_t> seen;
			seen.reserve(std::min(request.maximum_mapping_entries, request.maximum_raw_vector_reads));
			std::priority_queue<ApproximateCandidate, std::vector<ApproximateCandidate>, ApproximateLater> frontier;
			std::vector<ExactCandidate> candidates;
			candidates.reserve(std::min(request.maximum_mapping_entries, request.maximum_raw_vector_reads));
			LogicalGraphVectorSearchStats stats;
			stats.used_adjacency = true;
			stats.scanned_delta_entries = overlay.entryCount();
			stats.used_delta = !overlay.empty();
			const auto enqueue = [&](uint64_t vectorId) {
				checkCancellation(request);
				if (overlay.suppresses(vectorId)) {
					return;
				}
				if (seen.contains(vectorId)) {
					return;
				}
				if (seen.size() == request.maximum_mapping_entries || candidates.size() == maximumBaseCandidates) {
					throw std::runtime_error(
							"relocatable logical Vector adjacency search exceeded a hard candidate bound");
				}
				seen.insert(vectorId);
				ExactCandidate exact = readExactCandidate(snapshot, definition, manifest, request, vectorId,
														  std::nullopt, trees, vectors, metadata, stats);
				frontier.push({vectorId, exact.score});
				++stats.candidate_discoveries;
				stats.candidate_frontier_peak = std::max(stats.candidate_frontier_peak, frontier.size());
				candidates.push_back(exact);
			};

			enqueue(manifest.entry_point_vector_id);
			size_t expanded = 0U;
			while (!frontier.empty() && expanded < request.candidate_search_budget) {
				checkCancellation(request);
				const ApproximateCandidate current = frontier.top();
				frontier.pop();
				++expanded;
				++stats.candidate_expansions;
				for (const uint64_t neighbor:
					 readAdjacency(snapshot, manifest, current.vector_id, trees, adjacencies, metadata, stats)
							 .neighbors) {
					enqueue(neighbor);
				}
			}
			for (const uint64_t vectorId: overlay.inserted) {
				checkCancellation(request);
				candidates.push_back(readExactCandidate(snapshot, definition, manifest, request, vectorId, std::nullopt,
														trees, vectors, metadata, stats));
			}
			return {finishExact(std::move(candidates), request.top_k), stats};
		}

		[[nodiscard]] LogicalGraphVectorSearchResult
		searchPqAdjacency(const MetadataSnapshotPin &snapshot, const IndexDefinition &definition,
						  const VectorManifest &manifest, const LogicalGraphVectorSearchRequest &request,
						  BTreePageStore &trees, VectorPageStore &vectors, VectorAdjacencyPageStore &adjacencies,
						  VectorCodebookStore &codebooks, RelocatableMetadataStore &metadata,
						  const VectorDeltaOverlay &overlay) {
			if (overlay.inserted.size() > request.candidate_refinement_budget) {
				throw std::runtime_error(
						"relocatable logical Vector delta inserts exceed the caller-supplied refinement bound");
			}
			const size_t baseRefinementBudget = request.candidate_refinement_budget - overlay.inserted.size();
			const uint64_t maximumCodebookBytes = static_cast<uint64_t>(manifest.dimensions) * 256U * sizeof(float);
			const std::shared_ptr<const VectorCodebook> codebook = codebooks.readSharedBounded(
					snapshot, manifest.codebook_id, maximumCodebookBytes,
					VectorCodebookStore::requiredDataExtentCount(static_cast<size_t>(maximumCodebookBytes)));
			const VectorCodebookHeader &codebookHeader = codebook->header;
			const uint8_t invalidCodebook =
					static_cast<uint8_t>(codebookHeader.codebook_id != manifest.codebook_id) |
					static_cast<uint8_t>(codebookHeader.generation != manifest.codebook_generation) |
					static_cast<uint8_t>(codebookHeader.element_type != VectorCodebookElementType::VCET_FLOAT32) |
					static_cast<uint8_t>(codebookHeader.element_bytes != sizeof(float)) |
					static_cast<uint8_t>(codebookHeader.dimensions != manifest.dimensions);
			if (invalidCodebook != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
						"relocatable logical Vector search found an incompatible Codebook manifest");
			}
			// Validate the immutable PQ generation before falling back to exact search
			// for a tombstoned entry point. Otherwise a corrupt derived generation
			// could be silently hidden by a semantic delta.
			if (overlay.suppresses(manifest.entry_point_vector_id)) {
				LogicalGraphVectorSearchResult exact =
						searchExact(snapshot, definition, manifest, request, trees, vectors, metadata);
				exact.stats.scanned_delta_entries = overlay.entryCount();
				exact.stats.used_delta = true;
				return exact;
			}
			const PqDistanceTable table(*codebook, manifest, request.query_vector);
			std::set<PageId> pqInventory;
			for (const VectorManifestEntry &entry: manifest.entries) {
				if (entry.storage_kind == VectorStorageKind::VSK_PQ) {
					pqInventory.insert(entry.page_id);
				}
			}
			if (pqInventory.empty()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable logical Vector search PQ generation has no Manifest page inventory");
			}

			std::unordered_set<uint64_t> seen;
			const size_t initialCandidateCapacity =
					std::min(request.maximum_mapping_entries,
							 std::max(request.candidate_search_budget, request.candidate_refinement_budget));
			seen.reserve(initialCandidateCapacity);
			std::priority_queue<ApproximateCandidate, std::vector<ApproximateCandidate>, ApproximateLater> frontier;
			std::vector<ApproximateCandidate> discovered;
			discovered.reserve(initialCandidateCapacity);
			LogicalGraphVectorSearchStats stats;
			stats.used_adjacency = true;
			stats.used_pq = true;
			stats.scanned_delta_entries = overlay.entryCount();
			stats.used_delta = !overlay.empty();
			const auto enqueue = [&](uint64_t vectorId) {
				checkCancellation(request);
				if (overlay.suppresses(vectorId)) {
					return;
				}
				if (seen.contains(vectorId)) {
					return;
				}
				if (seen.size() == request.maximum_mapping_entries) {
					throw std::runtime_error(
							"relocatable logical Vector PQ search exceeded its hard candidate mapping bound");
				}
				seen.insert(vectorId);
				const std::optional<PageId> pageId =
						VectorPqMapping::findPage(snapshot, trees, metadata, manifest.pq_mapping_index_id, vectorId);
				++stats.derived_mapping_lookups;
				if (!pageId.has_value() || !pqInventory.contains(*pageId)) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable logical Vector search PQ mapping escaped its Manifest inventory");
				}
				const VectorPage page = vectors.read(snapshot, *pageId, BufferPoolReadHint::BPRH_NORMAL);
				const VectorPageHeader header = page.header();
				const uint8_t invalidPage =
						static_cast<uint8_t>(header.kind != VectorStorageKind::VSK_PQ) |
						static_cast<uint8_t>(header.index_id != definition.index_id) |
						static_cast<uint8_t>(header.page_id != *pageId) |
						static_cast<uint8_t>(header.codebook_generation != manifest.codebook_generation) |
						static_cast<uint8_t>(header.record_bytes != table.recordBytes());
				const std::optional<uint16_t> slot = page.findVectorId(vectorId);
				if (invalidPage != 0U || !slot.has_value()) {
					fail(FormatError::FERR_INVALID_STATE,
						 "relocatable logical Vector search resolved an incompatible PQ page");
				}
				const ApproximateCandidate candidate{vectorId, table.score(page.read(*slot))};
				++stats.pq_record_reads;
				frontier.push(candidate);
				++stats.candidate_discoveries;
				stats.candidate_frontier_peak = std::max(stats.candidate_frontier_peak, frontier.size());
				discovered.push_back(candidate);
			};

			enqueue(manifest.entry_point_vector_id);
			size_t expanded = 0U;
			while (!frontier.empty() && expanded < request.candidate_search_budget) {
				checkCancellation(request);
				const ApproximateCandidate current = frontier.top();
				frontier.pop();
				++expanded;
				++stats.candidate_expansions;
				for (const uint64_t neighbor:
					 readAdjacency(snapshot, manifest, current.vector_id, trees, adjacencies, metadata, stats)
							 .neighbors) {
					enqueue(neighbor);
				}
			}

			std::ranges::sort(discovered, approximateLess);
			if (discovered.size() > baseRefinementBudget) {
				discovered.resize(baseRefinementBudget);
			}
			std::vector<ExactCandidate> refined;
			refined.reserve(discovered.size() + overlay.inserted.size());
			for (const ApproximateCandidate &candidate: discovered) {
				checkCancellation(request);
				refined.push_back(readExactCandidate(snapshot, definition, manifest, request, candidate.vector_id,
													 std::nullopt, trees, vectors, metadata, stats));
			}
			for (const uint64_t vectorId: overlay.inserted) {
				checkCancellation(request);
				refined.push_back(readExactCandidate(snapshot, definition, manifest, request, vectorId, std::nullopt,
													 trees, vectors, metadata, stats));
			}
			return {finishExact(std::move(refined), request.top_k), stats};
		}

	} // namespace

	LogicalGraphVectorSearchResult RelocatableVectorSearch::search(
			const MetadataSnapshotPin &snapshot, const IndexDefinition &definition, const VectorManifest &manifest,
			const LogicalGraphVectorSearchRequest &request, BTreePageStore &trees, VectorPageStore &vectors,
			VectorAdjacencyPageStore &adjacencies, VectorCodebookStore &codebooks, RelocatableMetadataStore &metadata) {
		checkCancellation(request);
		validateSemanticManifest(snapshot, definition, manifest, metadata);
		if (request.query_vector.size() != manifest.dimensions ||
			!std::ranges::all_of(request.query_vector, [](float value) { return std::isfinite(value); })) {
			throw std::invalid_argument(
					"relocatable logical Vector search requires a finite query matching the persisted dimensions");
		}
		if (manifest.metric == VectorDistanceMetric::VDM_COSINE &&
			!std::ranges::any_of(request.query_vector, [](float value) { return value != 0.0F; })) {
			throw std::invalid_argument("relocatable cosine Vector search query must have a nonzero L2 norm");
		}
		if (request.mode == LogicalGraphVectorSearchMode::LGVSM_EXACT) {
			return searchExact(snapshot, definition, manifest, request, trees, vectors, metadata);
		}
		const bool hasAdjacency = validateAdjacencyIdentity(snapshot, manifest, metadata);
		const bool hasPq = validatePqIdentity(snapshot, manifest, metadata);
		if (!hasAdjacency) {
			return searchExact(snapshot, definition, manifest, request, trees, vectors, metadata);
		}
		const VectorDeltaOverlay overlay = loadDeltaOverlay(snapshot, definition, manifest, request, trees, metadata);
		const uint8_t invalidBudget =
				static_cast<uint8_t>(request.candidate_search_budget == 0U) |
				static_cast<uint8_t>(request.candidate_search_budget > request.maximum_mapping_entries) |
				static_cast<uint8_t>(hasPq && request.candidate_refinement_budget == 0U) |
				static_cast<uint8_t>(hasPq && request.candidate_refinement_budget > request.maximum_raw_vector_reads) |
				static_cast<uint8_t>(hasPq && request.top_k > request.candidate_refinement_budget);
		if (invalidBudget != 0U) {
			throw std::invalid_argument(
					"relocatable logical Vector adjacency search requires compatible search and refinement budgets");
		}
		if (hasPq) {
			return searchPqAdjacency(snapshot, definition, manifest, request, trees, vectors, adjacencies, codebooks,
									 metadata, overlay);
		}
		return searchRawAdjacency(snapshot, definition, manifest, request, trees, vectors, adjacencies, metadata,
								  overlay);
	}

} // namespace graph::storage
