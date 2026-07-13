/**
 * @file ProjectionSpecParser.hpp
 * @author Nexepic
 * @date 2026/7/7
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 **/

#pragma once

#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/query/algorithm/ProjectionSpec.hpp"

namespace graph::query::planner {

	class ProjectionSpecParser {
	public:
		static algorithm::ProjectionSpec parseGraphProjectArgs(const std::vector<PropertyValue> &args);

	private:
		ProjectionSpecParser() = delete;
	};

} // namespace graph::query::planner
