/**
 * @file FunctionRegistryTestFixture.hpp
 * @author ZYX Contributors
 * @date 2026
 *
 * @copyright Copyright (c) 2026
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <gtest/gtest.h>
#include "graph/query/expressions/FunctionRegistry.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/execution/Record.hpp"

using namespace graph;
using namespace graph::query::expressions;
using namespace graph::query::execution;

// ============================================================================
// Shared Test Fixture
// ============================================================================

class FunctionRegistryTest : public ::testing::Test {
protected:
	void SetUp() override {
		registry = &FunctionRegistry::getInstance();

		// Create a basic record for context
		record_.setValue("x", PropertyValue(42));
		record_.setValue("name", PropertyValue(std::string("Alice")));
		context_ = std::make_unique<EvaluationContext>(record_);
	}

	FunctionRegistry* registry;
	Record record_;
	std::unique_ptr<EvaluationContext> context_;
};
