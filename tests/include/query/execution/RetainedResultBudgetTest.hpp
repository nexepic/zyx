#pragma once
#include <array>
#include <memory>
#include <span>
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace graph::query::execution::test_support {
// Construct an inert complete response outside query execution. Its audited
// retained requests select whether complete materialization is a viable
// contract on this STL; the actual query still builds its own response.
inline size_t scalarResultRequests(std::span<const PropertyValue> values) {
    QueryResult result;
    result.setColumns({"value"});result.reserveRows(values.size());
    for (const auto &value : values) {
        QueryResult::Row row;row.reserve(1U);row.emplace("value",value);
        result.addRow(std::move(row));
    }
    size_t bytes=utils::detail::standardVectorHeapBufferBytes(result.getRows());
    bytes=utils::detail::addStandardHeapBytes(bytes,graph::query::detail::resultStringsHeapBytes(result.getColumns()));
    bytes=utils::detail::addStandardHeapBytes(bytes,graph::query::detail::resultStringsHeapBytes(result.getNotifications()));
    for(const auto &row:result.getRows())bytes=utils::detail::addStandardHeapBytes(bytes,graph::query::detail::resultRowHeapBytes(row));
    return bytes;
}

inline std::shared_ptr<detail::ResourceAccount> resultAccount(size_t cap) {
    using namespace detail;
    auto process=std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(1024U*1024U));
    auto database=std::make_shared<DatabaseResourceBudget>(process,ResourceLimits::uniform(1024U*1024U));
    return std::make_shared<ResourceAccount>(database,ResourceClass::RCK_QUERY,cap);
}
}
