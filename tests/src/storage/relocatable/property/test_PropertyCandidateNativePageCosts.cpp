#include <iostream>
#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/NativePropertyLayoutCandidateWriter.hpp"
#include "graph/core/Node.hpp"

using namespace graph::storage;
using namespace zyx::benchmark;
using graph::PropertyValue;
class PropertyCandidateNativePageCostsTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {
protected:
	void measure(bool singleton) {
		const std::string shape = singleton ? "singleton" : "shared";
		SCOPED_TRACE(shape);
		PropertyBlockOwnerSlots slots(singleton ? 12U : 11U, graph::Node::typeId);
		std::vector<PropertyCollectionRoot> roots;
		for (uint64_t owner = 1U; owner <= 64U; ++owner) {
			(void) slots.insert(owner);
			roots.push_back({owner, graph::Node::typeId, {{singleton ? static_cast<uint32_t>(owner) : 1U, PropertyValue(static_cast<int64_t>(owner))}}});
		}
		LargeValueStore values(io_, *metadata_);
		PropertyCollectionRootStore properties(*slotted_pages_, values, *metadata_);
		LargeValueWriteOptions options;
		options.first_data_slot = PhysicalSlotId(60U); options.last_data_slot = PhysicalSlotId(179U);
		options.first_manifest_slot = PhysicalSlotId(180U); options.last_manifest_slot = PhysicalSlotId(299U);
		const auto print = [&](const char *operation, size_t encodedBytes) {
			const auto stats = io_->stats();
			std::cout << "PROPERTY_CONTAINER_COST " << shape << ' ' << operation
				<< " encoded_bytes=" << encodedBytes << " read_bytes=" << stats.read_bytes << " read_calls=" << stats.read_calls
				<< " write_bytes=" << stats.write_bytes << " write_calls=" << stats.write_calls << '\n';
		};
		uint64_t lsn = metadata_->openSnapshot().superblock().checkpoint_lsn.value() + 1U;
		const auto publish = [&](const char *operation) {
			auto before = metadata_->openSnapshot();
			const PropertyBlockFieldMembership::SnapshotIdentity identity{before.superblock().database_id.value(), before.superblock().generation.value() + 1U};
			const auto image = PropertyLayoutCandidate::build(slots, roots, identity, 128U, 65536U, 4096U, 64U, 0U);
			io_->resetStats();
			const auto root = NativePropertyLayoutCandidateWriter::write(values, *metadata_, properties, before, slots, image,
				options, LogSequenceNumber(lsn++), 128U, 65536U);
			print(operation, static_cast<size_t>(root.logical_bytes));
			return root;
		};
		const auto initial = publish("create");
		for (const char *operation : {"point", "full", "scan_one_field"}) {
			auto snapshot = metadata_->openSnapshot(); const auto birth = snapshot.superblock().generation.value();
			io_->resetStats();
			const auto image = NativePropertyLayoutCandidateReader::read(values, snapshot, initial, slots, birth, 128U, 65536U, [](size_t) {});
			if (std::string_view(operation) == "scan_one_field") {
				for (const auto &expected : roots) EXPECT_EQ(image.readOwner(expected.owner_entity_id, {snapshot.superblock().database_id.value(), birth}), expected);
			} else EXPECT_EQ(image.readOwner(32U, {snapshot.superblock().database_id.value(), birth}), roots[31U]);
			print(operation, static_cast<size_t>(initial.logical_bytes));
		}
		roots[31U].entries[0].value = PropertyValue(int64_t(700));
		(void) publish("single_field_rewrite");
		// Empty properties keep the owner alive. This is not entity deletion or durable retirement.
		roots[31U].entries.clear();
		(void) publish("remove_field_without_reclaim");
	}
};

TEST_F(PropertyCandidateNativePageCostsTest, MeasuresSharedNativeContainerWithoutClaimingGraphRoutingOrReclamation) {
	measure(false);
}
TEST_F(PropertyCandidateNativePageCostsTest, MeasuresSingletonNativeContainerWithoutClaimingGraphRoutingOrReclamation) {
	measure(true);
}
