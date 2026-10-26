#pragma once

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "graph/storage/relocatable/entity/BuildingScanMembership.hpp"
#include "graph/storage/relocatable/format/ScanBuildingImageBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeGraphWritePlanner.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRootStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipDirectory.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipSource.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldView.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"

using namespace graph::storage;

namespace graph::storage {
class BuildingScanMembershipTestAccess final {
public:
    static BuildingScanMembershipRequest create() { return BuildingScanMembershipRequest{}; }
    static BuildingScanMaintenanceRequest maintenance() { return BuildingScanMaintenanceRequest{}; }
};
class ScanBuildingImageBuilderTestAccess final {
public:
    static const std::shared_ptr<StorageIO> &io(ScanBuildingImageBuilder &builder) { return builder.target_; }
    static RelocatableMetadataStore &metadata(ScanBuildingImageBuilder &builder) { return *builder.metadata_; }
};
}

#if ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
class BuildingScanMembershipTest : public ::testing::Test {
protected:
    std::filesystem::path path;
    std::shared_ptr<std::fstream> file;
    std::shared_ptr<StorageIO> io;
    std::unique_ptr<RelocatableMetadataStore> metadata;
    std::unique_ptr<EntityPageStore> entities;
    std::unique_ptr<SlottedPagePool> pages;
    std::unique_ptr<EntityLabelSetStore> labels;

    void initialize(bool building, bool routed = false) {
        labels.reset(); pages.reset(); entities.reset(); metadata.reset(); io.reset(); file.reset();
        file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file->is_open());
        io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1);
        root.checkpoint_lsn = LogSequenceNumber(1);
        if (building) root.flags |= RSS_SUPERBLOCK_FLAG_SCAN_BUILDING;
        metadata = routed ? RelocatableMetadataStore::initializeNewComplete(io, root) :
            RelocatableMetadataStore::initializeNew(io, root);
        entities = std::make_unique<EntityPageStore>(io, *metadata);
        pages = std::make_unique<SlottedPagePool>(io, *metadata);
        labels = std::make_unique<EntityLabelSetStore>(io, *metadata, *pages);
    }
    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
            ("zyx-building-membership-" + boost::uuids::to_string(boost::uuids::random_generator()()));
        initialize(true);
    }
    void TearDown() override {
        labels.reset(); pages.reset(); entities.reset(); metadata.reset(); io.reset(); file.reset();
        std::error_code ignored; std::filesystem::remove(path, ignored);
    }
    void insert(uint64_t id, uint64_t ordinal, std::initializer_list<uint64_t> tokens) {
        NodeEntityRecord node; node.inline_label_count = static_cast<uint8_t>(tokens.size());
        std::copy(tokens.begin(), tokens.end(), node.inline_label_ids.begin());
        const EntityPageNewPoolRequest pool{SegmentId(31 + ordinal), 901 + ordinal, Generation(1),
            PageId(101 + ordinal), PhysicalSlotId(40 + ordinal), PhysicalSlotId(40 + ordinal)};
        ASSERT_TRUE(entities->allocateNode(pool, id, node, LogSequenceNumber(2 + ordinal)).has_value());
    }
    void seedPinnedNodeVersions(size_t count, MetadataSnapshotPin &oldest,
        std::vector<MetadataSnapshotPin> &history) {
        PropertyCollectionRootStore properties(*pages, *metadata);
        BTreePageStore indexes(*pages, *metadata);
        NativeGraphWritePlannerOptions options;
        options.first_slot = PhysicalSlotId(40); options.last_slot = PhysicalSlotId(127);
        options.entity_pool_owner_id = 901;
        options.property_roots = {PhysicalSlotId(40), PhysicalSlotId(127), 989};
        NativeGraphWritePlanner planner(*metadata, *entities, *labels, properties, indexes, options);
        for (size_t i = 0; i < count; ++i) {
            auto transaction = planner.beginTransaction();
            static_cast<void>(transaction.createNode({}));
            ASSERT_TRUE(transaction.commit().committed());
            auto snapshot = metadata->openSnapshot();
            if (i == 0U) oldest = std::move(snapshot);
            else history.push_back(std::move(snapshot));
        }
    }
    std::vector<std::byte> makeLegacyRowSource() {
        const auto sourceRoot = metadata->openSnapshot().superblock();
        if (sourceRoot.hybrid_property_descriptor_root.isValid()) throw std::logic_error("legacy fixture cannot contain native fields");
    std::vector<std::byte> legacy(io->fileSize());
    if (io->readAt(0U, legacy.data(), legacy.size()) != legacy.size()) throw std::runtime_error("short legacy fixture read");
    for (size_t slot = 0; slot < 2; ++slot) {
        auto header = std::span(legacy).subspan(slot * RSS_SUPERBLOCK_BYTES, RSS_SUPERBLOCK_BYTES);
        std::array<std::byte, 32> reserve{};
        std::copy_n(header.begin() + RSS_SUPERBLOCK_HEADER_BYTES, reserve.size(), reserve.begin());
        std::fill(header.begin() + 336, header.end(), std::byte{});
        std::copy(reserve.begin(), reserve.end(), header.begin() + 336);
        LittleEndianCodec::writeU16(header, 4, 31); LittleEndianCodec::writeU16(header, 6, 336);
        LittleEndianCodec::writeU32(header, 8, LittleEndianCodec::readU32(header, 8) & ~RSS_SUPERBLOCK_FLAG_SCAN_BUILDING);
        LittleEndianCodec::writeU32(header, 112, graph::utils::calculateCrc(header.data() + 336, header.size() - 336));
        LittleEndianCodec::writeU32(header, 116, 0);
        LittleEndianCodec::writeU32(header, 116, graph::utils::calculateCrc(header.data(), 336));
        static_cast<void>(ScanFormatCloneCodec::legacySuperblockToBuilding(header));
    }
    for (uint64_t offset = sourceRoot.data_region_offset; offset < legacy.size(); offset += RSS_SEGMENT_SIZE) {
        auto header = std::span(legacy).subspan(offset, RSS_ENVELOPE_BYTES);
        if (LittleEndianCodec::readU32(header, 0) != RSS_SEGMENT_MAGIC) continue;
        LittleEndianCodec::writeU16(header, 4, 5); LittleEndianCodec::writeU32(header, 52, 0);
        LittleEndianCodec::writeU32(header, 52, graph::utils::calculateCrc(header.data(), header.size()));
    }
    labels.reset(); pages.reset(); entities.reset(); metadata.reset(); io.reset(); file.reset();
    { std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char *>(legacy.data()), static_cast<std::streamsize>(legacy.size()));
      if (!output.good()) throw std::runtime_error("cannot write legacy fixture"); }
        return legacy;
    }
    BuildingScanMembershipRequest request(uint64_t group) {
        auto pin = metadata->openSnapshot(); const auto &root = pin.superblock();
        auto result = BuildingScanMembershipTestAccess::create();
        result.expected_database_id = root.database_id; result.expected_generation = root.generation;
        result.expected_descriptor = root.hybrid_property_descriptor_root; result.group_id = group;
        result.first_slot = PhysicalSlotId(70); result.last_slot = PhysicalSlotId(127);
        result.field_pool_owner_id = 989;
        result.memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        return result;
    }
    Superblock install(const BuildingScanMembershipRequest &input, const MetadataPublishOptions &options = {}) {
        const auto root = metadata->openSnapshot().superblock();
        return metadata->installBuildingScanMembershipGroup(input, LogSequenceNumber(root.checkpoint_lsn.value() + 1), options);
    }
    HybridPropertyDescriptor descriptor(RelocatableMetadataStore &store, const MetadataSnapshotPin &pin) {
        static_cast<void>(store);
        MetadataPageFile pages(io, pin.superblock());
        return pages.readHybridPropertyDescriptor(pin.superblock().hybrid_property_descriptor_root, pin.superblock()).descriptor;
    }
    std::optional<ScanBlockMembership> read(RelocatableMetadataStore &store, const MetadataSnapshotPin &pin,
        const HybridPropertyDescriptor &desc, uint64_t group) {
        SlottedPagePool pages(io, store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, store);
        ScanMembershipDirectoryMap map(hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN), trees, store,
            ScanMembershipDirectoryCodec::bind(desc, pin));
        const auto encoded = map.find(pin, ScanMembershipDirectoryCodec::key(group));
        if (!encoded) return std::nullopt;
        const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*encoded);
        HybridPropertyFieldPageStore physical(io, store, pin);
        const auto page = physical.read(pin, slot.page_id);
        return page.view().readScanMembership(slot, pin.superblock().database_id, group, desc.semantic_generation);
    }
    HybridPropertyDirectoryIntegrity::Report verify(const MetadataSnapshotPin &pin) {
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        SlottedPagePool source(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        HybridPropertyReadContext context(io, source, *metadata, pin);
        return HybridPropertyDirectoryIntegrity::verify(context, pin, memory);
    }
    PageId replaceValidMembershipBytes(const MetadataSnapshotPin &pin, uint64_t group,
        const std::function<void(ScanBlockMembership &)> &change) {
        const auto desc = descriptor(*metadata, pin);
        SlottedPagePool source(io, *metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(source, *metadata);
        const auto binding = ScanMembershipDirectoryCodec::bind(desc, pin);
        ScanMembershipDirectoryMap directory(binding.root.index_id, trees, *metadata, binding);
        const auto value = directory.find(pin, ScanMembershipDirectoryCodec::key(group));
        if (!value) throw std::runtime_error("fixture membership missing");
        const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*value);
        const auto location = metadata->findPageLocation(pin, slot.page_id);
        const auto pool = metadata->findCatalog(pin, location.segment_id);
        const auto offset = PhysicalSegmentAllocator::checkedSlotOffset(pin.superblock().data_region_offset, pool.physical_slot) +
            uint64_t(location.page_index) * RSS_PAGE_SIZE;
        std::array<std::byte, RSS_PAGE_SIZE> frame{};
        if (io->readAt(offset, frame.data(), frame.size()) != frame.size()) throw std::runtime_error("short fixture frame");
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
        auto image = HybridPropertyPageImage::open(std::span(frame).subspan(RSS_PAGE_OWNER_HEADER_BYTES), slot.page_id,
            location.generation, pin.superblock().generation, memory);
        auto record = image.readScanMembership(slot, pin.superblock().database_id, group, desc.semantic_generation);
        change(record);
        if (record.group_id == group) image.replaceScanMembership(slot, record, desc.semantic_generation);
        else {
            // Deliberately change the authenticated body's group identity while
            // retaining the directory locator: checksum success must not hide it.
            auto page = SlottedPage::open(PageOwnerHeaderCodec::decode(std::span(frame).first(RSS_PAGE_OWNER_HEADER_BYTES)),
                std::span(frame).subspan(RSS_PAGE_OWNER_HEADER_BYTES), memory);
            page.overwrite(slot, record.encode());
            const auto bytes = page.serializeOwned(memory);
            std::copy(bytes.begin(), bytes.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
            io->writeAt(offset, frame.data(), frame.size()); io->sync(); return slot.page_id;
        }
        const auto bytes = image.serialize();
        std::copy(bytes.begin(), bytes.end(), frame.begin() + RSS_PAGE_OWNER_HEADER_BYTES);
        io->writeAt(offset, frame.data(), frame.size()); io->sync(); return slot.page_id;
    }
};

#endif
