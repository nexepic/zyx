/**
 * @file VectorSemanticCodec.cpp
 * @brief Canonical logical Vector encoding implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "graph/vector/core/BFloat16.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] std::vector<std::byte> encodeBFloat16Impl(const PropertyValue &value, uint32_t dimensions,
																								 bool logicalWrite) {
			const auto failInput = [logicalWrite](const char *message) {
				if (logicalWrite) {
					throw std::invalid_argument(message);
				}
				fail(FormatError::FERR_INVALID_HEADER, message);
			};
			if (dimensions == 0U || dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS ||
				value.getType() != PropertyType::LIST || value.getList().size() != dimensions) {
				failInput(logicalWrite ? "Vector property has a dimension mismatch or is not a bounded numeric list"
									   : "relocatable Vector value has a dimension mismatch or is not a bounded numeric list");
			}
			std::vector<std::byte> bytes(static_cast<size_t>(dimensions) * sizeof(uint16_t));
			for (uint32_t index = 0U; index < dimensions; ++index) {
				const PropertyValue &coordinate = value.getList()[index];
				float converted = 0.0F;
				if (coordinate.getType() == PropertyType::DOUBLE) {
					const double source = std::get<double>(coordinate.getVariant());
					if (!std::isfinite(source) || source > std::numeric_limits<float>::max() ||
						source < -std::numeric_limits<float>::max()) {
						failInput(logicalWrite ? "Vector property has a non-finite or float-unrepresentable coordinate"
											   : "relocatable Vector value has a non-finite or float-unrepresentable coordinate");
					}
					converted = static_cast<float>(source);
				} else if (coordinate.getType() == PropertyType::INTEGER) {
					converted = static_cast<float>(std::get<int64_t>(coordinate.getVariant()));
				} else {
					failInput(logicalWrite ? "Vector property has a non-numeric coordinate"
									   : "relocatable Vector value has a non-numeric coordinate");
				}
				const vector::BFloat16 encoded(converted);
				LittleEndianCodec::writeU16(bytes, static_cast<size_t>(index) * sizeof(uint16_t), encoded.data);
			}
			return bytes;
		}

	} // namespace

	std::vector<std::byte> VectorSemanticCodec::encodeBFloat16(const PropertyValue &value, uint32_t dimensions) {
		return encodeBFloat16Impl(value, dimensions, false);
	}

	std::vector<std::byte> VectorSemanticCodec::encodeBFloat16ForLogicalWrite(const PropertyValue &value,
																			 uint32_t dimensions) {
		return encodeBFloat16Impl(value, dimensions, true);
	}

	bool VectorSemanticCodec::hasNonZeroFiniteL2Norm(std::span<const std::byte> rawBfloat16) {
		if (rawBfloat16.empty() || rawBfloat16.size() % sizeof(uint16_t) != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector norm validation requires a nonempty canonical BF16 payload");
		}
		double squaredNorm = 0.0;
		for (size_t offset = 0U; offset < rawBfloat16.size(); offset += sizeof(uint16_t)) {
			vector::BFloat16 encoded;
			encoded.data = LittleEndianCodec::readU16(rawBfloat16, offset);
			const float coordinate = encoded.toFloat();
			if (!std::isfinite(coordinate)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector norm validation found a non-finite BF16 coordinate");
			}
			squaredNorm += static_cast<double>(coordinate) * coordinate;
		}
		if (!std::isfinite(squaredNorm)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector norm validation overflowed its finite BF16 coordinate range");
		}
		return squaredNorm > 0.0;
	}

	std::vector<std::byte> VectorSemanticCodec::rawRecord(uint64_t vectorId, std::span<const std::byte> rawBfloat16) {
		if (vectorId == 0U || rawBfloat16.empty() || rawBfloat16.size() % sizeof(uint16_t) != 0U ||
			rawBfloat16.size() > std::numeric_limits<size_t>::max() - sizeof(uint64_t)) {
			fail(rawBfloat16.size() > std::numeric_limits<size_t>::max() - sizeof(uint64_t)
					 ? FormatError::FERR_ARITHMETIC_OVERFLOW
					 : FormatError::FERR_INVALID_HEADER,
				 "relocatable raw Vector record has an invalid identity or BF16 payload");
		}
		std::vector<std::byte> bytes(sizeof(uint64_t) + rawBfloat16.size());
		LittleEndianCodec::writeU64(bytes, 0U, vectorId);
		std::copy(rawBfloat16.begin(), rawBfloat16.end(), bytes.begin() + sizeof(uint64_t));
		return bytes;
	}

	std::vector<std::byte> VectorSemanticCodec::nodeMappingKey(uint64_t nodeId) {
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector mapping cannot use Node ID zero");
		}
		std::vector<std::byte> key(sizeof(uint64_t));
		for (size_t index = 0U; index < key.size(); ++index) {
			const uint32_t shift = static_cast<uint32_t>((key.size() - 1U - index) * 8U);
			key[index] = static_cast<std::byte>((nodeId >> shift) & UINT64_C(0xFF));
		}
		return key;
	}

	std::vector<std::byte> VectorSemanticCodec::vectorMappingKey(uint64_t vectorId) {
		if (vectorId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector mapping cannot use Vector ID zero");
		}
		std::vector<std::byte> key(sizeof(uint64_t));
		for (size_t index = 0U; index < key.size(); ++index) {
			const uint32_t shift = static_cast<uint32_t>((key.size() - 1U - index) * 8U);
			key[index] = static_cast<std::byte>((vectorId >> shift) & UINT64_C(0xFF));
		}
		return key;
	}

	VectorDistanceMetric VectorSemanticCodec::metricFor(const IndexDefinition &definition) {
		if (definition.kind != IndexDefinitionKind::IDK_VECTOR) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector metric requires a Vector Index definition");
		}
		if (definition.vector_metric == "l2") {
			return VectorDistanceMetric::VDM_L2;
		}
		if (definition.vector_metric == "ip") {
			return VectorDistanceMetric::VDM_INNER_PRODUCT;
		}
		if (definition.vector_metric == "cosine") {
			return VectorDistanceMetric::VDM_COSINE;
		}
		fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector Index definition has an unsupported metric");
	}

} // namespace graph::storage
