/**
 * @file test_AdjacencyKeyCodec.cpp
 * @brief Canonical ordering and validation tests for global adjacency keys.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <functional>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"

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

	bool lessKey(const std::vector<std::byte> &left, const std::vector<std::byte> &right) {
		return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
	}

} // namespace

TEST(RelocatableAdjacencyKeyCodecTest, RoundTripsAndPreservesCanonicalOrdering) {
	const GlobalAdjacencyKey outFirst{GlobalAdjacencyDirection::GAD_OUT, 7U, 11U};
	const GlobalAdjacencyKey outNextEdge{GlobalAdjacencyDirection::GAD_OUT, 7U, 12U};
	const GlobalAdjacencyKey outNextNode{GlobalAdjacencyDirection::GAD_OUT, 8U, 1U};
	const GlobalAdjacencyKey incoming{GlobalAdjacencyDirection::GAD_IN, 1U, 1U};
	const std::vector<std::byte> outFirstBytes = AdjacencyKeyCodec::encode(outFirst);
	EXPECT_EQ(outFirstBytes.size(), RSS_GLOBAL_ADJACENCY_KEY_BYTES);
	EXPECT_EQ(AdjacencyKeyCodec::decode(outFirstBytes), outFirst);
	EXPECT_TRUE(lessKey(outFirstBytes, AdjacencyKeyCodec::encode(outNextEdge)));
	EXPECT_TRUE(lessKey(AdjacencyKeyCodec::encode(outNextEdge), AdjacencyKeyCodec::encode(outNextNode)));
	EXPECT_TRUE(lessKey(AdjacencyKeyCodec::encode(outNextNode), AdjacencyKeyCodec::encode(incoming)));
}

TEST(RelocatableAdjacencyKeyCodecTest, CreatesBoundsAndMatchesOnlyTheRequestedNodeDirection) {
	const std::vector<std::byte> lower = AdjacencyKeyCodec::lowerBound(GlobalAdjacencyDirection::GAD_OUT, 42U);
	const std::vector<std::byte> first =
			AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 42U, 1U});
	const std::vector<std::byte> nextNode =
			AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 43U, 1U});
	EXPECT_TRUE(lessKey(lower, first));
	EXPECT_TRUE(lessKey(first, nextNode));
	EXPECT_TRUE(AdjacencyKeyCodec::matchesNodeDirection(first, GlobalAdjacencyDirection::GAD_OUT, 42U));
	EXPECT_FALSE(AdjacencyKeyCodec::matchesNodeDirection(first, GlobalAdjacencyDirection::GAD_IN, 42U));
	EXPECT_FALSE(AdjacencyKeyCodec::matchesNodeDirection(first, GlobalAdjacencyDirection::GAD_OUT, 43U));
}

TEST(RelocatableAdjacencyKeyCodecTest, RejectsMalformedOrNonCanonicalKeys) {
	expectFormatError(
			[] { static_cast<void>(AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 0U, 1U})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[] { static_cast<void>(AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT, 1U, 0U})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[] {
				static_cast<void>(AdjacencyKeyCodec::encode(
						{static_cast<GlobalAdjacencyDirection>(99U), 1U, 1U}));
			},
			FormatError::FERR_INVALID_ENUM);
	const std::vector<std::byte> empty;
	expectFormatError([&] { static_cast<void>(AdjacencyKeyCodec::decode(empty)); },
			FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> malformed = AdjacencyKeyCodec::lowerBound(GlobalAdjacencyDirection::GAD_OUT, 1U);
	malformed[0] = std::byte{99U};
	expectFormatError([&] { static_cast<void>(AdjacencyKeyCodec::decode(malformed)); }, FormatError::FERR_INVALID_ENUM);
}
