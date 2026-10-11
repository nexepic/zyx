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
