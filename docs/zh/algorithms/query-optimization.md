# Query Optimization Algorithm

Metrix uses a cost-based query optimizer that transforms Cypher queries into efficient execution plans using rule-based optimization and statistics.

## Overview

The optimization pipeline:

1. **Parse**: Convert Cypher to AST
2. **Logical Plan**: Generate logical operators
3. **Optimize**: Apply optimization rules
4. **Physical Plan**: Choose physical implementations

## Optimization Rules

### Rule 1: Filter Pushdown

Move filters as close to data source as possible:

```cypher
// Before optimization
MATCH (u:User)
WHERE u.age > 25
RETURN u

// After optimization
// Filter pushed to node scan
```

**Implementation**:

```cpp
void pushDownFilters(LogicalPlan* plan) {
    for (auto* filterOp : plan->getOperators<FilterOperator>()) {
        // Find scan operator below filter
        auto* scanOp = plan->findBelow(filterOp, ScanOperator::Type);

        if (canPushDown(filterOp, scanOp)) {
            // Move filter below scan
            plan->reorder(filterOp, scanOp);
        }
    }
}
```

### Rule 2: Index Selection

Use indexes when available:

```cpp
void selectIndexes(LogicalPlan* plan) {
    for (auto* scanOp : plan->getOperators<ScanOperator>()) {
        if (hasIndex(scanOp->getLabel(), scanOp->getProperty())) {
            // Replace scan with index scan
            auto* indexScanOp = new IndexScanOperator(scanOp);
            plan->replace(scanOp, indexScanOp);
        }
    }
}
```

### Rule 3: Join Reordering

Optimize join order for performance:

```cpp
void reorderJoins(LogicalPlan* plan) {
    auto* join = plan->findJoin();

    if (join) {
        auto leftSize = estimateSize(join->getLeft());
        auto rightSize = estimateSize(join->getRight());

        // Swap if right side is smaller
        if (rightSize < leftSize) {
            join->swapChildren();
        }
    }
}
```

### Rule 4: Projection Pruning

Remove unused columns:

```cpp
void pruneProjections(LogicalPlan* plan) {
    auto* projectOp = plan->findTop<ProjectOperator>();

    if (projectOp) {
        auto usedColumns = collectUsedColumns(plan, projectOp);
        projectOp->pruneUnused(usedColumns);
    }
}
```

## Cost Estimation

### Statistics Collection

```cpp
struct TableStatistics {
    uint64_t rowCount;
    std::map<std::string, uint64_t> distinctCounts;
    std::map<std::string, double> histograms;
};
```

### Cost Model

```cpp
double estimateCost(LogicalOperator* op) {
    switch (op->getType()) {
        case OperatorType::Scan:
            return estimateScanCost(op);
        case OperatorType::IndexScan:
            return estimateIndexScanCost(op);
        case OperatorType::Filter:
            return estimateFilterCost(op);
        case OperatorType::Join:
            return estimateJoinCost(op);
        default:
            return 0.0;
    }
}

double estimateScanCost(ScanOperator* op) {
    double tableSize = stats.getRowCount(op->getLabel());
    return tableSize * SEQ_PAGE_COST;
}

double estimateFilterCost(FilterOperator* op) {
    double inputRows = op->getChild()->getOutputRows();
    double selectivity = estimateSelectivity(op->getPredicate());
    return inputRows * CPU_TUPLE_COST + inputRows * selectivity * CPU_TUPLE_COST;
}

double estimateJoinCost(JoinOperator* op) {
    double outerRows = op->getOuter()->getOutputRows();
    double innerRows = op->getInner()->getOutputRows();
    return outerRows * innerRows * CPU_TUPLE_COST;
}
```

## Plan Selection

### Enumerate Plans

```cpp
std::vector<PhysicalPlan*> enumeratePlans(LogicalPlan* logical) {
    std::vector<PhysicalPlan*> plans;

    // Generate different physical implementations
    plans.push_back(createNestedLoopJoinPlan(logical));
    plans.push_back(createHashJoinPlan(logical));

    return plans;
}
```

### Choose Best Plan

```cpp
PhysicalPlan* chooseBestPlan(const std::vector<PhysicalPlan*>& plans) {
    PhysicalPlan* best = nullptr;
    double minCost = std::numeric_limits<double>::max();

    for (auto* plan : plans) {
        double cost = estimateTotalCost(plan);
        if (cost < minCost) {
            minCost = cost;
            best = plan;
        }
    }

    return best;
}
```

## Optimizations

### Caching Plans

```cpp
class PlanCache {
    std::unordered_map<std::string, PhysicalPlan*> cache;

public:
    PhysicalPlan* getPlan(const std::string& query) {
        auto it = cache.find(query);
        if (it != cache.end()) {
            return it->second;
        }
        return nullptr;
    }

    void cachePlan(const std::string& query, PhysicalPlan* plan) {
        cache[query] = plan;
    }
};
```

### Adaptive Optimization

```cpp
class AdaptiveOptimizer {
    struct PlanStats {
        PhysicalPlan* plan;
        uint64_t executionCount;
        double avgExecutionTime;
    };

    std::unordered_map<std::string, PlanStats> planHistory;

public:
    PhysicalPlan* optimize(const std::string& query, LogicalPlan* logical) {
        // Check if we have history
        auto it = planHistory.find(query);
        if (it != planHistory.end()) {
            return it->second.plan;
        }

        // Generate and cache plan
        auto* plan = chooseBestPlan(enumeratePlans(logical));
        planHistory[query] = {plan, 0, 0.0};
        return plan;
    }

    void updateStats(const std::string& query, double executionTime) {
        auto& stats = planHistory[query];
        stats.executionCount++;
        stats.avgExecutionTime =
            (stats.avgExecutionTime * (stats.executionCount - 1) + executionTime) /
            stats.executionCount;
    }
};
```

## Performance Tips

### For Users

1. **Use indexes**: Create indexes on frequently queried properties
2. **Filter early**: Apply filters in WHERE clause
3. **Limit results**: Use LIMIT to avoid large result sets
4. **Avoid cross products**: Ensure patterns are connected
5. **Profile queries**: Use EXPLAIN to analyze plans

### For Developers

1. **Maintain statistics**: Keep table statistics up to date
2. **Tune cost model**: Adjust cost constants for your hardware
3. **Monitor plans**: Track plan execution times
4. **Test queries**: Validate plan quality regularly

## See Also

- [Query Engine](/en/architecture/query-engine) - Query execution
- [API Reference](/en/api/cpp-api) - Query API
- [Performance Optimization](/en/architecture/optimization) - Performance tuning
