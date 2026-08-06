/**
 * @file DeterministicProductQuantizer.hpp
 * @brief Bounded deterministic Product Quantizer for relocatable derived vector data.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace graph::storage {

	// Raw coordinates use the same explicit little-endian BF16 representation as
	// canonical migrated vectors. The caller supplies records in Vector ID order.
	struct DeterministicPqTrainingRecord {
		uint64_t vector_id = 0;
		std::vector<std::byte> raw_bfloat16;

		bool operator==(const DeterministicPqTrainingRecord &) const = default;
	};

	struct DeterministicPqOptions {
		uint32_t dimensions = 0;
		uint32_t subspace_count = 0;
		uint32_t centroid_count = 0;
		uint32_t iteration_count = 0;
		// Both limits bound all caller-controlled memory and CPU work. Sampling
		// selects a stable lowest-rank subset when input exceeds maximum_samples.
		size_t maximum_records = 0;
		size_t maximum_samples = 0;

		constexpr bool operator==(const DeterministicPqOptions &) const = default;
	};

	class DeterministicProductQuantizer final {
	public:
		[[nodiscard]] static DeterministicProductQuantizer train(std::span<const DeterministicPqTrainingRecord> records,
																 const DeterministicPqOptions &options);

		[[nodiscard]] const DeterministicPqOptions &options() const { return options_; }
		// The payload is ordered by subspace, centroid, then coordinate and is
		// ready for VectorCodebookStore as an explicit little-endian float32 span.
		[[nodiscard]] std::vector<std::byte> codebookPayload() const;
		// Returns one byte per PQ subspace. The first centroid wins deterministic
		// distance ties, so re-encoding is independent of host scheduling.
		[[nodiscard]] std::vector<uint8_t> encode(std::span<const std::byte> rawBfloat16) const;
		// Encodes against an immutable persisted Codebook without retraining. The
		// caller supplies the Codec shape explicitly so a stale or malformed
		// Codebook payload cannot be interpreted using a different Vector layout.
		[[nodiscard]] static std::vector<uint8_t> encodeWithCodebook(std::span<const std::byte> rawBfloat16,
																	 std::span<const std::byte> codebookPayload,
																	 uint32_t dimensions, uint32_t subspaceCount,
																	 uint32_t centroidCount);

	private:
		DeterministicProductQuantizer(DeterministicPqOptions options, std::vector<float> centroids);

		DeterministicPqOptions options_;
		std::vector<float> centroids_;
	};

} // namespace graph::storage
