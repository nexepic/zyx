#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "graph/core/Database.hpp"
#include "graph/storage/SegmentedSnapshot.hpp"
#include "zyx/zyx.hpp"
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
	TEST_F(CheckpointReadTest, DenseExtentWithAnInteriorDuplicateIsNotADirectory) {
		put(bytes, 48, 4 * 40);
		// Four rows spanning [2,5], but duplicated 3 leaves a hole at 4.
		record(bytes, 0, 1, 0, -2, 5, 0);
		record(bytes, 1, 1, 0, -1, 3, 0);
		record(bytes, 2, 1, 0, 0, 2, 0);
		record(bytes, 3, 1, 0, 1, 3, 0);
		std::fill(bytes.begin() + 88 + 160, bytes.end(), 0);
		seal(bytes, 4);
		save();
		EXPECT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OPEN_FAILED);
		EXPECT_NE(std::string(message()).find("Duplicate"), std::string::npos);
	}
	TEST_F(CheckpointReadTest, DeniesWriteTransaction) {
		ASSERT_EQ(zyx_driver_db_open(path.string().c_str(), &db, &err), ZYX_DRIVER_OK);
		EXPECT_NE(zyx_driver_txn_begin(db, &tx, &err), ZYX_DRIVER_OK);
		EXPECT_EQ(tx, nullptr);
	}
} // namespace


TEST_F(CheckpointReadTest, SegmentedNativeExportPinsContentAndRejectsDamagedAccess) {
	using namespace graph::storage;
	graph::Database source(path.string(), OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
	source.open();
	auto logical = std::dynamic_pointer_cast<LogicalGraphReadBackend>(source.getStorageBackend());
	auto pinned = logical->openLogicalGraphReadSnapshot();
	auto dest = path;
	dest += ".standalone";
	struct Cleanup {
		std::filesystem::path path;
		~Cleanup() {
			std::error_code ec;
			std::filesystem::remove(path, ec);
		}
	} cleanup{dest};
	{
		std::ofstream out(dest, std::ios::binary);
		exportSegmentedSnapshot(*pinned, out);
		out.close();
		ASSERT_TRUE(out);
	}
	graph::Database result(dest.string(), OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
	result.open();
	auto reader = std::dynamic_pointer_cast<LogicalGraphReadBackend>(result.getStorageBackend())
						  ->openLogicalGraphReadSnapshot();
	EXPECT_EQ(reader->readNode(5), pinned->readNode(5));
	EXPECT_EQ(reader->readEdge(9), pinned->readEdge(9));
	EXPECT_THROW(reader->readNode(3), std::runtime_error);
	reader.reset();
	result.close();
	{
		std::fstream f(dest, std::ios::in | std::ios::out | std::ios::binary);
		f.seekp(40);
		f.put(0);
		f.close();
		ASSERT_TRUE(f);
	}
	result.open();
	reader = std::dynamic_pointer_cast<LogicalGraphReadBackend>(result.getStorageBackend())
					 ->openLogicalGraphReadSnapshot();
	EXPECT_EQ(reader->readEdge(8), pinned->readEdge(8));
	EXPECT_THROW(reader->readNode(1), std::runtime_error);
	reader.reset();
	result.close();
	{
		std::fstream f(dest, std::ios::in | std::ios::out | std::ios::binary);
		f.seekg(8);
		auto byte = f.get();
		f.seekp(8);
		f.put(char(byte ^ 1));
	}
	EXPECT_THROW(result.open(), std::runtime_error);
}

TEST_F(CheckpointReadTest, ExportsNativeVariablePropertiesAndReadsLabelsWithoutThePropertySegment) {
	using namespace graph::storage;
	const auto dir = path.string() + ".work";
	ASSERT_TRUE(std::filesystem::create_directory(dir));
	struct RemoveDir {
		std::filesystem::path p;
		~RemoveDir() {
			std::error_code ec;
			std::filesystem::remove_all(p, ec);
		}
	} cleanup{dir};
	const auto live = dir + "/live", dest = dir + "/export";
	std::string title;
	for (size_t i = 0; i < 24000; ++i)
		title += "水";
	{
		zyx::Database writer(live);
		writer.open();
		ASSERT_TRUE(writer.execute("CREATE (:Paper:Draft {title:$p})", {{"p", zyx::Value(title)}}).isSuccess());
		ASSERT_TRUE(writer.execute("MATCH (a:Paper) CREATE (a)-[:Cites {note:$p}]->(a)", {{"p", zyx::Value(title)}})
							.isSuccess());
		writer.close();
	}
	graph::Database source(live, OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
	source.open();
	auto pinned = std::dynamic_pointer_cast<LogicalGraphReadBackend>(source.getStorageBackend())
						  ->openLogicalGraphReadSnapshot();
	{
		std::ofstream f(dest, std::ios::binary);
		exportSegmentedSnapshot(*pinned, f);
		f.close();
		ASSERT_TRUE(f);
	}
	auto checkSnapshot = [&](bool damaged) {
		graph::Database copy(dest, OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
		copy.open();
		auto r = std::dynamic_pointer_cast<LogicalGraphReadBackend>(copy.getStorageBackend())
						 ->openLogicalGraphReadSnapshot();
		EXPECT_EQ(r->readNodeShaped(1, {.include_labels = true}).labels, pinned->readNode(1).labels);
		EXPECT_EQ(r->readEdge(1), pinned->readEdge(1));
		ASSERT_TRUE(r->supportsNativeEdgeRefScans());
		std::vector<EntityRef> refs;
		r->visitEdgeRefs(1, [&](const EntityRef &ref) {
			refs.push_back(ref);
			return true;
		});
		ASSERT_EQ(refs, (std::vector<EntityRef>{{1, true, 1, 1, 0}}));
		auto topology = r->readEdgeShaped(1, {.include_type = true});
		EXPECT_EQ(topology.type, "Cites");
		EXPECT_TRUE(topology.properties.empty());
		if (damaged) {
			EXPECT_THROW(r->readNode(1), std::runtime_error);
		} else {
			EXPECT_EQ(r->readNode(1), pinned->readNode(1));
		}
	};
	checkSnapshot(false);
	// Read the canonical little-endian directory to locate the node property
	// block. Damaging it must leave labels and relationship properties readable.
	std::fstream file(dest, std::ios::binary | std::ios::in | std::ios::out);
	auto readWord = [&]() {
		std::array<unsigned char, 8> b{};
		file.read(reinterpret_cast<char *>(b.data()), 8);
		uint64_t v = 0;
		for (unsigned i = 0; i < 8; ++i)
			v |= uint64_t(b[i]) << (8 * i);
		return v;
	};
	file.seekg(-48, std::ios::end);
	auto directory = readWord(), bytes = readWord();
	bool changed = false;
	for (uint64_t i = 0; i < bytes; i += 72) {
		file.seekg(directory + i);
		if (readWord() != 2)
			continue;
		file.seekg(directory + i + 24);
		auto off = readWord();
		file.seekg(off + 16);
		auto old = file.get();
		file.seekp(off + 16);
		file.put(static_cast<char>(old ^ 1));
		changed = true;
		break;
	}
	file.close();
	ASSERT_TRUE(file);
	ASSERT_TRUE(changed);
	checkSnapshot(true);
}

TEST_F(CheckpointReadTest, BothTokenizedLayoutsPreserveSparseStableIdentities) {
	using namespace graph::storage;
	graph::Database source(path.string(), OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
	source.open();
	auto pinned = std::dynamic_pointer_cast<LogicalGraphReadBackend>(source.getStorageBackend())
						  ->openLogicalGraphReadSnapshot();
	for (bool columns : {false, true}) {
		auto dest = path;
		dest += ".tokens";
		struct Cleanup {
			std::filesystem::path p;
			~Cleanup() { std::error_code ec; std::filesystem::remove(p, ec); }
		} cleanup{dest};
		std::ofstream out(dest, std::ios::binary);
		exportTokenizedSnapshot(*pinned, out, columns);
		out.close();
		ASSERT_TRUE(out);
		graph::Database copy(dest.string(), OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
		copy.open();
		auto reader = std::dynamic_pointer_cast<LogicalGraphReadBackend>(copy.getStorageBackend())
							  ->openLogicalGraphReadSnapshot();
		for (uint64_t id : {5, 1})
			EXPECT_EQ(reader->readNode(id), pinned->readNode(id));
		for (uint64_t id : {8, 3, 9})
			EXPECT_EQ(reader->readEdge(id), pinned->readEdge(id));
		EXPECT_THROW(reader->readNode(3), std::runtime_error);
	}
}

TEST_F(CheckpointReadTest, AuthenticatedColumnRunsPreserveBorrowedTextAndPresence) {
	using namespace graph::storage;
	auto dir = std::filesystem::path(path.string() + ".fields");
	std::filesystem::create_directory(dir);
	struct Cleanup {
		std::filesystem::path p;
		~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); }
	} cleanup{dir};
	std::string longText(4096, 'x');
	longText[1] = '\0';
	longText += "关系";
	zyx::Database writer((dir / "source").string());
	writer.open();
	auto result = writer.execute("CREATE (:Shared {Shared:$text, small:42}), (:Shared {small:7})",
								 {{"text", zyx::Value(longText)}});
	ASSERT_TRUE(result.isSuccess()) << result.getError();
	std::ofstream file(dir / "copy", std::ios::binary);
	writer.exportSegmentedSnapshot(file, zyx::Database::SnapshotLayout::SL_TOKEN_COLUMN);
	file.close();
	ASSERT_TRUE(file);
	graph::Database copy((dir / "copy").string(), OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
	copy.open();
	auto reader = std::dynamic_pointer_cast<LogicalGraphReadBackend>(copy.getStorageBackend())
						  ->openLogicalGraphReadSnapshot();
	std::array<uint64_t, 2> ids{1, 2};
	std::array<std::string_view, 3> keys{"Shared", "small", "missing"};
	std::vector<std::string> texts;
	std::vector<uint64_t> numbers;
	PropertyColumnBatchReadSinks runs;
	runs.texts = [&](std::span<const size_t> rows, size_t key, std::span<const std::string_view> cells) {
		EXPECT_EQ(key, 0U);
		ASSERT_EQ(rows.size(), cells.size());
		for (size_t i = 0; i < rows.size(); ++i) {
			EXPECT_EQ(rows[i], 0U);
			texts.emplace_back(cells[i]);
		}
	};
	runs.scalars = [&](std::span<const size_t> rows, size_t key, graph::PropertyType type, std::span<const uint64_t> bits) {
		EXPECT_EQ(key, 1U);
		EXPECT_EQ(type, graph::PropertyType::INTEGER);
		ASSERT_EQ(rows.size(), bits.size());
		numbers.insert(numbers.end(), bits.begin(), bits.end());
	};
	auto noComplex = [](size_t, size_t, std::optional<graph::PropertyValue>) { ADD_FAILURE(); };
	ASSERT_TRUE(reader->tryReadNodePropertyColumnBatchesSortedWithAdmission(ids, keys, noComplex, runs, [](size_t) {}));
	EXPECT_EQ(texts, (std::vector<std::string>{longText}));
	EXPECT_EQ(numbers, (std::vector<uint64_t>{42, 7}));
	size_t attempts = 0;
	EXPECT_THROW(reader->tryReadNodePropertyColumnBatchesSortedWithAdmission(ids, keys, noComplex, runs, [&](size_t) {
		if (++attempts == 2) throw std::runtime_error("fixture budget");
	}), std::runtime_error);
	EXPECT_EQ(attempts, 2U);
}

TEST_F(CheckpointReadTest, DeclaredIndexIsBoundToOneExportAndItsCompleteLiveRows) {
	using Layout = zyx::Database::SnapshotLayout;
	auto dir = std::filesystem::path(path.string() + ".indexed");
	std::filesystem::create_directory(dir);
	struct Cleanup {
		std::filesystem::path dir;
		~Cleanup() {
			std::error_code ec;
			std::filesystem::remove_all(dir, ec);
		}
	} cleanup{dir};
	zyx::Database writer((dir / "source").string());
	writer.open();
	auto exec = [&](const char *q) {
		auto r = writer.execute(q);
		ASSERT_TRUE(r.isSuccess()) << r.getError();
		while (r.hasNext())
			r.next();
		ASSERT_TRUE(r.isSuccess()) << r.getError();
	};
	exec("CREATE (:K {K:7}),(:K {K:9}),(:K {K:7}),(:K),(:Other {K:7})");
	exec("MATCH (n:K) WHERE n.K=9 DELETE n");
	exec("MATCH (n:K {K:7}) CREATE (n)-[:K]->(n)");
	auto exportTo = [&](const char *name) {
		std::ofstream out(dir / name, std::ios::binary);
		writer.exportSegmentedSnapshot(out, Layout::SL_INDEXED_TOKEN_COLUMN);
		out.close();
		ASSERT_TRUE(out);
	};
	exportTo("undeclared");
	exec("CREATE INDEX declared FOR (n:K) ON (n.K)");
	exportTo("pinned");
	exec("CREATE (:K {K:7.0})");
	exportTo("mixed");
	{
		std::ofstream out(dir / "oracle", std::ios::binary);
		writer.exportSegmentedSnapshot(out, Layout::SL_SEGMENTED_ROW);
		out.close();
		ASSERT_TRUE(out);
	}
	writer.close();
	for (auto name: {"undeclared", "pinned", "mixed"}) {
		const bool indexed = std::string_view(name) == "pinned";
		graph::Database db((dir / name).string(), graph::storage::OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
		db.open();
		auto snapshot = std::dynamic_pointer_cast<graph::storage::LogicalGraphReadBackend>(db.getStorageBackend())
								->openLogicalGraphReadSnapshot();
		ASSERT_EQ(snapshot->supportsNativeExactNodeCandidates(), indexed);
		if (indexed) {
			std::vector<uint64_t> ids;
			auto done = snapshot->visitNodeCandidatesByPropertyRange(
					{"K"}, {"K", graph::PropertyValue(int64_t(6)), graph::PropertyValue(int64_t(8)), false, false}, 10,
					10, 4096, [&](const graph::storage::IndexCandidateBatch &batch) {
						EXPECT_EQ(batch.snapshot_generation, snapshot->nativeSnapshotGeneration());
						for (const auto &ref: batch.candidates)
							ids.push_back(ref.entity_id);
						return true;
					});
			ASSERT_EQ(done, std::optional<bool>(true));
			EXPECT_EQ(ids, (std::vector<uint64_t>{1, 3}));
		}
		zyx::Database publicDB((dir / name).string());
		publicDB.open();
		auto rows = publicDB.execute("MATCH(n:K) WHERE n.K=7 RETURN id(n) ORDER BY id(n)");
		ASSERT_TRUE(rows.isSuccess()) << rows.getError();
		std::vector<int64_t> ids;
		while (rows.hasNext()) {
			rows.next();
			ids.push_back(std::get<int64_t>(rows.get(0)));
		}
		ASSERT_TRUE(rows.isSuccess()) << rows.getError();
		if (std::string_view(name) == "mixed") {
			zyx::Database oracle((dir / "oracle").string());
			oracle.open();
			auto q = oracle.execute("MATCH(n:K) WHERE n.K=7 RETURN id(n) ORDER BY id(n)");
			ASSERT_TRUE(q.isSuccess());
			std::vector<int64_t> expected;
			while (q.hasNext()) {
				q.next();
				expected.push_back(std::get<int64_t>(q.get(0)));
			}
			ASSERT_TRUE(q.isSuccess());
			EXPECT_EQ(ids, expected);
			auto typed = publicDB.execute("MATCH(n:K) WHERE n.K=7.0 RETURN n.K");
			ASSERT_TRUE(typed.isSuccess());
			ASSERT_TRUE(typed.hasNext());
			typed.next();
			EXPECT_EQ(typed.get(0), zyx::Value(7.0));
		} else
			EXPECT_EQ(ids, (std::vector<int64_t>{1, 3}));
	}
}

TEST_F(CheckpointReadTest, WitnessIsIssuedForOnePinnedPublicationAndRejectsRebinding) {
	auto dir = std::filesystem::path(path.string() + ".publication");
	std::filesystem::create_directory(dir);
	struct Cleanup {
		std::filesystem::path dir;
		~Cleanup() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
	} cleanup{dir};
	zyx::Database source((dir / "live").string()), other((dir / "other").string());
	source.open();
	other.open();
	auto prepare = [](zyx::Database &db) {
		auto rows = db.execute("CREATE (:K {K:17}),(:K {K:19})");
		ASSERT_TRUE(rows.isSuccess()) << rows.getError();
		while (rows.hasNext()) rows.next();
		ASSERT_TRUE(rows.isSuccess());
		auto ddl = db.execute("CREATE INDEX k FOR (n:K) ON (n.K)");
		ASSERT_TRUE(ddl.isSuccess()) << ddl.getError();
	};
	prepare(source);
	prepare(other);
	auto a = source.beginReadOnlyTransaction();
	auto foreign = other.beginReadOnlyTransaction();
	auto publish = [&](zyx::Database &db, const zyx::Transaction &tx, const char *file) {
		std::ofstream dst(dir / file, std::ios::binary);
		auto witness = db.publishColumnSnapshot(tx, dst);
		dst.close();
		EXPECT_TRUE(dst);
		return witness;
	};
	std::ostringstream untouched;
	EXPECT_THROW(source.publishColumnSnapshot(foreign, untouched), std::logic_error);
	auto write = source.beginTransaction();
	EXPECT_THROW(source.publishColumnSnapshot(write, untouched), std::logic_error);
	ASSERT_EQ(untouched.str(), "");
	(void)write.createNode("K", {{"K", int64_t(17)}});
	write.commit();
	auto wa = publish(source, a, "A");
	a.commit();
	EXPECT_THROW(source.publishColumnSnapshot(a, untouched), std::logic_error);
	auto b = source.beginReadOnlyTransaction();
	auto wb = publish(source, b, "B");
	auto wf = publish(other, foreign, "foreign");
	EXPECT_NE(wa.root(), wf.root());
	EXPECT_NE(wa.root(), wb.root());
	zyx::Database wrong((dir / "A").string());
	EXPECT_THROW(wrong.openPublishedSnapshot(wf), std::runtime_error);
	EXPECT_THROW(wrong.openPublishedSnapshot(wb), std::runtime_error);
	EXPECT_FALSE(wrong.hasActiveTransaction());
	for (auto withProof : {true, false}) {
		for (auto later : {true, false}) {
			zyx::Database copy((dir / (later ? "B" : "A")).string());
			if (withProof) copy.openPublishedSnapshot(later ? wb : wa);
			else copy.open();
			auto result = copy.execute("MATCH (n:K) WHERE n.K=17 RETURN id(n) ORDER BY id(n)");
			ASSERT_TRUE(result.isSuccess()) << result.getError();
			std::vector<int64_t> ids;
			while (result.hasNext()) {
				result.next();
				ids.push_back(std::get<int64_t>(result.get(0)));
			}
			EXPECT_EQ(ids, later ? (std::vector<int64_t>{1,3}) : (std::vector<int64_t>{1}));
			ASSERT_TRUE(result.isSuccess());
		}
	}
}
