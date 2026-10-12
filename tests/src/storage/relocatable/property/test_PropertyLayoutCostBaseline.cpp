#include <gtest/gtest.h>
#include "benchmarks/graphdb-comparison/property-block-prototype/SparseGroupDescriptor.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/PackedPropertyColumnPage.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/PropertyLayoutCandidate.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/PropertyLayoutCandidateCodec.hpp"
#include "graph/utils/Sha256.hpp"

#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"

using namespace graph;
using namespace graph::storage;

TEST(PropertyLayoutCostBaselineTest, CandidateContainerRejectsChecksumValidInvalidHeadersAndSections) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	for (uint64_t owner = 1U; owner <= 64U; ++owner) static_cast<void>(slots.insert(owner));
	for (const bool unique : {false, true}) {
		std::vector<PropertyCollectionRoot> roots;
		for (uint64_t owner = 1U; owner <= 64U; ++owner)
			roots.push_back({owner, Node::typeId, {{unique ? static_cast<uint32_t>(owner) : 1U, PropertyValue(int64_t(1))}}});
		const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 64U, 10000U, 4096U, 64U, 0U);
		const auto original = PropertyLayoutCandidateCodec::encode(image, slots, 64U, 10000U);
		for (int mode = 0; mode < 10; ++mode) {
			SCOPED_TRACE(mode);
			auto bytes = original;
			if (mode == 0) LittleEndianCodec::writeU16(bytes, 4U, 2U);
			if (mode == 1) LittleEndianCodec::writeU16(bytes, 6U, 3U);
			if (mode == 2) LittleEndianCodec::writeU64(bytes, 24U, 12U);
			if (mode == 3) LittleEndianCodec::writeU32(bytes, 32U, Node::typeId + 1U);
			if (mode == 4) LittleEndianCodec::writeU32(bytes, 36U, 4097U);
			if (mode == 5) LittleEndianCodec::writeU32(bytes, PropertyLayoutCandidateCodec::HEADER, 5U);
			if (mode == 6) LittleEndianCodec::writeU32(bytes, PropertyLayoutCandidateCodec::HEADER + 4U, UINT32_MAX);
			if (mode == 7) LittleEndianCodec::writeU32(bytes, PropertyLayoutCandidateCodec::HEADER + 4U, 0U);
			if (mode == 8) { bytes.push_back(std::byte{}); LittleEndianCodec::writeU32(bytes, 40U, static_cast<uint32_t>(bytes.size())); }
			if (mode == 9) { bytes.resize(PropertyLayoutCandidateCodec::HEADER + 2U); LittleEndianCodec::writeU32(bytes, 40U, static_cast<uint32_t>(bytes.size())); }
			LittleEndianCodec::writeU32(bytes, 44U, 0U);
			LittleEndianCodec::writeU32(bytes, 44U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
			EXPECT_THROW(static_cast<void>(PropertyLayoutCandidateCodec::decode(bytes, slots, {123U, 1U}, 64U, 10000U)), std::runtime_error);
		}
		const auto decoded = PropertyLayoutCandidateCodec::decode(original, slots, {123U, 1U}, 64U, 10000U);
		EXPECT_EQ(PropertyLayoutCandidate::readOwner(decoded, slots, 1U, {123U, 1U}, 64U, 10000U), roots.front());
	}
}

TEST(PropertyLayoutCostBaselineTest, CandidateContainerCancelsEncodingAndDecodingWithoutChangingSource) {
	using namespace zyx::benchmark;
	struct Checkpoint final : CancellationCheckpoint {
		mutable size_t calls = 0U; size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
	};
	PropertyBlockOwnerSlots slots(11U, Node::typeId); static_cast<void>(slots.insert(1U));
	const std::vector<PropertyCollectionRoot> roots{{1U, Node::typeId, {{1U, PropertyValue(std::string(150000U, 'x'))}}}};
	const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 1U, 200000U, 4096U, 64U, 0U);
	Checkpoint encoded;
	const auto bytes = PropertyLayoutCandidateCodec::encode(image, slots, 1U, 200000U, &encoded);
	Checkpoint decoded;
	EXPECT_EQ(PropertyLayoutCandidateCodec::decode(bytes, slots, {123U, 1U}, 1U, 200000U, &decoded).owners, image.owners);
	for (int operation = 0; operation < 2; ++operation) {
		for (size_t stop = 1U; stop <= (operation == 0 ? encoded.calls : decoded.calls); ++stop) {
			Checkpoint cancelled; cancelled.stop = stop;
			if (operation == 0) EXPECT_THROW(static_cast<void>(PropertyLayoutCandidateCodec::encode(image, slots, 1U, 200000U, &cancelled)), std::runtime_error);
			else EXPECT_THROW(static_cast<void>(PropertyLayoutCandidateCodec::decode(bytes, slots, {123U, 1U}, 1U, 200000U, &cancelled)), std::runtime_error);
			EXPECT_EQ(cancelled.calls, stop);
		}
	}
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(image.owners[0U]), roots[0U]);
}

TEST(PropertyLayoutCostBaselineTest, VerifiedCandidateConstructionAndReadRespectEveryCancellationBoundary) {
	using namespace zyx::benchmark;
	struct Checkpoint final : CancellationCheckpoint {
		mutable size_t calls = 0U; size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
	};
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	for (uint64_t owner = 1U; owner <= 64U; ++owner) static_cast<void>(slots.insert(owner));
	for (const bool unique : {false, true}) {
		std::vector<PropertyCollectionRoot> roots;
		for (uint64_t owner = 1U; owner <= 64U; ++owner)
			roots.push_back({owner, Node::typeId, {{unique ? static_cast<uint32_t>(owner) : 1U, PropertyValue(int64_t(1))}}});
		const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 64U, 10000U, 4096U, 64U, 0U);
		Checkpoint count;
		const auto verified = PropertyLayoutCandidate::freeze(image, slots, {123U, 1U}, 64U, 10000U, &count);
		for (size_t stop = 1U; stop <= count.calls; ++stop) {
			Checkpoint cancelled; cancelled.stop = stop;
			EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::freeze(image, slots, {123U, 1U}, 64U, 10000U, &cancelled)), std::runtime_error);
			EXPECT_EQ(cancelled.calls, stop);
		}
		Checkpoint read;
		EXPECT_EQ(verified.readOwner(64U, {123U, 1U}, &read), roots.back());
		for (size_t stop = 1U; stop <= read.calls; ++stop) {
			Checkpoint cancelled; cancelled.stop = stop;
			EXPECT_THROW(static_cast<void>(verified.readOwner(64U, {123U, 1U}, &cancelled)), std::runtime_error);
			EXPECT_EQ(cancelled.calls, stop);
		}
		EXPECT_EQ(verified.readOwner(64U, {123U, 1U}), roots.back());
	}
}

TEST(PropertyLayoutCostBaselineTest, CandidateRejectsValidPayloadsThatDisagreeWithMembershipOrInactiveSlots) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	std::vector<PropertyCollectionRoot> roots;
	for (uint64_t owner = 1U; owner <= 63U; ++owner) {
		static_cast<void>(slots.insert(owner));
		roots.push_back({owner, Node::typeId, {{1U, PropertyValue(int64_t(1))}}});
	}
	const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 63U, 10000U, 4096U, 64U, 0U);
	ASSERT_EQ(image.layout, PropertyLayoutCandidate::Layout::PLC_PACKED_COLUMNS);
	for (int mode = 0; mode < 3; ++mode) {
		SCOPED_TRACE(mode);
		std::vector<std::optional<PropertyValue>> values(64U);
		for (const auto &root : roots) values[slots.route(root.owner_entity_id).slot] = root.entries.front().value;
		if (mode == 0) values[slots.route(1U).slot] = std::nullopt;
		if (mode == 1) values[63U] = PropertyValue(int64_t(9));
		const auto payload = CompactFixedPropertyColumn::encode(values);
		const std::vector<PackedPropertyColumnPage::Column> columns{
			{mode == 2 ? 2U : 1U, payload, PackedPropertyColumnPage::Kind::PCP_COMPACT_FIXED}};
		auto invalid = image;
		invalid.pages = PackedPropertyColumnPage::pack(columns, 4096U, 64U);
		EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::readOwner(invalid, slots, 1U, {123U, 1U}, 63U, 10000U)), std::runtime_error);
	}
	EXPECT_EQ(PropertyLayoutCandidate::readOwner(image, slots, 1U, {123U, 1U}, 63U, 10000U), roots.front());
}

TEST(PropertyLayoutCostBaselineTest, CandidateReadAdmitsAggregateBytesAndFieldsBeforeValueDecode) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	static_cast<void>(slots.insert(1U)); static_cast<void>(slots.insert(2U));
	const std::vector<PropertyCollectionRoot> roots{
		{1U, Node::typeId, {{1U, PropertyValue(int64_t(1))}}},
		{2U, Node::typeId, {{2U, PropertyValue(int64_t(2))}}}};
	const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 2U, 10000U, 4096U, 2U, 0U);
	ASSERT_EQ(image.layout, PropertyLayoutCandidate::Layout::PLC_OWNER_ROOTS);
	EXPECT_EQ(PropertyLayoutCandidate::readOwner(image, slots, 1U, {123U, 1U}, 2U, image.bytes), roots[0U]);
	EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::readOwner(image, slots, 1U, {123U, 1U}, 2U, image.bytes - 1U)), std::length_error);
	EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::readOwner(image, slots, 1U, {123U, 1U}, 1U, image.bytes)), std::length_error);
	EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::readOwner(image, slots, 1U, {123U, 1U}, 2U, 0U)), std::invalid_argument);
}

TEST(PropertyLayoutCostBaselineTest, CandidateCancellationIncludingLengthErrorsNeverBecomesFallback) {
	using namespace zyx::benchmark;
	struct Checkpoint final : CancellationCheckpoint {
		mutable size_t calls = 0U; size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::length_error("Test cancellation"); }
	};
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	std::vector<PropertyCollectionRoot> roots;
	for (uint64_t owner = 1U; owner <= 64U; ++owner) {
		static_cast<void>(slots.insert(owner));
		roots.push_back({owner, Node::typeId, {{1U, PropertyValue(int64_t(1))}}});
	}
	const auto run = [&](Checkpoint &checkpoint) {
		return PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 64U, 10000U, 4096U, 64U, 0U, &checkpoint);
	};
	Checkpoint count;
	EXPECT_EQ(run(count).layout, PropertyLayoutCandidate::Layout::PLC_PACKED_COLUMNS);
	for (size_t stop = 1U; stop <= count.calls; ++stop) {
		Checkpoint cancelled; cancelled.stop = stop;
		EXPECT_THROW(static_cast<void>(run(cancelled)), std::length_error);
		EXPECT_EQ(cancelled.calls, stop);
	}
	EXPECT_EQ(roots.front().entries.front().value, PropertyValue(int64_t(1)));
}

TEST(PropertyLayoutCostBaselineTest, CandidateColumnLimitFallsBackWithoutRejectingValidOwnerProperties) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	static_cast<void>(slots.insert(1U));
	PropertyCollectionRoot root{1U, Node::typeId, {}};
	for (uint32_t token = 1U; token <= 4097U; ++token) root.entries.push_back({token, PropertyValue(int64_t(token))});
	const std::vector<PropertyCollectionRoot> roots{root};
	const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 4097U, 1000000U, 4096U, 64U, 0U);
	EXPECT_EQ(image.layout, PropertyLayoutCandidate::Layout::PLC_OWNER_ROOTS);
	ASSERT_EQ(image.owners.size(), 1U);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(image.owners[0U]), root);
	EXPECT_TRUE(image.pages.empty());
	EXPECT_TRUE(image.slots.empty());
	EXPECT_TRUE(image.membership.empty());
}

TEST(PropertyLayoutCostBaselineTest, CandidateMetadataRejectionPreservesFallbackButDoesNotHideInvalidOwners) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	static_cast<void>(slots.insert(1U));
	std::vector<PropertyCollectionRoot> roots{{1U, Node::typeId, {{1U, PropertyValue(int64_t(1))}}}};
	const size_t exact = EntityPropertyCollectionRootCodec::encode(roots[0U]).size();
	const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 1U, exact, 4096U, 1U, SIZE_MAX);
	EXPECT_EQ(image.layout, PropertyLayoutCandidate::Layout::PLC_OWNER_ROOTS);
	EXPECT_EQ(image.bytes, exact);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(image.owners[0U]), roots[0U]);
	EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 1U, exact - 1U, 4096U, 1U, 0U)), std::length_error);
	roots[0U].owner_entity_id = 2U;
	EXPECT_ANY_THROW(static_cast<void>(PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 1U, exact, 4096U, 1U, SIZE_MAX)));
}

TEST(PropertyLayoutCostBaselineTest, LayoutCandidateRetainsOneAuthoritativePayloadAndRejectsSparseRegression) {
	using namespace zyx::benchmark;
	PropertyBlockOwnerSlots slots(11U, Node::typeId);
	for (uint64_t owner = 1U; owner <= 64U; ++owner) static_cast<void>(slots.insert(owner));
	for (const bool unique : {false, true}) {
		std::vector<PropertyCollectionRoot> roots;
		for (uint64_t owner = 1U; owner <= 64U; ++owner)
			roots.push_back({owner, Node::typeId, {{unique ? static_cast<uint32_t>(owner) : 1U, PropertyValue(int64_t(1))}}});
		const auto image = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 64U, 10000U, 4096U, 64U, 0U);
		EXPECT_EQ(image.layout, unique ? PropertyLayoutCandidate::Layout::PLC_OWNER_ROOTS : PropertyLayoutCandidate::Layout::PLC_PACKED_COLUMNS);
		EXPECT_EQ(image.bytes, unique ? 3456U : 2296U);
		EXPECT_EQ(image.owners.empty(), !unique);
		EXPECT_EQ(image.pages.empty(), unique);
		const auto encoded = PropertyLayoutCandidateCodec::encode(image, slots, 64U, 10000U);
		EXPECT_EQ(encoded.size(), unique ? 4016U : 2296U);
		const auto reopened = PropertyLayoutCandidateCodec::decode(encoded, slots, {123U, 1U}, 64U, 10000U);
		EXPECT_EQ(reopened.bytes, image.bytes);
		EXPECT_EQ(PropertyLayoutCandidate::readOwner(reopened, slots, 64U, {123U, 1U}, 64U, 10000U), roots.back());
		EXPECT_THROW(static_cast<void>(PropertyLayoutCandidateCodec::decode(encoded, slots, {123U, 2U}, 64U, 10000U)), std::runtime_error);
		EXPECT_THROW(static_cast<void>(PropertyLayoutCandidateCodec::encode(image, slots, 64U, encoded.size() - 1U)), std::length_error);
		const auto verified = PropertyLayoutCandidate::freeze(image, slots, {123U, 1U}, 64U, 10000U);
		EXPECT_EQ(verified.retainedEncodedBytes(), unique ? 3456U : 536U);
		EXPECT_EQ(verified.readOwner(1U, {123U, 1U}), roots.front());
		EXPECT_EQ(verified.readOwner(64U, {123U, 1U}), roots.back());
		EXPECT_THROW(static_cast<void>(verified.readOwner(1U, {123U, 2U})), std::runtime_error);
		auto mutableSource = image;
		const auto independent = PropertyLayoutCandidate::freeze(mutableSource, slots, {123U, 1U}, 64U, 10000U);
		mutableSource.owners.clear(); mutableSource.pages.clear();
		EXPECT_EQ(independent.readOwner(1U, {123U, 1U}), roots.front());
		EXPECT_EQ(PropertyLayoutCandidate::readOwner(image, slots, 1U, {123U, 1U}, 64U, 10000U), roots.front());
		EXPECT_EQ(PropertyLayoutCandidate::readOwner(image, slots, 64U, {123U, 1U}, 64U, 10000U), roots.back());
		EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::readOwner(image, slots, 1U, {123U, 2U}, 64U, 10000U)), std::runtime_error);
		if (!unique) {
			auto invalid = image;
			invalid.pages.push_back(image.pages.front());
			EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::readOwner(invalid, slots, 1U, {123U, 1U}, 64U, 10000U)), std::runtime_error);
			invalid = image; invalid.pages.clear();
			EXPECT_THROW(static_cast<void>(PropertyLayoutCandidate::readOwner(invalid, slots, 1U, {123U, 1U}, 64U, 10000U)), std::runtime_error);
		}
		if (!unique) {
			const auto rejected = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 64U, 10000U, 4096U, 64U, 2000U);
			EXPECT_EQ(rejected.layout, PropertyLayoutCandidate::Layout::PLC_OWNER_ROOTS);
			roots[0U].entries[0U].value = PropertyValue("mixed");
			const auto mixed = PropertyLayoutCandidate::build(slots, roots, {123U, 1U}, 64U, 10000U, 4096U, 64U, 0U);
			EXPECT_EQ(mixed.layout, PropertyLayoutCandidate::Layout::PLC_OWNER_ROOTS);
			EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(mixed.owners[0U]), roots[0U]);
		}
	}
}

TEST(PropertyLayoutCostBaselineTest, CompactPagesCountOwnerAndMembershipMetadataBeforeLayoutAdmission) {
	using namespace zyx::benchmark;
	const PropertyBlockFieldMembership::SnapshotIdentity identity{123U, 1U};
	for (const bool uniqueFields : {false, true}) {
		PropertyBlockOwnerSlots slots(11U, Node::typeId);
		std::vector<PropertyCollectionRoot> roots;
		size_t currentBytes = 0U;
		for (uint64_t owner = 1U; owner <= 64U; ++owner) {
			static_cast<void>(slots.insert(owner));
			roots.push_back({owner, Node::typeId, {{uniqueFields ? static_cast<uint32_t>(owner) : 1U, PropertyValue(int64_t(1))}}});
			currentBytes += EntityPropertyCollectionRootCodec::encode(roots.back()).size();
		}
		const auto directory = PropertyBlockFieldMembership::build(slots, roots, 64U, identity);
		const size_t directoryBytes = directory.encodeAdaptive(slots, identity, 10000U).size();
		std::vector<std::vector<std::byte>> images;
		for (uint32_t token = 1U; token <= (uniqueFields ? 64U : 1U); ++token) {
			std::vector<std::optional<PropertyValue>> values(64U);
			for (const auto &root : roots) if (root.entries.front().key_token == token)
				values[slots.route(root.owner_entity_id).slot] = root.entries.front().value;
			images.push_back(CompactFixedPropertyColumn::encode(values));
		}
		std::vector<PackedPropertyColumnPage::Column> columns;
		for (size_t index = 0U; index < images.size(); ++index) columns.push_back({static_cast<uint32_t>(index + 1U), images[index], PackedPropertyColumnPage::Kind::PCP_COMPACT_FIXED});
		const auto pages = PackedPropertyColumnPage::pack(columns, 4096U, 64U);
		size_t pageBytes = 0U;
		for (const auto &page : pages) pageBytes += page.size();
		const size_t candidateBytes = slots.encode().size() + directoryBytes + pageBytes;
		EXPECT_EQ(directoryBytes, 592U);
		EXPECT_EQ(pageBytes, uniqueFields ? 3096U : 576U);
		EXPECT_EQ(candidateBytes, uniqueFields ? 4744U : 2224U);
		EXPECT_EQ(currentBytes, 3456U);
		if (uniqueFields) EXPECT_GT(candidateBytes, currentBytes);
		else EXPECT_LT(candidateBytes, currentBytes);
		RecordProperty(uniqueFields ? "compact_sparse_with_routes_bytes" : "compact_dense_with_routes_bytes", static_cast<int>(candidateBytes));
	}
}

TEST(PropertyLayoutCostBaselineTest, SharedPagesStillRejectOneOffFieldsWithoutCompactColumnHeaders) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	size_t ownerBytes = 0U;
	std::vector<std::vector<std::byte>> images;
	images.reserve(64U);
	std::vector<std::optional<PropertyValue>> dense(64U);
	for (size_t row = 0U; row < 64U; ++row) {
		PropertyCollectionRoot root{row + 1U, Node::typeId, {{static_cast<uint32_t>(row + 1U), PropertyValue(int64_t(1))}}};
		ownerBytes += EntityPropertyCollectionRootCodec::encode(root).size();
		std::vector<std::optional<PropertyValue>> sparse(64U);
		sparse[row] = PropertyValue(int64_t(1));
		images.push_back(Column::encode(sparse, 4096U).encodeImage(4096U));
		dense[row] = PropertyValue(int64_t(1));
	}
	std::vector<Page::Column> columns;
	for (size_t index = 0U; index < images.size(); ++index) columns.push_back({static_cast<uint32_t>(index + 1U), images[index]});
	const auto sparsePages = Page::pack(columns, 4096U, 64U);
	size_t sparseBytes = 0U;
	for (const auto &page : sparsePages) sparseBytes += page.size();
	const auto denseImage = Column::encode(dense, 4096U).encodeImage(4096U);
	const std::vector<Page::Column> denseColumns{{1U, denseImage}};
	const auto densePage = Page::encode(denseColumns, 4096U);
	EXPECT_EQ(ownerBytes, 3456U);
	EXPECT_EQ(sparsePages.size(), 2U);
	EXPECT_EQ(sparseBytes, 6704U);
	EXPECT_EQ(densePage.size(), 632U);
	EXPECT_GT(sparseBytes, ownerBytes);
	RecordProperty("shared_sparse_page_bytes", static_cast<int>(sparseBytes));
	RecordProperty("shared_dense_page_bytes", static_cast<int>(densePage.size()));
	RecordProperty("current_owner_root_bytes", static_cast<int>(ownerBytes));
}

TEST(PropertyLayoutCostBaselineTest, TokenEnumerationAdmitsOnlyTokensAndRejectsInvalidDirectory) {
	const PropertyCollectionRoot root{1U, Node::typeId,
		{{2U, PropertyValue(std::string(16000U, 'x'))}, {9U, PropertyValue{}},
		 {1000U, PropertyValue(int64_t(7))}}};
	const auto bytes = EntityPropertyCollectionRootCodec::encode(root);
	size_t charged = 0U;
	const SelectedPropertyReadAdmission admission = [&](size_t size) { charged = size; };
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decodeTokens(bytes, &admission),
		(std::vector<uint32_t>{2U, 9U, 1000U}));
	EXPECT_EQ(charged, 3U * sizeof(uint32_t));
	const SelectedPropertyReadAdmission reject = [](size_t) { throw std::length_error("token budget exceeded"); };
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeTokens(bytes, &reject)), std::length_error);
	auto corrupt = bytes;
	const size_t header = RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES;
	const auto firstOffset = LittleEndianCodec::readU32(corrupt, header);
	LittleEndianCodec::writeU32(corrupt, header + firstOffset, 0U);
	charged = 0U;
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeTokens(corrupt, &admission)), FormatException);
	EXPECT_EQ(charged, 0U);
	EXPECT_TRUE(EntityPropertyCollectionRootCodec::decodeTokens(
		EntityPropertyCollectionRootCodec::encode({1U, Node::typeId, {}})).empty());
}

TEST(PropertyLayoutCostBaselineTest, WideOwnerUpdateRetainsWholeCollectionEncodingUnit) {
	PropertyCollectionRoot root{1U, Node::typeId, {}};
	for (uint32_t token = 1U; token <= 64U; ++token)
		root.entries.push_back({token, PropertyValue(static_cast<int64_t>(token))});
	const auto before = EntityPropertyCollectionRootCodec::encode(root);
	root.entries[31U].value = PropertyValue(int64_t(999));
	const auto after = EntityPropertyCollectionRootCodec::encode(root);
	EXPECT_EQ(before.size(), after.size());
	EXPECT_NE(before, after);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::entryCount(after), 64U);
	const auto selected = EntityPropertyCollectionRootCodec::decodeSelected(after, 32U);
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(selected->value, PropertyValue(int64_t(999)));
	RecordProperty("owner_fields", 64);
	RecordProperty("whole_owner_encoded_bytes", static_cast<int>(after.size()));
}

TEST(PropertyLayoutCostBaselineTest, LargeValueDirectorySeparatesFieldCountFromPayloadBytes) {
	const std::vector<PropertyCollectionRootEntry> entries{
		{1U, PropertyValue(int64_t(1))},
		{2U, PropertyValue(std::string(16000U, 'v'))},
		{3U, PropertyValue(true)}};
	const auto encoded = EntityPropertyLargeValuePayloadCodec::encode(entries);
	const auto directory = EntityPropertyLargeValuePayloadCodec::decodeDirectory(encoded);
	ASSERT_EQ(directory.size(), 3U);
	const auto selected = EntityPropertyLargeValuePayloadCodec::decodeSelected(encoded, 1U);
	ASSERT_TRUE(selected.has_value());
	EXPECT_EQ(selected->value, PropertyValue(int64_t(1)));
	EXPECT_FALSE(EntityPropertyLargeValuePayloadCodec::decodeSelected(encoded, 4U).has_value());
	EXPECT_GT(encoded.size(), 16000U);
	RecordProperty("actual_fields", 3);
	RecordProperty("variable_payload_bytes", 16000);
	RecordProperty("whole_large_value_encoded_bytes", static_cast<int>(encoded.size()));
}

TEST(PropertyLayoutCostBaselineTest, TokenDirectoryCancellationStopsValidationAndCopyBeforeCompletion) {
	class Stop final : public CancellationCheckpoint {
	public:
		mutable size_t calls = 0U;
		size_t limit;
		explicit Stop(size_t value) : limit(value) {}
		void check() const override { if (++calls == limit) throw std::runtime_error("directory cancelled"); }
	};
	PropertyCollectionRoot root{1U, Node::typeId, {}};
	for (uint32_t token = 1U; token <= 64U; ++token)
		root.entries.push_back({token, PropertyValue(int64_t(token))});
	const auto inlineBytes = EntityPropertyCollectionRootCodec::encode(root);
	const auto largeBytes = EntityPropertyLargeValuePayloadCodec::encode(root.entries);
	size_t admissions = 0U;
	const SelectedPropertyReadAdmission admit = [&](size_t) { ++admissions; };
	for (size_t limit : {size_t(8U), size_t(70U)}) {
		Stop inlineStop(limit);
		EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeTokens(inlineBytes, &admit, &inlineStop)), std::runtime_error);
		EXPECT_EQ(inlineStop.calls, limit);
		Stop largeStop(limit);
		EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeDirectory(largeBytes, &admit, &largeStop)), std::runtime_error);
		EXPECT_EQ(largeStop.calls, limit);
	}
	EXPECT_EQ(admissions, 4U);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decodeTokens(inlineBytes).size(), 64U);
	EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::decodeDirectory(largeBytes).size(), 64U);
}

TEST(PropertyLayoutCostBaselineTest, AdmissionTriggeredCancellationStopsBeforeDirectoryCapacityReadmission) {
	class Stop final : public CancellationCheckpoint {
	public:
		bool cancelled = false;
		void check() const override { if (cancelled) throw std::runtime_error("admission cancelled"); }
	};
	const PropertyCollectionRoot root{1U, Node::typeId, {{1U, PropertyValue(int64_t(1))}}};
	Stop stop;
	size_t calls = 0U;
	const SelectedPropertyReadAdmission admission = [&](size_t) { ++calls; stop.cancelled = true; };
	const auto large = EntityPropertyLargeValuePayloadCodec::encode(root.entries);
	EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decodeDirectory(large, &admission, &stop)), std::runtime_error);
	EXPECT_EQ(calls, 1U);
	stop.cancelled = false;
	calls = 0U;
	const auto bytes = EntityPropertyCollectionRootCodec::encode(root);
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeTokens(bytes, &admission, &stop)), std::runtime_error);
	EXPECT_EQ(calls, 1U);
}

TEST(PropertyLayoutCostBaselineTest, ActualTokenCapacityAdmissionCanRejectAndCancelBeforeCopy) {
	class Stop final : public CancellationCheckpoint {
	public:
		bool cancelled = false;
		void check() const override { if (cancelled) throw std::runtime_error("capacity cancelled"); }
	};
	const auto bytes = EntityPropertyCollectionRootCodec::encode(
		{1U, Node::typeId, {{1U, PropertyValue(int64_t(1))}}});
	size_t calls = 0U;
	const SelectedPropertyReadAdmission reject = [&](size_t) {
		if (++calls == 2U) throw std::length_error("actual capacity rejected");
	};
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeTokens(bytes, &reject)), std::length_error);
	EXPECT_EQ(calls, 2U);
	Stop stop;
	calls = 0U;
	const SelectedPropertyReadAdmission cancel = [&](size_t) { if (++calls == 2U) stop.cancelled = true; };
	EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decodeTokens(bytes, &cancel, &stop)), std::runtime_error);
	EXPECT_EQ(calls, 2U);
	EXPECT_EQ(EntityPropertyCollectionRootCodec::decodeTokens(bytes), (std::vector<uint32_t>{1U}));
}

TEST(PropertyLayoutCostBaselineTest, SparseNativeDescriptorCountsMetadataAgainstCurrentOwnerRoots) {
	using zyx::benchmark::PropertyBlockOwnerSlots;
	using zyx::benchmark::PropertyBlockFieldMembership;
	using zyx::benchmark::SparseFixedWidthPropertyColumn;
	using zyx::benchmark::SparseGroupDescriptor;
	const PropertyBlockFieldMembership::SnapshotIdentity identity{123U, 1U};
	for (const bool uniqueFields : {false, true}) {
		PropertyBlockOwnerSlots slots(11U, Node::typeId);
		for (uint64_t owner = 64U; owner != 0U; --owner) static_cast<void>(slots.insert(owner));
		std::vector<PropertyCollectionRoot> roots;
		size_t currentBytes = 0U;
		for (uint64_t owner = 1U; owner <= 64U; ++owner) {
			const uint32_t token = uniqueFields ? static_cast<uint32_t>(owner + 1000U) : 7U;
			roots.push_back({owner, Node::typeId, {{token, PropertyValue(static_cast<int64_t>(owner))}}});
			currentBytes += EntityPropertyCollectionRootCodec::encode(roots.back()).size();
		}
		const auto directory = PropertyBlockFieldMembership::build(slots, roots, 64U, identity);
		std::vector<std::vector<std::byte>> payloads;
		std::vector<SparseGroupDescriptor::Column> references;
		size_t payloadBytes = 0U;
		const uint32_t columns = uniqueFields ? 64U : 1U;
		for (uint32_t index = 0U; index < columns; ++index) {
			std::array<std::optional<PropertyValue>, 64U> values{};
			for (const auto &root : roots) {
				if (!uniqueFields || root.entries.front().key_token == index + 1001U)
					values[slots.route(root.owner_entity_id).slot] = root.entries.front().value;
			}
			payloads.push_back(SparseFixedWidthPropertyColumn::encode(values));
			payloadBytes += payloads.back().size();
			references.push_back({uniqueFields ? index + 1001U : 7U,
				{.value_id = LargeValueId(index + 1U), .generation = Generation(1U),
				 .logical_bytes = payloads.back().size(), .content_hash = graph::utils::Sha256::digest(payloads.back()),
				 .kind = LargeValueKind::LVK_BYTES, .codec = LargeValueCodec::LVC_RAW,
				 .media_type = SparseGroupDescriptor::COLUMN_MEDIA}});
		}
		std::vector<PropertyBlockFieldMembership::SparseColumnView> views;
		for (size_t index = 0U; index < payloads.size(); ++index) views.push_back({references[index].token, payloads[index]});
		directory.validateSparseColumns(slots, views, identity);
		const auto descriptor = SparseGroupDescriptor::encode(slots, directory, references, identity, 64U, 1024U * 1024U);
		const auto compactDirectory = directory.encodeSlotCounts(slots, identity, 4096U);
		EXPECT_EQ(compactDirectory.size(), 592U);
		EXPECT_EQ(LittleEndianCodec::readU32(compactDirectory, 36U), 64U);
		const auto decoded = PropertyBlockFieldMembership::decodeSlotCounts(compactDirectory, slots, identity, 64U, 4096U);
		for (const auto &root : roots) {
			const auto keys = decoded.fields(slots, slots.route(root.owner_entity_id), identity);
			ASSERT_EQ(keys.size(), 1U);
			EXPECT_EQ(keys.front(), root.entries.front().key_token);
		}
		EXPECT_THROW(static_cast<void>(directory.encodeSlotCounts(slots, identity, compactDirectory.size() - 1U)), std::length_error);
		EXPECT_THROW(static_cast<void>(directory.encodeSlotCounts(slots, {123U, 2U}, 4096U)), std::runtime_error);
		const auto prefix = uniqueFields ? std::string("unique_fields_") : std::string("shared_field_");
		RecordProperty(prefix + "current_owner_root_bytes", static_cast<int>(currentBytes));
		RecordProperty(prefix + "compact_field_directory_bytes", static_cast<int>(compactDirectory.size()));
		RecordProperty(prefix + "original_field_directory_bytes", static_cast<int>(directory.encode(4096U).size()));
		RecordProperty(prefix + "candidate_descriptor_bytes", static_cast<int>(descriptor.size()));
		RecordProperty(prefix + "candidate_payload_bytes", static_cast<int>(payloadBytes));
		RecordProperty(prefix + "candidate_total_encoded_bytes", static_cast<int>(descriptor.size() + payloadBytes));
		if (uniqueFields) EXPECT_GT(descriptor.size() + payloadBytes, currentBytes);
		EXPECT_EQ(payloadBytes, uniqueFields ? 64U * (SparseFixedWidthPropertyColumn::HEADER_BYTES + 8U)
			: SparseFixedWidthPropertyColumn::HEADER_BYTES + 64U * 8U);
	}
}
