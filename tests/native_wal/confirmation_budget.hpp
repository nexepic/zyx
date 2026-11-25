#pragma once
#include "graph/query/api/QueryEngine.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"
#include "graph/storage/StorageBackendFactory.hpp"

namespace confirmation_test {
	// Prepare the real Cypher plan with ordinary compilation resources; only
	// its execution account is constrained. Keep the real write domain intact.
	inline void deny(const std::string &path, const std::string &statement, bool previouslyDeclared) {
		using namespace graph;
		using namespace query::execution::detail;
		auto backend =
				storage::StorageBackendFactory::create(path, 64U << 20, storage::OpenMode::OPEN_EXISTING_FILE).backend;
		backend->open();
		{
			auto read = std::dynamic_pointer_cast<storage::LogicalGraphReadBackend>(backend);
			query::QueryEngine engine(read);
			auto plan = engine.buildPlan(statement);
			query::QueryContext context;
			// Admit copying the empty parameter map but not the confirmation's
			// result-column buffer. This includes debug-STL container overhead.
			const size_t parameters = utils::detail::standardUnorderedMapStructuralHeapBytes(context.parameters);
			context.resource_account = std::make_shared<ResourceAccount>(read->queryResourceDomain(),
																		 ResourceClass::RCK_QUERY, parameters + 1U);
			bool denied = false;
			try {
				(void) engine.executePlan(std::move(plan), context);
			} catch (const ResourceAdmissionDenied &) {
				denied = true;
			}
			if (!denied || context.resource_account->used() != 0)
				throw std::runtime_error("DDL confirmation was not denied and refunded");
		}
		backend->close();
		backend->open();
		auto schema = std::dynamic_pointer_cast<storage::LogicalGraphSchemaBackend>(backend);
		const auto definitions = schema->listLogicalConstraints(2);
		if (definitions.size() != static_cast<size_t>(previouslyDeclared) ||
			(previouslyDeclared &&
			 (definitions.front().name != "unique_k" || definitions.front().label != "Item" ||
			  definitions.front().property_keys.size() != 1 || definitions.front().property_keys.front() != "k")))
			throw std::runtime_error("Denied DDL changed the durable schema");
		backend->close();
	}
} // namespace confirmation_test
