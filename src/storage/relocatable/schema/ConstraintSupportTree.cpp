/**
 * @file ConstraintSupportTree.cpp
 * @brief Fixed-width SHA-256 routing keys for internal constraint support trees.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/Sha256.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(const char *message) { throw std::invalid_argument(message); }

		void appendBigEndianU64(std::vector<std::byte> &bytes, uint64_t value) {
			for (uint32_t shift = 56U;; shift -= 8U) {
				bytes.push_back(static_cast<std::byte>((value >> shift) & UINT64_C(0xFF)));
				if (shift == 0U)
					return;
			}
		}

		[[nodiscard]] uint64_t readBigEndianU64(std::span<const std::byte> bytes, size_t offset) {
			uint64_t value = 0U;
			for (size_t index = 0U; index < sizeof(value); ++index) {
				value = (value << 8U) | std::to_integer<uint8_t>(bytes[offset + index]);
			}
			return value;
		}

	} // namespace

	std::vector<std::byte> ConstraintSupportTree::canonicalKey(std::span<const PropertyValue> values) {
		if (values.empty())
			fail("relocatable constraint support key requires at least one Property value");
		std::vector<std::byte> key;
		for (const PropertyValue &value: values)
			CanonicalPropertyValueCodec::append(key, value);
		if (key.empty()) { // ZYX_COV_EXCL_LINE: CanonicalPropertyValueCodec writes at least one type byte per value.
			fail("relocatable constraint support key canonical encoding is unexpectedly empty");
		}
		return key;
	}

	std::array<std::byte, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES>
	ConstraintSupportTree::digest(std::span<const std::byte> canonicalKey) {
		if (canonicalKey.empty() || canonicalKey.size() > std::numeric_limits<uint64_t>::max() / 8U) {
			fail("relocatable constraint support digest requires a bounded nonempty canonical key");
		}
		return utils::Sha256::digest(canonicalKey);
	}

	std::vector<std::byte> ConstraintSupportTree::entryKey(std::span<const std::byte> canonicalKey, uint64_t entityId) {
		if (entityId == 0U)
			fail("relocatable constraint support tree entry requires a nonzero Entity ID");
		const auto digestBytes = digest(canonicalKey);
		std::vector<std::byte> key;
		key.reserve(RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES);
		key.insert(key.end(), digestBytes.begin(), digestBytes.end());
		appendBigEndianU64(key, entityId);
		return key;
	}

	std::vector<std::byte> ConstraintSupportTree::lowerBound(std::span<const std::byte> canonicalKey) {
		const auto digestBytes = digest(canonicalKey);
		std::vector<std::byte> key;
		key.reserve(RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES);
		key.insert(key.end(), digestBytes.begin(), digestBytes.end());
		appendBigEndianU64(key, 1U);
		return key;
	}

	bool ConstraintSupportTree::matchesDigest(std::span<const std::byte> candidate,
											  std::span<const std::byte> canonicalKey) {
		if (candidate.size() != RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES) {
			throw std::invalid_argument("relocatable constraint support tree entry key has an invalid width");
		}
		const auto expected = digest(canonicalKey);
		return std::equal(expected.begin(), expected.end(), candidate.begin());
	}

	uint64_t ConstraintSupportTree::entryEntityId(std::span<const std::byte> key) {
		if (key.size() != RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES) {
			fail("relocatable constraint support tree entry key has an invalid width");
		}
		const uint64_t entityId = readBigEndianU64(key, RSS_CONSTRAINT_SUPPORT_DIGEST_BYTES);
		if (entityId == 0U)
			fail("relocatable constraint support tree entry key has a zero Entity ID");
		return entityId;
	}

} // namespace graph::storage
