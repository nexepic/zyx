#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
TEST(FixtureEnvelopeProbe, ReportsReferenceAndCurrentLayouts) {
 using namespace graph::query::execution;
 using namespace direct_edge_test;
 auto graph=directGraph();
 for(auto &[id,node]:graph.nodes) node.properties.emplace_back("priority",graph::PropertyValue(int64_t(id)));
 const auto delta=resultEntityCapacityDelta(graph)+entityCacheMetadataDelta(graph);
 std::printf("LAYOUT_ENVELOPE row=%zu property=%zu deltas=%zu old_280_reference=%zu current_derived=%zu fixed_gate=4352\n",sizeof(LogicalPlanReadRow),sizeof(graph::PropertyValue),delta,9U*280U+12U*sizeof(graph::PropertyValue)+824U+delta,9U*sizeof(LogicalPlanReadRow)+12U*sizeof(graph::PropertyValue)+824U+delta);
}
