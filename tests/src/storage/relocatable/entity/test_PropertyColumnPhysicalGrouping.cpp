#include <array>
#include <string>
#include "storage/relocatable/entity/PageBackedGraphReadAdapterTestFixture.hpp"
#include "storage/relocatable/property/PropertyCollectionRootTestUtils.hpp"
#include "graph/core/Node.hpp"
#include "graph/debug/PerfTrace.hpp"
using namespace graph::storage;
using graph::storage::test::PageBackedGraphReadAdapterTest;

TEST_F(PageBackedGraphReadAdapterTest, SelectedColumnsGroupCowInterleavedPagesAndRestoreOwnerOrder) {
 Superblock initial;
 initial.database_id = DatabaseId(17);
 initial.generation = Generation(1);
 initial.checkpoint_lsn = LogSequenceNumber(1);
 auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
 EntityPageStore entities(io, *metadata);
 SlottedPagePool pages(io, *metadata);
 PropertyCollectionRootStore properties(pages, *metadata);
 const PropertyCollectionRootStoreOptions options{PhysicalSlotId(41), PhysicalSlotId(45), 902};
 const std::array roots = {
  PropertyCollectionRoot{1001, graph::Node::typeId, {{3, graph::PropertyValue(std::string(700, 'a'))}, {5, graph::PropertyValue()}}},
  PropertyCollectionRoot{1002, graph::Node::typeId, {{3, graph::PropertyValue(std::string(700, 'b'))}, {5, graph::PropertyValue()}}},
  PropertyCollectionRoot{1003, graph::Node::typeId, {{3, graph::PropertyValue(std::string(700, 'c'))}, {5, graph::PropertyValue()}}}
 };
 auto empty = metadata->openSnapshot();
 const auto plans = properties.planEntityRootReplacements(empty, roots, options);
 EntityPageBatchNewPool pool;
 pool.request = {.segment_id = SegmentId(31), .pool_owner_id = 901, .generation = Generation(1),
                 .page_id = PageId(101), .first_slot = PhysicalSlotId(40), .last_slot = PhysicalSlotId(40)};
 pool.record_kind = EntityPageRecordKind::EPRK_NODE;
 pool.node_insertions = {{1001, NodeEntityRecord{}}, {1002, NodeEntityRecord{}}, {1003, NodeEntityRecord{}}};
 const auto published = entities.applyNativeGraphBatchTransaction(empty, {}, {}, std::span(&pool, 1), plans,
                                                                  properties, options, LogSequenceNumber(2));
 ASSERT_EQ(published.properties.size(), 3U);
 ASSERT_EQ(published.properties[0].page_id, published.properties[2].page_id);
 empty.release();
 auto before = metadata->openSnapshot();
 const PropertyCollectionRoot replacement{1002, graph::Node::typeId,
  {{3, graph::PropertyValue(std::string(3000, 'd'))}, {5, graph::PropertyValue()}}};
 static_cast<void>(test::publishPropertyRoot(entities, properties, *metadata, replacement, options));
 auto after = metadata->openSnapshot();
 const std::array keys = {EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1001},
                         EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1002},
                         EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1003},
                         EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1004}};
 const auto first = metadata->findEntityPropertyLocation(after, keys[0]);
 const auto middle = metadata->findEntityPropertyLocation(after, keys[1]);
 const auto last = metadata->findEntityPropertyLocation(after, keys[2]);
 ASSERT_EQ(first.page_id, last.page_id);
 ASSERT_NE(first.page_id, middle.page_id);
 const std::array<uint32_t, 3> tokens{3, 5, 7};
 std::array<std::optional<PropertyCollectionRootEntry>, 12> values;
 auto cursor = properties.openScanCursor(after);
 const bool enabled = graph::debug::PerfTrace::areValuesEnabled();
 graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
 cursor.readEntityPropertiesOptionalSorted(after, keys, tokens, values, true);
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 graph::debug::PerfTrace::setValueCollectionEnabled(enabled);
 ASSERT_TRUE(trace.contains("native_graph.read.property_selected_column_batch_pages"));
 EXPECT_EQ(trace.at("native_graph.read.property_selected_column_batch_pages").totalValue, 2);
 ASSERT_TRUE(trace.contains("native_graph.read.shared_page_known_location_copy"));
 EXPECT_EQ(trace.at("native_graph.read.shared_page_known_location_copy").totalValue, 2);
 for (size_t owner = 0; owner < 3; ++owner) {
  ASSERT_TRUE(values[owner * 3]);
  EXPECT_EQ(values[owner * 3]->value, (owner == 1 ? replacement : roots[owner]).entries[0].value);
  ASSERT_TRUE(values[owner * 3 + 1]);
  EXPECT_EQ(values[owner * 3 + 1]->value, graph::PropertyValue());
  EXPECT_FALSE(values[owner * 3 + 2]);
 }
 EXPECT_FALSE(values[9]); EXPECT_FALSE(values[10]); EXPECT_FALSE(values[11]);
 auto oldCursor = properties.openScanCursor(before);
 oldCursor.readEntityPropertiesOptionalSorted(before, keys, tokens, values, true);
 ASSERT_TRUE(values[3]); EXPECT_EQ(values[3]->value, roots[1].entries[0].value);
}
