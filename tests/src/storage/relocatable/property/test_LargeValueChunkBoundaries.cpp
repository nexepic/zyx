#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/PropertyRowChunkPolicy.hpp"
#include <random>

namespace {
LargeValueWriteOptions chunkOptions() {
	LargeValueWriteOptions options; options.first_data_slot = PhysicalSlotId(40U); options.last_data_slot = PhysicalSlotId(80U);
	options.first_manifest_slot = PhysicalSlotId(81U); options.last_manifest_slot = PhysicalSlotId(90U);
	options.kind = LargeValueKind::LVK_BYTES; options.media_type = "application/test-chunks"; return options;
}
}

TEST_F(RelocatableMetadataStoreTest, LargeValueExplicitChunkEndsKeepCanonicalBytesRangesAndReopenIdentity) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U)); LargeValueStore store(io, *metadata);
	std::vector<std::byte> bytes(65600U); for (size_t index = 0U; index < bytes.size(); ++index) bytes[index] = static_cast<std::byte>(index % 251U);
	auto options = chunkOptions(); options.compression.logical_chunk_ends = {32U, 65536U, 65591U};
	const auto written = store.write(bytes, options, LogSequenceNumber(2U));
	ASSERT_EQ(written.manifest.chunks.size(), 5U);
	const std::array<uint64_t, 5U> starts{0U,32U,32800U,65536U,65591U};
	for (size_t index = 0U; index < starts.size(); ++index) EXPECT_EQ(written.manifest.chunks[index].uncompressed_offset, starts[index]);
	auto pin = metadata->openSnapshot(); EXPECT_EQ(store.read(pin, written.reference), bytes);
	for (const auto start : {0U, 25U, 32760U, 65530U, 65590U}) {
		const auto range = store.readRange(pin, written.reference, start, 10U);
		EXPECT_EQ(range, std::vector<std::byte>(bytes.begin()+start, bytes.begin()+start+10U));
	}
	pin.release(); metadata.reset(); auto reopened = RelocatableMetadataStore::open(io); LargeValueStore restored(io, *reopened);
	auto current = reopened->openSnapshot(); EXPECT_EQ(restored.read(current, written.reference), bytes);
}

TEST_F(RelocatableMetadataStoreTest, LargeValueChunkBoundaryValidationRejectsMalformedHintsBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U)); LargeValueStore store(io, *metadata);
	const std::vector<std::byte> bytes(128U); const auto before = metadata->openSnapshot().superblock();
	for (const auto &ends : std::vector<std::vector<uint64_t>>{{0U},{129U},{32U,31U},{32U,32U}}) {
		auto options = chunkOptions(); options.compression.logical_chunk_ends = ends;
		EXPECT_THROW(static_cast<void>(store.write(bytes, options, LogSequenceNumber(2U))), FormatException);
		EXPECT_EQ(metadata->openSnapshot().superblock(), before);
	}
	auto options = chunkOptions(); options.compression.logical_chunk_ends.resize(4097U);
	EXPECT_THROW(static_cast<void>(store.write(bytes, options, LogSequenceNumber(2U))), FormatException);
	EXPECT_EQ(metadata->openSnapshot().superblock(), before);
}

TEST(PropertyRowChunkPolicyTest, CostAdmissionKeepsSmallAndSingletonFieldsInRowModeAndAcceptsLargePayloadGroups) {
	std::vector<PropertyCollectionRootEntry> roots{{1U, graph::PropertyValue(std::string(1024U*1024U, 'x'))}, {2U, graph::PropertyValue(int64_t(7))}, {3U, graph::PropertyValue(true)}};
	const auto payload = EntityPropertyLargeValuePayloadCodec::encode(roots); LargeValueCompressionOptions base;
	const auto candidate = PropertyRowChunkPolicy::select(payload, base, true);
	ASSERT_EQ(candidate.logical_chunk_ends.size(), 2U);
	EXPECT_GT(candidate.logical_chunk_ends[0], RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES);
	EXPECT_LT(candidate.logical_chunk_ends[0], RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES);
	EXPECT_LT(candidate.logical_chunk_ends[1], payload.size());
	EXPECT_TRUE(PropertyRowChunkPolicy::select(payload, base, false).logical_chunk_ends.empty());
	roots[0].value = graph::PropertyValue(std::string(16384U, 'x'));
	EXPECT_TRUE(PropertyRowChunkPolicy::select(EntityPropertyLargeValuePayloadCodec::encode(roots), base, true).logical_chunk_ends.empty());
	base.codec = LargeValueCodec::LVC_LZ4;
	EXPECT_TRUE(PropertyRowChunkPolicy::select(payload, base, true).logical_chunk_ends.empty());
}

TEST(PropertyRowChunkPolicyTest, CompressionRetainsExplicitPoliciesAndRequiresRawFallback) {
	const auto payload = EntityPropertyLargeValuePayloadCodec::encode(
		std::vector<PropertyCollectionRootEntry>{{1U, graph::PropertyValue(std::string(256U * 1024U, 'x'))},
			{2U, graph::PropertyValue(7)}});
	LargeValueCompressionOptions base;
	EXPECT_EQ(PropertyRowChunkPolicy::select(payload, base, false), base);
	base.allow_raw_fallback = false;
	const auto strictRaw = PropertyRowChunkPolicy::select(payload, base, true);
	EXPECT_EQ(strictRaw.codec, LargeValueCodec::LVC_RAW);
	EXPECT_FALSE(strictRaw.allow_raw_fallback);
	base.codec = LargeValueCodec::LVC_ZSTD;
	EXPECT_EQ(PropertyRowChunkPolicy::select(payload, base, true), base);
	base = {};
	base.logical_chunk_ends = {32U};
	EXPECT_EQ(PropertyRowChunkPolicy::select(payload, base, true), base);
}

TEST_F(RelocatableMetadataStoreTest, AdmittedRowCompressionReducesActualPagesAndPreservesCanonicalValues) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<PropertyCollectionRootEntry> roots{
		{1U, graph::PropertyValue(std::string(256U * 1024U, 'x'))}, {2U, graph::PropertyValue(7)}};
	const auto payload = EntityPropertyLargeValuePayloadCodec::encode(roots);
	auto options = chunkOptions();
	options.compression = PropertyRowChunkPolicy::select(payload, {}, true);
	ASSERT_FALSE(options.compression.logical_chunk_ends.empty());
	auto raw = options.compression;
	raw.codec = LargeValueCodec::LVC_RAW;
	const auto rawPages = store.plannedDataPageCount(payload, raw);
	const auto written = store.write(payload, options, LogSequenceNumber(2U));
	auto pin = metadata->openSnapshot();
	size_t actualPages = 0U;
	for (const auto &manifest : store.manifestTree(pin, written.manifest)) {
		for (const auto &chunk : manifest.chunks) actualPages += chunk.data_page_count;
	}
#if ZYX_HAS_LZ4
	EXPECT_EQ(options.compression.codec, LargeValueCodec::LVC_LZ4);
	EXPECT_LT(actualPages, rawPages);
#else
	EXPECT_EQ(actualPages, rawPages);
#endif
	EXPECT_EQ(store.read(pin, written.reference), payload);
	EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::decode(store.read(pin, written.reference)), roots);
	for (const size_t offset : {size_t{0}, size_t{32760}, payload.size() - 16U}) {
		EXPECT_EQ(store.readRange(pin, written.reference, offset, 16U),
			std::vector<std::byte>(payload.begin() + offset, payload.begin() + offset + 16U));
	}
}

TEST_F(RelocatableMetadataStoreTest, IncompressibleRowChunksFallBackWithoutGrowingTheRawPageBound) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	std::mt19937 random(314159U);
	std::string body(256U * 1024U, ' ');
	for (auto &byte : body) byte = static_cast<char>(32U + random() % 95U);
	const auto payload = EntityPropertyLargeValuePayloadCodec::encode(
		std::vector<PropertyCollectionRootEntry>{{1U, graph::PropertyValue(body)}, {2U, graph::PropertyValue(7)}});
	auto options = chunkOptions();
	options.compression = PropertyRowChunkPolicy::select(payload, {}, true);
	auto raw = options.compression;
	raw.codec = LargeValueCodec::LVC_RAW;
	const auto rawPages = store.plannedDataPageCount(payload, raw);
	const auto written = store.write(payload, options, LogSequenceNumber(2U));
	auto pin = metadata->openSnapshot();
	size_t actualPages = 0U;
	bool rawBodyChunk = false;
	for (const auto &manifest : store.manifestTree(pin, written.manifest)) {
		for (const auto &chunk : manifest.chunks) {
			actualPages += chunk.data_page_count;
			rawBodyChunk |= chunk.codec == LargeValueCodec::LVC_RAW &&
				chunk.uncompressed_bytes == RSS_LARGE_VALUE_EXTENT_32K_BYTES;
		}
	}
	EXPECT_TRUE(rawBodyChunk);
	EXPECT_LE(actualPages, rawPages);
	EXPECT_EQ(store.read(pin, written.reference), payload);
}

TEST_F(RelocatableMetadataStoreTest, StreamedChunkBoundaryMaterializationUsesActualOffsetsAcrossManifestTreeLevels) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U)); LargeValueStore store(io, *metadata);
	const size_t chunks = static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U;
	const std::vector<std::byte> bytes(chunks * RSS_LARGE_VALUE_EXTENT_16K_BYTES + 19U);
	LargeValueCompressionOptions compression; compression.logical_chunk_ends = {31U, bytes.size()-11U};
	const auto reference = store.planReference(LargeValueId(1U),Generation(1U),bytes,LargeValueKind::LVK_BYTES,"application/test-chunks",compression,LargeValueExtentClass::LVEC_16K);
	const auto dataPages = store.plannedDataPageCount(bytes,compression,LargeValueExtentClass::LVEC_16K);
	const auto manifestPages = store.plannedManifestPageCount(bytes,compression,LargeValueExtentClass::LVEC_16K);
	ASSERT_GT(manifestPages,1U);
	std::vector<LargeValueManifest> manifests; size_t dataCount = 0U;
	store.materializeStreamedValue({reference,bytes,compression,LargeValueExtentClass::LVEC_16K,PageId(101U),dataPages,PageId(10000U),manifestPages},LogSequenceNumber(2U),
		[](PageId page,PageOwnerKind kind){return PageOwnerHeader{page,1U,kind,0U,RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,Generation(1U),LogSequenceNumber(2U)};},
		[&](const PageOwnerHeader &owner,std::vector<std::byte> payload,PageOwnerKind kind){
			if(kind==PageOwnerKind::POK_LARGE_VALUE_MANIFEST) manifests.push_back(LargeValueManifestPage::open(owner,payload).manifest());
			else ++dataCount;
		});
	EXPECT_EQ(dataCount,dataPages); EXPECT_EQ(manifests.size(),manifestPages);
	std::vector<LargeValueChunk> actual;
	for(const auto &manifest:manifests) actual.insert(actual.end(),manifest.chunks.begin(),manifest.chunks.end());
	std::ranges::sort(actual,{},&LargeValueChunk::ordinal); ASSERT_GT(actual.size(),chunks);
	uint64_t offset=0U;
	for(size_t ordinal=0U;ordinal<actual.size();++ordinal){EXPECT_EQ(actual[ordinal].ordinal,ordinal);EXPECT_EQ(actual[ordinal].uncompressed_offset,offset);offset+=actual[ordinal].uncompressed_bytes;}
	EXPECT_EQ(actual.front().uncompressed_bytes,31U); EXPECT_EQ(actual.back().uncompressed_bytes,11U); EXPECT_EQ(offset,bytes.size());
}

TEST_F(RelocatableMetadataStoreTest, RowChunkDataRelocationKeepsImmutableReferencesAndOldSnapshotReadsValid) {
	auto metadata = RelocatableMetadataStore::initializeNew(io,superblock(1U,1U)); LargeValueStore store(io,*metadata);
	const auto payload = EntityPropertyLargeValuePayloadCodec::encode(std::vector<PropertyCollectionRootEntry>{{1U,graph::PropertyValue(std::string(256U*1024U,'x'))},{2U,graph::PropertyValue(7)}});
	auto options = chunkOptions(); options.compression = PropertyRowChunkPolicy::select(payload,{},true);
	ASSERT_FALSE(options.compression.logical_chunk_ends.empty()); const auto written = store.write(payload,options,LogSequenceNumber(2U));
	const PageId moved = written.manifest.chunks.front().first_data_page_id;
	SharedPagePoolAllocator pools(io,*metadata); SharedPagePoolAllocationRequest destination;
	destination.segment_id=SegmentId(71U);destination.pool_kind=SegmentKind::SGK_LARGE_VALUE_DATA_POOL;
	destination.pool_owner_id=RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID;destination.generation=Generation(2U);
	destination.page_id=PageId(501U);destination.page_owner_id=written.reference.value_id.value();destination.page_owner_kind=PageOwnerKind::POK_LARGE_VALUE_DATA;
	const PageOwnerHeader owner{PageId(501U),written.reference.value_id.value(),PageOwnerKind::POK_LARGE_VALUE_DATA,0U,RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,Generation(2U),LogSequenceNumber(3U)};
	const LargeValueDataPage spare{written.reference.value_id,PageId(501U),written.reference.generation,0U,0U,1U,1U,0U,{std::byte{0xE1U}}};
	destination.payload=LargeValueDataFrame::create(owner,spare).serialize();
	ASSERT_TRUE(pools.allocateNewPoolPage(destination,PhysicalSlotId(100U),PhysicalSlotId(100U),LogSequenceNumber(3U)).has_value());
	auto old=metadata->openSnapshot();const auto oldLocation=metadata->findPageLocation(old,moved);
	const auto relocation=store.evacuatePage(moved,SegmentId(71U),LogSequenceNumber(4U));
	EXPECT_NE(relocation.location.segment_id,oldLocation.segment_id);EXPECT_EQ(store.read(old,written.reference),payload);
	auto current=metadata->openSnapshot();EXPECT_EQ(store.read(current,written.reference),payload);
	EXPECT_EQ(store.manifest(current,written.reference).generation,written.reference.generation);
	const auto rank=EntityPropertyLargeValuePayloadCodec::decode(store.read(current,written.reference));
	ASSERT_EQ(rank.size(),2U);EXPECT_EQ(rank[1].value,graph::PropertyValue(7));
}
