#include "graph/query/execution/VectorizedPredicate.hpp"

#include <algorithm>
#include <chrono>

#include "graph/debug/PerfTrace.hpp"

namespace graph::query::execution {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}
	} // namespace

	bool evaluatePredicateValue(const std::optional<PropertyValue> &actual,
	                            const VectorizedPropertyPredicate &predicate) {
		if (!actual.has_value()) {
			return false;
		}

		switch (predicate.op) {
		case VectorPredicateOp::VPO_EQ:
			return *actual == predicate.value;
		case VectorPredicateOp::VPO_NE:
			return *actual != predicate.value;
		case VectorPredicateOp::VPO_LT:
			return *actual < predicate.value;
		case VectorPredicateOp::VPO_LE:
			return *actual <= predicate.value;
		case VectorPredicateOp::VPO_GT:
			return *actual > predicate.value;
		case VectorPredicateOp::VPO_GE:
			return *actual >= predicate.value;
		case VectorPredicateOp::VPO_RANGE_CLOSED:
			return predicate.upperValue.has_value() && *actual >= predicate.value && *actual <= *predicate.upperValue;
		}

		return false;
	}

	void applyPredicate(NodeColumnBatch &batch, const VectorizedPropertyPredicate &predicate) {
		batch.ensureSelectionVector();

		auto columnIt = batch.propertyColumns.find(predicate.propertyKey);
		if (columnIt == batch.propertyColumns.end()) {
			std::fill(batch.selected.begin(), batch.selected.end(), uint8_t{0});
			return;
		}

		const auto &column = columnIt->second;
		for (size_t rowIndex = 0; rowIndex < batch.nodeIds.size(); ++rowIndex) {
			if (batch.selected[rowIndex] == 0) {
				continue;
			}

			const std::optional<PropertyValue> actual = rowIndex < column.size() ? column[rowIndex] : std::nullopt;
			if (!evaluatePredicateValue(actual, predicate)) {
				batch.selected[rowIndex] = 0;
			}
		}
	}

	void applyPredicates(NodeColumnBatch &batch, const std::vector<VectorizedPropertyPredicate> &predicates) {
		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto traceStart = traceEnabled ? Clock::now() : Clock::time_point{};

		for (const auto &predicate : predicates) {
			applyPredicate(batch, predicate);
		}

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.filter", elapsedNs(traceStart));
		}
	}

} // namespace graph::query::execution
