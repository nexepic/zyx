#include <gtest/gtest.h>
#include <bit>
#include <filesystem>
#include <fstream>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include "graph/storage/relocatable/entity/ScanMembershipSource.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipMutation.hpp"

using namespace graph::storage;
using namespace graph::storage::scan_candidate_detail;

class ScanMembershipSourceTest : public ::testing::Test {
protected:
    std::filesystem::path path;
    std::shared_ptr<std::fstream> file;
    std::shared_ptr<StorageIO> io;
    std::unique_ptr<RelocatableMetadataStore> metadata;
    std::unique_ptr<EntityPageStore> entities;
    std::unique_ptr<SlottedPagePool> pages;
    std::unique_ptr<EntityLabelSetStore> labels;

    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
            ("zyx-membership-source-" + boost::uuids::to_string(boost::uuids::random_generator()()));
        file = std::make_shared<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file->is_open());
        io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
        Superblock root; root.database_id = DatabaseId(71); root.generation = Generation(1);
        root.checkpoint_lsn = LogSequenceNumber(1);
        metadata = RelocatableMetadataStore::initializeNew(io, root);
        entities = std::make_unique<EntityPageStore>(io, *metadata);
        pages = std::make_unique<SlottedPagePool>(io, *metadata);
        labels = std::make_unique<EntityLabelSetStore>(io, *metadata, *pages);
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
};

TEST_F(ScanMembershipSourceTest, ReadsCompletePrimaryMembershipIncludingPropertylessAndUnlabeledNodes) {
    insert(1, 0, {7, 11}); insert(64, 1, {}); insert(65, 2, {7});
    auto snapshot = metadata->openSnapshot();
    const auto generation = snapshot.superblock().generation;
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    std::vector<ScanBlockMembership> groups;
    const auto counts = streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [&](const auto &group) { groups.push_back(group); });
    ASSERT_EQ(groups.size(), 2U); EXPECT_EQ(counts.nodes, 3U); EXPECT_EQ(counts.groups, 2U);
    EXPECT_EQ(groups[0].database_id, DatabaseId(71)); EXPECT_EQ(groups[0].semantic_generation, generation);
    EXPECT_EQ(groups[0].group_id, 1U); EXPECT_EQ(groups[0].live, 1U | (UINT64_C(1) << 63));
    EXPECT_EQ(groups[0].label_count, 2U); EXPECT_EQ(groups[0].labels[0].members, 1U);
    EXPECT_EQ(groups[0].select({}), groups[0].live);
    const std::array<uint64_t, 2> both{7, 11}; EXPECT_EQ(groups[0].select(both), 1U);
    EXPECT_EQ(groups[1].group_id, 2U); EXPECT_EQ(groups[1].live, 1U);
    EXPECT_EQ(metadata->openSnapshot().superblock().generation, generation);
    snapshot.release(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, CompleteOversizedLabelUnionUsesFallbackAndPreservesOldPin) {
    insert(1, 0, {1, 2, 3});
    auto snapshot = metadata->openSnapshot();
    auto old = metadata->openSnapshot();
    std::vector<uint64_t> actual;
    for (uint64_t token = 1; token <= 65; ++token) actual.push_back(token);
    EntityLabelSetWritePlan plan; plan.append_page = EntityLabelSetAppendRequest{SegmentId(31), PageId(102)};
    static_cast<void>(labels->updateNodeLabels(snapshot, 1, actual, plan, LogSequenceNumber(3)));
    snapshot.release(); snapshot = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    size_t emitted = 0;
    static_cast<void>(streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [&](const auto &group) {
            ++emitted; EXPECT_TRUE(group.labels_from_primary); EXPECT_EQ(group.label_count, 0U);
            EXPECT_EQ(group.live, 1U); EXPECT_EQ(group.wireSize(), ScanBlockMembership::HEADER_BYTES);
        }));
    EXPECT_EQ(emitted, 1U);
    EXPECT_EQ(labels->readNodeLabels(snapshot, entities->readNode(snapshot, 1)), actual);
    size_t oldEmitted = 0;
    const auto oldCounts = streamPrimaryMembership(*metadata, *entities, *labels, old, memory, [] {},
        [&](const auto &group) { ++oldEmitted; EXPECT_EQ(group.label_count, 3U); });
    EXPECT_EQ(oldEmitted, 1U); EXPECT_EQ(oldCounts.nodes, 1U);
    old.release();
    snapshot.release(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, LabelUnionAcrossDifferentEntitiesUsesExplicitPrimaryFallback) {
    for (uint64_t i = 0; i < 22; ++i) insert(i + 1, i, {3 * i + 1, 3 * i + 2, 3 * i + 3});
    auto snapshot = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    size_t emitted = 0;
    static_cast<void>(streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [&](const auto &group) {
            ++emitted; EXPECT_TRUE(group.labels_from_primary); EXPECT_EQ(group.label_count, 0U);
            EXPECT_EQ(std::popcount(group.live), 22);
        }));
    EXPECT_EQ(emitted, 1U);
    snapshot.release(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, EmptyInputDoesNotInventAMembershipGroup) {
    auto snapshot = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const auto counts = streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [](const auto &) { FAIL() << "empty primary stream emitted a group"; });
    EXPECT_EQ(counts.nodes, 0U); EXPECT_EQ(counts.groups, 0U);
    snapshot.release(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, CancellationAndConsumerFailureNeverReplayAnEmittedPrefix) {
    insert(1, 0, {}); insert(65, 1, {7});
    auto snapshot = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    size_t emitted = 0;
    EXPECT_THROW(static_cast<void>(streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory,
        [&] { if (emitted) throw std::runtime_error("cancelled"); },
        [&](const auto &) { ++emitted; })), std::runtime_error);
    EXPECT_EQ(emitted, 1U);
    emitted = 0;
    EXPECT_THROW(static_cast<void>(streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [&](const auto &) { ++emitted; throw std::runtime_error("consumer"); })), std::runtime_error);
    EXPECT_EQ(emitted, 1U);
    snapshot.release(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, WorkspaceDenialPrecedesSourceDelivery) {
    insert(1, 0, {7});
    auto snapshot = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1);
    size_t emitted = 0;
    EXPECT_THROW(static_cast<void>(streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [&](const auto &) { ++emitted; })), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(emitted, 0U); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, IdenticalDatabaseAndGenerationDoNotAuthorizeAForeignPin) {
    insert(1, 0, {7});
    auto other = RelocatableMetadataStore::openReadOnly(io);
    auto foreign = other->openSnapshot();
    auto own = metadata->openSnapshot();
    ASSERT_EQ(foreign.superblock().database_id, own.superblock().database_id);
    ASSERT_EQ(foreign.superblock().generation, own.superblock().generation);
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1);
    size_t emitted = 0;
    EXPECT_THROW(static_cast<void>(streamPrimaryMembership(*metadata, *entities, *labels, foreign, memory, [] {},
        [&](const auto &) { ++emitted; })), std::invalid_argument);
    EXPECT_EQ(emitted, 0U); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, ReleasingThePinInAConsumerRefusesFurtherDelivery) {
    insert(1, 0, {}); insert(65, 1, {7});
    auto snapshot = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    size_t emitted = 0;
    EXPECT_THROW(static_cast<void>(streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [&](const auto &) { ++emitted; snapshot.release(); })), std::invalid_argument);
    EXPECT_EQ(emitted, 1U); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, LogicalGroupsRemainOrderedWhenPhysicalPageOwnersAreNot) {
    insert(65, 0, {11}); insert(1, 1, {7}); insert(64, 2, {});
    auto snapshot = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    std::vector<ScanBlockMembership> actual;
    const auto counts = streamPrimaryMembership(*metadata, *entities, *labels, snapshot, memory, [] {},
        [&](const auto &group) { actual.push_back(group); });
    ASSERT_EQ(actual.size(), 2U); EXPECT_EQ(counts.nodes, 3U);
    EXPECT_EQ(actual[0].group_id, 1U); EXPECT_EQ(actual[0].live, 1U | (UINT64_C(1) << 63));
    EXPECT_EQ(actual[0].labels[0].token, 7U); EXPECT_EQ(actual[0].labels[0].members, 1U);
    EXPECT_EQ(actual[1].group_id, 2U); EXPECT_EQ(actual[1].live, 1U);
    EXPECT_EQ(actual[1].labels[0].token, 11U);
    snapshot.release(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, PreparesCreateDeleteAndRelabelTogetherWithoutChangingTheSource) {
    insert(1, 0, {7}); insert(64, 1, {}); insert(65, 2, {99});
    auto pin = metadata->openSnapshot(); const auto base = pin.superblock();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const std::array<uint64_t, 2> relabel{11, 13}; const std::array<uint64_t, 1> created{11};
    const std::array<ScanNodeChange, 3> changes{{
        {1, ScanNodeChangeKind::SNCK_RELABEL, relabel},
        {2, ScanNodeChangeKind::SNCK_CREATE, created},
        {64, ScanNodeChangeKind::SNCK_DELETE, {}}}};
    io->resetStats();
    const auto plan = ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 1,
        Generation(base.generation.value() + 1U), changes, memory);
    ASSERT_TRUE(plan.before); ASSERT_TRUE(plan.after);
    EXPECT_EQ(plan.before->live, UINT64_C(1) | (UINT64_C(1) << 63));
    EXPECT_EQ(plan.before->select(std::array<uint64_t, 1>{7}), 1U);
    EXPECT_EQ(plan.after->live, 3U); EXPECT_EQ(plan.after->select(created), 3U);
    EXPECT_EQ(plan.after->select(relabel), 1U);
    EXPECT_EQ(plan.after->semantic_generation, Generation(base.generation.value() + 1U));
    EXPECT_EQ(plan.live_node_delta, 0); EXPECT_EQ(plan.group_delta, 0);
    EXPECT_EQ(metadata->openSnapshot().superblock(), base); EXPECT_EQ(io->stats().write_calls, 0U);
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, MutationPreparationRepresentsEmptyAndFirstLiveGroupsWithExactCounts) {
    insert(1, 0, {}); insert(64, 1, {7}); auto pin = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const auto next = Generation(pin.superblock().generation.value() + 1U);
    const std::array<ScanNodeChange, 2> erase{{{1, ScanNodeChangeKind::SNCK_DELETE, {}}, {64, ScanNodeChangeKind::SNCK_DELETE, {}}}};
    const auto removed = ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 1, next, erase, memory);
    ASSERT_TRUE(removed.before); EXPECT_FALSE(removed.after);
    EXPECT_EQ(removed.live_node_delta, -2); EXPECT_EQ(removed.group_delta, -1);
    const std::array<ScanNodeChange, 1> create{{{129, ScanNodeChangeKind::SNCK_CREATE, {}}}};
    const auto added = ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 3, next, create, memory);
    EXPECT_FALSE(added.before); ASSERT_TRUE(added.after); EXPECT_EQ(added.after->live, 1U);
    EXPECT_EQ(added.live_node_delta, 1); EXPECT_EQ(added.group_delta, 1); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, MutationPreparationTransitionsBothWaysAcrossTheLabelFallbackBoundary) {
    insert(1, 0, {7}); auto old = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    std::vector<uint64_t> many; for (uint64_t i = 1; i <= 65; ++i) many.push_back(i);
    const std::array<ScanNodeChange, 1> wide{{{1, ScanNodeChangeKind::SNCK_RELABEL, many}}};
    const auto first = ScanMembershipMutation::prepare(*metadata, *entities, *labels, old, 1,
        Generation(old.superblock().generation.value() + 1U), wide, memory);
    ASSERT_TRUE(first.before); ASSERT_TRUE(first.after);
    EXPECT_FALSE(first.before->labels_from_primary); EXPECT_TRUE(first.after->labels_from_primary);
    EXPECT_EQ(first.after->label_count, 0U);
    EntityLabelSetWritePlan write; write.append_page = EntityLabelSetAppendRequest{SegmentId(31), PageId(102)};
    static_cast<void>(labels->updateNodeLabels(old, 1, many, write,
        LogSequenceNumber(old.superblock().checkpoint_lsn.value() + 1U)));
    auto current = metadata->openSnapshot();
    const std::array<uint64_t, 1> single{11};
    const std::array<ScanNodeChange, 1> narrow{{{1, ScanNodeChangeKind::SNCK_RELABEL, single}}};
    const auto second = ScanMembershipMutation::prepare(*metadata, *entities, *labels, current, 1,
        Generation(current.superblock().generation.value() + 1U), narrow, memory);
    ASSERT_TRUE(second.before); ASSERT_TRUE(second.after);
    EXPECT_TRUE(second.before->labels_from_primary); EXPECT_FALSE(second.after->labels_from_primary);
    EXPECT_EQ(second.after->select(single), 1U); EXPECT_EQ(second.live_node_delta, 0);
    const auto retained = ScanMembershipMutation::prepare(*metadata, *entities, *labels, old, 1,
        Generation(old.superblock().generation.value() + 1U), narrow, memory);
    ASSERT_TRUE(retained.before); EXPECT_FALSE(retained.before->labels_from_primary);
    EXPECT_EQ(retained.before->select(std::array<uint64_t, 1>{7}), 1U); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, MutationPreparationRejectsBadInputsAndWorkspaceBeforeIo) {
    insert(1, 0, {7}); auto pin = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const auto next = Generation(pin.superblock().generation.value() + 1U);
    const std::array<ScanNodeChange, 1> valid{{{1, ScanNodeChangeKind::SNCK_DELETE, {}}}};
    const std::array<ScanNodeChange, 2> duplicate{{valid[0], valid[0]}};
    io->resetStats();
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 1,
        next, duplicate, memory)), std::invalid_argument);
    const std::array<uint64_t, 2> unsorted{9, 7};
    const std::array<ScanNodeChange, 1> badLabels{{{1, ScanNodeChangeKind::SNCK_RELABEL, unsorted}}};
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 1,
        next, badLabels, memory)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 2,
        next, valid, memory)), std::invalid_argument);
    auto tiny = std::make_shared<graph::query::execution::MaterializedByteBudget>(1U);
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 1,
        next, valid, tiny)), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(tiny->used(), 0U);
    const std::array<ScanNodeChange, 1> existing{{{1, ScanNodeChangeKind::SNCK_CREATE, {}}}};
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 1,
        next, existing, memory)), FormatException);
    const std::array<ScanNodeChange, 1> absent{{{2, ScanNodeChangeKind::SNCK_DELETE, {}}}};
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, 1,
        next, absent, memory)), FormatException);
    EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(ScanMembershipSourceTest, MutationPreparationPreservesTerminalIdAndRefundsCancellation) {
    insert(UINT64_MAX, 0, {7}); auto pin = metadata->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U * 1024U);
    const auto next = Generation(pin.superblock().generation.value() + 1U);
    const std::array<uint64_t, 1> target{11};
    const std::array<ScanNodeChange, 1> changes{{{UINT64_MAX, ScanNodeChangeKind::SNCK_RELABEL, target}}};
    const auto group = ScanMembershipDirectoryCodec::MAX_GROUP;
    const auto result = ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, group, next, changes, memory);
    ASSERT_TRUE(result.before); ASSERT_TRUE(result.after);
    EXPECT_EQ(result.after->live, UINT64_C(1) << 62); EXPECT_EQ(result.after->select(target), UINT64_C(1) << 62);
    struct Cancel final : CancellationCheckpoint {
        mutable size_t calls = 0; size_t at = 1;
        void check() const override { if (++calls == at) throw std::runtime_error("cancel scan mutation"); }
    } cancel;
    io->resetStats();
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, group,
        next, changes, memory, &cancel)), std::runtime_error);
    EXPECT_EQ(io->stats().read_calls, 0U); EXPECT_EQ(memory->used(), 0U);
    cancel.calls = 0; cancel.at = 2;
    EXPECT_THROW(static_cast<void>(ScanMembershipMutation::prepare(*metadata, *entities, *labels, pin, group,
        next, changes, memory, &cancel)), std::runtime_error);
    EXPECT_GT(io->stats().read_calls, 0U); EXPECT_EQ(io->stats().write_calls, 0U); EXPECT_EQ(memory->used(), 0U);
}
