#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/utils/Sha256.hpp"
#include "zyx/zyx_driver_abi.h"

namespace {
	using Bytes = std::vector<unsigned char>;
	void put(Bytes &buf, size_t offset, uint64_t value) {
		for (unsigned i = 0; i < 8; ++i)
			buf[offset + i] = static_cast<unsigned char>(value >> (8 * i));
	}
	void record(Bytes &buf, size_t idx, unsigned kind, unsigned tag, int64_t key, uint64_t id, uint64_t value,
				uint64_t last = 0) {
		size_t p = 88 + 40 * idx;
		buf[p] = kind;
		buf[p + 1] = tag;
		put(buf, p + 4, key);
		put(buf, p + 12, id);
		put(buf, p + 20, value);
		put(buf, p + 28, last);
	}
	void seal(Bytes &b, size_t n) {
		graph::utils::Sha256 sha;
		sha.update(std::as_bytes(std::span(b).first(56)));
		sha.update(std::as_bytes(std::span(b).subspan(88, n * 40)));
		auto digest = sha.finalize();
		std::memcpy(b.data() + 56, digest.data(), 32);
	}
	struct CheckpointReadTest : testing::Test {
		std::filesystem::path path;
		Bytes bytes = Bytes(4096);
		zyx_driver_db_t *db = nullptr;
		zyx_driver_txn_t *tx = nullptr;
		zyx_driver_error_t *err = nullptr;
		zyx_driver_result_t *result = nullptr;
		void SetUp() override {
			path = std::filesystem::temp_directory_path() /
				   ("zyx-cp002-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::memcpy(bytes.data(), "ZYXcp002", 8);
			put(bytes, 32, 5);
			put(bytes, 40, 9);
			put(bytes, 48, 5 * 40);
			record(bytes, 0, 1, 4, -9, 5, 1ULL << 63);
			record(bytes, 1, 1, 3, 9, 1, 1);
			record(bytes, 2, 2, 0, 3, 5, 1, 17);
			record(bytes, 3, 2, 0, 8, 5, 1, 18);
			record(bytes, 4, 2, 0, 9, 1, 1, 19);
			seal(bytes, 5);
			save();
		}
		void save() {
			std::ofstream f(path, std::ios::binary);
			f.write(reinterpret_cast<char *>(bytes.data()), bytes.size());
		}
		void TearDown() override {
			zyx_driver_result_free(result);
			if (tx) {
				zyx_driver_txn_rollback(tx, nullptr);
				zyx_driver_txn_close(tx, nullptr);
			}
			if (db)
				zyx_driver_db_close(db, nullptr);
			zyx_driver_error_free(err);
			std::filesystem::remove(path);
		}
		void start() {
			ASSERT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OK) << message();
			ASSERT_EQ(zyx_driver_txn_begin_read_only(db, &tx, &err), ZYX_DRIVER_OK) << message();
		}
		const char *message() const { return err ? zyx_driver_error_message(err) : ""; }
		void query(const char *q) {
			zyx_driver_result_free(result);
			result = nullptr;
			ASSERT_EQ(zyx_driver_txn_execute(tx, q, nullptr, &result, &err), ZYX_DRIVER_OK) << message();
		}
		int64_t integer(unsigned col) {
			int64_t v = 0;
			EXPECT_EQ(zyx_driver_result_get_int64(result, col, &v, &err), ZYX_DRIVER_OK);
			return v;
		}
	};
	TEST_F(CheckpointReadTest, OrderedProjectionUsesOriginalIdsAndTypes) {
		ASSERT_NO_FATAL_FAILURE(start());
		ASSERT_NO_FATAL_FAILURE(query("MATCH (n:Item) RETURN id(n), n.id, n.val ORDER BY n.id"));
		ASSERT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_ROW) << message();
		EXPECT_EQ(integer(0), 5);
		EXPECT_EQ(integer(1), -9);
		double d = 1;
		ASSERT_EQ(zyx_driver_result_get_double(result, 2, &d, &err), ZYX_DRIVER_OK);
		EXPECT_EQ(std::bit_cast<uint64_t>(d), 1ULL << 63);
		ASSERT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_ROW);
		EXPECT_EQ(integer(0), 1);
		bool flag = false;
		ASSERT_EQ(zyx_driver_result_get_bool(result, 2, &flag, &err), ZYX_DRIVER_OK);
		EXPECT_TRUE(flag);
		EXPECT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_DONE);
	}
	TEST_F(CheckpointReadTest, ReverseTraversalRetainsParallelEdgesAndLoop) {
		ASSERT_NO_FATAL_FAILURE(start());
		ASSERT_NO_FATAL_FAILURE(query("MATCH (b:Item)<-[r:Link]-(a:Item) RETURN id(r),id(a),id(b),r.w ORDER BY id(r)"));
		for (auto id: {3, 8, 9}) {
			ASSERT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_ROW) << message();
			EXPECT_EQ(integer(0), id);
			EXPECT_EQ(integer(1), id == 9 ? 1 : 5);
			EXPECT_EQ(integer(2), 1);
			EXPECT_EQ(integer(3), id == 3 ? 17 : id == 8 ? 18 : 19);
		}
		EXPECT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_DONE);
		ASSERT_NO_FATAL_FAILURE(query("MATCH ()-[r]->() RETURN count(r)"));
		ASSERT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_ROW) << message();
		EXPECT_EQ(integer(0), 3);
	}
	TEST_F(CheckpointReadTest, EntityResultsAndAggregateUsePinnedImage) {
		ASSERT_NO_FATAL_FAILURE(start());
		bytes[108] ^= 1;
		save(); // named input may change after open
		ASSERT_NO_FATAL_FAILURE(query("MATCH (a)-[r]->(b) RETURN a,r,b ORDER BY id(r)"));
		for (auto id: {3, 8, 9}) {
			ASSERT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_ROW) << message();
			int64_t edge = 0, src = 0, dst = 0;
			ASSERT_EQ(zyx_driver_result_get_edge_id(result, 1, &edge, &err), ZYX_DRIVER_OK);
			ASSERT_EQ(zyx_driver_result_get_node_id(result, 0, &src, &err), ZYX_DRIVER_OK);
			ASSERT_EQ(zyx_driver_result_get_node_id(result, 2, &dst, &err), ZYX_DRIVER_OK);
			EXPECT_EQ(edge, id);
			EXPECT_EQ(src, id == 9 ? 1 : 5);
			EXPECT_EQ(dst, 1);
		}
		EXPECT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_DONE);
		ASSERT_NO_FATAL_FAILURE(query("MATCH (n) RETURN count(DISTINCT labels(n))"));
		ASSERT_EQ(zyx_driver_result_next(result, &err), ZYX_DRIVER_ROW);
		EXPECT_EQ(integer(0), 1);
	}
	TEST_F(CheckpointReadTest, BoundedAdjacencyResumesAtExactParallelEdge) {
		using namespace graph::storage;
		graph::Database graph(path.string(), OpenMode::OPEN_READ_ONLY_EXISTING_FILE, 4096);
		graph.open();
		auto reader = std::dynamic_pointer_cast<LogicalGraphReadBackend>(graph.getStorageBackend());
		auto snap = reader->openLogicalGraphReadSnapshot();
		auto page = snap->scanAdjacentEdges(5, LogicalGraphReadDirection::LGRD_OUTGOING, {}, 1);
		ASSERT_EQ(page.edges.size(), 1);
		EXPECT_EQ(page.edges[0].edge_id, 3);
		ASSERT_TRUE(page.continuation);
		auto next = snap->scanAdjacentEdges(5, LogicalGraphReadDirection::LGRD_OUTGOING, page.continuation, 1);
		ASSERT_EQ(next.edges.size(), 1);
		EXPECT_EQ(next.edges[0].edge_id, 8);
		EXPECT_FALSE(next.continuation);
		auto wrong = *page.continuation;
		wrong.previous_edge_id = 1;
		EXPECT_THROW((void) snap->scanAdjacentEdges(5, LogicalGraphReadDirection::LGRD_OUTGOING, wrong, 1),
					 std::runtime_error);
		EXPECT_THROW(graph.close(), std::runtime_error);
		snap.reset();
		graph.close();
	}
	TEST_F(CheckpointReadTest, WalDependentMainRefused) {
		bytes.resize(8192);
		save();
		EXPECT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OPEN_FAILED);
		EXPECT_NE(std::string(message()).find("requires its WAL"), std::string::npos);
	}
	TEST_F(CheckpointReadTest, CorruptDigestRefused) {
		bytes[108] ^= 1;
		save();
		EXPECT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OPEN_FAILED);
	}
	TEST_F(CheckpointReadTest, IncompleteRefused) {
		bytes.resize(512);
		save();
		EXPECT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OPEN_FAILED);
	}
	TEST_F(CheckpointReadTest, DanglingAuthenticatedEndpointRefused) {
		put(bytes, 88 + 80 + 12, 2);
		seal(bytes, 5);
		save();
		EXPECT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OPEN_FAILED);
	}
	TEST_F(CheckpointReadTest, StorageCacheAdmissionPrecedesAllocation) {
		graph::Database graph(path.string(), graph::storage::OpenMode::OPEN_READ_ONLY_EXISTING_FILE, 64);
		EXPECT_THROW(graph.open(), std::runtime_error);
	}
	TEST_F(CheckpointReadTest, IrregularMultiWindowRowsSurviveSourceOverwriteAndPreserveOrder) {
		// The high budget exercises compact lanes. The low budget uses the private backing file.
		constexpr size_t count = 1025;
		for (size_t budget: {131072U, 32768U}) {
			bytes.assign((88 + count * 40 + 4095) / 4096 * 4096, 0);
			std::memcpy(bytes.data(), "ZYXcp002", 8);
			put(bytes, 32, count * 2);
			put(bytes, 48, count * 40);
			for (size_t i = 0; i < count; ++i) {
				const int64_t key = static_cast<int64_t>(i) - 500;
				// Sorting key is distinct from graph identity. Payloads cross zero
				// and routinely exercise the full uint64 representation.
				record(bytes, i, 1, 2, key, 2 * (count - i), uint64_t(i) * 0x9e3779b97f4a7c15ULL);
			}
			seal(bytes, count);
			save();
			graph::Database graph(path.string(), graph::storage::OpenMode::OPEN_READ_ONLY_EXISTING_FILE, budget);
			graph.open();
			auto backend =
					std::dynamic_pointer_cast<graph::storage::LogicalGraphReadBackend>(graph.getStorageBackend());
			auto snapshot = backend->openLogicalGraphReadSnapshot();
			bytes.assign(88, 0);
			save();
			size_t visited = 0;
			uint64_t after = 0;
			do {
				auto page = snapshot->scanNodesAfter(after, 63, 1);
				ASSERT_FALSE(page.nodes.empty());
				for (auto &n: page.nodes) {
					++visited;
					EXPECT_EQ(n.node_id, 2 * visited);
					auto idx = count - visited;
					ASSERT_EQ(n.properties.size(), 2U);
					EXPECT_EQ(n.properties[0].value, graph::PropertyValue(static_cast<int64_t>(idx) - 500));
					EXPECT_EQ(n.properties[1].value,
							  graph::PropertyValue(std::bit_cast<int64_t>(idx * 0x9e3779b97f4a7c15ULL)));
					after = n.node_id;
				}
				if (!page.may_have_more)
					break;
			} while (visited <= count);
			EXPECT_EQ(visited, count);
			for (auto labels: {std::vector<std::string>{"Item"}, std::vector<std::string>{"Other"}}) {
				size_t calls = 0;
				snapshot->visitNodeRefsWithLabelMatches(10, labels, [&](const auto &ref, bool match) {
					++calls;
					EXPECT_EQ(ref.entity_id, calls * 2);
					EXPECT_EQ(match, labels.front() == "Item");
					return calls < 3;
				});
				EXPECT_EQ(calls, 3U);
			}
		}
	}
	TEST_F(CheckpointReadTest, CompleteVisitorsRejectTruncationButPreserveExactAndEarlyStops) {
		using namespace graph::storage;
		graph::Database graph(path.string(), OpenMode::OPEN_READ_ONLY_EXISTING_FILE, 4096);
		graph.open();
		auto backend = std::dynamic_pointer_cast<LogicalGraphReadBackend>(graph.getStorageBackend());
		auto snap = backend->openLogicalGraphReadSnapshot();
		size_t calls = 0;
		auto more = [&](const auto &) {
			++calls;
			return true;
		};
		EXPECT_THROW(snap->visitNodeRefs(1, more), std::runtime_error);
		EXPECT_EQ(calls, 1U);
		calls = 0;
		EXPECT_NO_THROW(snap->visitNodeRefs(2, more));
		EXPECT_EQ(calls, 2U);
		calls = 0;
		EXPECT_NO_THROW(snap->visitNodeRefs(1, [&](const auto &) {
			++calls;
			return false;
		}));
		EXPECT_EQ(calls, 1U);
		EXPECT_THROW(snap->visitNodes(1, more), std::runtime_error);
		EXPECT_THROW(snap->visitEdges(2, more), std::runtime_error);
		EXPECT_THROW(snap->visitAdjacentEdges(5, LogicalGraphReadDirection::LGRD_OUTGOING, 1, more),
					 std::runtime_error);
		EXPECT_NO_THROW(snap->visitAdjacentEdges(5, LogicalGraphReadDirection::LGRD_OUTGOING, 1,
												 [](const auto &) { return false; }));
	}
	TEST_F(CheckpointReadTest, DeniesWriteTransaction) {
		ASSERT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OK);
		EXPECT_NE(zyx_driver_txn_begin(db, &tx, &err), ZYX_DRIVER_OK);
		EXPECT_EQ(tx, nullptr);
	}
} // namespace
