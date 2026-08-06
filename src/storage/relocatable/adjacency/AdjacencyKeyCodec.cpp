/**
 * @file AdjacencyKeyCodec.cpp
 * @brief Canonical key codec for the global ordinary-graph adjacency tree.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool isDirection(GlobalAdjacencyDirection direction) {
			return direction == GlobalAdjacencyDirection::GAD_OUT || direction == GlobalAdjacencyDirection::GAD_IN;
		}

		void writeBigEndianU64(std::span<std::byte> bytes, size_t offset, uint64_t value) {
			for (uint32_t shift = 56U;; shift -= 8U) {
				bytes[offset++] = static_cast<std::byte>((value >> shift) & UINT64_C(0xFF));
				if (shift == 0U) {
					return;
				}
			}
		}

		[[nodiscard]] uint64_t readBigEndianU64(std::span<const std::byte> bytes, size_t offset) {
			uint64_t value = 0U;
			for (size_t index = 0U; index < sizeof(uint64_t); ++index) {
				value = (value << 8U) | std::to_integer<uint8_t>(bytes[offset + index]);
			}
			return value;
		}

		void validateDirectionAndNode(GlobalAdjacencyDirection direction, uint64_t nodeId) {
			if (!isDirection(direction)) {
				fail(FormatError::FERR_INVALID_ENUM, "global adjacency key has an unknown direction");
			}
			if (nodeId == 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "global adjacency key has a zero Node ID");
			}
		}

	} // namespace

	std::vector<std::byte> AdjacencyKeyCodec::encode(const GlobalAdjacencyKey &key) {
		validateDirectionAndNode(key.direction, key.node_id);
		if (key.edge_id == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "global adjacency key has a zero Edge ID");
		}
		std::vector<std::byte> bytes(RSS_GLOBAL_ADJACENCY_KEY_BYTES);
		bytes[0] = static_cast<std::byte>(key.direction);
		writeBigEndianU64(bytes, 1U, key.node_id);
		writeBigEndianU64(bytes, 1U + sizeof(uint64_t), key.edge_id);
		return bytes;
	}

	GlobalAdjacencyKey AdjacencyKeyCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() != RSS_GLOBAL_ADJACENCY_KEY_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "global adjacency key has an invalid encoded length");
		}
		GlobalAdjacencyKey key;
		key.direction = static_cast<GlobalAdjacencyDirection>(std::to_integer<uint8_t>(bytes[0]));
		key.node_id = readBigEndianU64(bytes, 1U);
		key.edge_id = readBigEndianU64(bytes, 1U + sizeof(uint64_t));
		validateDirectionAndNode(key.direction, key.node_id);
		if (key.edge_id == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "global adjacency key has a zero Edge ID");
		}
		return key;
	}

	std::vector<std::byte> AdjacencyKeyCodec::lowerBound(GlobalAdjacencyDirection direction, uint64_t nodeId) {
		validateDirectionAndNode(direction, nodeId);
		std::vector<std::byte> bytes(RSS_GLOBAL_ADJACENCY_KEY_BYTES);
		bytes[0] = static_cast<std::byte>(direction);
		writeBigEndianU64(bytes, 1U, nodeId);
		return bytes;
	}

	bool AdjacencyKeyCodec::matchesNodeDirection(std::span<const std::byte> bytes,
															 GlobalAdjacencyDirection direction, uint64_t nodeId) {
		validateDirectionAndNode(direction, nodeId);
		const GlobalAdjacencyKey key = decode(bytes);
		return key.direction == direction && key.node_id == nodeId;
	}

} // namespace graph::storage
