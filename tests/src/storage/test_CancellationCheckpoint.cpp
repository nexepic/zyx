/**
 * @file test_CancellationCheckpoint.cpp
 * @brief Contract tests for the storage cancellation boundary.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <memory>

#include <gtest/gtest.h>

#include "graph/storage/CancellationCheckpoint.hpp"

namespace graph::storage {
namespace {

	class CountingCancellationCheckpoint final : public CancellationCheckpoint {
	public:
		explicit CountingCancellationCheckpoint(bool &destroyed) : destroyed_(destroyed) {}

		~CountingCancellationCheckpoint() override { destroyed_ = true; }

		void check() const override { ++checks_; }

		[[nodiscard]] size_t checks() const { return checks_; }

	private:
		bool &destroyed_;
		mutable size_t checks_ = 0U;
	};

	TEST(CancellationCheckpointTest, DispatchesChecksAndDestroysImplementationsPolymorphically) {
		bool destroyed = false;
		{
			std::unique_ptr<CancellationCheckpoint> checkpoint =
					std::make_unique<CountingCancellationCheckpoint>(destroyed);
			checkpoint->check();
			checkpoint->check();
			EXPECT_EQ(static_cast<const CountingCancellationCheckpoint &>(*checkpoint).checks(), 2U);
		}
		EXPECT_TRUE(destroyed);
	}

} // namespace
} // namespace graph::storage
