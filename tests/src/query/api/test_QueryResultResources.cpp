#include <gtest/gtest.h>
#include <array>
#include "graph/query/api/QueryResult.hpp"

namespace graph::query {
namespace {
using namespace execution::detail;
std::shared_ptr<ResourceAccount> resultAccount(size_t maximum = 100000U) {
	auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(maximum));
	auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(maximum));
	return std::make_shared<ResourceAccount>(database, ResourceClass::RCK_QUERY, maximum);
}

TEST(QueryResultResourcesTest, BorrowedColumnCopyRejectsBeforeChangingOutputAndAllowsSelfAssignment) {
    const std::vector<std::string> large{std::string(8192U, 'c')};
    auto account = resultAccount(4096U);
    {
        QueryResult result{ResourceLease(account)};
        result.setColumns({"old"});
        const auto previous = account->used();
        EXPECT_THROW(result.setColumns(large), ResourceAdmissionDenied);
        ASSERT_EQ(result.getColumns().size(), 1U);
        EXPECT_EQ(result.getColumns().front(), "old");
        EXPECT_EQ(account->used(), previous);
        result.setColumns(result.getColumns());
        EXPECT_EQ(account->used(), previous);
    }
    EXPECT_EQ(account->used(), 0U);
}

TEST(QueryResultResourcesTest, RowsColumnsAndNotificationsRemainChargedUntilDestruction) {
	auto account = resultAccount();
	{
		QueryResult result{ResourceLease(account)};
		result.setColumns({std::string(1000U, 'c')});
		const auto columns = account->used(); EXPECT_GE(columns, 1000U);
		result.addRow({{"value", PropertyValue(std::string(2000U, 'v'))}});
		const auto rows = account->used(); EXPECT_GE(rows - columns, 2000U);
		result.addNotification(std::string(3000U, 'n'));
		EXPECT_GE(account->used() - rows, 3000U);
		EXPECT_EQ(account->used(), result.retainedResourceBytes());
	}
	EXPECT_EQ(account->used(), 0U);
}

TEST(QueryResultResourcesTest, RejectedGrowthPreservesOutputAndRefundsTemporaryAdmission) {
	// Leave room for one row and the platform's container bookkeeping, while
	// keeping each requested growth well above the complete account limit.
	constexpr size_t limit = 8192U;
	constexpr size_t oversized = 16384U;
	auto account = resultAccount(limit);
	QueryResult result{ResourceLease(account)};
	result.addRow({{"x", PropertyValue(7)}});
	const auto previous = account->used();
	EXPECT_THROW(result.addRow({{"large", PropertyValue(std::string(oversized, 'x'))}}), ResourceAdmissionDenied);
	EXPECT_THROW(result.reserveRows(10000U), ResourceAdmissionDenied);
	EXPECT_THROW(result.setColumns({std::string(oversized, 'x')}), ResourceAdmissionDenied);
	EXPECT_THROW(result.addNotification(std::string(oversized, 'x')), ResourceAdmissionDenied);
	EXPECT_EQ(result.rowCount(), 1U);
	EXPECT_EQ(result.getRows()[0].at("x").asPrimitive(), PropertyValue(7));
	EXPECT_TRUE(result.getColumns().empty()); EXPECT_TRUE(result.getNotifications().empty());
	EXPECT_EQ(account->used(), previous);
}

TEST(QueryResultResourcesTest, DeepCopyHasAnIndependentChargeAndMoveTransfersItOnce) {
	auto account = resultAccount();
	QueryResult original{ResourceLease(account)};
	original.addRow({{"x", PropertyValue(std::string(2000U, 'x'))}});
	const auto originalBytes = original.retainedResourceBytes();
	{
		QueryResult copy(original);
		EXPECT_EQ(account->used(), 2U * originalBytes);
		copy.addRow({{"x", PropertyValue(9)}});
		EXPECT_EQ(original.rowCount(), 1U); EXPECT_EQ(copy.rowCount(), 2U);
		const auto combined = account->used();
		QueryResult moved(std::move(copy));
		EXPECT_EQ(account->used(), combined + 3U * utils::detail::standardContainerProxyBytes());
        EXPECT_EQ(moved.rowCount(), 2U);
        EXPECT_TRUE(copy.getRows().empty());
        EXPECT_EQ(copy.retainedResourceBytes(), 3U * utils::detail::standardContainerProxyBytes());
	}
	EXPECT_EQ(account->used(), originalBytes);
}

TEST(QueryResultResourcesTest, CopyDenialPreservesSourceAndAssignmentDestination) {
	auto sourceAccount = resultAccount(10000U), targetAccount = resultAccount();
	QueryResult source{ResourceLease(sourceAccount)}, target{ResourceLease(targetAccount)};
	source.addRow({{"source", PropertyValue(std::string(6000U, 'x'))}});
	target.addRow({{"target", PropertyValue(42)}});
	const auto sourceBytes = sourceAccount->used(), targetBytes = targetAccount->used();
	EXPECT_THROW((void)QueryResult(source), ResourceAdmissionDenied);
	EXPECT_THROW(target = source, ResourceAdmissionDenied);
	EXPECT_EQ(sourceAccount->used(), sourceBytes); EXPECT_EQ(targetAccount->used(), targetBytes);
	EXPECT_EQ(source.rowCount(), 1U);
	EXPECT_EQ(target.getRows()[0].at("target").asPrimitive(), PropertyValue(42));
}

TEST(QueryResultResourcesTest, MoveAssignmentReleasesOldOwnershipAndRetainsNewHierarchy) {
	auto first = resultAccount(), second = resultAccount();
	{
		QueryResult source{ResourceLease(first)}, destination{ResourceLease(second)};
		source.addRow({{"x", PropertyValue(1)}}); destination.addRow({{"y", PropertyValue(2)}});
		const auto bytes = first->used();
		destination = std::move(source);
		const auto emptySourceBytes = 3U * utils::detail::standardContainerProxyBytes();
		EXPECT_TRUE(source.getRows().empty());
		EXPECT_EQ(source.retainedResourceBytes(), emptySourceBytes);
		EXPECT_EQ(second->used(), 0U); EXPECT_EQ(first->used(), bytes + emptySourceBytes);
		EXPECT_EQ(destination.getRows()[0].at("x").asPrimitive(), PropertyValue(1));
		destination = QueryResult{};
		EXPECT_EQ(first->used(), emptySourceBytes);
	}
	EXPECT_EQ(first->used(), 0U);
	EXPECT_EQ(second->used(), 0U);
}

TEST(QueryResultResourcesTest, ResultKeepsAccountAliveAfterItsExternalOwnerIsReleased) {
	std::weak_ptr<ResourceAccount> lifetime;
	{
		auto account = resultAccount(); lifetime = account;
		QueryResult result{ResourceLease(account)};
		result.addRow({{"x", PropertyValue(42)}});
		account.reset();
		ASSERT_FALSE(lifetime.expired()); EXPECT_GT(lifetime.lock()->used(), 0U);
	}
	EXPECT_TRUE(lifetime.expired());
}

TEST(QueryResultResourcesTest, ReplacingColumnStorageReleasesTheOldCharacterBuffers) {
	auto account = resultAccount();
	QueryResult result{ResourceLease(account)};
	result.setColumns({std::string(6000U, 'x')});
	const auto large = account->used();
	result.setColumns({"x"});
	EXPECT_LT(account->used(), large - 5000U);
	EXPECT_EQ(result.getColumns(), (std::vector<std::string>{"x"}));
}

TEST(QueryResultResourcesTest, NestedAndEntityPayloadsCannotBypassAdmission) {
	auto account = resultAccount(10000U);
	QueryResult result{ResourceLease(account)};
	PropertyValue nested(PropertyValue::MapType{{"items", PropertyValue(std::vector<PropertyValue>{PropertyValue(std::string(20000U, 'x'))})}});
	EXPECT_THROW(result.addRow({{"nested", nested}}), ResourceAdmissionDenied);
	LogicalNodeResult node{1U, {"Person"}, {{"large", PropertyValue(std::string(20000U, 'x'))}}};
	EXPECT_THROW(result.addRow({{"node", ResultValue(node)}}), ResourceAdmissionDenied);
	EXPECT_TRUE(result.isEmpty()); EXPECT_EQ(account->used(), 3U * standardContainerProxyBytes());
}

TEST(QueryResultResourcesTest, EmptyMetadataAndLargeRowCapacityUseAllocatorRequestModel) {
	auto account = resultAccount(1000000U);
	{
		QueryResult result{ResourceLease(account)};
		const auto initial = 3U * standardContainerProxyBytes();
		EXPECT_EQ(account->used(), initial);
		result.reserveRows(256U);
		const auto capacityBytes = standardVectorHeapBufferBytes(result.getRows()) - standardContainerProxyBytes();
		EXPECT_EQ(account->used(), initial + capacityBytes);
		result.reserveRows(1024U);
		EXPECT_EQ(account->used(), initial + standardVectorHeapBufferBytes(result.getRows()) - standardContainerProxyBytes());
		result.setColumns({"x"});
		EXPECT_EQ(account->used(), 2U * standardContainerProxyBytes() +
			standardVectorHeapBufferBytes(result.getRows()) - standardContainerProxyBytes() + detail::resultStringsHeapBytes(result.getColumns()));
	}
	EXPECT_EQ(account->used(), 0U);
}

TEST(QueryResultResourcesTest, NewEmptyResultRejectsAnAlreadyChargedLease) {
	auto account = resultAccount(); ResourceLease lease(account); lease.set(100U);
	EXPECT_THROW((void)QueryResult(std::move(lease)), std::invalid_argument);
	EXPECT_EQ(account->used(), 0U);
}
TEST(QueryResultResourcesTest, StringViewsAndJoinedFieldsAreAdmittedBeforeReplacingOrAppendingOutput) {
    auto account = resultAccount(8192U);
    const std::string wide(16384U, 'w');
    const std::array<std::string_view, 1U> columns{"value"};
    const std::array<StringResultField, 1U> original{{{"value", "old"}}};
    const std::array<StringResultField, 1U> denied{{{"value", wide}}};
    {
        QueryResult result{ResourceLease(account)};
        result.setColumnViews(columns); result.addStringRow(original);
        const auto retained = account->used();
        EXPECT_THROW(result.addStringRow(denied), ResourceAdmissionDenied);
        const std::array<std::string_view, 1U> deniedColumns{wide};
        EXPECT_THROW(result.setColumnViews(deniedColumns), ResourceAdmissionDenied);
        EXPECT_EQ(account->used(), retained);
        ASSERT_EQ(result.rowCount(), 1U);
        EXPECT_EQ(std::get<std::string>(result.getRows()[0].at("value").asPrimitive().getVariant()), "old");
        EXPECT_EQ(result.getColumns().front(), "value");
        const std::vector<std::string> pieces{"left", "right"};
        const std::array<StringResultField, 1U> joined{StringResultField::joined("value", pieces, ":")};
        result.addStringRow(joined);
        EXPECT_EQ(std::get<std::string>(result.getRows()[1].at("value").asPrimitive().getVariant()), "left:right");
    }
    EXPECT_EQ(account->used(), 0U);
}

TEST(QueryResultResourcesTest, DuplicateStringFieldsRefundPartialConstructionAndKeepPriorRows) {
    auto account = resultAccount();
    QueryResult result{ResourceLease(account)};
    const std::array<StringResultField, 1U> initial{{{"value", "old"}}};
    result.addStringRow(initial);
    const auto retained = account->used();
    const std::array<StringResultField, 2U> repeated{{{"value", "first"}, {"value", "second"}}};
    EXPECT_THROW(result.addStringRow(repeated), std::invalid_argument);
    EXPECT_EQ(account->used(), retained);
    ASSERT_EQ(result.rowCount(), 1U);
    EXPECT_EQ(std::get<std::string>(result.getRows()[0].at("value").asPrimitive().getVariant()), "old");
}

TEST(QueryResultResourcesTest, MoveUnderFullAccountEitherTransfersOrRejectsBeforeChangingTheSource) {
    auto account = resultAccount();
    {
        QueryResult source{ResourceLease(account)};
        source.addStringRow(std::array<StringResultField, 1U>{{{"value", "kept"}}});
        const auto original = source.retainedResourceBytes();
        ResourceLease pressure(account); pressure.set(account->available());
        const auto full = account->used();
        if constexpr (utils::detail::standardContainerProxyBytes() != 0U) {
            EXPECT_THROW((void)QueryResult(std::move(source)), ResourceAdmissionDenied);
            EXPECT_EQ(source.rowCount(), 1U);
            EXPECT_EQ(source.retainedResourceBytes(), original);
            EXPECT_EQ(account->used(), full);
            pressure.reset();
            QueryResult moved(std::move(source));
            EXPECT_EQ(moved.rowCount(), 1U);
            EXPECT_EQ(account->used(), moved.retainedResourceBytes() + source.retainedResourceBytes());
        } else {
            QueryResult moved(std::move(source));
            EXPECT_EQ(moved.rowCount(), 1U);
            EXPECT_TRUE(source.getRows().empty());
            EXPECT_EQ(account->used(), full);
            EXPECT_EQ(moved.retainedResourceBytes(), original);
        }
    }
    EXPECT_EQ(account->used(), 0U);
}

struct ThrowingMoveRow {
    inline static int defaultsUntilFailure = -1;
    inline static int copies = 0;
    std::unique_ptr<int> payload;
    ThrowingMoveRow() {
        if (defaultsUntilFailure == 0) throw std::bad_alloc();
        if (defaultsUntilFailure > 0) --defaultsUntilFailure;
        payload = std::make_unique<int>(0);
    }
    explicit ThrowingMoveRow(int value) : payload(std::make_unique<int>(value)) {}
    ThrowingMoveRow(const ThrowingMoveRow &) { ++copies; throw std::runtime_error("payload copy forbidden"); }
    ThrowingMoveRow(ThrowingMoveRow &&other) noexcept(false) : payload(std::move(other.payload)) {}
    friend void swap(ThrowingMoveRow &left, ThrowingMoveRow &right) noexcept { left.payload.swap(right.payload); }
};

TEST(QueryResultResourcesTest, SwapGrowthAvoidsThrowingMoveCopiesAndRollsBackDestinationFailure) {
    std::vector<ThrowingMoveRow> rows;
    rows.reserve(2U); rows.emplace_back(7); rows.emplace_back(9);
    const auto first = rows[0].payload.get(), second = rows[1].payload.get();
    EXPECT_THROW(rows.reserve(8U), std::runtime_error);
    EXPECT_GT(ThrowingMoveRow::copies, 0);
    ThrowingMoveRow::copies = 0;
    ThrowingMoveRow::defaultsUntilFailure = 1;
    EXPECT_THROW(detail::reserveResultRowsBySwap(rows, 8U), std::bad_alloc);
    EXPECT_EQ(rows.capacity(), 2U);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[0].payload.get(), first); EXPECT_EQ(rows[1].payload.get(), second);
    ThrowingMoveRow::defaultsUntilFailure = -1;
    detail::reserveResultRowsBySwap(rows, 8U);
    EXPECT_GE(rows.capacity(), 8U);
    EXPECT_EQ(rows[0].payload.get(), first); EXPECT_EQ(rows[1].payload.get(), second);
    EXPECT_EQ(*rows[0].payload, 7); EXPECT_EQ(*rows[1].payload, 9);
    EXPECT_EQ(ThrowingMoveRow::copies, 0);
}

TEST(QueryResultResourcesTest, ChargedAndUnchargedRowGrowthKeepIndependentHeapPayloadAddresses) {
    for (bool charged : {false, true}) {
        auto account = resultAccount(200000U);
        {
            QueryResult result = charged ? QueryResult{ResourceLease(account)} : QueryResult{};
            result.addRow({{"payload", PropertyValue(std::string(32768U, 'x'))}});
            const auto *value = &result.getRows().front().at("payload");
            const auto before = account->used();
            const auto oldBuffer = execution::detail::standardVectorHeapBufferBytes(result.getRows());
            result.reserveRows(16U);
            EXPECT_EQ(&result.getRows().front().at("payload"), value);
            EXPECT_EQ(std::get<std::string>(value->asPrimitive().getVariant()), std::string(32768U, 'x'));
            if (charged) {
                const auto newBuffer = execution::detail::standardVectorHeapBufferBytes(result.getRows());
                EXPECT_EQ(account->used(), before - oldBuffer + newBuffer);
                EXPECT_EQ(result.retainedResourceBytes(), account->used());
            }
        }
        EXPECT_EQ(account->used(), 0U);
    }
}

} // namespace
} // namespace graph::query
