#include "graph/query/execution/VectorizedPredicate.hpp"

#include <chrono>

#include "graph/debug/PerfTrace.hpp"
#include "graph/query/execution/PropertyPredicateKernel.hpp"

namespace graph::query::execution {

	namespace {
		using Clock = std::chrono::steady_clock;

		uint64_t elapsedNs(Clock::time_point start) {
			return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
		}
	} // namespace

	bool evaluatePredicateValue(const std::optional<PropertyValue> &actual,
	                            const VectorizedPropertyPredicate &predicate) {
		return evaluatePredicateWithKernel(actual, predicate);
	}

	void applyPredicate(NodeColumnBatch &batch, const VectorizedPropertyPredicate &predicate) {
		PropertyPredicateKernel kernel({predicate});
		kernel.apply(batch);
	}

	void applyPredicates(NodeColumnBatch &batch, const std::vector<VectorizedPropertyPredicate> &predicates) {
		const bool traceEnabled = debug::PerfTrace::isEnabled();
		const auto traceStart = traceEnabled ? Clock::now() : Clock::time_point{};

		PropertyPredicateKernel kernel(predicates);
		kernel.apply(batch);

		if (traceEnabled) {
			debug::PerfTrace::addDuration("node_scan.filter", elapsedNs(traceStart));
		}
	}

} // namespace graph::query::execution
