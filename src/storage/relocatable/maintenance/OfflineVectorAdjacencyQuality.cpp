/**
 * @file OfflineVectorAdjacencyQuality.cpp
 * @brief Bounded quality measurement for a rebuilt Vector adjacency graph.
 */

#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyQuality.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace graph::storage {

	namespace {

		static_assert(std::numeric_limits<size_t>::digits <= std::numeric_limits<uint64_t>::digits);
		// A finite BF16 coordinate difference squared for every addressable dimension remains finite in double.
		static_assert(static_cast<long double>(std::numeric_limits<uint64_t>::max()) * 4.0L *
				  static_cast<long double>(std::numeric_limits<float>::max()) *
				  static_cast<long double>(std::numeric_limits<float>::max()) <
				  static_cast<long double>(std::numeric_limits<double>::max()));

		struct PreparedVector {
			uint64_t vector_id = 0;
			std::vector<float> values;
		};

		struct DistanceCandidate {
			double distance = 0.0;
			uint64_t vector_id = 0;
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] std::vector<PreparedVector> prepareVectors(std::span<const CanonicalVectorRecord> records) {
			if (records.empty() || records.front().raw_bfloat16.empty() ||
				records.front().raw_bfloat16.size() % sizeof(uint16_t) != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline adjacency quality requires nonempty canonical BF16 records");
			}
			const size_t byteWidth = records.front().raw_bfloat16.size();
			uint64_t previousId = 0U;
			std::vector<PreparedVector> result;
			result.reserve(records.size());
			for (const CanonicalVectorRecord &record: records) {
				if (record.vector_id == 0U || record.vector_id <= previousId || record.raw_bfloat16.size() != byteWidth) {
					fail(FormatError::FERR_INVALID_HEADER,
							 "offline adjacency quality requires canonical sorted equal-width BF16 records");
				}
				PreparedVector prepared;
				prepared.vector_id = record.vector_id;
				prepared.values.reserve(byteWidth / sizeof(uint16_t));
				for (size_t offset = 0U; offset < byteWidth; offset += sizeof(uint16_t)) {
					const uint16_t bfloat = LittleEndianCodec::readU16(record.raw_bfloat16, offset);
					const float value = std::bit_cast<float>(static_cast<uint32_t>(bfloat) << 16U);
					if (!std::isfinite(value)) {
						fail(FormatError::FERR_INVALID_HEADER,
								 "offline adjacency quality rejects non-finite BF16 vector coordinates");
					}
					prepared.values.push_back(value);
				}
				previousId = record.vector_id;
				result.push_back(std::move(prepared));
			}
			return result;
		}

		[[nodiscard]] double squaredL2(std::span<const float> left, std::span<const float> right) {
			double distance = 0.0;
			for (size_t dimension = 0U; dimension < left.size(); ++dimension) {
				const double delta = static_cast<double>(left[dimension]) - static_cast<double>(right[dimension]);
				distance += delta * delta;
			}
			return distance;
		}

		void validateAdjacency(std::span<const PreparedVector> vectors, std::span<const VectorAdjacencyRecord> adjacency) {
			if (adjacency.size() != vectors.size()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "offline adjacency quality requires one adjacency record per Vector");
			}
			for (size_t ordinal = 0U; ordinal < adjacency.size(); ++ordinal) {
				const VectorAdjacencyRecord &record = adjacency[ordinal];
				if (record.vector_id != vectors[ordinal].vector_id || !std::ranges::is_sorted(record.neighbors) ||
					std::ranges::adjacent_find(record.neighbors) != record.neighbors.end() ||
					std::ranges::binary_search(record.neighbors, record.vector_id)) {
					fail(FormatError::FERR_INVALID_HEADER,
							 "offline adjacency quality requires canonical adjacency records without self-links");
				}
				for (const uint64_t neighbor: record.neighbors) {
					if (!std::ranges::binary_search(vectors, neighbor, {}, &PreparedVector::vector_id)) {
						fail(FormatError::FERR_INVALID_HEADER,
								 "offline adjacency quality found a neighbor outside the canonical Vector set");
					}
				}
			}
		}

	} // namespace

	VectorAdjacencyQualityReport OfflineVectorAdjacencyQuality::evaluate(
			std::span<const CanonicalVectorRecord> records, std::span<const VectorAdjacencyRecord> adjacency,
			const VectorAdjacencyQualityOptions &options) {
		if (options.maximum_records == 0U || records.size() > options.maximum_records || records.size() < 2U ||
			options.recall_at == 0U || static_cast<size_t>(options.recall_at) >= records.size()) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "offline adjacency quality requires a bounded multi-Vector recall@k input");
		}
		const uint64_t recallAt = static_cast<uint64_t>(options.recall_at);
		if (static_cast<uint64_t>(options.maximum_records) > std::numeric_limits<uint64_t>::max() / recallAt) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "offline adjacency quality maximum record count overflows the report");
		}
		const std::vector<PreparedVector> vectors = prepareVectors(records);
		validateAdjacency(vectors, adjacency);
		const uint64_t vectorCount = static_cast<uint64_t>(vectors.size());

		VectorAdjacencyQualityReport result;
		result.vector_count = vectorCount;
		result.expected_exact_neighbors = vectorCount * recallAt;
		for (size_t sourceOrdinal = 0U; sourceOrdinal < vectors.size(); ++sourceOrdinal) {
			std::vector<DistanceCandidate> candidates;
			candidates.reserve(vectors.size() - 1U);
			for (size_t candidateOrdinal = 0U; candidateOrdinal < vectors.size(); ++candidateOrdinal) {
				if (candidateOrdinal != sourceOrdinal) {
					candidates.push_back({squaredL2(vectors[sourceOrdinal].values, vectors[candidateOrdinal].values),
											  vectors[candidateOrdinal].vector_id});
				}
			}
			std::ranges::sort(candidates, [](const DistanceCandidate &left, const DistanceCandidate &right) {
				return std::pair{left.distance, left.vector_id} < std::pair{right.distance, right.vector_id};
			});
			for (size_t ordinal = 0U; ordinal < static_cast<size_t>(options.recall_at); ++ordinal) {
				if (std::ranges::binary_search(adjacency[sourceOrdinal].neighbors, candidates[ordinal].vector_id)) {
					++result.retained_exact_neighbors;
				}
			}
		}
		result.adjacency_neighbor_recall = static_cast<double>(result.retained_exact_neighbors) /
											   static_cast<double>(result.expected_exact_neighbors);
		return result;
	}

} // namespace graph::storage
