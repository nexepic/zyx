#pragma once
#include <utility>
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

    // Subsequent integer rows are built in their final slot as well. Grow the
    // row array before constructing the new map: the relocation's empty maps
    // and the incoming populated map need not be simultaneously alive.
    // Existing rows and their retained authority survive every failed stage.
    template<class Checkpoint, class Reclaim>
    static void appendInteger(QueryResult &result, const std::string &column,
            int64_t value, MaterializedByteBudget &budget,
            MaterializedByteReservation &retained, Checkpoint &&checkpoint, Reclaim &&reclaim) {
        using namespace utils::detail;
        if (result.resource_charge_ || result.rows_.empty())
            throw std::logic_error("integer append requires an execution-owned nonempty result");
        const size_t count = result.rows_.size();
        const size_t logical = addStandardHeapBytes(column.size(), sizeof(ResultValue) + sizeof(int64_t));
        const size_t physical = addStandardHeapBytes(
            standardFreshUnorderedMapHeapUpper<std::string, ResultValue>(1U), standardFreshStringHeapUpper(column.size()));
        const size_t incomingUpper = (std::max)(logical, physical);
        if (count == result.rows_.capacity()) {
            const size_t previous = retained.bytes();
            const size_t oldBacking = standardVectorHeapBufferBytes(result.rows_) - standardContainerProxyBytes();
            const size_t minimum = addStandardHeapBytes(count, 1U);
            size_t capacity = addStandardHeapBytes(count, count);
            const size_t relocation = graph::query::detail::resultRowsRelocationHeapUpper(result.rows_);
            const auto backing = [](size_t slots) {
                return standardAllocatorRequestBytes(
                    multiplyPropertyBufferBytes(slots, sizeof(QueryResult::Row)), alignof(QueryResult::Row));
            };
            const auto peak = [&] {
                return addStandardHeapBytes(previous, addStandardHeapBytes(backing(capacity), relocation));
            };
            const auto appendPeak = [&] {
                return addStandardHeapBytes(previous - oldBacking, addStandardHeapBytes(backing(capacity), incomingUpper));
            };
            // Growth must leave room for the following map construction too.
            // Optional doubling must not strand an otherwise admissible row.
            if (capacity > minimum && (std::max)(peak(), appendPeak()) - previous > budget.availableWithParent())
                capacity = minimum;
            const auto admit = [&] {
                reclaim(peak(), previous);
                retained.set(peak());
            };
            // Spare capacity is optional under either local or parent pressure.
            try { admit(); }
            catch (const MaterializedByteLimitExceeded &) {
                if (capacity == minimum) throw;
                capacity = minimum; admit();
            } catch (const ResourceAdmissionDenied &) {
                if (capacity == minimum) throw;
                capacity = minimum; admit();
            }
            try { checkpoint(); result.reserveRows(capacity); }
            catch (...) { retained.set(previous); throw; }
            retained.set(addStandardHeapBytes(previous - oldBacking,
                standardVectorHeapBufferBytes(result.rows_) - standardContainerProxyBytes()));
        }
        MaterializedByteReservation incoming(&budget);
        reclaim(incomingUpper, 0U);
        incoming.set(incomingUpper);
        try {
            checkpoint();
            result.rows_.emplace_back();
            auto &row = result.rows_.back();
            row.reserve(1U);
            row.try_emplace(column, PropertyValue(value));
            checkpoint();
            const size_t actual = (std::max)(logical, graph::query::detail::resultRowHeapBytes(row));
            if (actual > incoming.bytes())
                throw std::logic_error("integer result append exceeded its admitted storage");
            incoming.set(actual);
        } catch (...) {
            if (result.rows_.size() != count) result.rows_.pop_back();
            throw; // destroy the partial row before incoming refunds
        }
        retained.absorb(std::move(incoming));
    }


    template<class Checkpoint>
    static void appendInteger(QueryResult &result, const std::string &column,
            int64_t value, MaterializedByteBudget &budget,
            MaterializedByteReservation &retained, Checkpoint &&checkpoint) {
        appendInteger(result, column, value, budget, retained,
            std::forward<Checkpoint>(checkpoint), [](size_t, size_t) {});
    }

};
}
