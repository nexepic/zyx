/**
 * @file test_BoundedBatch.cpp
 * @brief Tests for the shared bounded execution batch.
 */

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "graph/query/execution/BoundedBatch.hpp"

using graph::query::execution::BoundedBatch;

TEST(BoundedBatchTest, EnforcesRowsAndReusesStorageAfterClear) {
	BoundedBatch<std::string> batch(2U, 16U, [](const std::string &value) { return value.size(); });
	batch.reserve(10U);
	batch.append("abc");
	batch.append("de");
	EXPECT_EQ(batch.size(), 2U);
	EXPECT_EQ(batch.bytes(), 5U);
	EXPECT_THROW(batch.append("f"), std::runtime_error);
	batch.clear();
	EXPECT_TRUE(batch.empty());
	EXPECT_EQ(batch.bytes(), 0U);
	batch.append("123456789");
	EXPECT_EQ(batch.bytes(), 9U);
}

TEST(BoundedBatchTest, EnforcesByteBudgetAndRejectsOversizedValue) {
	BoundedBatch<std::string> batch(4U, 5U, [](const std::string &value) { return value.size(); });
	EXPECT_FALSE(batch.wouldExceedBytes("12345"));
	EXPECT_THROW(batch.append("123456"), std::runtime_error);
	batch.append("123");
	EXPECT_TRUE(batch.wouldExceedBytes("456"));
	EXPECT_THROW(batch.append("456"), std::runtime_error);
}

TEST(BoundedBatchTest, RecalculatesBytesAfterRowsAreEnriched) {
	BoundedBatch<std::string> batch(2U, 8U, [](const std::string &value) { return value.size(); });
	batch.append("a");
	batch.values().front().append("bcdef");
	EXPECT_EQ(batch.bytes(), 1U);
	batch.recalculateBytes();
	EXPECT_EQ(batch.bytes(), 6U);
	batch.values().front().append("123");
	EXPECT_THROW(batch.recalculateBytes(), std::runtime_error);
}

TEST(BoundedBatchTest, RejectsInvalidConstruction) {
	EXPECT_THROW((BoundedBatch<int>(0U, 1U, [](const int &) { return size_t{1U}; })), std::invalid_argument);
	EXPECT_THROW((BoundedBatch<int>(1U, 0U, [](const int &) { return size_t{1U}; })), std::invalid_argument);
	EXPECT_THROW((BoundedBatch<int>(1U, 1U, {})), std::invalid_argument);
}

TEST(BoundedBatchTest, CapsEagerReservationByTheByteBudget) {
	using Row = std::array<std::byte, 4096U>;
	const size_t maximumRows = std::numeric_limits<size_t>::max();
	BoundedBatch<Row> batch(maximumRows, 3U * sizeof(Row), [](const Row &) { return sizeof(Row); });
	EXPECT_NO_THROW(batch.reserve(maximumRows));
	EXPECT_LE(batch.values().capacity(), 3U);
	EXPECT_TRUE(batch.empty());
	EXPECT_EQ(batch.bytes(), 0U);
	for (size_t index = 0U; index < 3U; ++index) {
		batch.append(Row{});
	}
	EXPECT_EQ(batch.size(), 3U);
	EXPECT_THROW(batch.append(Row{}), std::runtime_error);
	batch.clear();
	EXPECT_NO_THROW(batch.reserve(maximumRows));
	EXPECT_LE(batch.values().capacity(), 3U);

	BoundedBatch<Row> tooSmall(maximumRows, sizeof(Row) - 1U, [](const Row &) { return sizeof(Row); });
	EXPECT_NO_THROW(tooSmall.reserve(maximumRows));
	EXPECT_EQ(tooSmall.values().capacity(), 0U);
	EXPECT_THROW(tooSmall.append(Row{}), std::runtime_error);
}

TEST(BoundedBatchTest, SharesLiveBytesAcrossOwnersAndRefundsClearAndDestruction) {
	using graph::query::execution::MaterializedByteBudget;
	using graph::query::execution::MaterializedByteReservation;
	MaterializedByteBudget budget(10U);
	{
		MaterializedByteReservation source(&budget);
		source.set(3U);
		BoundedBatch<std::string> first(4U, 10U, [](const auto &value) { return value.size(); }, &budget, false);
		first.append("1234");
		EXPECT_EQ(budget.used(), 7U);
		EXPECT_TRUE(first.wouldExceedBytes("5678"));
		EXPECT_THROW(first.append("5678"), std::runtime_error);
		EXPECT_EQ(first.size(), 1U);
		EXPECT_EQ(budget.used(), 7U);
		{
			BoundedBatch<std::string> second(4U, 10U, [](const auto &value) { return value.size(); }, &budget, false);
			second.append("abc");
			EXPECT_EQ(budget.available(), 0U);
			first.clear();
			EXPECT_EQ(budget.available(), 4U);
		}
		EXPECT_EQ(budget.used(), 3U);
		first.append("abcdefg");
	}
	EXPECT_EQ(budget.available(), 10U);
}

TEST(BoundedBatchTest, SharedRecalculationAndOwnershipTransferPreserveAccounting) {
	using graph::query::execution::MaterializedByteBudget;
	using graph::query::execution::MaterializedByteReservation;
	MaterializedByteBudget budget(10U);
	MaterializedByteReservation source(&budget);
	source.set(7U);
	BoundedBatch<std::string> batch(4U, 10U, [](const auto &value) { return value.size(); }, &budget, false);
	batch.append("a");
	batch.values().front() = "abcd";
	EXPECT_THROW(batch.recalculateBytes(), std::runtime_error);
	EXPECT_EQ(batch.bytes(), 1U);
	EXPECT_EQ(budget.used(), 8U);
	source.reset();
	batch.recalculateBytes();
	EXPECT_EQ(budget.used(), 4U);
	batch.values().front() = "ab";
	batch.recalculateBytes();
	EXPECT_EQ(budget.used(), 2U);
	BoundedBatch<std::string> receiver(4U, 10U, [](const auto &value) { return value.size(); }, &budget, false);
	receiver.append(batch.popBack());
	EXPECT_TRUE(batch.empty());
	EXPECT_EQ(batch.bytes(), 0U);
	EXPECT_EQ(budget.used(), 2U);
	EXPECT_THROW(batch.popBack(), std::logic_error);
}

TEST(BoundedBatchTest, ChargedPopKeepsMovedValueInTheSharedBudget) {
	graph::query::execution::MaterializedByteBudget budget(8U);
	BoundedBatch<std::string> batch(3U, 8U, [](const std::string &value) { return value.size(); }, &budget, false);
	batch.append("aa");
	batch.append("bbbb");
	EXPECT_EQ(budget.used(), 6U);
	{
		auto popped = batch.popBackCharged();
		EXPECT_EQ(popped.value, "bbbb");
		EXPECT_EQ(popped.reservation.bytes(), 4U);
		EXPECT_EQ(batch.bytes(), 2U);
		EXPECT_EQ(budget.used(), 6U);
		EXPECT_THROW(batch.append("ccc"), std::runtime_error);
	}
	EXPECT_EQ(budget.used(), 2U);
	batch.append("ccc");
	EXPECT_EQ(budget.used(), 5U);
}

TEST(BoundedBatchTest, ChargedAppendTransfersAnExistingRowWithoutDoubleCharge) {
	using graph::query::execution::MaterializedByteBudget;
	using graph::query::execution::MaterializedByteReservation;
	MaterializedByteBudget budget(6U);
	BoundedBatch<std::string> source(2U, 6U, [](const auto &value) { return value.size(); }, &budget, false);
	BoundedBatch<std::string> destination(2U, 6U, [](const auto &value) { return value.size(); }, &budget, false);
	source.append("abcd");
	destination.append("xy");
	auto moved = source.popBackCharged();
	EXPECT_EQ(budget.used(), 6U);
	destination.appendCharged(std::move(moved.value), std::move(moved.reservation));
	EXPECT_EQ(destination.bytes(), 6U);
	EXPECT_EQ(budget.used(), 6U);
	destination.clear();
	EXPECT_EQ(budget.used(), 0U);
	MaterializedByteReservation stale(&budget);
	stale.set(3U);
	EXPECT_THROW(destination.appendCharged("abcd", std::move(stale)), std::logic_error);
	EXPECT_TRUE(destination.empty());
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, ReservationSplitTransfersBytesWithoutChangingTheSharedTotal) {
	graph::query::execution::MaterializedByteBudget budget(7U);
	graph::query::execution::MaterializedByteReservation owner(&budget);
	owner.set(7U);
	{
		auto transferred = owner.splitOff(3U);
		EXPECT_EQ(owner.bytes(), 4U);
		EXPECT_EQ(transferred.bytes(), 3U);
		EXPECT_EQ(budget.used(), 7U);
		EXPECT_THROW(static_cast<void>(owner.splitOff(5U)), std::logic_error);
		EXPECT_EQ(budget.used(), 7U);
	}
	EXPECT_EQ(budget.used(), 4U);
	owner.reset();
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, ReservationAbsorbTransfersLiveBytesWithoutRefunding) {
	using graph::query::execution::MaterializedByteBudget;
	using graph::query::execution::MaterializedByteReservation;
	MaterializedByteBudget budget(8U);
	MaterializedByteBudget unrelatedBudget(8U);
	MaterializedByteReservation pending(&budget);
	pending.set(6U);
	MaterializedByteReservation retained(&budget);
	retained.set(2U);
	auto row = pending.splitOff(3U);
	EXPECT_EQ(budget.used(), 8U);
	MaterializedByteReservation unrelated(&unrelatedBudget);
	unrelated.set(1U);
	EXPECT_THROW(retained.absorb(std::move(unrelated)), std::logic_error);
	EXPECT_EQ(unrelated.bytes(), 1U);
	retained.absorb(std::move(row));
	EXPECT_EQ(retained.bytes(), 5U);
	retained.absorb(std::move(retained));
	EXPECT_EQ(retained.bytes(), 5U);
	EXPECT_EQ(row.bytes(), 0U);
	EXPECT_EQ(pending.bytes(), 3U);
	EXPECT_EQ(budget.used(), 8U);
	retained.reset();
	EXPECT_EQ(budget.used(), 3U);
}

TEST(BoundedBatchTest, FailedInsertionRefundsSharedReservation) {
	struct Row {
		Row() = default;
		Row(Row &&) { throw std::runtime_error("insertion failure"); }
	};
	graph::query::execution::MaterializedByteBudget budget(10U);
	BoundedBatch<Row> batch(4U, 10U, [](const Row &) { return size_t{4U}; }, &budget, false);
	EXPECT_THROW(batch.append(Row{}), std::runtime_error);
	EXPECT_TRUE(batch.empty());
	EXPECT_EQ(batch.bytes(), 0U);
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, ReservationsMoveWithoutDuplicatingChargesAndCannotOverflow) {
	using graph::query::execution::MaterializedByteBudget;
	using graph::query::execution::MaterializedByteReservation;
	const size_t maximum = std::numeric_limits<size_t>::max();
	MaterializedByteBudget budget(maximum);
	{
		MaterializedByteReservation first(&budget);
		first.set(maximum - 1U);
		MaterializedByteReservation moved(std::move(first));
		MaterializedByteReservation last(&budget);
		EXPECT_THROW(last.set(2U), std::runtime_error);
		last.set(1U);
		EXPECT_EQ(budget.available(), 0U);
		moved = std::move(last);
		EXPECT_EQ(budget.used(), 1U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, MoveConstructionEmptiesSourceAndRetainsItsSharedBudgetForReuse) {
	using graph::query::execution::MaterializedByteBudget;
	MaterializedByteBudget budget(10U);
	{
		BoundedBatch<std::string> source(3U, 10U, [](const std::string &value) { return value.size(); }, &budget, false);
		source.append("abcdef");
		BoundedBatch<std::string> moved(std::move(source));
		EXPECT_TRUE(std::ranges::equal(moved.values(), std::vector<std::string>{"abcdef"}));
		EXPECT_EQ(moved.bytes(), 6U);
		EXPECT_TRUE(source.empty());
		EXPECT_EQ(source.bytes(), 0U);
		EXPECT_EQ(budget.used(), 6U);
		EXPECT_THROW(source.append("12345"), std::runtime_error);
		source.append("1234");
		EXPECT_EQ(budget.used(), 10U);
		EXPECT_TRUE(moved.wouldExceedBytes("x"));
		moved.clear();
		EXPECT_EQ(budget.used(), 4U);
		EXPECT_EQ(source.popBack(), "1234");
		EXPECT_EQ(budget.used(), 0U);
		moved.append("1234567890");
		EXPECT_EQ(budget.used(), 10U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, MoveAssignmentRefundsOldOwnerAndKeepsBothBatchesReusable) {
	using graph::query::execution::MaterializedByteBudget;
	for (bool sameBudget : {false, true}) {
		SCOPED_TRACE(sameBudget);
		MaterializedByteBudget first(10U);
		MaterializedByteBudget second(10U);
		{
			BoundedBatch<std::string> source(3U, 10U, [](const std::string &value) { return value.size(); }, &first, false);
			BoundedBatch<std::string> destination(1U, 3U, [](const std::string &value) { return value.size(); }, sameBudget ? &first : &second, false);
			source.append("abcdef");
			destination.append("old");
			destination = std::move(source);
			EXPECT_EQ(first.used(), 6U);
			EXPECT_EQ(second.used(), 0U);
			EXPECT_EQ(destination.maximumRows(), 3U);
			EXPECT_EQ(destination.maximumBytes(), 10U);
			EXPECT_EQ(destination.bytes(), 6U);
			EXPECT_TRUE(source.empty());
			EXPECT_EQ(source.bytes(), 0U);
			source.append("1234");
			EXPECT_EQ(first.used(), 10U);
			EXPECT_THROW(destination.append("x"), std::runtime_error);
			auto *self = &destination;
			destination = std::move(*self);
			EXPECT_TRUE(std::ranges::equal(destination.values(), std::vector<std::string>{"abcdef"}));
			EXPECT_EQ(first.used(), 10U);
			source.clear();
			destination.append("more");
			EXPECT_EQ(first.used(), 10U);
		}
		EXPECT_EQ(first.used(), 0U);
		EXPECT_EQ(second.used(), 0U);
	}
}

namespace {
	struct CopyFailingSizer {
		bool *fail;
		explicit CopyFailingSizer(bool &enabled) : fail(&enabled) {}
		CopyFailingSizer(const CopyFailingSizer &other) : fail(other.fail) {
			if (*fail) throw std::runtime_error("estimator copy rejected");
		}
		size_t operator()(const std::string &value) const { return value.size(); }
	};
}

TEST(BoundedBatchTest, FailedEstimatorCopyPreservesSourceDestinationAndReservations) {
	using graph::query::execution::MaterializedByteBudget;
	bool fail = false;
	MaterializedByteBudget budget(10U);
	BoundedBatch<std::string> source(3U, 10U, CopyFailingSizer(fail), &budget, false);
	BoundedBatch<std::string> destination(1U, 3U, [](const std::string &value) { return value.size(); }, &budget, false);
	source.append("abcdef");
	destination.append("old");
	fail = true;
	EXPECT_THROW(static_cast<void>(BoundedBatch<std::string>(std::move(source))), std::runtime_error);
	EXPECT_THROW(destination = std::move(source), std::runtime_error);
	EXPECT_TRUE(std::ranges::equal(source.values(), std::vector<std::string>{"abcdef"}));
	EXPECT_EQ(source.bytes(), 6U);
	EXPECT_TRUE(std::ranges::equal(destination.values(), std::vector<std::string>{"old"}));
	EXPECT_EQ(destination.bytes(), 3U);
	EXPECT_EQ(destination.maximumRows(), 1U);
	EXPECT_EQ(destination.maximumBytes(), 3U);
	EXPECT_EQ(budget.used(), 9U);
	fail = false;
	destination = std::move(source);
	EXPECT_EQ(budget.used(), 6U);
	source.append("1234");
	EXPECT_EQ(budget.used(), 10U);
}

TEST(BoundedBatchTest, RecalculationAbsorbsEnrichmentWithoutChargingTwice) {
	using graph::query::execution::MaterializedByteBudget;
	using graph::query::execution::MaterializedByteReservation;
	MaterializedByteBudget budget(6U);
	{
		BoundedBatch<std::string> batch(1U, 6U,
			[](const std::string &value) { return value.size(); }, &budget, false);
		batch.append("abc");
		MaterializedByteReservation enrichment(&budget);
		enrichment.set(3U);
		batch.values().front() += "def";
		EXPECT_EQ(budget.used(), 6U);
		EXPECT_NO_THROW(batch.recalculateBytes(std::move(enrichment)));
		EXPECT_EQ(batch.bytes(), 6U);
		EXPECT_EQ(budget.used(), 6U);
		EXPECT_EQ(enrichment.bytes(), 0U);
		auto charged = batch.popBackCharged();
		EXPECT_EQ(charged.value, "abcdef");
		EXPECT_EQ(charged.reservation.bytes(), 6U);
		EXPECT_EQ(budget.used(), 6U);
	}
	EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, CapacityOwnerRefundsOnlyAfterClearReleasesItsBuffer) {
	graph::query::execution::MaterializedByteBudget budget(1024U);
	{
		BoundedBatch<std::string> batch(4U,1024U,[](const auto &value){return value.size();},&budget);
		batch.append("abc");const auto slots=batch.values().capacity()*sizeof(std::string);
		EXPECT_EQ(budget.used(),slots+3U);
		batch.clear();EXPECT_EQ(batch.values().capacity(),0U);EXPECT_EQ(budget.used(),0U);
	}
	EXPECT_EQ(budget.used(),0U);
}
TEST(BoundedBatchTest, CapacityAdmissionFailureRefundsPayloadAndKeepsBatchReusable) {
	graph::query::execution::MaterializedByteBudget budget(sizeof(std::string));
	BoundedBatch<std::string> batch(4U,sizeof(std::string),[](const auto &value){return value.size();},&budget);
	EXPECT_THROW(batch.append("x"),graph::query::execution::MaterializedByteLimitExceeded);
	EXPECT_TRUE(batch.empty());EXPECT_EQ(batch.bytes(),0U);EXPECT_EQ(budget.used(),0U);
	batch.append("");EXPECT_EQ(batch.size(),1U);EXPECT_EQ(budget.used(),sizeof(std::string));
}

TEST(BoundedBatchTest, CapacityMoveAcrossBudgetsTransfersTheAllocatorOwnerAndRefundsOldSlots) {
	graph::query::execution::MaterializedByteBudget first(1024U),second(1024U);
	{
		BoundedBatch<std::string> source(3U,1024U,[](const auto &value){return value.size();},&first);
		BoundedBatch<std::string> destination(3U,1024U,[](const auto &value){return value.size();},&second);
		source.append("abcdef");destination.append("old");destination=std::move(source);
		EXPECT_EQ(second.used(),0U);EXPECT_EQ(destination.values().get_allocator().resource(),&first);
		const auto sourceSlots=destination.values().capacity()*sizeof(std::string);
		EXPECT_EQ(first.used(),sourceSlots+6U);
		source.append("x");EXPECT_EQ(first.used(),sourceSlots+7U+source.values().capacity()*sizeof(std::string));
	}
	EXPECT_EQ(first.used(),0U);EXPECT_EQ(second.used(),0U);
}

TEST(BoundedBatchTest, MoveConstructionPreservesEmbeddedObjectClassification) {
    using namespace graph::query::execution;
    MaterializedByteBudget budget(1024U);
    {
        BoundedBatch<uint64_t> source(8U, 1024U, [](const uint64_t &) { return sizeof(uint64_t); },
                                      &budget, true, sizeof(uint64_t));
        source.append(42U);
        const size_t retained = budget.used();
        BoundedBatch<uint64_t> moved(std::move(source));
        EXPECT_EQ(budget.used(), retained);
        moved.append(43U);
        EXPECT_EQ(budget.used(), moved.values().capacity() * sizeof(uint64_t));
        auto last = moved.popBackCharged();
        EXPECT_EQ(last.value, 43U);
        EXPECT_EQ(last.reservation.bytes(), sizeof(uint64_t));
        EXPECT_EQ(budget.used(), moved.values().capacity() * sizeof(uint64_t) + sizeof(uint64_t));
        source.append(7U);
        EXPECT_EQ(budget.used(), (moved.values().capacity() + source.values().capacity() + 1U) * sizeof(uint64_t));
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, EscapedEmbeddedValueDenialLeavesThePublishedBatchIntact) {
    using namespace graph::query::execution;
    MaterializedByteBudget budget(sizeof(uint64_t));
    BoundedBatch<uint64_t> batch(1U, sizeof(uint64_t), [](const uint64_t &) { return sizeof(uint64_t); },
                                 &budget, true, sizeof(uint64_t));
    batch.append(42U);
    EXPECT_THROW(static_cast<void>(batch.popBackCharged()), MaterializedByteLimitExceeded);
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch.values()[0], 42U);
    EXPECT_EQ(batch.bytes(), sizeof(uint64_t));
    EXPECT_EQ(budget.used(), sizeof(uint64_t));
    batch.clear();
    EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, ThrowingEscapeMoveRestoresTheEmbeddedChargeBeforeRetry) {
    using namespace graph::query::execution;
    struct Value {
        uint64_t number;
        bool *fail;
        Value(uint64_t number, bool &fail) : number(number), fail(&fail) {}
        Value(Value &&other) : number(other.number), fail(other.fail) {
            if (*fail) throw std::runtime_error("move denied");
        }
    };
    bool fail = false;
    MaterializedByteBudget budget(1024U);
    BoundedBatch<Value> batch(2U, 1024U, [](const Value &) { return sizeof(Value); }, &budget, true, sizeof(Value));
    batch.append(Value(42U, fail));
    const size_t retained = budget.used();
    fail = true;
    EXPECT_THROW(static_cast<void>(batch.popBackCharged()), std::runtime_error);
    EXPECT_EQ(budget.used(), retained);
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch.values()[0].number, 42U);
    fail = false;
    {
        auto escaped = batch.popBackCharged();
        EXPECT_TRUE(batch.empty());
        EXPECT_EQ(escaped.value.number, 42U);
        EXPECT_EQ(budget.used(), sizeof(Value));
    }
    EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, DiscardDestroysPayloadBeforeRefundingItsSharedCharge) {
    using namespace graph::query::execution;
    struct Value {
        MaterializedByteBudget *budget;
        size_t *observed;
        std::vector<std::byte> payload;
        Value(MaterializedByteBudget &budget, size_t &observed)
            : budget(&budget), observed(&observed), payload(64U) {}
        Value(Value &&) noexcept = default;
        ~Value() { if (!payload.empty()) *observed = budget->used(); }
    };
    MaterializedByteBudget budget(1024U);
    size_t atDestruction = 0U;
    BoundedBatch<Value> batch(2U, 1024U,
        [](const Value &value) { return sizeof(Value) + value.payload.capacity(); }, &budget, true, sizeof(Value));
    batch.append(Value(budget, atDestruction));
    const size_t retained = budget.used();
    EXPECT_GT(retained, 64U);
    batch.discardBack();
    EXPECT_EQ(atDestruction, retained);
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(budget.used(), 0U);
    EXPECT_THROW(batch.discardBack(), std::logic_error);
    EXPECT_EQ(budget.used(), 0U);
}

TEST(BoundedBatchTest, FailedChargedInsertionDestroysRejectedPayloadBeforeRefunding) {
    using namespace graph::query::execution;
    struct Value {
        MaterializedByteBudget *budget;
        size_t *observed;
        std::vector<std::byte> payload;
        Value(MaterializedByteBudget &budget, size_t &observed)
            : budget(&budget), observed(&observed), payload(64U) {}
        Value(Value &&) noexcept = default;
        ~Value() { if (!payload.empty()) *observed = budget->used(); }
    };
    const size_t rowBytes = sizeof(Value) + 64U;
    MaterializedByteBudget budget(2U * rowBytes);
    size_t atDestruction = 0U;
    BoundedBatch<Value> batch(4U, 2U * rowBytes,
        [](const Value &value) { return sizeof(Value) + value.payload.capacity(); }, &budget, true, sizeof(Value));
    batch.append(Value(budget, atDestruction));
    MaterializedByteReservation incoming(&budget);
    incoming.set(rowBytes);
    EXPECT_THROW(batch.appendCharged(Value(budget, atDestruction), std::move(incoming)), MaterializedByteLimitExceeded);
    EXPECT_GE(atDestruction, 2U * 64U + sizeof(Value));
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch.bytes(), rowBytes);
    EXPECT_EQ(budget.used(), rowBytes);
    batch.clear();
    EXPECT_EQ(budget.used(), 0U);
}
