#include <gtest/gtest.h>
#include "graph/utils/Sha256.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/SparseGroupDescriptor.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/SparsePropertyGroup.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/PropertyBlockFieldMembership.hpp"

using zyx::benchmark::PropertyBlockFieldMembership;
using zyx::benchmark::PropertyBlockOwnerSlots;
using graph::storage::PropertyCollectionRoot;
using graph::PropertyValue;
using graph::storage::LittleEndianCodec;

constexpr PropertyBlockFieldMembership::SnapshotIdentity identity{123U, 1U};

TEST(PropertyBlockFieldMembershipPrototypeTest, EnumeratesOnlyActualFieldsForInterleavedSparseShapes) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto second = slots.insert(9U);
	const auto first = slots.insert(2U);
	const std::vector<PropertyCollectionRoot> owners{
		{2U, 1U, {{1U, PropertyValue(1)}, {1000000U, PropertyValue(true)}}},
		{9U, 1U, {{777U, PropertyValue("value")}}}};
	const auto membership = PropertyBlockFieldMembership::build(slots, owners, 3U, identity);
	const auto fields = membership.fields(slots, first, identity);
	EXPECT_EQ(std::vector<uint32_t>(fields.begin(), fields.end()), (std::vector<uint32_t>{1U, 1000000U}));
	EXPECT_EQ(membership.fields(slots, second, identity).size(), 1U);
	RecordProperty("candidate_metadata_capacity_bytes", static_cast<int>(membership.metadataCapacityBytes()));
}

TEST(PropertyBlockFieldMembershipPrototypeTest, RejectsStaleMembershipAfterSlotReuseAndPreservesOldSnapshot) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto oldRoute = slots.insert(2U);
	const auto oldSlots = slots;
	const std::vector<PropertyCollectionRoot> owners{{2U, 1U, {{1U, PropertyValue(1)}}}};
	const auto oldMembership = PropertyBlockFieldMembership::build(slots, owners, 1U, identity);
	slots.erase(oldRoute);
	const auto newRoute = slots.insert(9U);
	EXPECT_EQ(newRoute.slot, oldRoute.slot);
	EXPECT_THROW(static_cast<void>(oldMembership.fields(slots, newRoute, identity)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(oldMembership.fields(slots, oldRoute, identity)), std::runtime_error);
	EXPECT_EQ(oldMembership.fields(oldSlots, oldRoute, identity).front(), 1U);
	const auto current = PropertyBlockFieldMembership::build(slots, std::vector<PropertyCollectionRoot>{{9U, 1U, {}}}, 1U, identity);
	EXPECT_TRUE(current.fields(slots, newRoute, identity).empty());
}

TEST(PropertyBlockFieldMembershipPrototypeTest, RejectsUnboundedAndMalformedMetadataBeforeBuilding) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	slots.insert(2U);
	std::vector<PropertyCollectionRoot> owners{{2U, 1U, {{1U, PropertyValue(1)}, {2U, PropertyValue(2)}}}};
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::build(slots, owners, 0U, identity)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::build(slots, owners, 1U, identity)), std::runtime_error);
	owners.front().entries.back().key_token = 1U;
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::build(slots, owners, 2U, identity)), std::invalid_argument);
	owners.front().entries.front().key_token = 0U;
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::build(slots, owners, 2U, identity)), std::invalid_argument);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, RejectsForeignSnapshotsWhenOwnerRouteRemainsUnchanged) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto route = slots.insert(2U);
	const std::vector<PropertyCollectionRoot> oldOwners{{2U, 1U, {{1U, PropertyValue(1)}}}};
	const std::vector<PropertyCollectionRoot> newOwners{{2U, 1U, {{2U, PropertyValue(true)}}}};
	const PropertyBlockFieldMembership::SnapshotIdentity next{123U, 2U};
	const auto before = PropertyBlockFieldMembership::build(slots, oldOwners, 1U, identity);
	const auto after = PropertyBlockFieldMembership::build(slots, newOwners, 1U, next);
	EXPECT_EQ(before.fields(slots, route, identity).front(), 1U);
	EXPECT_EQ(after.fields(slots, route, next).front(), 2U);
	EXPECT_THROW(static_cast<void>(before.fields(slots, route, next)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(after.fields(slots, route, identity)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(before.fields(slots, route, {124U, 1U})), std::runtime_error);
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::build(slots, oldOwners, 1U, {0U, 1U})), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::build(slots, oldOwners, 1U, {123U, 0U})), std::invalid_argument);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, ValidatesPresenceAgainstActualFieldsDespiteDifferentSlotAndRowOrders) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	slots.insert(9U);
	slots.insert(2U);
	std::vector<PropertyCollectionRoot> owners{
		{2U, 1U, {{1U, PropertyValue{}}, {3U, PropertyValue(7)}}},
		{9U, 1U, {{2U, PropertyValue(true)}}}};
	const auto membership = PropertyBlockFieldMembership::build(slots, owners, 3U, identity);
	const auto verified = zyx::benchmark::TypedPropertyBlock::verify(
		zyx::benchmark::TypedPropertyBlock::encode(11U, owners), 11U, 1U);
	EXPECT_NO_THROW(membership.validateBlockMembership(slots, verified, identity));
	owners.front().entries.erase(owners.front().entries.begin());
	const auto missing = zyx::benchmark::TypedPropertyBlock::verify(
		zyx::benchmark::TypedPropertyBlock::encode(11U, owners), 11U, 1U);
	EXPECT_THROW(membership.validateBlockMembership(slots, missing, identity), std::runtime_error);
	owners.front().entries.insert(owners.front().entries.begin(), {2U, PropertyValue(5)});
	const auto different = zyx::benchmark::TypedPropertyBlock::verify(
		zyx::benchmark::TypedPropertyBlock::encode(11U, owners), 11U, 1U);
	EXPECT_THROW(membership.validateBlockMembership(slots, different, identity), std::runtime_error);
	EXPECT_THROW(membership.validateBlockMembership(slots, verified, {123U, 2U}), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, EmptyMembershipStillAuthenticatesBlockIdentity) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto membership = PropertyBlockFieldMembership::build(slots, {}, 1U, identity);
	const auto empty = zyx::benchmark::TypedPropertyBlock::verify(
		zyx::benchmark::TypedPropertyBlock::encodeEmpty(11U, 1U), 11U, 1U);
	EXPECT_NO_THROW(membership.validateBlockMembership(slots, empty, identity));
	PropertyBlockOwnerSlots foreign(12U, 1U);
	const auto foreignEmpty = zyx::benchmark::TypedPropertyBlock::verify(
		zyx::benchmark::TypedPropertyBlock::encodeEmpty(12U, 1U), 12U, 1U);
	EXPECT_THROW(membership.validateBlockMembership(foreign, foreignEmpty, identity), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, EnumeratesDistinctSingleUseFieldsAtFullOwnerCapacity) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	for (uint64_t owner = 64U; owner != 0U; --owner) slots.insert(owner);
	std::vector<PropertyCollectionRoot> owners;
	for (uint64_t owner = 1U; owner <= 64U; ++owner)
		owners.push_back({owner, 1U, {{static_cast<uint32_t>(owner * 1000U), PropertyValue(static_cast<int64_t>(owner))}}});
	const auto membership = PropertyBlockFieldMembership::build(slots, owners, 64U, identity);
	for (const auto &owner : owners) {
		const auto fields = membership.fields(slots, slots.route(owner.owner_entity_id), identity);
		ASSERT_EQ(fields.size(), 1U);
		EXPECT_EQ(fields.front(), owner.entries.front().key_token);
	}
	const auto proof = zyx::benchmark::TypedPropertyBlock::verify(
		zyx::benchmark::TypedPropertyBlock::encode(11U, owners), 11U, 1U);
	EXPECT_NO_THROW(membership.validateBlockMembership(slots, proof, identity));
	size_t payloadBytes = 0U;
	for (const auto &column : proof.image().columns) payloadBytes += column->size();
	RecordProperty("candidate_metadata_capacity_bytes", static_cast<int>(membership.metadataCapacityBytes()));
	RecordProperty("column_payload_bytes", static_cast<int>(payloadBytes));
	RecordProperty("block_metadata_bytes", static_cast<int>(proof.image().metadata.size()));
	RecordProperty("owner_slot_wire_bytes", static_cast<int>(slots.encode().size()));
}

TEST(PropertyBlockFieldMembershipPrototypeTest, SparseColumnPresenceUsesPhysicalSlotsAndRejectsUnmappedValues) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto second = slots.insert(9U);
	const auto first = slots.insert(2U);
	const std::vector<PropertyCollectionRoot> owners{
		{2U, 1U, {{7U, PropertyValue{}}}}, {9U, 1U, {{7U, PropertyValue(9)}}}};
	const auto membership = PropertyBlockFieldMembership::build(slots, owners, 2U, identity);
	std::vector<std::optional<PropertyValue>> values(64U);
	values[first.slot] = PropertyValue{};
	values[second.slot] = PropertyValue(int64_t(9));
	const auto bytes = zyx::benchmark::SparseFixedWidthPropertyColumn::encode(values);
	EXPECT_NO_THROW(membership.validateSparseColumn(slots, 7U, bytes, identity));
	values[63U] = PropertyValue(int64_t(3));
	const auto unmapped = zyx::benchmark::SparseFixedWidthPropertyColumn::encode(values);
	EXPECT_THROW(membership.validateSparseColumn(slots, 7U, unmapped, identity), std::runtime_error);
	values[63U].reset(); values[first.slot].reset();
	const auto omitted = zyx::benchmark::SparseFixedWidthPropertyColumn::encode(values);
	EXPECT_THROW(membership.validateSparseColumn(slots, 7U, omitted, identity), std::runtime_error);
	EXPECT_THROW(membership.validateSparseColumn(slots, 8U, bytes, identity), std::runtime_error);
	slots.insert(13U);
	EXPECT_THROW(membership.validateSparseColumn(slots, 7U, bytes, identity), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, SparseGroupRejectsOmittedDuplicateAndUnorderedColumns) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto first = slots.insert(2U);
	const auto second = slots.insert(9U);
	const std::vector<PropertyCollectionRoot> owners{
		{2U, 1U, {{1U, PropertyValue(2)}, {3U, PropertyValue(true)}}},
		{9U, 1U, {{1U, PropertyValue{}}}}};
	const auto membership = PropertyBlockFieldMembership::build(slots, owners, 3U, identity);
	std::vector<std::optional<PropertyValue>> values(64U);
	values[first.slot] = PropertyValue(int64_t(2)); values[second.slot] = PropertyValue{};
	const auto one = zyx::benchmark::SparseFixedWidthPropertyColumn::encode(values);
	values[first.slot] = PropertyValue(true); values[second.slot].reset();
	const auto three = zyx::benchmark::SparseFixedWidthPropertyColumn::encode(values);
	using View = PropertyBlockFieldMembership::SparseColumnView;
	std::vector<View> columns{{1U, one}, {3U, three}};
	EXPECT_NO_THROW(membership.validateSparseColumns(slots, columns, identity));
	columns.pop_back();
	EXPECT_NO_THROW(membership.validateSparseColumn(slots, 1U, one, identity));
	EXPECT_THROW(membership.validateSparseColumns(slots, columns, identity), std::runtime_error);
	columns = {{1U, one}, {1U, one}};
	EXPECT_THROW(membership.validateSparseColumns(slots, columns, identity), std::runtime_error);
	columns = {{3U, three}, {1U, one}};
	EXPECT_THROW(membership.validateSparseColumns(slots, columns, identity), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, FieldlessGroupStillRejectsReusedOwnerRoutes) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto route = slots.insert(2U);
	const auto membership = PropertyBlockFieldMembership::build(
		slots, std::vector<PropertyCollectionRoot>{{2U, 1U, {}}}, 1U, identity);
	EXPECT_NO_THROW(membership.validateSparseColumns(slots, {}, identity));
	slots.erase(route);
	slots.insert(9U);
	EXPECT_EQ(slots.activeOwnerCount(), 1U);
	EXPECT_THROW(membership.validateSparseColumns(slots, {}, identity), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, FieldDirectoryRoundTripsNonmonotonicSlicesAndEmptyMembers) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto second = slots.insert(9U);
	const auto first = slots.insert(2U);
	const auto empty = slots.insert(12U);
	const std::vector<PropertyCollectionRoot> owners{
		{2U, 1U, {{1U, PropertyValue(1)}, {3U, PropertyValue(true)}}},
		{9U, 1U, {{7U, PropertyValue(2)}}}, {12U, 1U, {}}};
	const auto membership = PropertyBlockFieldMembership::build(slots, owners, 3U, identity);
	const auto bytes = membership.encode(156U);
	EXPECT_EQ(bytes.size(), 156U);
	const auto restored = PropertyBlockFieldMembership::decode(bytes, slots, identity, 3U, 156U);
	EXPECT_EQ(restored.encode(156U), bytes);
	EXPECT_EQ(restored.fields(slots, first, identity).size(), 2U);
	EXPECT_EQ(restored.fields(slots, second, identity).front(), 7U);
	EXPECT_TRUE(restored.fields(slots, empty, identity).empty());
	EXPECT_THROW(static_cast<void>(membership.encode(155U)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decode(bytes, slots, identity, 2U, 156U)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decode(bytes, slots, identity, 3U, 155U)), std::runtime_error);
	RecordProperty("field_directory_wire_bytes", static_cast<int>(bytes.size()));
}

TEST(PropertyBlockFieldMembershipPrototypeTest, FieldDirectoryRejectsChecksumValidIdentityAndSliceCorruption) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	slots.insert(2U); slots.insert(9U);
	const std::vector<PropertyCollectionRoot> owners{
		{2U, 1U, {{1U, PropertyValue(1)}}}, {9U, 1U, {{2U, PropertyValue(2)}}}};
	const auto original = PropertyBlockFieldMembership::build(slots, owners, 2U, identity).encode(120U);
	for (int mode = 0; mode < 7; ++mode) {
		SCOPED_TRACE(mode);
		auto bytes = original;
		if (mode == 0) LittleEndianCodec::writeU16(bytes, 4U, 2U);
		if (mode == 1) LittleEndianCodec::writeU64(bytes, 16U, 2U);
		if (mode == 2) LittleEndianCodec::writeU64(bytes, 56U, 2U);
		if (mode == 3) LittleEndianCodec::writeU32(bytes, 100U, 0U);
		if (mode == 4) LittleEndianCodec::writeU32(bytes, 76U, 1U);
		if (mode == 5) LittleEndianCodec::writeU32(bytes, 112U, 0U);
		if (mode == 6) LittleEndianCodec::writeU32(bytes, 36U, 65U);
		LittleEndianCodec::writeU32(bytes, 44U, 0U);
		LittleEndianCodec::writeU32(bytes, 44U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decode(bytes, slots, identity, 2U, 120U)), std::runtime_error);
	}
	auto corrupted = original; corrupted.back() ^= std::byte{1U};
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decode(corrupted, slots, identity, 2U, 120U)), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, FieldPresenceUpdatePairsWithSparseColumnAndPreservesOldDirectory) {
	using zyx::benchmark::SparseFixedWidthPropertyColumn;
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto a = slots.insert(9U), b = slots.insert(2U);
	const std::vector<PropertyCollectionRoot> roots{{2U, 1U, {{7U, PropertyValue(2)}}}, {9U, 1U, {}}};
	const auto before = PropertyBlockFieldMembership::build(slots, roots, 2U, identity);
	const PropertyBlockFieldMembership::SnapshotIdentity next{123U, 2U}, last{123U, 3U};
	std::array<std::optional<PropertyValue>, 64U> values{};
	values[b.slot] = PropertyValue(2);
	const auto oldColumn = SparseFixedWidthPropertyColumn::encode(values);
	const auto after = before.withFieldPresence(slots, a, 7U, true, identity, next, 2U);
	const auto newColumn = SparseFixedWidthPropertyColumn::replace(oldColumn, a.slot, PropertyValue{});
	EXPECT_NO_THROW(after.validateSparseColumn(slots, 7U, newColumn, next));
	EXPECT_THROW(after.validateSparseColumn(slots, 7U, oldColumn, next), std::runtime_error);
	EXPECT_NO_THROW(before.validateSparseColumn(slots, 7U, oldColumn, identity));
	EXPECT_TRUE(before.fields(slots, a, identity).empty());
	const auto removed = after.withFieldPresence(slots, b, 7U, false, next, last, 2U);
	const auto removedColumn = SparseFixedWidthPropertyColumn::replace(newColumn, b.slot, std::nullopt);
	EXPECT_NO_THROW(removed.validateSparseColumn(slots, 7U, removedColumn, last));
	EXPECT_TRUE(removed.fields(slots, b, last).empty());
	EXPECT_EQ(removed.fields(slots, a, last).front(), 7U);
	EXPECT_THROW(static_cast<void>(before.withFieldPresence(slots, a, 7U, true, identity, next, 1U)), std::length_error);
	EXPECT_THROW(static_cast<void>(before.withFieldPresence(slots, a, 7U, true, identity, identity, 2U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(before.withFieldPresence(slots, a, 0U, true, identity, next, 2U)), std::invalid_argument);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, SparseGroupUpdateReusesOtherColumnsAndRetiresEmptyColumn) {
	using zyx::benchmark::SparsePropertyGroup;
	using zyx::benchmark::SparseFixedWidthPropertyColumn;
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto a = slots.insert(2U), b = slots.insert(9U);
	const std::vector<PropertyCollectionRoot> roots{
		{2U, 1U, {{7U, PropertyValue(1)}, {8U, PropertyValue(true)}}},
		{9U, 1U, {{7U, PropertyValue(2)}}}};
	const auto directory = PropertyBlockFieldMembership::build(slots, roots, 4U, identity);
	std::array<std::optional<PropertyValue>, 64U> integers{}, booleans{};
	integers[a.slot] = PropertyValue(1); integers[b.slot] = PropertyValue(2);
	booleans[a.slot] = PropertyValue(true);
	auto integerBytes = SparseFixedWidthPropertyColumn::encode(integers);
	const auto booleanBytes = SparseFixedWidthPropertyColumn::encode(booleans);
	const std::array<PropertyBlockFieldMembership::SparseColumnView, 2U> views{{{7U, integerBytes}, {8U, booleanBytes}}};
	EXPECT_THROW(static_cast<void>(SparsePropertyGroup::freeze(slots, directory,
		std::span<const PropertyBlockFieldMembership::SparseColumnView>(views).first(1U), identity)), std::runtime_error);
	const auto before = SparsePropertyGroup::freeze(slots, directory, views, identity);
	integerBytes[0] = std::byte{0};
	const auto after = before.replace(slots, a, 7U, PropertyValue{}, {123U, 2U}, 4U);
	EXPECT_EQ(after.column(8U).get(), before.column(8U).get());
	EXPECT_NE(after.column(7U).get(), before.column(7U).get());
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(*before.column(7U), a.slot), PropertyValue(1));
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(*after.column(7U), a.slot), PropertyValue{});
	EXPECT_EQ(before.readOwner(slots, a, identity, 2U), roots.front());
	EXPECT_FALSE(after.read(slots, a, 999U, after.identity()));
	const auto nullValue = after.read(slots, a, 7U, after.identity());
	ASSERT_TRUE(nullValue.has_value());
	EXPECT_EQ(*nullValue, PropertyValue{});
	EXPECT_THROW(static_cast<void>(after.read(slots, a, 7U, identity)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(before.readOwner(slots, a, identity, 1U)), std::length_error);
	const auto removed = after.replace(slots, a, 8U, std::nullopt, {123U, 3U}, 4U);
	EXPECT_FALSE(removed.column(8U));
	EXPECT_TRUE(before.column(8U));
	EXPECT_EQ(removed.column(7U).get(), after.column(7U).get());
	const auto oneRemaining = removed.replace(slots, a, 7U, std::nullopt, {123U, 4U}, 4U);
	const auto empty = oneRemaining.replace(slots, b, 7U, std::nullopt, {123U, 5U}, 4U);
	EXPECT_FALSE(empty.column(7U));
	EXPECT_TRUE(empty.directory().fields(slots, a, empty.identity()).empty());
	EXPECT_TRUE(empty.directory().fields(slots, b, empty.identity()).empty());
	EXPECT_TRUE(empty.readOwner(slots, b, empty.identity(), 1U).entries.empty());
	const auto added = removed.replace(slots, b, 9U, PropertyValue(true), {123U, 4U}, 4U);
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(*added.column(9U), b.slot), PropertyValue(true));
	EXPECT_THROW(static_cast<void>(before.replace(slots, a, 7U, PropertyValue(true), {123U, 2U}, 4U)), std::invalid_argument);
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(*before.column(7U), a.slot), PropertyValue(1));
}

TEST(PropertyBlockFieldMembershipPrototypeTest, SparseDescriptorRoundTripsBoundReferencesAndRejectsMissingCoverage) {
	using zyx::benchmark::SparseGroupDescriptor;
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto route = slots.insert(2U);
	const std::vector<PropertyCollectionRoot> roots{{2U, 1U, {{7U, PropertyValue(1)}}}};
	const auto directory = PropertyBlockFieldMembership::build(slots, roots, 1U, identity);
	const std::vector<std::byte> childBytes(56U, std::byte{1U});
	const graph::storage::LargeValueRef reference{
		.value_id = graph::storage::LargeValueId(1U), .generation = graph::storage::Generation(1U),
		.logical_bytes = 56U, .content_hash = graph::utils::Sha256::digest(childBytes), .kind = graph::storage::LargeValueKind::LVK_BYTES,
		.codec = graph::storage::LargeValueCodec::LVC_RAW, .media_type = SparseGroupDescriptor::COLUMN_MEDIA};
	const std::array<SparseGroupDescriptor::Column, 1U> columns{{{7U, reference}}};
	const auto bytes = SparseGroupDescriptor::encode(slots, directory, columns, identity, 1U, 4096U);
	const auto decoded = SparseGroupDescriptor::decode(bytes, 11U, 1U, identity, 1U, 4096U);
	ASSERT_EQ(decoded.columns.size(), 1U);
	EXPECT_EQ(decoded.columns.front().reference, reference);
	EXPECT_EQ(decoded.directory.fields(decoded.slots, route, identity).front(), 7U);
	EXPECT_THROW(static_cast<void>(SparseGroupDescriptor::encode(slots, directory, {}, identity, 1U, 4096U)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(SparseGroupDescriptor::decode(bytes, 11U, 1U, {123U, 2U}, 1U, 4096U)), std::runtime_error);
	EXPECT_THROW(static_cast<void>(SparseGroupDescriptor::decode(bytes, 11U, 1U, identity, 1U, bytes.size() - 1U)), std::runtime_error);
	auto corrupt = bytes;
	corrupt.back() ^= std::byte{1U};
	EXPECT_THROW(static_cast<void>(SparseGroupDescriptor::decode(corrupt, 11U, 1U, identity, 1U, 4096U)), std::runtime_error);
	corrupt = bytes;
	LittleEndianCodec::writeU16(corrupt, 6U, 1U);
	LittleEndianCodec::writeU32(corrupt, 20U, 0U);
	LittleEndianCodec::writeU32(corrupt, 20U, graph::utils::calculateCrc(corrupt.data(), corrupt.size()));
	EXPECT_THROW(static_cast<void>(SparseGroupDescriptor::decode(corrupt, 11U, 1U, identity, 1U, 4096U)), std::runtime_error);
	corrupt.assign(bytes.begin(), bytes.begin() + SparseGroupDescriptor::HEADER + PropertyBlockOwnerSlots::WIRE_BYTES +
		LittleEndianCodec::readU32(bytes, 8U));
	LittleEndianCodec::writeU32(corrupt, 12U, 0U);
	LittleEndianCodec::writeU32(corrupt, 20U, 0U);
	LittleEndianCodec::writeU32(corrupt, 20U, graph::utils::calculateCrc(corrupt.data(), corrupt.size()));
	EXPECT_THROW(static_cast<void>(SparseGroupDescriptor::decode(corrupt, 11U, 1U, identity, 1U, 4096U)), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, CompactDirectoryRejectsOwnerMixingAndChecksumValidMalformedCounts) {
	PropertyBlockOwnerSlots slots(11U, 1U);
	const auto a = slots.insert(2U), b = slots.insert(9U);
	const std::vector<PropertyCollectionRoot> roots{{2U, 1U, {{1U, PropertyValue(1)}, {7U, PropertyValue(true)}}}, {9U, 1U, {}}};
	const auto directory = PropertyBlockFieldMembership::build(slots, roots, 2U, identity);
	const auto bytes = directory.encodeSlotCounts(slots, identity, 4096U);
	const auto decoded = PropertyBlockFieldMembership::decodeSlotCounts(bytes, slots, identity, 2U, 4096U);
	EXPECT_EQ(decoded.fields(slots, a, identity).size(), 2U);
	EXPECT_TRUE(decoded.fields(slots, b, identity).empty());
	auto other = slots;
	other.erase(b);
	static_cast<void>(other.insert(10U));
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decodeSlotCounts(bytes, other, identity, 2U, 4096U)), std::runtime_error);
	for (const auto corruption : {0U, 1U, 2U, 3U}) {
		auto malformed = bytes;
		if (corruption == 0U) LittleEndianCodec::writeU32(malformed, 336U, 0U);
		if (corruption == 1U) LittleEndianCodec::writeU32(malformed, 80U + 63U * 4U, 1U);
		if (corruption == 2U) LittleEndianCodec::writeU32(malformed, 44U, 1U);
		if (corruption == 3U) LittleEndianCodec::writeU16(malformed, 4U, 1U);
		LittleEndianCodec::writeU32(malformed, 40U, 0U);
		LittleEndianCodec::writeU32(malformed, 40U, graph::utils::calculateCrc(malformed.data(), malformed.size()));
		EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decodeSlotCounts(malformed, slots, identity, 2U, 4096U)), std::runtime_error);
	}
	EXPECT_THROW(static_cast<void>(PropertyBlockFieldMembership::decodeSlotCounts(bytes, slots, identity, 1U, 4096U)), std::runtime_error);
}

TEST(PropertyBlockFieldMembershipPrototypeTest, AdaptiveDescriptorSelectsCompactDirectoryWithoutSmallGroupRegression) {
	using zyx::benchmark::SparseGroupDescriptor;
	for (size_t count : {size_t(1U), size_t(9U), size_t(10U), size_t(64U)}) {
		PropertyBlockOwnerSlots slots(11U, 1U);
		std::vector<PropertyCollectionRoot> roots;
		for (size_t index = 0U; index < count; ++index) {
			static_cast<void>(slots.insert(index + 1U));
			roots.push_back({index + 1U, 1U, {}});
		}
		const auto directory = PropertyBlockFieldMembership::build(slots, roots, 64U, identity);
		auto bytes = SparseGroupDescriptor::encode(slots, directory, {}, identity, 64U, 4096U);
		const size_t offset = SparseGroupDescriptor::HEADER + PropertyBlockOwnerSlots::WIRE_BYTES;
		EXPECT_EQ(LittleEndianCodec::readU32(bytes, offset), count <= 9U ? PropertyBlockFieldMembership::WIRE_MAGIC : 0x43464D5AU);
		EXPECT_EQ(LittleEndianCodec::readU32(bytes, 8U), count <= 9U ? 48U + count * 32U : 336U);
		const auto decoded = SparseGroupDescriptor::decode(bytes, 11U, 1U, identity, 64U, 4096U);
		for (const auto &root : roots)
			EXPECT_TRUE(decoded.directory.fields(decoded.slots, decoded.slots.route(root.owner_entity_id), identity).empty());
		LittleEndianCodec::writeU16(bytes, 4U, 1U);
		LittleEndianCodec::writeU32(bytes, 20U, 0U);
		LittleEndianCodec::writeU32(bytes, 20U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		if (count <= 9U) EXPECT_NO_THROW(static_cast<void>(SparseGroupDescriptor::decode(bytes, 11U, 1U, identity, 64U, 4096U)));
		else EXPECT_THROW(static_cast<void>(SparseGroupDescriptor::decode(bytes, 11U, 1U, identity, 64U, 4096U)), std::runtime_error);
	}
}
