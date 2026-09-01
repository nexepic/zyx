/**
 * @file test_RelocatableMetadataStoreGraphMutation_PropertyBatchValidation.cpp
 * @brief Property page batch input contracts for graph metadata publication.
 */

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <functional>
#include <vector>

namespace {

const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

void noOpGraphMaterializer(std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {}

} // namespace

TEST_F(RelocatableMetadataStoreTest, RejectsMalformedFreshPropertyPageBatchesBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	const auto reject = [&](const std::function<void(NativeGraphPropertyPageBatchRequest &)> &forge,
						   FormatError expected = FormatError::FERR_INVALID_HEADER) {
		NativeGraphPropertyPageBatchRequest request;
		request.kind = NativeGraphPropertyPageBatchKind::NGPBK_CREATE_POOL;
		request.page_owner_id = 1U;
		request.first_slot = PhysicalSlotId(40U);
		request.last_slot = PhysicalSlotId(40U);
		request.page_property_keys = {{{EntityPropertyOwnerKind::EPOK_NODE, 1U}}};
		forge(request);
		const std::array<NativeGraphPropertyPageBatchRequest, 1U> batches{{request}};
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, batches,
							noLargeValues, {}, LogSequenceNumber(2U), noOpGraphMaterializer, {}));
				},
				expected);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		unchanged.release();
	};

	reject([](auto &request) { request.kind = static_cast<NativeGraphPropertyPageBatchKind>(255U); });
	reject([](auto &request) { request.page_owner_id = 0U; });
	reject([](auto &request) { request.page_property_keys.clear(); });
	reject([](auto &request) { request.page_property_keys = {{}}; });
	reject([](auto &request) { request.page_property_keys.front().front() = {}; });
	reject([](auto &request) {
		request.page_property_keys.front().push_back({EntityPropertyOwnerKind::EPOK_NODE, 1U});
	},
		   FormatError::FERR_INVALID_STATE);
	reject([](auto &request) { request.expected_pool = entry(40U, 1U, 1U); });
	reject([](auto &request) { request.first_slot = {}; });
	reject([](auto &request) { request.last_slot = {}; });
	reject([](auto &request) {
		request.first_slot = PhysicalSlotId(41U);
		request.last_slot = PhysicalSlotId(40U);
	});
}
