/**
 * @file DeterministicProductQuantizer.cpp
 * @brief Bounded deterministic Product Quantizer for relocatable derived vector data.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/DeterministicProductQuantizer.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <utility>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint64_t stableRank(uint64_t value) {
			// SplitMix64 gives a stable full-width ranking without keeping all source
			// records in memory. It is a sampling order, not a security primitive.
			value += UINT64_C(0x9E3779B97F4A7C15);
			value = (value ^ (value >> 30U)) * UINT64_C(0xBF58476D1CE4E5B9);
			value = (value ^ (value >> 27U)) * UINT64_C(0x94D049BB133111EB);
			return value ^ (value >> 31U);
		}

		void validateOptions(const DeterministicPqOptions &options) {
			const uint8_t invalid = static_cast<uint8_t>(options.dimensions == 0U) |
									static_cast<uint8_t>(options.subspace_count == 0U) |
									static_cast<uint8_t>(options.subspace_count > options.dimensions) |
									static_cast<uint8_t>(options.dimensions % options.subspace_count != 0U) |
									static_cast<uint8_t>(options.centroid_count == 0U) |
									static_cast<uint8_t>(options.centroid_count > UINT8_MAX + 1U) |
									static_cast<uint8_t>(options.iteration_count == 0U) |
									static_cast<uint8_t>(options.maximum_records == 0U) |
									static_cast<uint8_t>(options.maximum_samples == 0U) |
									static_cast<uint8_t>(options.maximum_samples > options.maximum_records);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable deterministic PQ requires bounded compatible dimensions, centroids, and iterations");
			}
			const uint64_t coordinateCount = static_cast<uint64_t>(options.dimensions) * options.centroid_count;
			if (coordinateCount > std::numeric_limits<size_t>::max() ||
				coordinateCount > std::numeric_limits<size_t>::max() / sizeof(float)) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "relocatable deterministic PQ codebook exceeds host-addressable memory");
			}
		}

		[[nodiscard]] DeterministicPqOptions encodingOptions(uint32_t dimensions, uint32_t subspaceCount,
															 uint32_t centroidCount) {
			// Encoding does not consume training samples or iterations, but reuse the
			// same layout validation with minimal nonzero bounded training fields.
			const DeterministicPqOptions options{dimensions, subspaceCount, centroidCount, 1U, 1U, 1U};
			validateOptions(options);
			return options;
		}

		[[nodiscard]] std::vector<float> decodeCodebook(std::span<const std::byte> payload,
														const DeterministicPqOptions &options) {
			const size_t coordinateCount = static_cast<size_t>(options.dimensions) * options.centroid_count;
			const size_t expectedBytes = coordinateCount * sizeof(float);
			if (payload.size() != expectedBytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable deterministic PQ Codebook payload does not match its declared shape");
			}
			std::vector<float> centroids;
			centroids.reserve(coordinateCount);
			for (size_t ordinal = 0U; ordinal < coordinateCount; ++ordinal) {
				const float value = std::bit_cast<float>(LittleEndianCodec::readU32(payload, ordinal * sizeof(float)));
				if (!std::isfinite(value)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable deterministic PQ Codebook contains a non-finite centroid");
				}
				centroids.push_back(value);
			}
			return centroids;
		}

		[[nodiscard]] size_t expectedRawBytes(const DeterministicPqOptions &options) {
			if constexpr (sizeof(size_t) < sizeof(uint32_t)) {
				if (options.dimensions > std::numeric_limits<size_t>::max() / sizeof(uint16_t)) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "relocatable deterministic PQ raw vector length exceeds host-addressable memory");
				}
			}
			return static_cast<size_t>(options.dimensions) * sizeof(uint16_t);
		}

		[[nodiscard]] std::vector<float> decodeRaw(std::span<const std::byte> bytes,
												   const DeterministicPqOptions &options) {
			if (bytes.size() != expectedRawBytes(options)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable deterministic PQ raw Vector record does not match its configured dimensions");
			}
			std::vector<float> decoded;
			decoded.reserve(options.dimensions);
			for (uint32_t coordinate = 0; coordinate < options.dimensions; ++coordinate) {
				const uint32_t bits =
						static_cast<uint32_t>(LittleEndianCodec::readU16(bytes, coordinate * sizeof(uint16_t))) << 16U;
				const float value = std::bit_cast<float>(bits);
				if (!std::isfinite(value)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable deterministic PQ raw Vector contains a non-finite BF16 coordinate");
				}
				decoded.push_back(value);
			}
			return decoded;
		}

		struct Candidate {
			uint64_t vector_id = 0;
			uint64_t rank = 0;
			std::vector<float> values;

			[[nodiscard]] bool operator<(const Candidate &other) const {
				// stableRank is a permutation of uint64_t and records have unique IDs.
				return rank < other.rank;
			}
		};

		[[nodiscard]] std::vector<std::vector<float>>
		selectSamples(std::span<const DeterministicPqTrainingRecord> records, const DeterministicPqOptions &options) {
			if (records.empty() || records.size() > options.maximum_records) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable deterministic PQ training record count is empty or exceeds its configured bound");
			}
			std::priority_queue<Candidate> selected;
			uint64_t previousId = 0U;
			for (const DeterministicPqTrainingRecord &record: records) {
				if (record.vector_id == 0U || record.vector_id <= previousId) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable deterministic PQ training records are not strictly ordered by Vector ID");
				}
				previousId = record.vector_id;
				Candidate candidate{record.vector_id, stableRank(record.vector_id),
									decodeRaw(record.raw_bfloat16, options)};
				if (selected.size() < options.maximum_samples) {
					selected.push(std::move(candidate));
				} else if (candidate < selected.top()) {
					selected.pop();
					selected.push(std::move(candidate));
				}
			}
			std::vector<Candidate> ordered;
			ordered.reserve(selected.size());
			while (!selected.empty()) {
				ordered.push_back(std::move(selected.top()));
				selected.pop();
			}
			std::ranges::sort(ordered, {}, &Candidate::vector_id);
			std::vector<std::vector<float>> result;
			result.reserve(ordered.size());
			for (Candidate &candidate: ordered) {
				result.push_back(std::move(candidate.values));
			}
			return result;
		}

		[[nodiscard]] double squaredDistance(const std::vector<float> &sample, size_t sampleOffset,
											 const std::vector<float> &centroids, size_t centroidOffset,
											 uint32_t subspaceDimensions) {
			double distance = 0.0;
			for (uint32_t coordinate = 0; coordinate < subspaceDimensions; ++coordinate) {
				const double delta = static_cast<double>(sample[sampleOffset + coordinate]) -
									 static_cast<double>(centroids[centroidOffset + coordinate]);
				distance += delta * delta;
			}
			return distance;
		}

		[[nodiscard]] size_t centroidOffset(const DeterministicPqOptions &options, uint32_t subspace,
											uint32_t centroid) {
			const uint32_t subspaceDimensions = options.dimensions / options.subspace_count;
			return (static_cast<size_t>(subspace) * options.centroid_count + centroid) * subspaceDimensions;
		}

		[[nodiscard]] std::vector<float> trainCentroids(const std::vector<std::vector<float>> &samples,
														const DeterministicPqOptions &options) {
			const uint32_t subspaceDimensions = options.dimensions / options.subspace_count;
			std::vector<float> centroids(static_cast<size_t>(options.dimensions) * options.centroid_count);
			for (uint32_t subspace = 0; subspace < options.subspace_count; ++subspace) {
				const size_t sampleOffset = static_cast<size_t>(subspace) * subspaceDimensions;
				for (uint32_t centroid = 0; centroid < options.centroid_count; ++centroid) {
					const size_t sourceIndex = static_cast<size_t>(centroid) * samples.size() / options.centroid_count;
					const size_t destination = centroidOffset(options, subspace, centroid);
					std::copy_n(samples[sourceIndex].begin() + static_cast<std::ptrdiff_t>(sampleOffset),
								subspaceDimensions, centroids.begin() + static_cast<std::ptrdiff_t>(destination));
				}
			}

			for (uint32_t iteration = 0; iteration < options.iteration_count; ++iteration) {
				std::vector<double> sums(centroids.size(), 0.0);
				std::vector<uint64_t> counts(static_cast<size_t>(options.subspace_count) * options.centroid_count, 0U);
				for (const std::vector<float> &sample: samples) {
					for (uint32_t subspace = 0; subspace < options.subspace_count; ++subspace) {
						const size_t sampleOffset = static_cast<size_t>(subspace) * subspaceDimensions;
						uint32_t bestCentroid = 0U;
						double bestDistance =
								squaredDistance(sample, sampleOffset, centroids, centroidOffset(options, subspace, 0U),
												subspaceDimensions);
						for (uint32_t centroid = 1U; centroid < options.centroid_count; ++centroid) {
							const double distance =
									squaredDistance(sample, sampleOffset, centroids,
													centroidOffset(options, subspace, centroid), subspaceDimensions);
							if (distance < bestDistance) {
								bestCentroid = centroid;
								bestDistance = distance;
							}
						}
						const size_t countIndex = static_cast<size_t>(subspace) * options.centroid_count + bestCentroid;
						if (counts[countIndex] == std::numeric_limits<uint64_t>::max()) { // ZYX_COV_EXCL_LINE: a
							// bounded in-memory span cannot contain UINT64_MAX assignments.
							fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
								 "relocatable deterministic PQ centroid assignment count overflowed");
						}
						++counts[countIndex];
						const size_t destination = centroidOffset(options, subspace, bestCentroid);
						for (uint32_t coordinate = 0; coordinate < subspaceDimensions; ++coordinate) {
							sums[destination + coordinate] += sample[sampleOffset + coordinate];
						}
					}
				}
				for (uint32_t subspace = 0; subspace < options.subspace_count; ++subspace) {
					const size_t sampleOffset = static_cast<size_t>(subspace) * subspaceDimensions;
					for (uint32_t centroid = 0; centroid < options.centroid_count; ++centroid) {
						const size_t countIndex = static_cast<size_t>(subspace) * options.centroid_count + centroid;
						const size_t destination = centroidOffset(options, subspace, centroid);
						if (counts[countIndex] == 0U) {
							const size_t sourceIndex =
									(static_cast<size_t>(subspace) * options.centroid_count + centroid) %
									samples.size();
							std::copy_n(samples[sourceIndex].begin() + static_cast<std::ptrdiff_t>(sampleOffset),
										subspaceDimensions,
										centroids.begin() + static_cast<std::ptrdiff_t>(destination));
							continue;
						}
						for (uint32_t coordinate = 0; coordinate < subspaceDimensions; ++coordinate) {
							const double mean =
									sums[destination + coordinate] / static_cast<double>(counts[countIndex]);
							// ZYX_COV_EXCL_START: validated finite BF16 inputs have finite float32-representable means.
							if (!std::isfinite(mean) || mean > static_cast<double>(std::numeric_limits<float>::max()) ||
								mean < -static_cast<double>(std::numeric_limits<float>::max())) {
								fail(FormatError::FERR_INVALID_STATE,
									 "relocatable deterministic PQ centroid is not representable as finite float32");
							}
							// ZYX_COV_EXCL_STOP
							centroids[destination + coordinate] = static_cast<float>(mean);
						}
					}
				}
			}
			return centroids;
		}

	} // namespace

	DeterministicProductQuantizer::DeterministicProductQuantizer(DeterministicPqOptions options,
																 std::vector<float> centroids) :
		options_(options), centroids_(std::move(centroids)) {}

	DeterministicProductQuantizer
	DeterministicProductQuantizer::train(std::span<const DeterministicPqTrainingRecord> records,
										 const DeterministicPqOptions &options) {
		validateOptions(options);
		std::vector<std::vector<float>> samples = selectSamples(records, options);
		return DeterministicProductQuantizer(options, trainCentroids(samples, options));
	}

	std::vector<std::byte> DeterministicProductQuantizer::codebookPayload() const {
		validateOptions(options_);
		if (centroids_.size() !=
			static_cast<size_t>(options_.dimensions) * options_.centroid_count) { // ZYX_COV_EXCL_LINE:
			// the private constructor receives exactly trainCentroids' validated allocation.
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable deterministic PQ codebook has an invalid centroid count");
		}
		std::vector<std::byte> bytes(centroids_.size() * sizeof(float));
		for (size_t index = 0; index < centroids_.size(); ++index) {
			if (!std::isfinite(
						centroids_[index])) { // ZYX_COV_EXCL_LINE: valid private state contains only checked means.
				fail(FormatError::FERR_INVALID_STATE,
					 "relocatable deterministic PQ codebook contains a non-finite centroid");
			}
			LittleEndianCodec::writeU32(bytes, index * sizeof(float), std::bit_cast<uint32_t>(centroids_[index]));
		}
		return bytes;
	}

	std::vector<uint8_t> DeterministicProductQuantizer::encode(std::span<const std::byte> rawBfloat16) const {
		validateOptions(options_);
		if (centroids_.size() !=
			static_cast<size_t>(options_.dimensions) * options_.centroid_count) { // ZYX_COV_EXCL_LINE:
			// the private constructor receives exactly trainCentroids' validated allocation.
			fail(FormatError::FERR_INVALID_STATE,
				 "relocatable deterministic PQ codebook has an invalid centroid count");
		}
		const std::vector<float> vector = decodeRaw(rawBfloat16, options_);
		const uint32_t subspaceDimensions = options_.dimensions / options_.subspace_count;
		std::vector<uint8_t> codes(options_.subspace_count);
		for (uint32_t subspace = 0; subspace < options_.subspace_count; ++subspace) {
			const size_t sampleOffset = static_cast<size_t>(subspace) * subspaceDimensions;
			uint32_t bestCentroid = 0U;
			double bestDistance = squaredDistance(vector, sampleOffset, centroids_,
												  centroidOffset(options_, subspace, 0U), subspaceDimensions);
			for (uint32_t centroid = 1U; centroid < options_.centroid_count; ++centroid) {
				const double distance =
						squaredDistance(vector, sampleOffset, centroids_, centroidOffset(options_, subspace, centroid),
										subspaceDimensions);
				if (distance < bestDistance) {
					bestCentroid = centroid;
					bestDistance = distance;
				}
			}
			codes[subspace] = static_cast<uint8_t>(bestCentroid);
		}
		return codes;
	}

	std::vector<uint8_t> DeterministicProductQuantizer::encodeWithCodebook(std::span<const std::byte> rawBfloat16,
																		   std::span<const std::byte> codebookPayload,
																		   uint32_t dimensions, uint32_t subspaceCount,
																		   uint32_t centroidCount) {
		const DeterministicPqOptions options = encodingOptions(dimensions, subspaceCount, centroidCount);
		return DeterministicProductQuantizer(options, decodeCodebook(codebookPayload, options)).encode(rawBfloat16);
	}

} // namespace graph::storage
