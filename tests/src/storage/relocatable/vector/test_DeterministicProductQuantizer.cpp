/**
 * @file test_DeterministicProductQuantizer.cpp
 * @brief Deterministic relocatable Product Quantizer tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/vector/DeterministicProductQuantizer.hpp"
#include "graph/storage/relocatable/vector/VectorCodebook.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	std::vector<std::byte> bfloat16(std::initializer_list<float> values) {
		std::vector<std::byte> bytes(values.size() * sizeof(uint16_t));
		size_t index = 0;
		for (const float value: values) {
			LittleEndianCodec::writeU16(bytes, index * sizeof(uint16_t),
										static_cast<uint16_t>(std::bit_cast<uint32_t>(value) >> 16U));
			++index;
		}
		return bytes;
	}

	DeterministicPqOptions options() { return {4, 2, 2, 4, 8, 3}; }

	std::vector<DeterministicPqTrainingRecord> records() {
		return {{1, bfloat16({0.0F, 0.0F, 10.0F, 10.0F})},
				{2, bfloat16({0.0F, 1.0F, 9.0F, 10.0F})},
				{3, bfloat16({9.0F, 10.0F, 0.0F, 0.0F})},
				{4, bfloat16({10.0F, 9.0F, 1.0F, 0.0F})},
				{5, bfloat16({5.0F, 5.0F, 5.0F, 5.0F})}};
	}

} // namespace

TEST(RelocatableDeterministicProductQuantizerTest, TrainsRepeatablyAndProducesACompatibleCodebookPayload) {
	const std::vector<DeterministicPqTrainingRecord> input = records();
	const DeterministicProductQuantizer first = DeterministicProductQuantizer::train(input, options());
	const DeterministicProductQuantizer second = DeterministicProductQuantizer::train(input, options());
	const std::vector<std::byte> firstPayload = first.codebookPayload();
	EXPECT_EQ(first.options(), options());
	EXPECT_EQ(firstPayload, second.codebookPayload());
	EXPECT_EQ(firstPayload.size(), 4U * 2U * sizeof(float));

	VectorCodebookHeader header;
	header.codebook_id = 17;
	header.generation = Generation(1);
	header.element_type = VectorCodebookElementType::VCET_FLOAT32;
	header.dimensions = options().dimensions;
	header.subspace_count = options().subspace_count;
	header.centroid_count = options().centroid_count;
	header.element_bytes = sizeof(float);
	header.payload_bytes = static_cast<uint32_t>(firstPayload.size());
	header.payload_crc = graph::utils::calculateCrc(firstPayload.data(), firstPayload.size());
	VectorCodebookHeaderCodec::validatePayload(header, firstPayload);

	EXPECT_EQ(first.encode(input.front().raw_bfloat16), second.encode(input.front().raw_bfloat16));
	EXPECT_EQ(first.encode(input.back().raw_bfloat16).size(), options().subspace_count);
	EXPECT_EQ(DeterministicProductQuantizer::encodeWithCodebook(input.front().raw_bfloat16, firstPayload,
																options().dimensions, options().subspace_count,
																options().centroid_count),
			  first.encode(input.front().raw_bfloat16));
}

TEST(RelocatableDeterministicProductQuantizerTest, UsesStableTieBreaksAndReinitializesEmptyCentroids) {
	DeterministicPqOptions small = options();
	small.centroid_count = 2;
	small.maximum_records = 1;
	small.maximum_samples = 1;
	const DeterministicPqTrainingRecord record{1, bfloat16({2.0F, 2.0F, 2.0F, 2.0F})};
	const DeterministicProductQuantizer quantizer = DeterministicProductQuantizer::train(std::span(&record, 1), small);
	EXPECT_EQ(quantizer.encode(record.raw_bfloat16), std::vector<uint8_t>({0, 0}));
	EXPECT_EQ(quantizer.codebookPayload().size(), 4U * 2U * sizeof(float));
}

TEST(RelocatableDeterministicProductQuantizerTest, RejectsUnboundedMalformedAndNonCanonicalTrainingInput) {
	const std::vector<DeterministicPqTrainingRecord> input = records();
	DeterministicPqOptions invalid = options();
	invalid.subspace_count = 3;
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(input, invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = options();
	invalid.centroid_count = 257;
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(input, invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = options();
	invalid.maximum_samples = invalid.maximum_records + 1U;
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(input, invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train({}, options())); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<DeterministicPqTrainingRecord> unordered = input;
	std::swap(unordered[0], unordered[1]);
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(unordered, options())); },
					  FormatError::FERR_INVALID_HEADER);
	std::vector<DeterministicPqTrainingRecord> malformed = input;
	malformed.front().raw_bfloat16.pop_back();
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(malformed, options())); },
					  FormatError::FERR_INVALID_HEADER);
	malformed = input;
	LittleEndianCodec::writeU16(malformed.front().raw_bfloat16, 0, 0x7F80U);
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(malformed, options())); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableDeterministicProductQuantizerTest, RejectsEveryZeroAndExceededTrainingBound) {
	const std::vector<DeterministicPqTrainingRecord> input = records();
	const auto expectInvalidOptions = [&input](DeterministicPqOptions invalid) {
		expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(input, invalid)); },
						  FormatError::FERR_INVALID_HEADER);
	};

	DeterministicPqOptions invalid = options();
	invalid.dimensions = 0U;
	expectInvalidOptions(invalid);
	invalid = options();
	invalid.subspace_count = 0U;
	expectInvalidOptions(invalid);
	invalid = options();
	invalid.subspace_count = invalid.dimensions + 1U;
	expectInvalidOptions(invalid);
	invalid = options();
	invalid.centroid_count = 0U;
	expectInvalidOptions(invalid);
	invalid = options();
	invalid.iteration_count = 0U;
	expectInvalidOptions(invalid);
	invalid = options();
	invalid.maximum_records = 0U;
	expectInvalidOptions(invalid);
	invalid = options();
	invalid.maximum_samples = 0U;
	expectInvalidOptions(invalid);
	invalid = options();
	invalid.maximum_records = input.size() - 1U;
	invalid.maximum_samples = invalid.maximum_records;
	expectInvalidOptions(invalid);

	std::vector<DeterministicPqTrainingRecord> nonCanonical = input;
	nonCanonical.front().vector_id = 0U;
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(nonCanonical, options())); },
					  FormatError::FERR_INVALID_HEADER);
	nonCanonical = input;
	nonCanonical[1].vector_id = nonCanonical.front().vector_id;
	expectFormatError([&] { static_cast<void>(DeterministicProductQuantizer::train(nonCanonical, options())); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableDeterministicProductQuantizerTest, RejectsEncodeShapesThatDoNotMatchItsCanonicalRawLayout) {
	const DeterministicProductQuantizer quantizer = DeterministicProductQuantizer::train(records(), options());
	std::vector<std::byte> malformed = bfloat16({1.0F, 2.0F, 3.0F});
	expectFormatError([&] { static_cast<void>(quantizer.encode(malformed)); }, FormatError::FERR_INVALID_HEADER);
	malformed = bfloat16({1.0F, 2.0F, 3.0F, std::numeric_limits<float>::infinity()});
	expectFormatError([&] { static_cast<void>(quantizer.encode(malformed)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableDeterministicProductQuantizerTest, RejectsMalformedPersistedCodebookBeforeEncoding) {
	const std::vector<DeterministicPqTrainingRecord> input = records();
	const DeterministicProductQuantizer quantizer = DeterministicProductQuantizer::train(input, options());
	std::vector<std::byte> payload = quantizer.codebookPayload();
	expectFormatError(
			[&] {
				static_cast<void>(DeterministicProductQuantizer::encodeWithCodebook(
						input.front().raw_bfloat16, std::span(payload).first(payload.size() - 1U), options().dimensions,
						options().subspace_count, options().centroid_count));
			},
			FormatError::FERR_INVALID_HEADER);
	LittleEndianCodec::writeU32(payload, 0U, std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()));
	expectFormatError(
			[&] {
				static_cast<void>(DeterministicProductQuantizer::encodeWithCodebook(
						input.front().raw_bfloat16, payload, options().dimensions, options().subspace_count,
						options().centroid_count));
			},
			FormatError::FERR_INVALID_HEADER);
}
