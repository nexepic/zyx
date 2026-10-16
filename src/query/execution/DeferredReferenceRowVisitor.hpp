#pragma once
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
namespace graph::query::execution::detail {
// Internal mutation consumer: borrows pinned EntityRef/scalar rows. Full public
// callbacks still use the owning output contract. The fence and guards remain
// active before and after every callback.
LogicalPlanReadRuntimeStats visitDeferredReferenceRows(storage::LogicalGraphReadSnapshot &snapshot,
    const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &bounds,
    const LogicalPlanReadRuntime::RowVisitor &visitor, const QueryContext &context);
size_t retainedLogicalReadRowBytes(const LogicalPlanReadRow &row, const QueryContext &context);
}
