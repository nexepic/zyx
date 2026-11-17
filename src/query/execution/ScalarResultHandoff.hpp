#pragma once
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::query::execution::detail {
// Execution owns the admission until its finalizer transfers the completed
// result to the parent account. Never expose a partially constructed row.
struct ScalarResultHandoff final {
    static size_t integerRowUpper(const std::string &column) {
        using namespace utils::detail;
        const size_t physical = addStandardHeapBytes(
            standardAllocatorRequestBytes(sizeof(QueryResult::Row), alignof(QueryResult::Row)),
            addStandardHeapBytes(standardFreshUnorderedMapHeapUpper<std::string, ResultValue>(1U),
                standardFreshStringHeapUpper(column.size())));
        const size_t logical = addStandardHeapBytes(sizeof(QueryResult::Row),
            addStandardHeapBytes(column.size(), sizeof(ResultValue) + sizeof(int64_t)));
        return (std::max)(physical, logical);
    }

    template<class Checkpoint>
    static MaterializedByteReservation appendFirstInteger(QueryResult &result,
            const std::string &column, int64_t value, MaterializedByteBudget &budget,
            Checkpoint &&checkpoint) {
        if (result.resource_charge_ || !result.rows_.empty() || result.rows_.capacity() != 0U)
            throw std::logic_error("inline integer result requires an empty execution-owned destination");
        MaterializedByteReservation output(&budget);
        output.set(integerRowUpper(column));
        MaterializedByteReservation rollback(&budget);
        rollback.set(utils::detail::standardContainerProxyBytes());
        std::vector<QueryResult::Row> abandoned;
        try {
            checkpoint();
            result.rows_.reserve(1U);
            result.rows_.emplace_back();
            checkpoint();
            auto &row = result.rows_.back();
            row.reserve(1U);
            row.try_emplace(column, PropertyValue(value));
            checkpoint();
            const size_t physical = utils::detail::addStandardHeapBytes(
                utils::detail::standardVectorHeapBufferBytes(result.rows_) -
                    utils::detail::standardContainerProxyBytes(),
                graph::query::detail::resultRowHeapBytes(row));
            const size_t logical = utils::detail::addStandardHeapBytes(sizeof(QueryResult::Row),
                utils::detail::addStandardHeapBytes(column.size(), sizeof(ResultValue) + sizeof(int64_t)));
            const size_t retained = (std::max)(physical, logical);
            if (retained > output.bytes())
                throw std::logic_error("inline integer result exceeded its admitted storage");
            output.set(retained);
        } catch (...) {
            result.rows_.swap(abandoned);
            throw; // abandoned storage dies before either admission refunds
        }
        return output;
    }
};
}
