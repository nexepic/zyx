#pragma once

#include "graph/query/execution/TypedScalarValue.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	inline TypedScalarValue scalarValueFromStorage(const storage::PropertyEntityScalarValue &value) {
		return TypedScalarValue{
				value.type,
				value.boolValue,
				value.intValue,
				value.doubleValue,
				value.stringValue,
				value.durationValue,
				value.fallbackValue};
	}

} // namespace graph::query::execution
