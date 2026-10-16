#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"

using namespace graph::storage;

class MetadataPageReadContextTest : public ::testing::Test {
protected:
	std::filesystem::path path;
	std::shared_ptr<std::fstream> stream;
	std::shared_ptr<StorageIO> io;
	Superblock root;
	void SetUp() override {
		path = std::filesystem::temp_directory_path() /
			("zyx_metadata_context_" + boost::uuids::to_string(boost::uuids::random_generator()()) + ".db");
		stream = std::make_shared<std::fstream>(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
		ASSERT_TRUE(stream->is_open());
		io = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		root.database_id = DatabaseId(123U);
		root.generation = Generation(1U);
		root.checkpoint_lsn = LogSequenceNumber(1U);
	}
	void TearDown() override {
		graph::debug::PerfTrace::setEnabled(false);
		graph::debug::PerfTrace::setValueCollectionEnabled(false);
		graph::debug::PerfTrace::reset();
		io.reset(); stream->close(); stream.reset();
		std::filesystem::remove(path);
	}
	PhysicalPageRef writeRadix(MetadataPageFile &pages, uint8_t frame, uint64_t generation = 1U) {
		PhysicalPageRef ref{PhysicalSlotId(1U), frame, Generation(generation)};
		MetadataRadixPage page;
		page.page_id = MetadataPageId(frame);
		page.generation = Generation(generation);
		page.committed_lsn = LogSequenceNumber(generation);
		pages.writeMetadataRadix(ref, page);
		return ref;
	}
};

TEST_F(MetadataPageReadContextTest, ReusesDecodedPagesOnlyInsideAnActiveScope) {
	MetadataPageFile pages(io, root);
	const auto ref = writeRadix(pages, 1U);
	auto context = std::make_shared<MetadataPageReadContext>(root);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	{
		MetadataPageReadContext::Scope scope(context, *io);
		const auto first = pages.readMetadataRadixUnresolved(ref);
		const auto second = pages.readMetadataRadixUnresolved(ref);
		EXPECT_EQ(first.page_id, second.page_id);
		EXPECT_FALSE(second.logical_children_resolved);
	}
	EXPECT_EQ(MetadataPageReadContext::current(*io, root), nullptr);
	static_cast<void>(pages.readMetadataRadixUnresolved(ref));
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();
	EXPECT_EQ(trace.at("native_graph.read.metadata_decodes.radix").totalValue, 2);
	EXPECT_EQ(trace.at("native_graph.read.decoded_metadata_cache_hits").totalValue, 1);
}

TEST_F(MetadataPageReadContextTest, InvalidatesAfterDirectWritesAndDiagnosticResetsDoNotResetTheEpoch) {
	MetadataPageFile pages(io, root);
	const auto ref = writeRadix(pages, 1U);
	auto context = std::make_shared<MetadataPageReadContext>(root);
	MetadataPageReadContext::Scope scope(context, *io);
	EXPECT_EQ(pages.readMetadataRadixUnresolved(ref).page_id, MetadataPageId(1U));
	auto replacement = pages.readMetadataRadixUnresolved(ref);
	replacement.page_id = MetadataPageId(77U);
	pages.writeMetadataRadix(ref, replacement);
	const auto epoch = io->dataMutationEpoch();
	io->resetStats();
	EXPECT_EQ(io->dataMutationEpoch(), epoch);
	EXPECT_EQ(pages.readMetadataRadixUnresolved(ref).page_id, MetadataPageId(77U));
	const std::byte bad{0};
	io->writeAt(pages.pageOffset(ref), &bad, 1U);
	EXPECT_THROW(pages.readMetadataRadixUnresolved(ref), FormatException);
}

TEST_F(MetadataPageReadContextTest, BoundsBytesAndEvictsWithoutChangingReadResults) {
	MetadataPageFile pages(io, root);
	const auto first = writeRadix(pages, 1U);
	const auto second = writeRadix(pages, 2U);
	const auto third = writeRadix(pages, 3U);
	const size_t limit = 2U * sizeof(MetadataRadixPage) + 1024U;
	auto context = std::make_shared<MetadataPageReadContext>(root, limit);
	graph::debug::PerfTrace::setValueCollectionEnabled(true);
	MetadataPageReadContext::Scope scope(context, *io);
	EXPECT_EQ(pages.readMetadataRadixUnresolved(first).page_id, MetadataPageId(1U));
	EXPECT_EQ(pages.readMetadataRadixUnresolved(second).page_id, MetadataPageId(2U));
	EXPECT_EQ(pages.readMetadataRadixUnresolved(third).page_id, MetadataPageId(3U));
	EXPECT_LE(context->chargedBytes(), limit);
	EXPECT_LE(context->size(), 2U);
	EXPECT_EQ(pages.readMetadataRadixUnresolved(first).page_id, MetadataPageId(1U));
	EXPECT_GT(graph::debug::PerfTrace::snapshotAndReset().at("native_graph.read.decoded_metadata_cache_evictions").totalValue, 0);
}

TEST_F(MetadataPageReadContextTest, ZeroBudgetRetainsTheUncachedPath) {
	MetadataPageFile pages(io, root);
	const auto ref = writeRadix(pages, 1U);
	auto context = std::make_shared<MetadataPageReadContext>(root, 0U);
	MetadataPageReadContext::Scope scope(context, *io);
	EXPECT_EQ(pages.readMetadataRadixUnresolved(ref).page_id, MetadataPageId(1U));
	EXPECT_EQ(pages.readMetadataRadixUnresolved(ref).page_id, MetadataPageId(1U));
	EXPECT_EQ(context->size(), 0U);
	EXPECT_EQ(context->chargedBytes(), 0U);
}

TEST_F(MetadataPageReadContextTest, RestoresNestedScopesAndRejectsAnotherIoIdentity) {
	MetadataPageFile pages(io, root);
	const auto ref = writeRadix(pages, 1U);
	auto first = std::make_shared<MetadataPageReadContext>(root);
	auto second = std::make_shared<MetadataPageReadContext>(root);
	MetadataPageReadContext::Scope outer(first, *io);
	static_cast<void>(pages.readMetadataRadixUnresolved(ref));
	{
		MetadataPageReadContext::Scope inner(second, *io);
		EXPECT_EQ(MetadataPageReadContext::current(*io, root), second.get());
		static_cast<void>(pages.readMetadataRadixUnresolved(ref));
	}
	EXPECT_EQ(MetadataPageReadContext::current(*io, root), first.get());
	StorageIO other(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	EXPECT_THROW((MetadataPageReadContext::Scope(first, other)), std::invalid_argument);
	EXPECT_EQ(MetadataPageReadContext::current(*io, root), first.get());
	Superblock different = root;
	different.database_id = DatabaseId(124U);
	EXPECT_EQ(MetadataPageReadContext::current(*io, different), nullptr);
	EXPECT_FALSE(first->authenticates(different));
}

TEST_F(MetadataPageReadContextTest, DoesNotCacheFutureCandidateFramesAndStillRejectsInvalidHints) {
	MetadataPageFile pages(io, root);
	const auto ref = writeRadix(pages, 1U, 2U);
	auto context = std::make_shared<MetadataPageReadContext>(root);
	MetadataPageReadContext::Scope scope(context, *io);
	static_cast<void>(pages.readMetadataRadixUnresolved(ref));
	static_cast<void>(pages.readMetadataRadixUnresolved(ref));
	EXPECT_EQ(context->size(), 0U);
	const auto published = writeRadix(pages, 2U);
	static_cast<void>(pages.readMetadataRadixUnresolved(published));
	EXPECT_THROW(pages.readMetadataRadixUnresolved(published, static_cast<BufferPoolReadHint>(255U)), FormatException);
}

TEST_F(MetadataPageReadContextTest, ScopeIsThreadLocalWhileOneSnapshotCanBeReadByTwoThreads) {
	MetadataPageFile pages(io, root);
	const auto ref = writeRadix(pages, 1U);
	auto context = std::make_shared<MetadataPageReadContext>(root);
	MetadataPageReadContext::Scope outer(context, *io);
	static_cast<void>(pages.readMetadataRadixUnresolved(ref));
	std::thread worker([&] {
		EXPECT_EQ(MetadataPageReadContext::current(*io, root), nullptr);
		MetadataPageReadContext::Scope inner(context, *io);
		EXPECT_EQ(pages.readMetadataRadixUnresolved(ref).page_id, MetadataPageId(1U));
	});
	worker.join();
	EXPECT_EQ(context->size(), 1U);
}

#include "graph/query/execution/PrepaidMemoryResource.hpp"
namespace {
std::shared_ptr<graph::query::execution::detail::PrepaidMemoryResource> metadataReadGrant(size_t bytes) {
    using namespace graph::query::execution::detail;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(bytes));
    auto domain = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(bytes));
    return std::make_shared<PrepaidMemoryResource>(std::make_shared<ResourceAccount>(domain, ResourceClass::RCK_CACHE, bytes), bytes);
}
CatalogLeafPage oneCatalogPage() {
    CatalogLeafPage page;
    page.page_id = MetadataPageId(1U);
    page.minimum_segment_id = SegmentId(100U);
    page.maximum_segment_id = SegmentId(100U);
    page.generation = Generation(1U);
    page.committed_lsn = LogSequenceNumber(1U);
    page.entries.push_back({SegmentId(100U), PhysicalSlotId(2U), SegmentKind::SGK_ENTITY_PAGE_POOL,
                           100U, Generation(1U), SegmentState::SGS_SEALED, LogSequenceNumber(1U)});
    return page;
}
}
TEST_F(MetadataPageReadContextTest, DecodedEntryBuffersAndEscapedCopiesRetainTheirActualGrant) {
    auto memory = metadataReadGrant(64U * 1024U);
    std::weak_ptr<graph::query::execution::detail::PrepaidMemoryResource> lifetime = memory;
    auto context = std::allocate_shared<MetadataPageReadContext>(graph::utils::OwnedPmrAllocator<MetadataPageReadContext>(memory),
        root, MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, memory);
    MetadataPageFile pages(io, root);
    const PhysicalPageRef reference{PhysicalSlotId(1U), 1U, Generation(1U)};
    pages.writeCatalogLeaf(reference, oneCatalogPage());
    CatalogLeafPage escaped;
    {
        MetadataPageReadContext::Scope scope(context, *io);
        auto decoded = pages.readCatalogLeaf(reference);
        EXPECT_EQ(decoded.entries.get_allocator().owner(), memory);
        ASSERT_EQ(context->size(), 1U);
        const size_t before = memory->used();
        {
            const auto copy = decoded;
            ASSERT_EQ(copy.entries.size(), decoded.entries.size());
            EXPECT_EQ(copy.entries[0].segment_id, decoded.entries[0].segment_id);
            EXPECT_EQ(memory->used(), before + copy.entries.capacity() * sizeof(CatalogEntry));
        }
        EXPECT_EQ(memory->used(), before);
        escaped = std::move(decoded);
    }
    context.reset();
    EXPECT_EQ(memory->used(), escaped.entries.capacity() * sizeof(CatalogEntry));
    memory.reset();
    EXPECT_FALSE(lifetime.expired());
    ASSERT_EQ(escaped.entries.size(), 1U);
    EXPECT_EQ(escaped.entries[0].segment_id, SegmentId(100U));
    escaped = CatalogLeafPage{};
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(MetadataPageReadContextTest, SharedQuotaDropsOptionalCacheCopiesAndRetriesTheAuthenticatedDecode) {
    auto memory = metadataReadGrant(32U * 1024U);
    auto context = std::allocate_shared<MetadataPageReadContext>(graph::utils::OwnedPmrAllocator<MetadataPageReadContext>(memory),
        root, MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, memory);
    MetadataPageFile pages(io, root);
    const PhysicalPageRef reference{PhysicalSlotId(1U), 1U, Generation(1U)};
    pages.writeCatalogLeaf(reference, oneCatalogPage());
    {
        MetadataPageReadContext::Scope scope(context, *io);
        static_cast<void>(pages.readCatalogLeaf(reference));
        ASSERT_EQ(context->size(), 1U);
        const size_t pressureBytes = memory->capacity() - memory->used();
        ASSERT_GT(pressureBytes, 0U);
        auto releasePressure = [memory, pressureBytes](void *pointer) { memory->deallocate(pointer, pressureBytes); };
        std::unique_ptr<void, decltype(releasePressure)> pressure(memory->allocate(pressureBytes), releasePressure);
        {
            const auto decoded = pages.readCatalogLeaf(reference);
            ASSERT_EQ(decoded.entries.size(), 1U);
            EXPECT_EQ(decoded.entries[0].segment_id, SegmentId(100U));
            EXPECT_EQ(context->size(), 0U);
            EXPECT_LE(memory->used(), memory->capacity());
        }
        pressure.reset();
        EXPECT_GE(memory->denials(), 2U);
        static_cast<void>(pages.readCatalogLeaf(reference));
        EXPECT_EQ(context->size(), 1U);
    }
    context.reset();
    EXPECT_EQ(memory->used(), 0U);
}

TEST_F(MetadataPageReadContextTest, UnsupportedHeapOwningPagesDoNotEvictOrBypassACoordinatedCache) {
    struct UnownedPage { std::vector<uint64_t> entries; };
    auto memory = metadataReadGrant(32U * 1024U);
    auto context = std::allocate_shared<MetadataPageReadContext>(graph::utils::OwnedPmrAllocator<MetadataPageReadContext>(memory),
        root, MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, memory);
    MetadataPageFile pages(io, root);
    const auto reference = writeRadix(pages, 1U);
    MetadataPageReadContext::Scope scope(context, *io);
    static_cast<void>(pages.readMetadataRadixUnresolved(reference));
    ASSERT_EQ(context->size(), 1U);
    const size_t retained = memory->used();
    UnownedPage unsupported{std::vector<uint64_t>(512U, 7U)};
    context->put(PhysicalPageRef{PhysicalSlotId(1U), 2U, Generation(1U)}, unsupported,
                 unsupported.entries.capacity() * sizeof(uint64_t));
    EXPECT_EQ(context->size(), 1U);
    EXPECT_EQ(memory->used(), retained);
    struct NestedUnownedPage { graph::utils::OwnedVector<std::string> entries; };
    NestedUnownedPage nested;
    nested.entries.emplace_back(1024U, 'x');
    context->put(PhysicalPageRef{PhysicalSlotId(1U), 3U, Generation(1U)}, nested, 1024U + sizeof(std::string));
    EXPECT_EQ(context->size(), 1U);
    EXPECT_EQ(memory->used(), retained);
}

TEST_F(MetadataPageReadContextTest, CapturedScopeOwnerSurvivesCreationAndNestedBindingsRestoreTheirPredecessor) {
    auto outer = std::make_shared<MetadataPageReadContext>(root);
    auto inner = std::make_shared<MetadataPageReadContext>(root);
    auto otherIo = std::make_shared<StorageIO>(stream, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
    std::shared_ptr<MetadataPageReadContext> captured;
    EXPECT_EQ(MetadataPageReadContext::Scope::currentOwner(*io), nullptr);
    {
        MetadataPageReadContext::Scope scope(outer, *io);
        captured = MetadataPageReadContext::Scope::currentOwner(*io);
        EXPECT_EQ(captured, outer);
        EXPECT_EQ(MetadataPageReadContext::Scope::currentOwner(*otherIo), nullptr);
        {
            MetadataPageReadContext::Scope nested(inner, *io);
            EXPECT_EQ(MetadataPageReadContext::Scope::currentOwner(*io), inner);
        }
        EXPECT_EQ(MetadataPageReadContext::Scope::currentOwner(*io), outer);
        EXPECT_THROW((void)MetadataPageReadContext::Scope(outer, *otherIo), std::invalid_argument);
        EXPECT_EQ(MetadataPageReadContext::Scope::currentOwner(*io), outer);
        std::thread independent([&] { EXPECT_EQ(MetadataPageReadContext::Scope::currentOwner(*io), nullptr); });
        independent.join();
    }
    EXPECT_EQ(MetadataPageReadContext::Scope::currentOwner(*io), nullptr);
    EXPECT_EQ(captured, outer);
}

namespace {
MetadataPageRouteLeafPage viewRoutePage() {
    MetadataPageRouteLeafPage page;
    page.page_id = MetadataPageId(17U);
    page.generation = Generation(1U);
    page.committed_lsn = LogSequenceNumber(1U);
    for (uint64_t id = 1U; id <= 3U; ++id)
        page.routes.push_back({MetadataPageId(id), {PhysicalSlotId(id + 1U), 1U, Generation(1U)}});
    return page;
}
}

TEST_F(MetadataPageReadContextTest, ImmutableRouteViewsDoNotClonePayloadWhenTheArenaIsFull) {
    using graph::query::execution::detail::PrepaidMemoryResource;
    auto memory = metadataReadGrant(64U * 1024U);
    std::weak_ptr<PrepaidMemoryResource> lifetime = memory;
    auto context = std::allocate_shared<MetadataPageReadContext>(graph::utils::OwnedPmrAllocator<MetadataPageReadContext>(memory),
        root, MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, memory);
    MetadataPageFile pages(io, root);
    const PhysicalPageRef reference{PhysicalSlotId(1U), 1U, Generation(1U)};
    pages.writeMetadataPageRouteLeaf(reference, viewRoutePage());
    std::optional<DecodedMetadataPageView<MetadataPageRouteLeafPage>> escaped;
    {
        MetadataPageReadContext::Scope scope(context, *io);
        { auto warm = pages.readMetadataPageRouteLeafView(reference); EXPECT_EQ(warm->routes.size(), 3U); }
        auto borrowed = pages.readMetadataPageRouteLeafView(reference);
        const size_t retained = memory->used();
        {
            PrepaidMemoryResource::ExternalReservation pressure(memory, memory->capacity() - retained);
            for (size_t index = 0U; index < 20U; ++index) {
                const auto repeated = pages.readMetadataPageRouteLeafView(reference);
                EXPECT_EQ(repeated.get(), borrowed.get());
                EXPECT_EQ(memory->used(), memory->capacity());
            }
        }
        EXPECT_EQ(memory->used(), retained);
        EXPECT_EQ(context->findView<CatalogLeafPage>(reference), nullptr);
        escaped.emplace(std::move(borrowed));
    }
    context.reset();
    memory.reset();
    EXPECT_FALSE(lifetime.expired());
    ASSERT_EQ((*escaped)->routes.size(), 3U);
    EXPECT_EQ((*escaped)->routes[0].page_id, MetadataPageId(1U));
    escaped.reset();
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(MetadataPageReadContextTest, UncachedRouteViewNeedsOnlyItsDecodedPayloadAllocation) {
    using graph::query::execution::detail::PrepaidMemoryResource;
    auto memory = metadataReadGrant(32U * 1024U);
    auto context = std::make_shared<MetadataPageReadContext>(root, 0U, memory);
    MetadataPageFile pages(io, root);
    const PhysicalPageRef reference{PhysicalSlotId(1U), 1U, Generation(1U)};
    pages.writeMetadataPageRouteLeaf(reference, viewRoutePage());
    const size_t payload = 3U * sizeof(MetadataPageRoute);
    MetadataPageReadContext::Scope scope(context, *io);
    PrepaidMemoryResource::ExternalReservation pressure(memory, memory->capacity() - memory->used() - payload);
    auto view = pages.readMetadataPageRouteLeafView(reference);
    EXPECT_EQ(view->routes.size(), 3U);
    EXPECT_EQ(context->size(), 0U);
    EXPECT_EQ(memory->used(), memory->capacity());
}

TEST_F(MetadataPageReadContextTest, RouteViewsRemainImmutableAcrossMutableCopiesAndEpochInvalidation) {
    auto memory = metadataReadGrant(64U * 1024U);
    auto context = std::make_shared<MetadataPageReadContext>(root, MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, memory);
    MetadataPageFile pages(io, root);
    const PhysicalPageRef reference{PhysicalSlotId(1U), 1U, Generation(1U)};
    const auto initial = viewRoutePage();
    pages.writeMetadataPageRouteLeaf(reference, initial);
    MetadataPageReadContext::Scope scope(context, *io);
    { auto warm = pages.readMetadataPageRouteLeafView(reference); }
    auto original = pages.readMetadataPageRouteLeafView(reference);
    {
        auto writable = pages.readMetadataPageRouteLeaf(reference);
        writable.routes[0].physical_reference = {PhysicalSlotId(9U), 1U, Generation(1U)};
        EXPECT_NE(writable.routes.data(), original->routes.data());
        EXPECT_EQ(original->routes[0], initial.routes[0]);
    }
    auto changed = initial;
    changed.routes[0].physical_reference = {PhysicalSlotId(10U), 1U, Generation(1U)};
    pages.writeMetadataPageRouteLeaf(reference, changed);
    const auto fresh = pages.readMetadataPageRouteLeafView(reference);
    EXPECT_EQ(fresh->routes[0], changed.routes[0]);
    EXPECT_EQ(original->routes[0], initial.routes[0]);
    EXPECT_NE(fresh.get(), original.get());
    EXPECT_THROW((void)pages.readMetadataPageRouteLeafView(reference, static_cast<BufferPoolReadHint>(0xFFU)), FormatException);
    EXPECT_THROW((void)pages.readMetadataPageRouteLeafView({reference.physical_slot, reference.frame_index, Generation(2U)}), FormatException);
    using View = DecodedMetadataPageView<MetadataPageRouteLeafPage>;
    static_assert(!std::is_copy_constructible_v<View>);
    static_assert(std::is_same_v<decltype(*std::declval<const View &>()), const MetadataPageRouteLeafPage &>);
    EXPECT_THROW((void)View(std::shared_ptr<const MetadataPageRouteLeafPage>{}), std::invalid_argument);
}

TEST_F(MetadataPageReadContextTest, CatalogAndLocationViewsShareImmutableEntriesUnderArenaPressure) {
    using graph::query::execution::detail::PrepaidMemoryResource;
    MetadataPageFile pages(io, root);
    const auto verify = [&](const auto &input, PhysicalPageRef reference, auto write, auto readView, auto readCopy) {
        write(reference, input);
        auto memory = metadataReadGrant(32U * 1024U);
        auto context = std::make_shared<MetadataPageReadContext>(root, MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, memory);
        MetadataPageReadContext::Scope scope(context, *io);
        { auto warm = readView(reference); EXPECT_EQ(warm->entries.size(), input.entries.size()); }
        const auto original = readView(reference);
        {
            PrepaidMemoryResource::ExternalReservation pressure(memory, memory->capacity() - memory->used());
            const auto repeated = readView(reference);
            EXPECT_EQ(repeated.get(), original.get());
            EXPECT_EQ(repeated->entries.data(), original->entries.data());
            EXPECT_EQ(memory->used(), memory->capacity());
        }
        auto writable = readCopy(reference);
        EXPECT_NE(writable.entries.data(), original->entries.data());
        writable.entries.clear();
        EXPECT_EQ(original->entries.size(), input.entries.size());
        EXPECT_EQ(readView(reference).get(), original.get());
    };
    verify(oneCatalogPage(), {PhysicalSlotId(1U), 2U, Generation(1U)},
        [&](auto reference, const auto &page) { pages.writeCatalogLeaf(reference, page); },
        [&](auto reference) { return pages.readCatalogLeafView(reference); },
        [&](auto reference) { return pages.readCatalogLeaf(reference); });
    LocationLeafPage location;
    location.page_id = MetadataPageId(18U);
    location.directory_type = MetadataDirectoryType::MDT_ENTITY_PROPERTY;
    location.generation = Generation(1U);
    location.committed_lsn = LogSequenceNumber(1U);
    location.entries = {{1U, SegmentId(20U), Generation(1U), 0U, 1U}};
    verify(location, {PhysicalSlotId(1U), 3U, Generation(1U)},
        [&](auto reference, const auto &page) { pages.writeLocationLeaf(reference, page); },
        [&](auto reference) { return pages.readLocationLeafView(reference); },
        [&](auto reference) { return pages.readLocationLeaf(reference); });
}

TEST_F(MetadataPageReadContextTest, ExhaustedCacheCannotPreventEmptyContextAndOptionalCacheCanRetry) {
    using graph::query::execution::detail::PrepaidMemoryResource;
    auto memory = metadataReadGrant(32U * 1024U);
    const auto page = oneCatalogPage();
    const PhysicalPageRef reference{PhysicalSlotId(1U), 2U, Generation(1U)};
    std::shared_ptr<MetadataPageReadContext> context;
    {
        PrepaidMemoryResource::ExternalReservation pressure(memory, memory->capacity());
        ASSERT_NO_THROW(context = std::make_shared<MetadataPageReadContext>(root,
            MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, memory));
        MetadataPageReadContext::Scope scope(context, *io);
        EXPECT_NO_THROW(context->put(reference, page, page.entries.capacity() * sizeof(CatalogEntry)));
        EXPECT_EQ(context->size(), 0U);
        EXPECT_EQ(memory->used(), memory->capacity());
    }
    {
        MetadataPageReadContext::Scope scope(context, *io);
        context->put(reference, page, page.entries.capacity() * sizeof(CatalogEntry));
        EXPECT_EQ(context->size(), 1U);
        EXPECT_TRUE(context->findView<CatalogLeafPage>(reference));
    }
    context.reset(); EXPECT_EQ(memory->used(), 0U);
}

TEST_F(MetadataPageReadContextTest, PrivateCatalogCopyUsesTheCallerWhileCacheCreditIsExhausted) {
    using graph::query::execution::detail::PrepaidMemoryResource;
    MetadataPageFile pages(io, root);
    const auto input = oneCatalogPage();
    const PhysicalPageRef reference{PhysicalSlotId(1U), 2U, Generation(1U)};
    pages.writeCatalogLeaf(reference, input);
    auto cache = metadataReadGrant(32U * 1024U);
    auto caller = metadataReadGrant(32U * 1024U);
    auto context = std::make_shared<MetadataPageReadContext>(root,
        MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, cache);
    MetadataPageReadContext::Scope readScope(context, *io);
    static_cast<void>(pages.readCatalogLeafView(reference)); // Warm the immutable shared entry first.
    const auto original = pages.readCatalogLeafView(reference);
    {
        PrepaidMemoryResource::ExternalReservation pressure(cache, cache->capacity() - cache->used());
        graph::utils::OwnedAllocationScope callerScope(caller);
        {
            auto copied = pages.readCatalogLeaf(reference);
            EXPECT_EQ(CatalogLeafPageCodec::encode(copied), CatalogLeafPageCodec::encode(*original));
            EXPECT_NE(copied.entries.data(), original->entries.data());
            EXPECT_GT(caller->used(), 0U);
            EXPECT_EQ(cache->used(), cache->capacity());
            copied.entries.clear(); EXPECT_EQ(original->entries.size(), input.entries.size());
        }
        EXPECT_EQ(caller->used(), 0U);
        EXPECT_EQ(pages.readCatalogLeafView(reference).get(), original.get());
    }
}

TEST_F(MetadataPageReadContextTest, PrivateCopyDenialPreservesTheSharedEntryAndAvoidsARedecode) {
    using graph::query::execution::detail::PrepaidMemoryResource;
    using graph::query::execution::detail::ResourceAdmissionDenied;
    MetadataPageFile pages(io, root);
    const auto input = oneCatalogPage();
    const PhysicalPageRef reference{PhysicalSlotId(1U), 2U, Generation(1U)};
    pages.writeCatalogLeaf(reference, input);
    auto cache = metadataReadGrant(32U * 1024U);
    auto caller = metadataReadGrant(32U * 1024U);
    auto context = std::make_shared<MetadataPageReadContext>(root,
        MetadataPageReadContext::MAXIMUM_CHARGED_BYTES, cache);
    MetadataPageReadContext::Scope scope(context, *io);
    static_cast<void>(pages.readCatalogLeafView(reference)); // Warm the immutable shared entry first.
    const auto original = pages.readCatalogLeafView(reference);
    const auto cachedBytes = cache->used();
    {
        PrepaidMemoryResource::ExternalReservation pressure(caller, caller->capacity());
        graph::utils::OwnedAllocationScope callerScope(caller);
        io->resetStats();
        EXPECT_THROW(static_cast<void>(pages.readCatalogLeaf(reference)), ResourceAdmissionDenied);
        EXPECT_EQ(io->stats().read_bytes, 0U);
        EXPECT_EQ(context->size(), 1U);
        EXPECT_EQ(pages.readCatalogLeafView(reference).get(), original.get());
        EXPECT_EQ(cache->used(), cachedBytes);
    }
    EXPECT_EQ(caller->used(), 0U);
}
