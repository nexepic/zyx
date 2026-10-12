#include "storage/relocatable/entity/NativeGraphWritePlannerTestFixture.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/core/Node.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/SparseGroupRetirementCandidates.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/NativeSparseGroupWriter.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/NativeSparseGroupReader.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/PropertyBlockFieldMembership.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/NativePropertyLayoutCandidateReader.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/NativePropertyLayoutCandidateWriter.hpp"

using namespace graph::storage;
using graph::PropertyValue;
using zyx::benchmark::PropertyBlockOwnerSlots;
using zyx::benchmark::PropertyBlockFieldMembership;

class PropertyBlockMembershipSnapshotTest : public graph::storage::test::NativeGraphWritePlannerTestFixture {};

TEST_F(PropertyBlockMembershipSnapshotTest, NativeCandidatePublishesNextIdentityWithExplicitSourceFence) {
	using namespace zyx::benchmark;
	LargeValueStore store(io_, *metadata_);
	PropertyCollectionRootStore properties(*slotted_pages_, store, *metadata_);
	auto initial = metadata_->openSnapshot();
	const PropertyBlockFieldMembership::SnapshotIdentity identity{
		initial.superblock().database_id.value(), initial.superblock().generation.value() + 1U};
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId); static_cast<void>(slots.insert(1U));
	const std::vector<PropertyCollectionRoot> roots{{1U, graph::Node::typeId, {{7U, PropertyValue(int64_t(9))}}}};
	const auto image = PropertyLayoutCandidate::build(slots, roots, identity, 1U, 10000U, 4096U, 64U, 0U);
	LargeValueWriteOptions options;
	options.first_data_slot = PhysicalSlotId(60U); options.last_data_slot = PhysicalSlotId(79U);
	options.first_manifest_slot = PhysicalSlotId(80U); options.last_manifest_slot = PhysicalSlotId(100U);
	for (const auto failpoint : {BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE,
	    BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE}) {
		MetadataPublishOptions fail; fail.bootstrap_failpoint = failpoint;
		EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties,
		    initial, slots, image, options, LogSequenceNumber(2U), 1U, 10000U, fail)), BootstrapPublishInterrupted);
		EXPECT_EQ(metadata_->openSnapshot().superblock(), initial.superblock());
		EXPECT_EQ(SuperblockFile(io_).recover(), initial.superblock());
	}
	struct Checkpoint final : CancellationCheckpoint {
		mutable size_t calls = 0U; size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
	};
	Checkpoint count;
	MetadataPublishOptions beforePublish; beforePublish.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties, initial,
	    slots, image, options, LogSequenceNumber(2U), 1U, 10000U, beforePublish, &count)), BootstrapPublishInterrupted);
	for (size_t stop = 1U; stop <= count.calls; ++stop) {
		Checkpoint cancelled; cancelled.stop = stop;
		EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties, initial,
		    slots, image, options, LogSequenceNumber(2U), 1U, 10000U, {}, &cancelled)), std::runtime_error);
		EXPECT_EQ(cancelled.calls, stop);
		EXPECT_EQ(metadata_->openSnapshot().superblock(), initial.superblock());
	}
	const auto reference = NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties, initial, slots, image,
		options, LogSequenceNumber(2U), 1U, 10000U);
	auto published = metadata_->openSnapshot();
	EXPECT_EQ(published.superblock().generation.value(), identity.generation);
	EXPECT_EQ(published.superblock().next_large_value_id, initial.superblock().next_large_value_id + 1U);
	const SelectedPropertyReadAdmission admit = [](size_t) {};
	const auto loaded = NativePropertyLayoutCandidateReader::read(store, published, reference, slots, identity.generation, 1U, 10000U, admit);
	EXPECT_EQ(loaded.readOwner(1U, identity), roots.front());
	EXPECT_ANY_THROW(static_cast<void>(NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties, initial,
		slots, image, options, LogSequenceNumber(3U), 1U, 10000U)));
	EXPECT_EQ(metadata_->openSnapshot().superblock().generation, published.superblock().generation);
	EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties, published,
		slots, image, options, LogSequenceNumber(3U), 1U, 10000U)), std::invalid_argument);
	const PropertyBlockFieldMembership::SnapshotIdentity nextIdentity{identity.database_id, identity.generation + 1U};
	const std::vector<PropertyCollectionRoot> nextRoots{{1U, graph::Node::typeId, {{7U, PropertyValue(int64_t(10))}}}};
	const auto nextImage = PropertyLayoutCandidate::build(slots, nextRoots, nextIdentity, 1U, 10000U, 4096U, 64U, 0U);
	const auto nextReference = NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties, published, slots,
		nextImage, options, LogSequenceNumber(3U), 1U, 10000U);
	auto nextSnapshot = metadata_->openSnapshot();
	EXPECT_EQ(nextSnapshot.superblock().generation.value(), nextIdentity.generation);
	const auto next = NativePropertyLayoutCandidateReader::read(store, nextSnapshot, nextReference, slots,
		nextIdentity.generation, 1U, 10000U, admit);
	EXPECT_EQ(next.readOwner(1U, nextIdentity), nextRoots.front());
	const auto old = NativePropertyLayoutCandidateReader::read(store, published, reference, slots, identity.generation, 1U, 10000U, admit);
	EXPECT_EQ(old.readOwner(1U, identity), roots.front());
	const auto retained = NativePropertyLayoutCandidateReader::read(store, nextSnapshot, reference, slots, identity.generation, 1U, 10000U, admit);
	EXPECT_EQ(retained.readOwner(1U, identity), roots.front());
	EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateReader::read(store, published, nextReference, slots,
		nextIdentity.generation, 1U, 10000U, admit)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateReader::read(store, nextSnapshot, reference, slots,
		nextIdentity.generation, 1U, 10000U, admit)), std::runtime_error);
}

TEST_F(PropertyBlockMembershipSnapshotTest, NativeCandidateRecoversDurablePublicationAfterInterruptedAcknowledgment) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId); static_cast<void>(slots.insert(1U));
	const std::vector<PropertyCollectionRoot> roots{{1U, graph::Node::typeId, {{7U, PropertyValue(int64_t(9))}}}};
	PropertyBlockFieldMembership::SnapshotIdentity identity{};
	uint64_t valueId = 0U;
	{
		LargeValueStore store(io_, *metadata_);
		PropertyCollectionRootStore properties(*slotted_pages_, store, *metadata_);
		auto initial = metadata_->openSnapshot();
		identity = {initial.superblock().database_id.value(), initial.superblock().generation.value() + 1U};
		valueId = initial.superblock().next_large_value_id;
		const auto image = PropertyLayoutCandidate::build(slots, roots, identity, 1U, 10000U, 4096U, 64U, 0U);
		LargeValueWriteOptions options;
		options.first_data_slot = PhysicalSlotId(60U); options.last_data_slot = PhysicalSlotId(79U);
		options.first_manifest_slot = PhysicalSlotId(80U); options.last_manifest_slot = PhysicalSlotId(100U);
		MetadataPublishOptions fail; fail.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateWriter::write(store, *metadata_, properties,
		    initial, slots, image, options, LogSequenceNumber(2U), 1U, 10000U, fail)), BootstrapPublishInterrupted);
		EXPECT_EQ(SuperblockFile(io_).recover().generation.value(), identity.generation);
	}
	reopenNativeStores();
	LargeValueStore store(io_, *metadata_);
	auto recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation.value(), identity.generation);
	const auto reference = store.manifest(recovered, LargeValueId(valueId)).reference();
	const SelectedPropertyReadAdmission admit = [](size_t) {};
	const auto loaded = NativePropertyLayoutCandidateReader::read(store, recovered, reference, slots, identity.generation, 1U, 10000U, admit);
	EXPECT_EQ(loaded.readOwner(1U, identity), roots.front());
}

TEST_F(PropertyBlockMembershipSnapshotTest, NativeCandidateContainerReadsPackedOwnersAndSurvivesReopen) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	std::vector<PropertyCollectionRoot> roots;
	for (uint64_t owner = 1U; owner <= 64U; ++owner) {
		static_cast<void>(slots.insert(owner));
		roots.push_back({owner, graph::Node::typeId, {{7U, PropertyValue(int64_t(owner))}}});
	}
	PropertyBlockFieldMembership::SnapshotIdentity identity{};
	LargeValueRef reference;
	{
		LargeValueStore store(io_, *metadata_);
		auto initial = metadata_->openSnapshot();
		identity = {initial.superblock().database_id.value(), initial.superblock().generation.value()};
		const auto image = PropertyLayoutCandidate::build(slots, roots, identity, 64U, 10000U, 4096U, 64U, 0U);
		ASSERT_EQ(image.layout, PropertyLayoutCandidate::Layout::PLC_PACKED_COLUMNS);
		const auto bytes = PropertyLayoutCandidateCodec::encode(image, slots, 64U, 10000U);
		LargeValueWriteOptions options;
		options.first_data_slot = PhysicalSlotId(60U); options.last_data_slot = PhysicalSlotId(79U);
		options.first_manifest_slot = PhysicalSlotId(80U); options.last_manifest_slot = PhysicalSlotId(100U);
		options.kind = LargeValueKind::LVK_BYTES; options.media_type = NativePropertyLayoutCandidateReader::MEDIA;
		reference = store.write(bytes, options, LogSequenceNumber(2U)).reference;
		auto snapshot = metadata_->openSnapshot();
		const SelectedPropertyReadAdmission admit = [](size_t) {};
		const auto loaded = NativePropertyLayoutCandidateReader::read(store, snapshot, reference, slots, identity.generation, 64U, 10000U, admit);
		EXPECT_EQ(loaded.readOwner(64U, identity), roots.back());
		size_t admissions = 0U;
		const SelectedPropertyReadAdmission counted = [&](size_t) { ++admissions; };
		for (int mode = 0; mode < 4; ++mode) {
			auto wrong = reference;
			if (mode == 0) wrong.media_type = "application/octet-stream";
			if (mode == 1) wrong.kind = LargeValueKind::LVK_UTF8_TEXT;
			const auto birth = mode == 2 ? snapshot.superblock().generation.value() + 1U : mode == 3 ? 0U : identity.generation;
			EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateReader::read(store, snapshot, wrong, slots, birth, 64U, 10000U, counted)), std::invalid_argument);
			EXPECT_EQ(admissions, 0U);
		}
		auto forged = reference; forged.content_hash[0U] ^= std::byte{1U};
		EXPECT_ANY_THROW(static_cast<void>(NativePropertyLayoutCandidateReader::read(store, snapshot, forged, slots, identity.generation, 64U, 10000U, admit)));
		struct Checkpoint final : CancellationCheckpoint {
			mutable size_t calls = 0U; size_t stop = 0U;
			void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
		};
		Checkpoint count;
		static_cast<void>(NativePropertyLayoutCandidateReader::read(store, snapshot, reference, slots, identity.generation, 64U, 10000U, admit, &count));
		for (size_t stop = 1U; stop <= count.calls; ++stop) {
			Checkpoint cancelled; cancelled.stop = stop;
			EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateReader::read(store, snapshot, reference, slots, identity.generation, 64U, 10000U, admit, &cancelled)), std::runtime_error);
			EXPECT_EQ(cancelled.calls, stop);
		}
		EXPECT_EQ(loaded.readOwner(1U, identity), roots.front());
		const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("Test admission rejection"); };
		EXPECT_THROW(static_cast<void>(NativePropertyLayoutCandidateReader::read(store, snapshot, reference, slots, identity.generation, 64U, 10000U, reject)), std::length_error);
		EXPECT_ANY_THROW(static_cast<void>(NativePropertyLayoutCandidateReader::read(store, initial, reference, slots, identity.generation, 64U, 10000U, admit)));
	}
	reopenNativeStores();
	LargeValueStore reopened(io_, *metadata_);
	auto snapshot = metadata_->openSnapshot();
	const SelectedPropertyReadAdmission admit = [](size_t) {};
	const auto restored = NativePropertyLayoutCandidateReader::read(reopened, snapshot, reference, slots, identity.generation, 64U, 10000U, admit);
	EXPECT_EQ(restored.readOwner(1U, identity), roots.front());
}

TEST_F(PropertyBlockMembershipSnapshotTest, LargeValueTokenReadUsesAdmittedDirectoryAndRejectsBudgets) {
	LargeValueStore largeValues(io_, *metadata_);
	PropertyCollectionRootStore properties(*slotted_pages_, largeValues, *metadata_);
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(40U); options.last_slot = PhysicalSlotId(80U);
	options.entity_pool_owner_id = 901U;
	options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, properties, *btree_pages_, options);
	auto create = planner.beginTransaction();
	const auto id = create.createNode(std::array<uint64_t, 0U>{});
	create.replaceNodeProperty({id, graph::Node::typeId,
		{{17U, PropertyValue(std::string(16000U, 'v'))}, {19U, PropertyValue(true)}}});
	ASSERT_TRUE(create.commit().committed());
	auto snapshot = metadata_->openSnapshot();
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, id};
	size_t tokenBytes = 0U, lastTransient = 0U, transientCalls = 0U;
	const SelectedPropertyReadAdmission tokens = [&](size_t bytes) { tokenBytes = bytes; };
	const SelectedPropertyReadAdmission transient = [&](size_t bytes) { lastTransient = bytes; ++transientCalls; };
	EXPECT_EQ(properties.readEntityPropertyTokens(snapshot, key, tokens, transient), (std::vector<uint32_t>{17U, 19U}));
	EXPECT_GT(transientCalls, 0U);
	EXPECT_EQ(lastTransient, 0U);
	PageBackedGraphReadAdapter reader(*metadata_, *entities_, *labels_, properties, *btree_pages_);
	EXPECT_EQ(reader.readEntityPropertyTokens(snapshot, key, tokens, transient), (std::vector<uint32_t>{17U, 19U}));
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	const auto route = slots.insert(id);
	const auto membership = PropertyBlockFieldMembership::buildFromNativeSnapshot(slots, reader, snapshot, 2U, transient);
	EXPECT_EQ(membership.fieldsSnapshotBound(slots, route, snapshot).size(), 2U);
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::buildFromNativeSnapshot(
		slots, reader, snapshot, 1U, transient)), std::runtime_error);
	EXPECT_EQ(tokenBytes, 8U);
	const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("directory budget rejected"); };
	EXPECT_THROW(static_cast<void>(properties.readEntityPropertyTokens(snapshot, key, tokens, reject)), std::length_error);
	EXPECT_THROW(static_cast<void>(properties.readEntityPropertyTokens(snapshot, key, reject, transient)), std::length_error);
	EXPECT_EQ(properties.readEntityPropertyTokens(snapshot, key, tokens, transient), (std::vector<uint32_t>{17U, 19U}));
}

TEST_F(PropertyBlockMembershipSnapshotTest, NativeTokenReadAuthenticatesSourcesAndPreservesSnapshotFields) {
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(40U); options.last_slot = PhysicalSlotId(80U);
	options.entity_pool_owner_id = 901U;
	options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	auto create = planner.beginTransaction();
	const auto id = create.createNode(std::array<uint64_t, 0U>{});
	create.replaceNodeProperty({id, graph::Node::typeId, {{17U, PropertyValue(1)}, {18U, PropertyValue{}}}});
	ASSERT_TRUE(create.commit().committed());
	auto before = metadata_->openSnapshot();
	size_t admitted = 0U;
	const SelectedPropertyReadAdmission tokens = [&](size_t bytes) { admitted = bytes; };
	const SelectedPropertyReadAdmission transient = [](size_t) {};
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, id};
	EXPECT_EQ(properties_->readEntityPropertyTokens(before, key, tokens, transient), (std::vector<uint32_t>{17U, 18U}));
	EXPECT_EQ(admitted, 8U);
	const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("token capacity rejected"); };
	EXPECT_THROW(static_cast<void>(properties_->readEntityPropertyTokens(before, key, reject, transient)), std::length_error);
	EXPECT_THROW(static_cast<void>(properties_->readEntityPropertyTokens(before, key, {}, transient)), FormatException);
	auto update = planner.beginTransaction();
	update.replaceNodeProperty({id, graph::Node::typeId, {{19U, PropertyValue(true)}}});
	ASSERT_TRUE(update.commit().committed());
	auto after = metadata_->openSnapshot();
	EXPECT_EQ(properties_->readEntityPropertyTokens(after, key, tokens, transient), (std::vector<uint32_t>{19U}));
	EXPECT_EQ(properties_->readEntityPropertyTokens(before, key, tokens, transient), (std::vector<uint32_t>{17U, 18U}));
}

TEST_F(PropertyBlockMembershipSnapshotTest, BindsDirectoryToRealSnapshotAcrossPropertyShapeUpdates) {
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(40U);
	options.last_slot = PhysicalSlotId(80U);
	options.entity_pool_owner_id = 901U;
	options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	auto create = planner.beginTransaction();
	const auto id = create.createNode(std::array<uint64_t, 0U>{});
	create.replaceNodeProperty({id, graph::Node::typeId, {{17U, PropertyValue(1)}}});
	ASSERT_TRUE(create.commit().committed());
	auto before = metadata_->openSnapshot();
	const auto oldNode = graph.readNode(before, id);
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	const auto route = slots.insert(id);
	const std::vector<PropertyCollectionRoot> oldRoots{{id, graph::Node::typeId, oldNode.properties}};
	const auto oldDirectory = PropertyBlockFieldMembership::buildSnapshotBound(slots, oldRoots, 1U, before);
	auto update = planner.beginTransaction();
	update.replaceNodeProperty({id, graph::Node::typeId, {{18U, PropertyValue(true)}}});
	ASSERT_TRUE(update.commit().committed());
	auto after = metadata_->openSnapshot();
	ASSERT_NE(before.superblock().generation, after.superblock().generation);
	const auto newNode = graph.readNode(after, id);
	const std::vector<PropertyCollectionRoot> newRoots{{id, graph::Node::typeId, newNode.properties}};
	const auto newDirectory = PropertyBlockFieldMembership::buildSnapshotBound(slots, newRoots, 1U, after);
	EXPECT_EQ(oldDirectory.fieldsSnapshotBound(slots, route, before).front(), 17U);
	EXPECT_EQ(newDirectory.fieldsSnapshotBound(slots, route, after).front(), 18U);
	EXPECT_THROW(static_cast<void>(oldDirectory.fieldsSnapshotBound(slots, route, after)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(newDirectory.fieldsSnapshotBound(slots, route, before)), std::runtime_error);
	EXPECT_EQ(graph.readNode(before, id).properties, oldNode.properties);
}

TEST_F(PropertyBlockMembershipSnapshotTest, DescriptorVersionAllowsUnchangedDirectoryAcrossUnrelatedCommit) {
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(40U); options.last_slot = PhysicalSlotId(80U);
	options.entity_pool_owner_id = 901U;
	options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	auto create = planner.beginTransaction();
	const auto id = create.createNode(std::array<uint64_t, 0U>{});
	create.replaceNodeProperty({id, graph::Node::typeId, {{17U, PropertyValue(1)}}});
	ASSERT_TRUE(create.commit().committed());
	auto before = metadata_->openSnapshot();
	const auto node = graph.readNode(before, id);
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	const auto route = slots.insert(id);
	const auto directory = PropertyBlockFieldMembership::buildSnapshotBound(
		slots, std::vector<PropertyCollectionRoot>{{id, graph::Node::typeId, node.properties}}, 1U, before);
	const auto bytes = directory.encode(84U);
	const auto version = before.superblock().generation.value();
	auto unrelated = planner.beginTransaction();
	static_cast<void>(unrelated.createNode(std::array<uint64_t, 0U>{}));
	ASSERT_TRUE(unrelated.commit().committed());
	auto after = metadata_->openSnapshot();
	EXPECT_EQ(graph.readNode(after, id).properties, node.properties);
	const auto restored = PropertyBlockFieldMembership::decodeFromDescriptor(bytes, slots, after, version, 1U, 84U);
	EXPECT_EQ(restored.fieldsFromDescriptor(slots, route, after, version).front(), 17U);
	EXPECT_THROW(static_cast<void>(restored.fieldsSnapshotBound(slots, route, after)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decodeFromDescriptor(
		bytes, slots, after, after.superblock().generation.value(), 1U, 84U)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(restored.fieldsFromDescriptor(slots, route, before, version + 1U)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(restored.fieldsFromDescriptor(slots, route, after, 0U)), std::runtime_error);
}

TEST_F(PropertyBlockMembershipSnapshotTest, BuildsNodeAndEdgeFieldDirectoriesFromAdmittedNativeSources) {
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(40U); options.last_slot = PhysicalSlotId(80U);
	options.entity_pool_owner_id = 901U;
	options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	PageBackedGraphReadAdapter graph(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	auto create = planner.beginTransaction();
	const auto node = create.createNode(std::array<uint64_t, 0U>{});
	const auto edge = create.createEdge(node, node, 0U);
	create.replaceNodeProperty({node, graph::Node::typeId, {{17U, PropertyValue(1)}}});
	create.replaceEdgeProperty({edge, graph::Edge::typeId, {{18U, PropertyValue(true)}}});
	ASSERT_TRUE(create.commit().committed());
	auto snapshot = metadata_->openSnapshot();
	for (bool isEdge : {false, true}) {
		SCOPED_TRACE(isEdge);
		PropertyBlockOwnerSlots slots(11U, isEdge ? graph::Edge::typeId : graph::Node::typeId);
		const auto route = slots.insert(isEdge ? edge : node);
		size_t admissions = 0U;
		SelectedPropertyReadAdmission admission = [&](size_t) { ++admissions; };
		const auto directory = PropertyBlockFieldMembership::buildFromNativeSnapshot(slots, graph, snapshot, 1U, admission);
		EXPECT_GT(admissions, 0U);
		bool cancelled = false;
		size_t cancelledAdmissions = 0U;
		SelectedPropertyReadAdmission cancelDuringAdmission = [&](size_t) { ++cancelledAdmissions; cancelled = true; };
		const std::function<void()> checkpoint = [&] { if (cancelled) throw std::runtime_error("Native directory construction cancelled"); };
		const auto generation = snapshot.superblock().generation;
		EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::buildFromNativeSnapshot(
			slots, graph, snapshot, 1U, cancelDuringAdmission, checkpoint)), std::runtime_error);
		EXPECT_EQ(cancelledAdmissions, 1U);
		EXPECT_EQ(metadata_->openSnapshot().superblock().generation, generation);
		EXPECT_EQ(directory.fieldsSnapshotBound(slots, route, snapshot).front(), isEdge ? 18U : 17U);
		SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("Source rejected by test budget"); };
		EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::buildFromNativeSnapshot(slots, graph, snapshot, 1U, reject)), std::length_error);
		EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::buildFromNativeSnapshot(slots, graph, snapshot, 1U, {})), std::invalid_argument);
	}
	PropertyBlockOwnerSlots absent(11U, graph::Node::typeId);
	absent.insert(99999U);
	SelectedPropertyReadAdmission admission = [](size_t) {};
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::buildFromNativeSnapshot(absent, graph, snapshot, 1U, admission)), std::runtime_error);
}

TEST_F(PropertyBlockMembershipSnapshotTest, NativeMembershipCancellationInterruptsDirectoryWorkAndAllowsRetry) {
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(40U); options.last_slot = PhysicalSlotId(80U);
	options.entity_pool_owner_id = 901U;
	options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(80U), 902U};
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	PageBackedGraphReadAdapter reader(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	auto create = planner.beginTransaction();
	const auto id = create.createNode(std::array<uint64_t, 0U>{});
	PropertyCollectionRoot root{id, graph::Node::typeId, {}};
	for (uint32_t token = 1U; token <= 64U; ++token)
		root.entries.push_back({token, PropertyValue(int64_t(token))});
	create.replaceNodeProperty(root);
	ASSERT_TRUE(create.commit().committed());
	auto snapshot = metadata_->openSnapshot();
	const auto generation = snapshot.superblock().generation;
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	const auto route = slots.insert(id);
	for (size_t limit : {size_t(20U), size_t(100U)}) {
		size_t checks = 0U, admissions = 0U;
		const auto checkpoint = [&] { if (++checks == limit) throw std::runtime_error("membership cancelled"); };
		const SelectedPropertyReadAdmission admit = [&](size_t) { ++admissions; };
		EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::buildFromNativeSnapshot(
			slots, reader, snapshot, 64U, admit, checkpoint)), std::runtime_error);
		EXPECT_EQ(checks, limit);
		EXPECT_GT(admissions, 0U);
	}
	size_t retained = 0U, peak = 0U;
	const SelectedPropertyReadAdmission admit = [&](size_t bytes) { retained = bytes; peak = (std::max)(peak, bytes); };
	const auto result = PropertyBlockFieldMembership::buildFromNativeSnapshot(slots, reader, snapshot, 64U, admit);
	EXPECT_EQ(retained, result.metadataCapacityBytes());
	EXPECT_GT(peak, retained);
	const SelectedPropertyReadAdmission rejectPeak = [&](size_t bytes) {
		if (bytes > result.metadataCapacityBytes()) throw std::length_error("membership peak rejected");
	};
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::buildFromNativeSnapshot(
		slots, reader, snapshot, 64U, rejectPeak)), std::length_error);
	EXPECT_EQ(result.fieldsSnapshotBound(slots, route, snapshot).size(), 64U);
	EXPECT_EQ(metadata_->openSnapshot().superblock().generation, generation);
}

TEST_F(PropertyBlockMembershipSnapshotTest, MultiOwnerNativeDirectoryUsesBoundedGrowthAndReleasesOwnerScratch) {
	NativeGraphWritePlannerOptions options;
	options.first_slot = PhysicalSlotId(40U); options.last_slot = PhysicalSlotId(100U);
	options.entity_pool_owner_id = 901U;
	options.property_roots = {PhysicalSlotId(40U), PhysicalSlotId(100U), 902U};
	NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, *btree_pages_, options);
	PageBackedGraphReadAdapter reader(*metadata_, *entities_, *labels_, *properties_, *btree_pages_);
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	std::vector<PropertyCollectionRoot> roots;
	auto create = planner.beginTransaction();
	for (uint32_t token = 1U; token <= 33U; ++token) {
		const auto id = create.createNode(std::array<uint64_t, 0U>{});
		static_cast<void>(slots.insert(id));
		roots.push_back({id, graph::Node::typeId, {{token, PropertyValue(int64_t(token))}}});
		create.replaceNodeProperty(roots.back());
	}
	ASSERT_TRUE(create.commit().committed());
	auto snapshot = metadata_->openSnapshot();
	size_t retained = 0U, peak = 0U;
	const SelectedPropertyReadAdmission admission = [&](size_t bytes) { retained = bytes; peak = (std::max)(peak, bytes); };
	const auto directory = PropertyBlockFieldMembership::buildFromNativeSnapshot(slots, reader, snapshot, 1000U, admission);
	const auto compact = PropertyBlockFieldMembership::buildSnapshotBound(slots, roots, 1000U, snapshot);
	EXPECT_EQ(retained, directory.metadataCapacityBytes());
	EXPECT_GT(peak, retained);
	EXPECT_LE(directory.metadataCapacityBytes(), compact.metadataCapacityBytes() + 33U * sizeof(uint32_t));
	for (const auto &root : roots) {
		const auto fields = directory.fieldsSnapshotBound(slots, slots.route(root.owner_entity_id), snapshot);
		ASSERT_EQ(fields.size(), 1U);
		EXPECT_EQ(fields.front(), root.entries.front().key_token);
	}
}

TEST_F(PropertyBlockMembershipSnapshotTest, NativeSparseReaderAuthenticatesDescriptorAndColumnPresence) {
	using zyx::benchmark::NativeSparseGroupReader;
	using zyx::benchmark::SparseGroupDescriptor;
	using zyx::benchmark::SparseFixedWidthPropertyColumn;
	auto store = std::make_unique<LargeValueStore>(io_, *metadata_);
	auto original = metadata_->openSnapshot();
	const PropertyBlockFieldMembership::SnapshotIdentity identity{
		original.superblock().database_id.value(), original.superblock().generation.value()};
	original.release();
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	const auto route = slots.insert(2U);
	const std::vector<PropertyCollectionRoot> roots{{2U, graph::Node::typeId, {{7U, PropertyValue(1)}}}};
	const auto directory = PropertyBlockFieldMembership::build(slots, roots, 1U, identity);
	std::array<std::optional<PropertyValue>, 64U> values{};
	values[route.slot] = PropertyValue(1);
	LargeValueWriteOptions options;
	options.first_data_slot = PhysicalSlotId(60U); options.last_data_slot = PhysicalSlotId(79U);
	options.first_manifest_slot = PhysicalSlotId(80U); options.last_manifest_slot = PhysicalSlotId(100U);
	options.kind = LargeValueKind::LVK_BYTES; options.media_type = SparseGroupDescriptor::COLUMN_MEDIA;
	const auto child = store->write(SparseFixedWidthPropertyColumn::encode(values), options, LogSequenceNumber(2U)).reference;
	const std::array<SparseGroupDescriptor::Column, 1U> references{{{7U, child}}};
	const auto encoded = SparseGroupDescriptor::encode(slots, directory, references, identity, 1U, 4096U);
	options.media_type = NativeSparseGroupReader::ROOT_MEDIA;
	const auto root = store->write(encoded, options, LogSequenceNumber(3U)).reference;
	auto snapshot = metadata_->openSnapshot();
	const SelectedPropertyReadAdmission admit = [](size_t) {};
	const auto loaded = NativeSparseGroupReader::read(*store, snapshot, root, 11U, graph::Node::typeId,
		identity.generation, 1U, 4096U, admit);
	EXPECT_EQ(loaded.group.readOwner(loaded.descriptor.slots, route, identity, 1U), roots.front());
	const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("native reader budget rejected"); };
	EXPECT_THROW(static_cast<void>(NativeSparseGroupReader::read(*store, snapshot, root, 11U, graph::Node::typeId,
		identity.generation, 1U, 4096U, reject)), std::length_error);
	values[route.slot] = std::nullopt;
	options.media_type = SparseGroupDescriptor::COLUMN_MEDIA;
	const auto wrongChild = store->write(SparseFixedWidthPropertyColumn::encode(values), options, LogSequenceNumber(4U)).reference;
	const std::array<SparseGroupDescriptor::Column, 1U> wrongReferences{{{7U, wrongChild}}};
	const auto wrongEncoded = SparseGroupDescriptor::encode(slots, directory, wrongReferences, identity, 1U, 4096U);
	options.media_type = NativeSparseGroupReader::ROOT_MEDIA;
	const auto wrongRoot = store->write(wrongEncoded, options, LogSequenceNumber(5U)).reference;
	auto later = metadata_->openSnapshot();
	EXPECT_THROW(static_cast<void>(NativeSparseGroupReader::read(*store, later, wrongRoot, 11U, graph::Node::typeId,
		identity.generation, 1U, 4096U, admit)), std::runtime_error);
	EXPECT_EQ(loaded.group.readOwner(loaded.descriptor.slots, route, identity, 1U), roots.front());
	snapshot.release(); later.release(); store.reset();
	reopenNativeStores();
	LargeValueStore reopened(io_, *metadata_);
	auto reopenedSnapshot = metadata_->openSnapshot();
	const auto restored = NativeSparseGroupReader::read(reopened, reopenedSnapshot, root, 11U, graph::Node::typeId,
		identity.generation, 1U, 4096U, admit);
	EXPECT_EQ(restored.group.readOwner(restored.descriptor.slots, route, identity, 1U), roots.front());
}

TEST_F(PropertyBlockMembershipSnapshotTest, NativeSparsePublicationIsSingleGenerationAndReusesUnchangedColumns) {
	using zyx::benchmark::NativeSparseGroupReader;
	using zyx::benchmark::NativeSparseGroupWriter;
	using zyx::benchmark::SparsePropertyGroup;
	using zyx::benchmark::SparseFixedWidthPropertyColumn;
	LargeValueStore store(io_, *metadata_);
	PropertyCollectionRootStore properties(*slotted_pages_, store, *metadata_);
	auto initial = metadata_->openSnapshot();
	const PropertyBlockFieldMembership::SnapshotIdentity identity{
		initial.superblock().database_id.value(), initial.superblock().generation.value() + 1U};
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	const auto route = slots.insert(2U);
	const std::vector<PropertyCollectionRoot> roots{{2U, graph::Node::typeId, {{7U, PropertyValue(1)}, {8U, PropertyValue(true)}}}};
	const auto directory = PropertyBlockFieldMembership::build(slots, roots, 2U, identity);
	std::array<std::optional<PropertyValue>, 64U> integers{}, booleans{};
	integers[route.slot] = PropertyValue(1); booleans[route.slot] = PropertyValue(true);
	const auto integerBytes = SparseFixedWidthPropertyColumn::encode(integers);
	const auto booleanBytes = SparseFixedWidthPropertyColumn::encode(booleans);
	const std::array<PropertyBlockFieldMembership::SparseColumnView, 2U> views{{{7U, integerBytes}, {8U, booleanBytes}}};
	const auto group = SparsePropertyGroup::freeze(slots, directory, views, identity);
	LargeValueWriteOptions options;
	options.first_data_slot = PhysicalSlotId(60U); options.last_data_slot = PhysicalSlotId(79U);
	options.first_manifest_slot = PhysicalSlotId(80U); options.last_manifest_slot = PhysicalSlotId(120U);
	for (const auto failpoint : {BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE,
		BootstrapPublishFailpoint::BPF_AFTER_PARTIAL_INACTIVE_WRITE}) {
		MetadataPublishOptions fail;
		fail.bootstrap_failpoint = failpoint;
		EXPECT_THROW(static_cast<void>(NativeSparseGroupWriter::write(store, *metadata_, properties, initial, slots, group,
			options, LogSequenceNumber(2U), 2U, 4096U, nullptr, fail)), BootstrapPublishInterrupted);
		EXPECT_EQ(metadata_->openSnapshot().superblock(), initial.superblock());
		EXPECT_EQ(SuperblockFile(io_).recover(), initial.superblock());
	}
	const auto root = NativeSparseGroupWriter::write(store, *metadata_, properties, initial, slots, group,
		options, LogSequenceNumber(2U), 2U, 4096U);
	auto before = metadata_->openSnapshot();
	EXPECT_EQ(before.superblock().generation.value(), identity.generation);
	EXPECT_EQ(before.superblock().next_large_value_id - initial.superblock().next_large_value_id, 3U);
	const SelectedPropertyReadAdmission admit = [](size_t) {};
	const auto loaded = NativeSparseGroupReader::read(store, before, root, 11U, graph::Node::typeId, identity.generation, 2U, 4096U, admit);
	const auto changed = group.replace(slots, route, 7U, PropertyValue(3), {identity.database_id, identity.generation + 1U}, 2U);
	const auto nextRoot = NativeSparseGroupWriter::write(store, *metadata_, properties, before, slots, changed,
		options, LogSequenceNumber(3U), 2U, 4096U, &loaded);
	auto after = metadata_->openSnapshot();
	EXPECT_EQ(after.superblock().generation.value(), identity.generation + 1U);
	EXPECT_EQ(after.superblock().next_large_value_id - before.superblock().next_large_value_id, 2U);
	const auto next = NativeSparseGroupReader::read(store, after, nextRoot, 11U, graph::Node::typeId,
		identity.generation + 1U, 2U, 4096U, admit);
	EXPECT_EQ(next.descriptor.columns.back().reference, loaded.descriptor.columns.back().reference);
	EXPECT_NE(next.descriptor.columns.front().reference, loaded.descriptor.columns.front().reference);
	const auto candidates = zyx::benchmark::SparseGroupRetirementCandidates::collect(
		root, loaded.descriptor.columns, nextRoot, next.descriptor.columns);
	ASSERT_EQ(candidates.size(), 2U);
	EXPECT_EQ(candidates.front(), loaded.descriptor.columns.front().reference);
	EXPECT_EQ(candidates.back(), root);
	const auto unchanged = zyx::benchmark::SparseGroupRetirementCandidates::collect(
		root, loaded.descriptor.columns, root, loaded.descriptor.columns);
	EXPECT_TRUE(unchanged.empty());
	auto inconsistent = next.descriptor.columns;
	inconsistent.back().reference.logical_bytes += 1U;
	EXPECT_THROW(static_cast<void>(zyx::benchmark::SparseGroupRetirementCandidates::collect(
		root, loaded.descriptor.columns, nextRoot, inconsistent)), std::invalid_argument);
	EXPECT_EQ(next.group.read(slots, route, 7U, next.group.identity()), PropertyValue(3));
	const auto old = NativeSparseGroupReader::read(store, before, root, 11U, graph::Node::typeId, identity.generation, 2U, 4096U, admit);
	EXPECT_EQ(old.group.read(slots, route, 7U, identity), PropertyValue(1));
	EXPECT_THROW(static_cast<void>(NativeSparseGroupWriter::write(store, *metadata_, properties, after, slots, changed,
		options, LogSequenceNumber(4U), 2U, 4096U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(NativeSparseGroupWriter::write(store, *metadata_, properties, before, slots, changed,
		options, LogSequenceNumber(4U), 2U, 4096U, &loaded)), std::runtime_error);
	const auto future = changed.replace(slots, route, 7U, PropertyValue(4),
		{identity.database_id, identity.generation + 2U}, 2U);
	EXPECT_THROW(static_cast<void>(NativeSparseGroupWriter::write(store, *metadata_, properties, after, slots, future,
		options, LogSequenceNumber(4U), 2U, 1U)), std::runtime_error);
	EXPECT_EQ(metadata_->openSnapshot().superblock().generation, after.superblock().generation);
	EXPECT_EQ(metadata_->openSnapshot().superblock().next_large_value_id, after.superblock().next_large_value_id);
	const auto withoutBoolean = changed.replace(slots, route, 8U, std::nullopt,
		{identity.database_id, identity.generation + 2U}, 2U);
	const auto reducedRoot = NativeSparseGroupWriter::write(store, *metadata_, properties, after, slots, withoutBoolean,
		options, LogSequenceNumber(4U), 2U, 4096U, &next);
	auto reducedSnapshot = metadata_->openSnapshot();
	const auto reduced = NativeSparseGroupReader::read(store, reducedSnapshot, reducedRoot, 11U, graph::Node::typeId,
		identity.generation + 2U, 2U, 4096U, admit);
	ASSERT_EQ(reduced.descriptor.columns.size(), 1U);
	EXPECT_EQ(reducedSnapshot.superblock().next_large_value_id - after.superblock().next_large_value_id, 1U);
	EXPECT_EQ(reduced.descriptor.columns.front().reference, next.descriptor.columns.front().reference);
	const auto deletedCandidates = zyx::benchmark::SparseGroupRetirementCandidates::collect(
		nextRoot, next.descriptor.columns, reducedRoot, reduced.descriptor.columns);
	ASSERT_EQ(deletedCandidates.size(), 2U);
	EXPECT_EQ(deletedCandidates.front(), next.descriptor.columns.back().reference);
	EXPECT_EQ(deletedCandidates.back(), nextRoot);
	EXPECT_FALSE(reduced.group.read(slots, route, 8U, reduced.group.identity()));
}

TEST_F(PropertyBlockMembershipSnapshotTest, NativeSparsePublicationRecoversDurableGenerationAfterInterruptedAcknowledgment) {
	using zyx::benchmark::NativeSparseGroupReader;
	using zyx::benchmark::NativeSparseGroupWriter;
	using zyx::benchmark::SparsePropertyGroup;
	using zyx::benchmark::SparseFixedWidthPropertyColumn;
	PropertyBlockOwnerSlots slots(11U, graph::Node::typeId);
	const auto route = slots.insert(2U);
	PropertyBlockFieldMembership::SnapshotIdentity identity{};
	uint64_t descriptorId = 0U;
	{
		LargeValueStore store(io_, *metadata_);
		PropertyCollectionRootStore properties(*slotted_pages_, store, *metadata_);
		auto snapshot = metadata_->openSnapshot();
		identity = {snapshot.superblock().database_id.value(), snapshot.superblock().generation.value() + 1U};
		descriptorId = snapshot.superblock().next_large_value_id + 1U;
		const std::vector<PropertyCollectionRoot> roots{{2U, graph::Node::typeId, {{7U, PropertyValue(1)}}}};
		const auto directory = PropertyBlockFieldMembership::build(slots, roots, 1U, identity);
		std::array<std::optional<PropertyValue>, 64U> values{};
		values[route.slot] = PropertyValue(1);
		const auto bytes = SparseFixedWidthPropertyColumn::encode(values);
		const std::array<PropertyBlockFieldMembership::SparseColumnView, 1U> views{{{7U, bytes}}};
		const auto group = SparsePropertyGroup::freeze(slots, directory, views, identity);
		LargeValueWriteOptions options;
		options.first_data_slot = PhysicalSlotId(60U); options.last_data_slot = PhysicalSlotId(79U);
		options.first_manifest_slot = PhysicalSlotId(80U); options.last_manifest_slot = PhysicalSlotId(120U);
		MetadataPublishOptions fail;
		fail.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(NativeSparseGroupWriter::write(store, *metadata_, properties, snapshot, slots, group,
			options, LogSequenceNumber(2U), 1U, 4096U, nullptr, fail)), BootstrapPublishInterrupted);
		EXPECT_EQ(SuperblockFile(io_).recover().generation.value(), identity.generation);
	}
	reopenNativeStores();
	LargeValueStore store(io_, *metadata_);
	auto recovered = metadata_->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation.value(), identity.generation);
	const auto reference = store.manifest(recovered, LargeValueId(descriptorId)).reference();
	const SelectedPropertyReadAdmission admit = [](size_t) {};
	const auto loaded = NativeSparseGroupReader::read(store, recovered, reference, 11U, graph::Node::typeId,
		identity.generation, 1U, 4096U, admit);
	EXPECT_EQ(loaded.group.read(slots, route, 7U, identity), PropertyValue(1));
	EXPECT_EQ(loaded.descriptor.columns.size(), 1U);
}
