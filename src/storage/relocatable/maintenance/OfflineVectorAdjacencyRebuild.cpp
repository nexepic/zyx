/**
 * @file OfflineVectorAdjacencyRebuild.cpp
 * @brief Offline, resumable publication of a derived adjacency generation.
 */

#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <utility>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"

namespace graph::storage {

	namespace {

		static_assert(sizeof(size_t) <= sizeof(uint64_t),
				  "the supported native targets must represent every in-memory count in a persisted work counter");

		struct PreparedVector {
			uint64_t vector_id = 0;
			std::vector<float> values;
		};

		struct DistanceCandidate {
			double distance = 0.0;
			uint64_t vector_id = 0;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		void checkCancellation(const CancellationCheckpoint *cancellation) {
			if (cancellation != nullptr) {
				cancellation->check();
			}
		}

		void addWork(uint64_t &counter, uint64_t amount, const char *message) {
			if (amount > std::numeric_limits<uint64_t>::max() - counter) { // ZYX_COV_EXCL_LINE: a bounded in-memory
				// rebuild cannot perform 2^64 work units; reaching this guard needs infeasible input or iteration count.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			counter += amount;
		}

		void recordFrontierPeak(VectorAdjacencyBuildWork &work, size_t frontierSize) {
			work.candidate_frontier_peak = std::max(work.candidate_frontier_peak,
												 static_cast<uint64_t>(frontierSize));
		}

		[[nodiscard]] LogSequenceNumber nextCheckpointLsn(RelocatableMetadataStore &metadata) {
			const MetadataSnapshotPin snapshot = metadata.openSnapshot();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) { // ZYX_COV_EXCL_LINE:
				// a safe test would require publishing 2^64-1 COW generations; corrupt headers are rejected earlier.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "offline adjacency rebuild cannot advance the destination checkpoint LSN");
			}
			return LogSequenceNumber(snapshot.superblock().checkpoint_lsn.value() + 1U);
		}

		[[nodiscard]] std::vector<std::byte> rawRecord(uint64_t vectorId, std::span<const std::byte> rawBfloat16) {
			std::vector<std::byte> result(sizeof(uint64_t) + rawBfloat16.size());
			LittleEndianCodec::writeU64(result, 0, vectorId);
			std::copy(rawBfloat16.begin(), rawBfloat16.end(), result.begin() + sizeof(uint64_t));
			return result;
		}

		void validateOptions(uint64_t indexId, std::span<const CanonicalVectorRecord> records,
							 const OfflineVectorAdjacencyRebuildOptions &options) {
			const bool exactMode = options.build_mode == VectorAdjacencyBuildMode::VABM_EXACT;
			const bool candidateMode = options.build_mode == VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
			const uint8_t invalid =
					static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(records.empty()) |
					static_cast<uint8_t>(!exactMode && !candidateMode) |
					static_cast<uint8_t>(exactMode && options.maximum_exact_records == 0U) |
					static_cast<uint8_t>(exactMode && records.size() > options.maximum_exact_records) |
					static_cast<uint8_t>(candidateMode && options.maximum_candidate_records == 0U) |
					static_cast<uint8_t>(candidateMode && records.size() > options.maximum_candidate_records) |
					static_cast<uint8_t>(candidateMode && options.candidate_search_budget == 0U) |
					static_cast<uint8_t>(candidateMode &&
										 options.candidate_search_budget > options.maximum_candidate_records) |
					static_cast<uint8_t>(candidateMode && !std::isfinite(options.pruning_alpha)) |
					static_cast<uint8_t>(candidateMode && options.pruning_alpha < 1.0F) |
					static_cast<uint8_t>(options.maximum_degree == 0U) |
					static_cast<uint8_t>(options.maximum_degree > RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS) |
					static_cast<uint8_t>(!options.adjacency_generation.isValid()) |
					static_cast<uint8_t>(options.pages.maximum_records == 0U) |
					static_cast<uint8_t>(records.size() > options.pages.maximum_records) |
					static_cast<uint8_t>(options.pages.maximum_pages == 0U) |
					static_cast<uint8_t>(!options.pages.first_slot.isValid()) |
					static_cast<uint8_t>(!options.pages.last_slot.isValid()) |
					static_cast<uint8_t>(options.pages.first_slot > options.pages.last_slot) |
					static_cast<uint8_t>(options.mapping.maximum_entries == 0U) |
					static_cast<uint8_t>(records.size() > options.mapping.maximum_entries) |
					static_cast<uint8_t>(options.mapping.tree.maximum_entries != options.mapping.maximum_entries) |
					static_cast<uint8_t>(options.mapping.tree.pool_owner_id != options.mapping_index_id) |
					static_cast<uint8_t>(!options.mapping.tree.first_slot.isValid()) |
					static_cast<uint8_t>(!options.mapping.tree.last_slot.isValid()) |
					static_cast<uint8_t>(options.mapping.tree.first_slot > options.mapping.tree.last_slot) |
					static_cast<uint8_t>(options.mapping.tree.maximum_pages == 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "offline adjacency rebuild requires bounded construction input, "
													   "compatible page, and mapping identities");
			}
			VectorAdjacencyMapping::validateMappingIndexId(options.mapping_index_id);
		}

		[[nodiscard]] std::vector<PreparedVector> prepareVectors(std::span<const CanonicalVectorRecord> records,
															 uint32_t dimensions,
															 const CancellationCheckpoint *cancellation) {
			// Both public callers validate a nonzero uint32 dimension count before this internal conversion.
			const size_t expectedBytes = static_cast<size_t>(dimensions) * sizeof(uint16_t);
			uint64_t previousVectorId = 0U;
			std::vector<PreparedVector> result;
			result.reserve(records.size());
			for (const CanonicalVectorRecord &record: records) {
				checkCancellation(cancellation);
				if (record.vector_id == 0U || record.vector_id <= previousVectorId ||
					record.raw_bfloat16.size() != expectedBytes) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "offline adjacency rebuild records are not canonical sorted BF16 vectors");
				}
				PreparedVector prepared;
				prepared.vector_id = record.vector_id;
				prepared.values.reserve(dimensions);
				for (uint32_t dimension = 0; dimension < dimensions; ++dimension) {
					checkCancellation(cancellation);
					const uint16_t bfloat =
							LittleEndianCodec::readU16(record.raw_bfloat16, dimension * sizeof(uint16_t));
					const float value = std::bit_cast<float>(static_cast<uint32_t>(bfloat) << 16U);
					if (!std::isfinite(value)) {
						fail(FormatError::FERR_INVALID_HEADER,
							 "offline adjacency rebuild rejects non-finite BF16 vector coordinates");
					}
					prepared.values.push_back(value);
				}
				previousVectorId = record.vector_id;
				result.push_back(std::move(prepared));
			}
			return result;
		}

		void verifyRawSource(const MetadataSnapshotPin &snapshot, VectorPageStore &vectors,
							 RelocatableMetadataStore &metadata, uint64_t indexId,
							 std::span<const CanonicalVectorRecord> records) {
			for (const CanonicalVectorRecord &record: records) {
				const VectorLocation location = metadata.findVectorLocation(snapshot, record.vector_id);
				const VectorPage page = vectors.read(snapshot, location.page_id);
				const uint8_t invalid = static_cast<uint8_t>(page.header().index_id != indexId) |
										static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_RAW) |
										static_cast<uint8_t>(page.read(location.slot_id, location.slot_generation) !=
															 rawRecord(record.vector_id, record.raw_bfloat16));
				if (invalid != 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline adjacency rebuild raw Vector Location disagrees with canonical source input");
				}
			}
		}

		[[nodiscard]] double squaredL2(std::span<const float> left, std::span<const float> right) {
			double distance = 0.0;
			for (size_t dimension = 0; dimension < left.size(); ++dimension) {
				const double delta = static_cast<double>(left[dimension]) - static_cast<double>(right[dimension]);
				distance += delta * delta;
			}
			return distance;
		}

		[[nodiscard]] bool isKnownMetric(VectorDistanceMetric metric) {
			return metric == VectorDistanceMetric::VDM_L2 || metric == VectorDistanceMetric::VDM_COSINE ||
				   metric == VectorDistanceMetric::VDM_INNER_PRODUCT;
		}

		[[nodiscard]] double pairDistance(VectorDistanceMetric metric, std::span<const float> left,
										 std::span<const float> right) {
			if (metric == VectorDistanceMetric::VDM_L2) {
				return squaredL2(left, right);
			}
			double dot = 0.0;
			double leftSquaredNorm = 0.0;
			double rightSquaredNorm = 0.0;
			for (size_t dimension = 0U; dimension < left.size(); ++dimension) {
				const double leftValue = static_cast<double>(left[dimension]);
				const double rightValue = static_cast<double>(right[dimension]);
				dot += leftValue * rightValue;
				leftSquaredNorm += leftValue * leftValue;
				rightSquaredNorm += rightValue * rightValue;
			}
			if (metric == VectorDistanceMetric::VDM_INNER_PRODUCT) {
				return -dot;
			}
			if (leftSquaredNorm == 0.0 || rightSquaredNorm == 0.0) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline cosine adjacency rebuild rejects zero-norm canonical vectors");
			}
			return 1.0 - std::clamp(dot / std::sqrt(leftSquaredNorm * rightSquaredNorm), -1.0, 1.0);
		}

		[[nodiscard]] std::vector<VectorAdjacencyMaterializationRecord>
		buildExactGraph(VectorDistanceMetric metric, std::span<const PreparedVector> vectors, uint16_t maximumDegree,
								VectorAdjacencyBuildWork &work, const CancellationCheckpoint *cancellation) {
			std::vector<VectorAdjacencyMaterializationRecord> result;
			result.reserve(vectors.size());
			for (size_t sourceOrdinal = 0; sourceOrdinal < vectors.size(); ++sourceOrdinal) {
				checkCancellation(cancellation);
				const PreparedVector &source = vectors[sourceOrdinal];
				std::vector<DistanceCandidate> candidates;
				candidates.reserve(vectors.size() - 1U);
				for (size_t candidateOrdinal = 0; candidateOrdinal < vectors.size(); ++candidateOrdinal) {
					checkCancellation(cancellation);
					if (candidateOrdinal == sourceOrdinal) {
						continue;
					}
					const PreparedVector &candidate = vectors[candidateOrdinal];
					addWork(work.exact_pair_distance_evaluations, 1U,
							"offline adjacency rebuild exact distance counter overflowed");
					candidates.push_back({pairDistance(metric, source.values, candidate.values), candidate.vector_id});
				}
				std::ranges::sort(candidates, [](const DistanceCandidate &left, const DistanceCandidate &right) {
					return std::pair{left.distance, left.vector_id} < std::pair{right.distance, right.vector_id};
				});
				VectorAdjacencyMaterializationRecord adjacency;
				adjacency.vector_id = source.vector_id;
				const size_t selected = std::min(candidates.size(), static_cast<size_t>(maximumDegree));
				adjacency.neighbors.reserve(selected);
				for (size_t ordinal = 0; ordinal < selected; ++ordinal) {
					adjacency.neighbors.push_back(candidates[ordinal].vector_id);
				}
				std::ranges::sort(adjacency.neighbors);
				result.push_back(std::move(adjacency));
			}
			return result;
		}

		struct OrdinalDistance {
			double distance = 0.0;
			size_t ordinal = 0U;
		};

		struct OrdinalDistanceGreater {
			[[nodiscard]] bool operator()(const OrdinalDistance &left, const OrdinalDistance &right) const {
				return std::pair{left.distance, left.ordinal} > std::pair{right.distance, right.ordinal};
			}
		};

		[[nodiscard]] std::vector<size_t> pruneCandidates(VectorDistanceMetric metric,
															 std::span<const PreparedVector> vectors, size_t sourceOrdinal,
															 std::span<const size_t> candidates, uint16_t maximumDegree,
															 float pruningAlpha, VectorAdjacencyBuildWork &work,
															 const CancellationCheckpoint *cancellation) {
			checkCancellation(cancellation);
			addWork(work.candidate_prune_calls, 1U, "offline adjacency rebuild candidate prune counter overflowed");
			std::vector<size_t> canonical(candidates.begin(), candidates.end());
			std::ranges::sort(canonical);
			canonical.erase(std::unique(canonical.begin(), canonical.end()), canonical.end());
			canonical.erase(std::remove(canonical.begin(), canonical.end(), sourceOrdinal), canonical.end());
			addWork(work.candidate_prune_candidates, static_cast<uint64_t>(canonical.size()),
					"offline adjacency rebuild candidate count overflowed");
			std::ranges::sort(canonical, [&](size_t left, size_t right) {
				checkCancellation(cancellation);
				return std::pair{pairDistance(metric, vectors[sourceOrdinal].values, vectors[left].values),
								 vectors[left].vector_id} <
					   std::pair{pairDistance(metric, vectors[sourceOrdinal].values, vectors[right].values),
								 vectors[right].vector_id};
			});

			std::vector<size_t> selected;
			selected.reserve(std::min(canonical.size(), static_cast<size_t>(maximumDegree)));
			for (const size_t candidate: canonical) {
				checkCancellation(cancellation);
				const double sourceDistance =
						pairDistance(metric, vectors[sourceOrdinal].values, vectors[candidate].values);
				bool dominated = false;
				for (const size_t accepted: selected) {
					checkCancellation(cancellation);
					addWork(work.candidate_prune_pair_checks, 1U,
							"offline adjacency rebuild candidate prune-pair counter overflowed");
					// The alpha dominance rule is valid for nonnegative metric distances.
					// Inner product is a similarity transformed into an unbounded signed
					// ordering, so its degree cap is the safe pruning boundary instead.
					if (metric != VectorDistanceMetric::VDM_INNER_PRODUCT &&
						sourceDistance > static_cast<double>(pruningAlpha) *
															 pairDistance(metric, vectors[candidate].values, vectors[accepted].values)) {
						dominated = true;
						break;
					}
				}
				if (!dominated) {
					selected.push_back(candidate);
					if (selected.size() == maximumDegree) {
						break;
					}
				}
			}
			return selected;
		}

		[[nodiscard]] std::vector<VectorAdjacencyMaterializationRecord>
		buildCandidatePrunedGraph(VectorDistanceMetric metric, std::span<const PreparedVector> vectors,
								  uint16_t maximumDegree, size_t searchBudget,
									  float pruningAlpha, VectorAdjacencyBuildWork &work,
									  const CancellationCheckpoint *cancellation) {
			std::vector<std::vector<size_t>> graph(vectors.size());
			std::vector<uint64_t> visitedEpoch(vectors.size(), 0U);
			uint64_t epoch = 0U;
			for (size_t sourceOrdinal = 0U; sourceOrdinal < vectors.size(); ++sourceOrdinal) {
				checkCancellation(cancellation);
				if (++epoch == 0U) { // ZYX_COV_EXCL_LINE: options bound one rebuild far below 2^64 source records.
					std::ranges::fill(visitedEpoch, 0U);
					epoch = 1U;
				}
				std::priority_queue<OrdinalDistance, std::vector<OrdinalDistance>, OrdinalDistanceGreater> frontier;
				const auto enqueue = [&](size_t candidateOrdinal) {
					checkCancellation(cancellation);
					if (candidateOrdinal >= sourceOrdinal || visitedEpoch[candidateOrdinal] == epoch) {
						return;
					}
					visitedEpoch[candidateOrdinal] = epoch;
					frontier.push({pairDistance(metric, vectors[sourceOrdinal].values, vectors[candidateOrdinal].values),
								   candidateOrdinal});
					addWork(work.candidate_frontier_pushes, 1U,
							"offline adjacency rebuild candidate frontier-push counter overflowed");
					recordFrontierPeak(work, frontier.size());
				};
				enqueue(0U);
				if (sourceOrdinal > 1U) {
					enqueue(sourceOrdinal - 1U);
					enqueue(sourceOrdinal / 2U);
				}
				const size_t effectiveSearchBudget = std::min(searchBudget, sourceOrdinal);
				std::vector<size_t> candidates;
				candidates.reserve(effectiveSearchBudget);
				while (!frontier.empty() && candidates.size() < effectiveSearchBudget) {
					checkCancellation(cancellation);
					const size_t currentOrdinal = frontier.top().ordinal;
					frontier.pop();
					addWork(work.candidate_frontier_pops, 1U,
							"offline adjacency rebuild candidate frontier-pop counter overflowed");
					candidates.push_back(currentOrdinal);
					for (const size_t neighborOrdinal: graph[currentOrdinal]) {
						enqueue(neighborOrdinal);
					}
				}
				graph[sourceOrdinal] =
						pruneCandidates(metric, vectors, sourceOrdinal, candidates, maximumDegree, pruningAlpha, work,
										cancellation);
				for (const size_t neighborOrdinal: graph[sourceOrdinal]) {
					checkCancellation(cancellation);
					addWork(work.candidate_reverse_link_updates, 1U,
							"offline adjacency rebuild reverse-link counter overflowed");
					std::vector<size_t> reverseCandidates = graph[neighborOrdinal];
					reverseCandidates.push_back(sourceOrdinal);
					graph[neighborOrdinal] = pruneCandidates(metric, vectors, neighborOrdinal, reverseCandidates,
																			 maximumDegree, pruningAlpha, work, cancellation);
				}
			}

			std::vector<VectorAdjacencyMaterializationRecord> result;
			result.reserve(vectors.size());
			for (size_t sourceOrdinal = 0U; sourceOrdinal < vectors.size(); ++sourceOrdinal) {
				checkCancellation(cancellation);
				VectorAdjacencyMaterializationRecord adjacency;
				adjacency.vector_id = vectors[sourceOrdinal].vector_id;
				adjacency.neighbors.reserve(graph[sourceOrdinal].size());
				for (const size_t neighborOrdinal: graph[sourceOrdinal]) {
					adjacency.neighbors.push_back(vectors[neighborOrdinal].vector_id);
				}
				std::ranges::sort(adjacency.neighbors);
				result.push_back(std::move(adjacency));
			}
			return result;
		}

		void verifyMapping(const MetadataSnapshotPin &snapshot, BTreePageStore &trees,
						   VectorAdjacencyPageStore &adjacencies, RelocatableMetadataStore &metadata, uint64_t indexId,
						   uint64_t mappingIndexId, std::span<const VectorAdjacencyMaterializationRecord> expected,
						   size_t maximumEntries) {
			const std::vector<BTreePageEntry> actual =
					BTreeIndex(mappingIndexId, trees, metadata).scan(snapshot, std::nullopt, maximumEntries + 1U);
			if (actual.size() != expected.size()) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline adjacency rebuild existing mapping has a different canonical Vector ID set");
			}
			for (size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
				const VectorAdjacencyMaterializationRecord &record = expected[ordinal];
				const BTreePageEntry &entry = actual[ordinal];
				if (entry.key != VectorAdjacencyMapping::key(record.vector_id) || entry.value_or_child == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline adjacency rebuild existing mapping disagrees with canonical Vector IDs");
				}
				const PageId pageId(entry.value_or_child);
				const SlottedPage page = adjacencies.readPage(snapshot, pageId);
				if (page.ownerId() != VectorAdjacencyPageStore::derivedOwnerId(indexId) ||
					adjacencies.readRecord(snapshot, pageId, record.vector_id) !=
							VectorAdjacencyRecord{record.vector_id, record.neighbors}) {
					fail(FormatError::FERR_INVALID_STATE,
						 "offline adjacency rebuild existing mapping resolves to an invalid adjacency record");
				}
			}
		}

		[[nodiscard]] bool manifestMatches(const VectorManifest &manifest, uint64_t indexId,
										   const OfflineVectorAdjacencyRebuildOptions &options,
										   uint64_t entryPointVectorId) {
			return manifest.index_id == indexId && isKnownMetric(manifest.metric) &&
				   manifest.adjacency_generation == options.adjacency_generation &&
				   manifest.adjacency_mapping_index_id == options.mapping_index_id &&
				   manifest.entry_point_vector_id == entryPointVectorId;
		}

	} // namespace

	OfflineVectorAdjacencyPreparedGraph OfflineVectorAdjacencyRebuild::buildGraph(
			VectorDistanceMetric metric, uint32_t dimensions, std::span<const CanonicalVectorRecord> records,
			const OfflineVectorAdjacencyRebuildOptions &options, const CancellationCheckpoint *cancellation) {
		const bool exactMode = options.build_mode == VectorAdjacencyBuildMode::VABM_EXACT;
		const bool candidateMode = options.build_mode == VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
		const uint8_t invalid =
				static_cast<uint8_t>(!isKnownMetric(metric)) | static_cast<uint8_t>(dimensions == 0U) |
				static_cast<uint8_t>(records.empty()) |
				static_cast<uint8_t>(!exactMode && !candidateMode) |
				static_cast<uint8_t>(exactMode && options.maximum_exact_records == 0U) |
				static_cast<uint8_t>(exactMode && records.size() > options.maximum_exact_records) |
				static_cast<uint8_t>(candidateMode && options.maximum_candidate_records == 0U) |
				static_cast<uint8_t>(candidateMode && records.size() > options.maximum_candidate_records) |
				static_cast<uint8_t>(candidateMode && options.candidate_search_budget == 0U) |
				static_cast<uint8_t>(candidateMode && options.candidate_search_budget > options.maximum_candidate_records) |
				static_cast<uint8_t>(candidateMode && !std::isfinite(options.pruning_alpha)) |
				static_cast<uint8_t>(candidateMode && options.pruning_alpha < 1.0F) |
				static_cast<uint8_t>(options.maximum_degree == 0U) |
				static_cast<uint8_t>(options.maximum_degree > RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "offline adjacency graph construction requires bounded canonical records and graph options");
		}
		checkCancellation(cancellation);
		const std::vector<PreparedVector> prepared = prepareVectors(records, dimensions, cancellation);
		OfflineVectorAdjacencyPreparedGraph result;
		result.records = exactMode ? buildExactGraph(metric, prepared, options.maximum_degree, result.build_work, cancellation)
								   : buildCandidatePrunedGraph(metric, prepared, options.maximum_degree,
															 options.candidate_search_budget, options.pruning_alpha,
															 result.build_work, cancellation);
		return result;
	}

	OfflineVectorAdjacencyRebuildResult OfflineVectorAdjacencyRebuild::run(
			BTreePageStore &trees, VectorPageStore &vectors, VectorAdjacencyPageStore &adjacencies,
			VectorManifestStore &manifests, RelocatableMetadataStore &metadata, uint64_t indexId,
			std::span<const CanonicalVectorRecord> records, const OfflineVectorAdjacencyRebuildOptions &options,
			const CancellationCheckpoint *cancellation) {
		validateOptions(indexId, records, options);
		checkCancellation(cancellation);
		const uint8_t differentStores = static_cast<uint8_t>(!trees.usesMetadataStore(metadata)) |
										static_cast<uint8_t>(!vectors.usesMetadataStore(metadata)) |
										static_cast<uint8_t>(!adjacencies.usesMetadataStore(metadata)) |
										static_cast<uint8_t>(!manifests.usesMetadataStore(metadata));
		if (differentStores != 0U) {
			fail(FormatError::FERR_INVALID_STATE, "offline adjacency rebuild requires one destination metadata store");
		}

		MetadataSnapshotPin sourceSnapshot = metadata.openSnapshot();
		const VectorManifest base = manifests.read(sourceSnapshot, indexId).manifest();
		if (!isKnownMetric(base.metric)) {
			sourceSnapshot.release();
			fail(FormatError::FERR_INVALID_STATE,
					 "offline adjacency rebuild requires a Manifest with a supported Vector metric");
		}
		const OfflineVectorAdjacencyPreparedGraph preparedGraph =
					buildGraph(base.metric, base.dimensions, records, options, cancellation);
			const std::vector<VectorAdjacencyMaterializationRecord> &graph = preparedGraph.records;
		verifyRawSource(sourceSnapshot, vectors, metadata, indexId, records);
		const PageLocation manifestLocation = metadata.findPageLocation(sourceSnapshot, base.page_id);
		sourceSnapshot.release();

			OfflineVectorAdjacencyRebuildResult result;
			result.vector_count = records.size();
		result.build_work = preparedGraph.build_work;
		const uint64_t entryPointVectorId = records.front().vector_id;
		MetadataSnapshotPin mappingSnapshot = metadata.openSnapshot();
		const std::optional<IndexRootLocation> existingRoot =
				metadata.findIndexRootOptional(mappingSnapshot, options.mapping_index_id);
		if (existingRoot.has_value()) {
			verifyMapping(mappingSnapshot, trees, adjacencies, metadata, indexId, options.mapping_index_id, graph,
						  options.mapping.maximum_entries);
			mappingSnapshot.release();
			result.reused_mapping = true;
		} else {
			mappingSnapshot.release();
			const VectorAdjacencyMaterializationResult staged =
					adjacencies.materialize(indexId, graph, options.pages, nextCheckpointLsn(metadata));
			static_cast<void>(VectorAdjacencyMapping::build(trees, metadata, options.mapping_index_id, staged.mappings,
															options.mapping, nextCheckpointLsn(metadata)));
		}

		MetadataSnapshotPin finalSnapshot = metadata.openSnapshot();
		const VectorManifest current = manifests.read(finalSnapshot, indexId).manifest();
		verifyRawSource(finalSnapshot, vectors, metadata, indexId, records);
		const IndexRootLocation mappingRoot = metadata.findIndexRoot(finalSnapshot, options.mapping_index_id);
		verifyMapping(finalSnapshot, trees, adjacencies, metadata, indexId, options.mapping_index_id, graph,
					  options.mapping.maximum_entries);
		if (manifestMatches(current, indexId, options, entryPointVectorId)) {
			const uint8_t invalidCurrent =
					static_cast<uint8_t>(current.adjacency_mapping_root_page_id != mappingRoot.root_page_id) |
					static_cast<uint8_t>(current.adjacency_mapping_root_generation != mappingRoot.root_generation);
			finalSnapshot.release();
			if (invalidCurrent != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "offline adjacency rebuild visible Manifest disagrees with its adjacency mapping root");
			}
			result.manifest = current;
			MetadataSnapshotPin publishedSnapshot = metadata.openSnapshot();
			result.superblock = publishedSnapshot.superblock();
			publishedSnapshot.release();
			result.already_published = true;
			return result;
		}
		const uint8_t nonMonotonicGeneration = static_cast<uint8_t>(
				base.adjacency_generation.isValid() && options.adjacency_generation <= base.adjacency_generation);
		finalSnapshot.release();
		if (nonMonotonicGeneration != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "offline adjacency rebuild generation must advance a different visible adjacency generation");
		}

		VectorManifest candidate = base;
		candidate.entry_point_vector_id = entryPointVectorId;
		candidate.adjacency_generation = options.adjacency_generation;
		candidate.adjacency_mapping_index_id = options.mapping_index_id;
		candidate.adjacency_mapping_root_page_id = mappingRoot.root_page_id;
		candidate.adjacency_mapping_root_generation = mappingRoot.root_generation;
		const LogSequenceNumber checkpointLsn = nextCheckpointLsn(metadata);
		candidate.committed_lsn = checkpointLsn;
		const PageOwnerHeader owner{candidate.page_id,
									candidate.index_id,
									PageOwnerKind::POK_VECTOR_MANIFEST,
									0,
									RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									candidate.generation,
									checkpointLsn};
		const VectorManifestPage page = VectorManifestPage::create(owner, candidate);
		const SharedPagePoolAllocation published = manifests.replace(
				page, manifestLocation.segment_id, base.committed_lsn, checkpointLsn, options.manifest_publish_options);
		result.superblock = published.superblock;
		result.manifest = candidate;
		return result;
	}

} // namespace graph::storage
