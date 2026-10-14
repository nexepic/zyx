#include <gtest/gtest.h>
#include "graph/query/api/QueryEngine.hpp"

namespace graph::query {
namespace {

		class IdentityOnlyLogicalGraphReadBackend final : public storage::LogicalGraphReadBackend {
		public:
			std::optional<storage::LogicalGraphSchemaIdentity> identity{storage::LogicalGraphSchemaIdentity{1U, 2U, {}, 0U, 0U}};
			mutable size_t observations = 0U;
			std::optional<storage::LogicalGraphSchemaIdentity> logicalPlanCacheIdentity() const override {
				++observations; return identity;
			}
			std::unique_ptr<storage::LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				throw std::logic_error("cache identity must not open a logical snapshot");
			}
		};
}

	TEST(QueryEngineLogicalBackendBoundaryTest, InvalidatesLogicalCacheOnIdentityOrCapabilityChangeWithoutOpeningLogicalSnapshots) {
		auto backend = std::make_shared<IdentityOnlyLogicalGraphReadBackend>(); QueryEngine engine(backend);
		for (int variant = 0; variant < 5; ++variant) {
			engine.buildPlan("RETURN 1"); engine.buildPlan("RETURN 2"); ASSERT_EQ(engine.getPlanCache().size(), 2U);
			if (variant == 0) ++backend->identity->database_token;
			if (variant == 1) ++backend->identity->format_token;
			if (variant == 2) ++backend->identity->catalog_tokens[0];
			if (variant == 3) backend->identity.reset();
			if (variant == 4) backend->identity = storage::LogicalGraphSchemaIdentity{9U, 2U, {}, 0U, 0U};
			engine.buildPlan("RETURN 1"); EXPECT_EQ(engine.getPlanCache().size(), 1U);
		}
		const auto observations = backend->observations;
		engine.buildPlan("EXPLAIN MATCH (n) RETURN n");
		EXPECT_EQ(backend->observations, observations);
		engine.resetEphemeralState(); EXPECT_EQ(engine.getPlanCache().size(), 0U);
	}
}
