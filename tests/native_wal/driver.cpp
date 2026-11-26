#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/NativeWalDatabase.hpp"
#include "graph/storage/StorageBackendFactory.hpp"
#include "zyx/zyx.hpp"
#include "LocatorBudget.hpp"
#include "LongProperties.hpp"
#include "LargeRows.hpp"
#include "SnapshotLargeProperties.hpp"
#include "StreamedTransactions.hpp"
#include "RelocationGraph.hpp"
namespace {
	void expect(bool condition, const std::string &error) {
		if (!condition)
			throw std::runtime_error(error);
	}
	template<class Connection>
	auto query(Connection &handle, const std::string &text) {
		auto r = handle.execute(text);
		expect(r.isSuccess(), text + ": " + r.getError());
		return r;
	}
	template<class Connection>
	int64_t scalar(Connection &handle, const std::string &text) {
		auto result = query(handle, text);
		expect(result.hasNext(), "Missing row: " + text);
		result.next();
		auto value = std::get<int64_t>(result.get(0));
		expect(!result.hasNext(), "Extra row");
		return value;
	}
	const std::string count = "MATCH (n) RETURN count(n) AS c";
	void exerciseGraph(zyx::Database &db, const std::string &path) {
		auto empty = db.beginReadOnlyTransaction();
		auto writer = db.beginTransaction();
		query(writer, "CREATE (:Person {v: 7})-[:LINK {w: 5}]->(:Person {v: 8})");
		expect(scalar(writer, count) == 2, "Missing private nodes");
		expect(scalar(empty, count) == 0, "Private nodes visible");
		writer.commit();
		expect(scalar(empty, count) == 0, "Old snapshot changed");
		empty.commit();
		auto old = db.beginReadOnlyTransaction();
		expect(scalar(old, "MATCH ()-[r:LINK]->() RETURN r.w AS w") == 5, "Missing edge");
		{
			std::ofstream out(path + ".cp003", std::ios::binary);
			db.exportSegmentedSnapshot(out);
			out.close();
			expect(out.good(), "Export write failed");
		}
		auto update = db.beginTransaction();
		query(update, "MATCH (n:Person) WHERE n.v = 7 SET n.v = 9");
		query(update, "MATCH ()-[r:LINK]->() SET r.w = 6");
		expect(scalar(update, "MATCH ()-[r:LINK]->() RETURN r.w AS w") == 6, "Own update missing");
		update.commit();
		expect(scalar(old, "MATCH ()-[r:LINK]->() RETURN r.w AS w") == 5, "Pinned edge changed");
		db.save();
		expect(scalar(old, "MATCH (n:Person) WHERE n.v = 7 RETURN id(n) AS id") == 1,
			   "Checkpoint changed pinned stable ID");
		old.commit();
		auto remove = db.beginTransaction();
		query(remove, "MATCH ()-[r:LINK]->() DELETE r");
		query(remove, "MATCH (n:Person) WHERE n.v = 9 DELETE n");
		expect(scalar(remove, count) == 1, "Own delete missing");
		remove.commit();
		auto a = db.beginTransaction();
		auto b = db.beginTransaction();
		auto aid = a.createNode("New");
		auto bid = b.createNode("Contender");
		expect(aid == 3 && bid == 3, "Wrong provisional ID");
		a.commit();
		bool conflicted = false;
		try {
			b.commit();
		} catch (const std::exception &) {
			conflicted = true;
		}
		expect(conflicted, "Stale transaction committed");
		auto undo = db.beginTransaction();
		(void) undo.createNode("Undo");
		undo.rollback();
		db.close();
		db.open();
		expect(scalar(db, count) == 2, "Wrong reopened count");
		zyx::Database exported(path + ".cp003");
		exported.open();
		expect(scalar(exported, "MATCH (n:Person) WHERE n.v = 7 RETURN id(n) AS id") == 1, "Export has wrong owner");
		expect(scalar(exported, "MATCH ()-[r:LINK]->() RETURN r.w AS w") == 5, "Export has wrong edge");
		exported.close();
	}
	void expectRolloverValue(auto &handle, char value) {
		auto result =
				handle.execute("MATCH (n:Roll) WHERE n.payload=$s RETURN count(n)", {{"s", std::string(8192, value)}});
		expect(result.isSuccess() && result.hasNext(), result.getError());
		result.next();
		expect(std::get<int64_t>(result.get(0)) == 120 && !result.hasNext(), "Wrong rollover snapshot");
	}
} // namespace
int main(int argc, char **argv) try {
	if (argc < 3)
		throw std::runtime_error("driver OP MAIN_OR_NEW_DIRECTORY [FAULT]");
	std::string op = argv[1], path = argv[2];
	if (op.starts_with("snapshot-property-")) {
		snapshot_property_test::exercise(op, path, std::stoull(argv[3]), std::string_view(argv[4]) == "edge");
		return 0;
	}
	if (op.starts_with("large-row-")) {
		large_row_test::exercise(op, path, std::stoull(argv[3]), argc > 4 ? argv[4] : nullptr);
		return 0;
	}
	if (op == "stream-export") {
		zyx::Database db(path);
		db.open();
		std::ofstream out(path + ".view", std::ios::binary);
		db.exportSegmentedSnapshot(out);
		out.close();
		expect(out.good(), "Failed to finish standalone export");
		return 0;
	}
	if (op == "stream-budget-read" || op == "stream-budget-write" ||
		op == "stream-budget-small-read" || op == "stream-budget-small-write") {
		streamed_transaction_test::exerciseBudget(path, std::stoull(argv[3]), op.ends_with("-read"),
												 op.starts_with("stream-budget-small-"));
		return 0;
	}
	if (op.starts_with("stream-")) {
		streamed_transaction_test::exercise(op, path, argc > 3 ? std::stoull(argv[3]) : 0, argc > 4 ? argv[4] : nullptr);
		return 0;
	}
	if (op.starts_with("long-")) {
		long_property_test::exercise(op, path, argc > 3 ? argv[3] : nullptr);
		return 0;
	}
	if (op == "init") {
		std::cout << graph::storage::createNativeWalDatabase(path) << "\n";
		return 0;
	}
	if (op == "relocation-graph") {
		relocation_test::exercise(path);
		return 0;
	}
	if (op == "relocation-export") {
		relocation_test::verifyIndependentExport(path);
		return 0;
	}
	if (op == "locator-budget") {
		exerciseLocatorBudget(path);
		return 0;
	}
	if (op == "budget-point" || op == "budget-recover" || op == "late-damage") {
		using namespace graph::storage;
		auto selection = StorageBackendFactory::create(path, std::stoull(argv[3]),
													   op != "budget-recover" ? OpenMode::OPEN_READ_ONLY_EXISTING_FILE
																			  : OpenMode::OPEN_EXISTING_FILE);
		selection.backend->open();
		auto &reader = dynamic_cast<LogicalGraphReadBackend &>(*selection.backend);
		auto old = reader.openLogicalGraphReadSnapshot();
		auto get = [](auto &r, int id) {
			auto n = r.readNode(id);
			return std::get<std::string>(n.properties.front().value.getVariant());
		};
		expect(get(*old, 1) == std::string(8192, 'z') && get(*old, 320) == std::string(8192, '0'),
			   "Wrong budgeted recovery");
		if (op == "late-damage") {
			// Last row is 320 and the last 32 bytes are the authenticated footer.
			std::fstream f(path + ".base", std::ios::in | std::ios::out | std::ios::binary);
			f.seekp(-64, std::ios::end);
			f.put('!');
			f.flush();
			expect(bool(f), "Cannot damage disposable base");
			expect(get(*old, 1) == std::string(8192, 'z'), "Unrelated row changed");
			bool refused = false;
			try {
				(void) get(*old, 320);
			} catch (const std::exception &e) {
				refused = std::string(e.what()).find("base row changed") != std::string::npos;
			}
			expect(refused, "Authenticated deferred row accepted changed contents");
		}
		if (op == "budget-recover") {
			auto &writer = dynamic_cast<LogicalGraphTransactionalBackend &>(*selection.backend);
			auto tx = writer.beginLogicalGraphTransaction(false);
			LogicalGraphMutationBatch batch;
			LogicalGraphMutationEntityReference ref;
			ref.value = 320;
			batch.property_replacements.push_back({ref, {{"payload", graph::PropertyValue(std::string(8192, 'B'))}}});
			tx->stageMutationBatch(std::move(batch));
			tx->commit();
			selection.backend->flush();
			auto fresh = reader.openLogicalGraphReadSnapshot();
			expect(get(*fresh, 320) == std::string(8192, 'B') && get(*old, 320) == std::string(8192, '0'),
				   "Budgeted checkpoint broke old view");
			fresh.reset();
			tx.reset();
			for (int cycle = 0; cycle < 12; ++cycle) {
				if (cycle == 6)
					old.reset();
				auto step = writer.beginLogicalGraphTransaction(false);
				auto id = static_cast<uint64_t>(2 + (cycle * 131) % 317);
				auto value = std::string(cycle % 2 ? 8192 : 43, 'k' + cycle);
				LogicalGraphMutationBatch updates;
				ref.value = id;
				updates.property_replacements.push_back({ref, {{"payload", graph::PropertyValue(value)}}});
				step->stageMutationBatch(std::move(updates));
				step->commit();
				step.reset();
				selection.backend->flush();
				auto view = reader.openLogicalGraphReadSnapshot();
				expect(get(*view, id) == value && get(*view, 320) == std::string(8192, 'B') &&
							   (!old || get(*old, id) == std::string(8192, '0')),
					   "Tight-budget relocation changed snapshot contents");
			}
		}
		old.reset();
		selection.backend->close();
		return 0;
	}
	if (op == "tiny-wal-writes") {
		using namespace graph::storage;
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		auto selection = StorageBackendFactory::create(path, 4096, OpenMode::OPEN_EXISTING_FILE);
		selection.backend->open();
		auto &writer = dynamic_cast<LogicalGraphTransactionalBackend &>(*selection.backend);
		for (int i = 0; i < 2; ++i) {
			auto tx = writer.beginLogicalGraphTransaction(false);
			LogicalGraphMutationBatch batch;
			LogicalGraphNodeCreateRequest node;
			node.labels = {"Tiny"};
			node.properties.push_back({"p", graph::PropertyValue(std::string(16, 'a' + i))});
			batch.node_creates.push_back(std::move(node));
			(void) tx->stageMutationBatch(std::move(batch));
			(void) tx->commit();
		}
		// Leave the two aligned frames for a fresh recovery process.
		raise(SIGKILL);
	}
	if (op == "nested-budget-write" || op == "nested-budget-open") {
		using namespace graph::storage;
		expect(argc == 4, "Expected version budget");
		auto selected = StorageBackendFactory::create(path, std::stoull(argv[3]), OpenMode::OPEN_EXISTING_FILE);
		selected.backend->open();
		if (op == "nested-budget-write") {
			auto &w = dynamic_cast<LogicalGraphTransactionalBackend &>(*selected.backend);
			auto tx = w.beginLogicalGraphTransaction(false);
			LogicalGraphMutationBatch batch;
			LogicalGraphNodeCreateRequest request;
			request.labels = {"Dense"};
			request.properties.push_back({"v", graph::PropertyValue(std::vector<graph::PropertyValue>(1900))});
			batch.node_creates.push_back(std::move(request));
			(void) tx->stageMutationBatch(std::move(batch));
			(void) tx->commit();
		}
		selected.backend->close();
		return 0;
	}
	if (op == "budget") {
		using namespace graph::storage;
		{
			zyx::Database seed(path);
			seed.open();
			auto t = seed.beginTransaction();
			(void) t.createNode("Bound", {{"v", std::string(4096, '0')}});
			t.commit();
			seed.close();
		}
		auto selection = StorageBackendFactory::create(path, 32 * 1024, OpenMode::OPEN_EXISTING_FILE);
		selection.backend->open();
		auto &writer = dynamic_cast<LogicalGraphTransactionalBackend &>(*selection.backend);
		auto &reader = dynamic_cast<LogicalGraphReadBackend &>(*selection.backend);
		std::vector<std::unique_ptr<LogicalGraphReadSnapshot>> pins;
		auto replace = [&](char c) {
			auto t = writer.beginLogicalGraphTransaction(false);
			LogicalGraphMutationBatch batch;
			LogicalGraphMutationEntityReference node;
			node.value = 1;
			batch.property_replacements.push_back({node, {{"v", graph::PropertyValue(std::string(4096, c))}}});
			(void) t->stageMutationBatch(std::move(batch));
			(void) t->commit();
		};
		bool full = false;
		for (int i = 0; i < 32; ++i) {
			pins.emplace_back(reader.openLogicalGraphReadSnapshot());
			try {
				replace('a' + i % 20);
			} catch (const std::length_error &e) {
				expect(std::string(e.what()).find("version memory budget") != std::string::npos, e.what());
				full = true;
				break;
			}
		}
		expect(full && pins.size() > 1, "Retained versions escaped their budget");
		expect(pins.front()->readNode(1).properties.front().value == graph::PropertyValue(std::string(4096, '0')),
			   "Old version changed");
		pins.clear();
		replace('z');
		selection.backend->close();
		return 0;
	}
	if (op == "hold-read" || op == "read-only") {
		auto sel = graph::storage::StorageBackendFactory::create(
				path, 8 * 1024 * 1024, graph::storage::OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
		sel.backend->open();
		expect(sel.backend->isReadOnly() && !sel.backend->capabilities().supports_read_write_transactions,
			   "Wrong read-only capability");
		if (op == "hold-read") {
			std::cout << "shared\n" << std::flush;
			char c;
			std::cin.get(c);
		}
		sel.backend->close();
		return 0;
	}
	zyx::Database db(path);
	db.open();
	if (op == "nested-unique") {
		query(db, "CREATE CONSTRAINT nested_key FOR(n:Composite) REQUIRE n.k IS UNIQUE");
		auto insert = [&](const std::string &expression) {
			auto tx = db.beginTransaction();
			query(tx, "CREATE (:Composite {k:" + expression + "})");
			tx.commit();
		};
		insert("{a:[], b:[1,NULL,{x:false}]}");
		bool duplicate = false;
		try {
			insert("{b:[1,NULL,{x:false}], a:[]}");
		} catch (const std::exception &e) {
			duplicate = std::string_view(e.what()).find("duplicate key") != std::string_view::npos;
		}
		expect(duplicate, "Reordered map evaded UNIQUE");
		insert("{a:[], b:[1.0,NULL,{x:false}]}");
		insert("{a:[], b:[NULL,1,{x:false}]}");
		db.save();
		db.close();
		db.open();
		expect(scalar(db, count) == 3, "Nested key type or list order lost");
		db.close();
		return 0;
	}
	if (op == "nested-write" || op == "nested-check") {
		auto read = [&](auto &handle, bool edge, int64_t expected) {
			auto r = query(handle, edge ? "MATCH ()-[r:NEST]->() RETURN r.payload" :
										"MATCH (n:Nested) RETURN n.payload");
			expect(r.hasNext(), "Missing nested property");
			r.next();
			// Result::get exposes the product C++ map text representation.
			auto expectedResult = query(handle, "RETURN {items:[" + std::to_string(expected) + ",NULL,{}]}");
			expect(expectedResult.hasNext(), "Missing literal result");
			expectedResult.next();
			expect(r.get(0) == expectedResult.get(0) && !r.hasNext(), "Wrong nested projection");
		};
		if (op == "nested-write") {
			auto w = db.beginTransaction();
			query(w, "CREATE (:Nested {payload:{items:[1,NULL,{}]}})-[:NEST {payload:{items:[2,NULL,{}]}}]->(:End)");
			read(w, false, 1);
			w.commit();
			auto old = db.beginReadOnlyTransaction();
			auto u = db.beginTransaction();
			query(u, "MATCH ()-[r:NEST]->() SET r.payload={items:[3,NULL,{}]}");
			read(u, true, 3);
			u.rollback();
			read(db, true, 2);
			{
				std::ofstream out(path + ".snapshot", std::ios::binary);
				db.exportSegmentedSnapshot(out);
				out.close();
				expect(out.good(), "Nested export failed");
			}
			auto v = db.beginTransaction();
			query(v, "MATCH (n:Nested) SET n.payload={items:[4,NULL,{}]}");
			v.commit();
			db.save();
			read(old, false, 1);
			old.commit();
			auto final = db.beginTransaction();
			query(final, "MATCH ()-[r:NEST]->() SET r.payload={items:[5,NULL,{}]}");
			final.commit();
			raise(SIGKILL);
		}
		bool exported = path.ends_with(".snapshot");
		read(db, false, exported ? 1 : 4);
		read(db, true, exported ? 2 : 5);
		db.close();
		return 0;
	}
	if (op == "checkpoint-locators") {
		auto orphanCount = [&] {
			size_t count = 0;
			for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
				std::error_code error;
				auto link = std::filesystem::read_symlink(entry.path(), error).string();
				count += !error && link.starts_with(path) && link.ends_with(" (deleted)");
			}
			return count;
		};
		auto payload = [](auto &handle, int64_t id) {
			auto r = handle.execute("MATCH (n:Large) WHERE id(n)=$i RETURN n.payload", {{"i", id}});
			expect(r.isSuccess() && r.hasNext(), "Missing checkpoint row");
			r.next();
			return std::get<std::string>(r.get(0));
		};
		auto put = [](auto &tx, size_t size, char value) {
			auto r = tx.execute("MATCH (n:Large) WHERE id(n)=1 SET n.payload=$s", {{"s", std::string(size, value)}});
			expect(r.isSuccess(), r.getError());
		};
		{
			auto old = db.beginReadOnlyTransaction();
			auto pending = db.beginTransaction();
			put(pending, 8192, 'p');
			db.save();
			expect(orphanCount() == 1 && payload(old, 1) == std::string(8192, 'z'), "Old base was released early");
			pending.commit();
			expect(payload(db, 320) == std::string(8192, '0'), "Pending transaction lost unchanged row");
			old.commit();
		}
		db.save();
		expect(orphanCount() == 0, "Current snapshot retained unlinked base after release");
		{
			auto w = db.beginTransaction();
			put(w, 43, 'q');
			w.commit();
		}
		db.save();
		expect(payload(db, 1) == std::string(43, 'q') && payload(db, 320) == std::string(8192, '0'),
			   "Moved row used an old offset in new image");
		expect(orphanCount() == 0, "Shifted rows retain a superseded inode");
		db.close();
		db.open();
		expect(payload(db, 320) == std::string(8192, '0'), "Checkpoint relocation failed reopen");
		db.close();
		return 0;
	}
	if (op == "large-leave") {
		for (int i = 0; i < 10; ++i) {
			auto tx = db.beginTransaction();
			std::vector<std::unordered_map<std::string, zyx::Value>> rows(32, {{"payload", std::string(8192, '0')}});
			tx.createNodes("Large", rows);
			tx.commit();
		}
		db.save();
		for (int i = 0; i < 80; ++i) {
			auto tx = db.beginTransaction();
			auto r = tx.execute("MATCH (n:Large) WHERE id(n)=1 SET n.payload=$s",
								{{"s", std::string(8192, i == 79 ? 'z' : 'a' + i % 20)}});
			expect(r.isSuccess(), r.getError());
			tx.commit();
		}
		std::cout << "ack" << std::endl;
		kill(getpid(), SIGKILL);
	}
	if (op == "unreachable-base") {
		auto a = db.beginTransaction();
		query(a, "CREATE (:A)");
		a.commit();
		auto pinned = db.beginReadOnlyTransaction();
		std::filesystem::rename(path + ".base", path + ".held");
		auto b = db.beginTransaction();
		query(b, "CREATE (:B)");
		bool rejected = false;
		try {
			b.commit();
		} catch (const std::exception &e) {
			rejected = std::string(e.what()).find("base") != std::string::npos;
		}
		std::filesystem::rename(path + ".held", path + ".base");
		expect(rejected, "Acknowledged an append while its required base name was missing");
		expect(scalar(pinned, count) == 1, "Already pinned reader lost its acknowledged snapshot");
		pinned.commit();
		bool blocked = false;
		try {
			auto tx = db.beginTransaction();
		} catch (const std::exception &) {
			blocked = true;
		}
		expect(blocked, "Writer continued after loss of a dependency");
		return 0;
	}
	if (op == "replace-path") {
		std::filesystem::copy_file(path, path + ".replacement");
		std::filesystem::rename(path + ".replacement", path);
		auto tx = db.beginTransaction();
		query(tx, "CREATE (:Lost)");
		bool rejected = false;
		try {
			tx.commit();
		} catch (const std::exception &e) {
			rejected = std::string(e.what()).find("base") != std::string::npos;
		}
		expect(rejected, "Acknowledged write against replaced base identity");
		expect(!std::filesystem::exists(path + ".wal"), "Uncertain base published a WAL");
		return 0;
	}
	if (op == "hold-failed-fence") {
		auto tx = db.beginTransaction();
		query(tx, "CREATE (:Confirmed)");
		tx.commit();
		setenv("ZYX_TEST_NATIVE_IO", "m.dirsync=eio", 1);
		bool rejected = false;
		try {
			db.save();
		} catch (const std::exception &error) {
			rejected = std::string(error.what()).find("sync native graph") != std::string::npos;
		}
		unsetenv("ZYX_TEST_NATIVE_IO");
		expect(rejected, "Injected publication fence unexpectedly succeeded");
		std::cout << "failed fence" << std::endl;
		std::cin.get();
		return 0;
	}
	if (op == "locked-checkpoint" || op == "locked-close") {
		auto old = db.beginReadOnlyTransaction();
		auto w = db.beginTransaction();
		query(w, "CREATE (:A)");
		w.commit();
		if (op == "locked-close") {
			old.commit();
			setenv("ZYX_TEST_NATIVE_IO", "w.unlinked=stop", 1);
			db.close();
			return 0;
		}
		setenv("ZYX_TEST_NATIVE_IO", argc > 3 ? argv[3] : "m.renamed=stop", 1);
		db.save();
		unsetenv("ZYX_TEST_NATIVE_IO");
		expect(scalar(old, count) == 0 && scalar(db, count) == 1, "Checkpoint changed transaction snapshot");
		old.commit();
		db.close();
		return 0;
	}
	if (op == "uncertain") {
		auto a = db.beginTransaction();
		(void) a.createNode("First");
		a.commit();
		auto b = db.beginTransaction();
		(void) b.createNode("Second");
		setenv("ZYX_TEST_NATIVE_IO", "m.dirsync=eio", 1);
		bool lost = false;
		try {
			b.commit();
		} catch (const std::exception &) {
			lost = true;
		}
		expect(lost, "Uncertain publish acknowledged");
		unsetenv("ZYX_TEST_NATIVE_IO");
		bool blocked = false;
		try {
			auto t = db.beginTransaction();
		} catch (const std::exception &) {
			blocked = true;
		}
		expect(blocked, "Used poisoned generation");
		try {
			db.close();
		} catch (const std::exception &) {
		}
		db.open();
		expect(scalar(db, count) == 2, "Wrong visible state after uncertain directory sync");
		auto c = db.beginTransaction();
		(void) c.createNode("AfterRecovery");
		c.commit();
		db.close();
		db.open();
		expect(scalar(db, count) == 3, "Failed recovery continuation");
		return 0;
	}
	if (op == "graph") {
		exerciseGraph(db, path);
		return 0;
	}
	if (op == "topology") {
		auto first = db.beginTransaction();
		auto a = first.createNode("A"), b = first.createNode("B");
		expect(first.createEdge(a, b, "Fan") == 1 && first.createEdge(a, b, "Fan") == 2 &&
					   first.createEdge(a, a, "Fan") == 3,
			   "Missing distinct relationship IDs");
		first.commit();
		auto old = db.beginReadOnlyTransaction();
		auto rejected = db.beginTransaction();
		auto error = rejected.execute("MATCH (n:A) DELETE n");
		expect(!error.isSuccess(), "Non-detach deletion succeeded");
		rejected.rollback();
		auto tx = db.beginTransaction();
		query(tx, "MATCH (n:A) DETACH DELETE n");
		tx.commit();
		expect(scalar(old, "MATCH ()-[r:Fan]->() RETURN count(r) AS c") == 3, "Pinned topology changed");
		old.commit();
		expect(scalar(db, count) == 1 && scalar(db, "MATCH ()-[r:Fan]->() RETURN count(r) AS c") == 0,
			   "Incomplete detach");
		auto next = db.beginTransaction();
		expect(next.createNode("C") == 3 && next.createEdge(b, b, "Loop") == 4, "Reused committed ID");
		next.commit();
		db.close();
		db.open();
		expect(scalar(db, "MATCH ()-[r:Loop]->() RETURN id(r) AS i") == 4, "Wrong reopened relationship ID");
		return 0;
	}
	if (op == "cap") {
		auto tx = db.beginTransaction();
		auto ids = tx.createNodes("Bound", std::vector<std::unordered_map<std::string, zyx::Value>>(4097));
		expect(ids.size() == 4097, "Byte-admitted rows missing");
		tx.commit();
		tx = db.beginTransaction();
		std::vector<std::unordered_map<std::string, zyx::Value>> wide(256, {{"s", std::string(12000, 'x')}});
		expect(tx.createNodes("Wide", wide).size() == wide.size(), "Admitted wide batch missing rows");
		tx.rollback();
		db.close();
		db.open();
		expect(scalar(db, count) == 4097, "Rolled back wide statement leaked");
		return 0;
	}
	if (op == "types") {
		std::string utf8;
		for (int i = 0; i < 2000; ++i)
			utf8 += "图";
		auto tx = db.beginTransaction();
		auto id = tx.createNode(std::vector<std::string>{"Owner", "Second"},
								{{"s", utf8}, {"b", true}, {"x", -0.0}, {"i", int64_t(9223372036854775806LL)}});
		expect(id == 1, "Incorrect stable node ID");
		tx.commit();
		db.close();
		db.open();
		auto r = query(db, "MATCH (n:Second) RETURN n.s,n.b,n.x,n.i,n.missing");
		expect(r.hasNext(), "Missing scalar node");
		r.next();
		expect(std::get<std::string>(r.get(0)) == utf8 && std::get<bool>(r.get(1)) &&
					   std::signbit(std::get<double>(r.get(2))) &&
					   std::get<int64_t>(r.get(3)) == 9223372036854775806LL &&
					   std::holds_alternative<std::monostate>(r.get(4)),
			   "Wrong scalar after reopen");
		auto bad = db.beginTransaction();
		bool rejected = false;
		try {
			(void) bad.createEdge(1, 999, "Bad");
		} catch (const std::exception &) {
			rejected = true;
		}
		expect(rejected, "Accepted dangling edge");
		bad.rollback();
		auto ddl = db.execute("CREATE CONSTRAINT experiment_key FOR (n:Owner) REQUIRE n.i IS UNIQUE");
		expect(ddl.isSuccess(), "Could not publish a single-property native UNIQUE");
		auto unsupported = db.execute("CREATE CONSTRAINT experiment_nn FOR (n:Owner) REQUIRE n.i IS NOT NULL");
		expect(!unsupported.isSuccess(), "Claimed an unsupported NOT NULL constraint");
		return 0;
	}
	if (op == "rollover-value") {
		expect(argc == 4 && std::string(argv[3]).size() == 1, "Missing last acknowledged value");
		expectRolloverValue(db, argv[3][0]);
		return 0;
	}
	if (op == "rollover") {
		auto seed = db.beginTransaction();
		std::vector<std::unordered_map<std::string, zyx::Value>> rows(120, {{"payload", std::string(8192, '0')}});
		(void) seed.createNodes("Roll", rows);
		seed.commit();
		auto old = db.beginReadOnlyTransaction();
		uintmax_t prior = 0;
		bool rolled = false;
		if (argc == 4)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		for (int i = 0; i < 26; ++i) {
			auto tx = db.beginTransaction();
			auto result = tx.execute("MATCH (n:Roll) SET n.payload=$s", {{"s", std::string(8192, 'a' + i)}});
			expect(result.isSuccess(), result.getError());
			tx.commit();
			if (argc == 4)
				std::cout << char('a' + i) << std::endl;
			auto size = std::filesystem::file_size(path + ".wal");
			rolled |= size < prior;
			prior = size;
		}
		expect(rolled, "Automatic WAL rollover did not run");

		expectRolloverValue(old, '0');
		expectRolloverValue(db, 'z');
		old.commit();
		db.close();
		db.open();
		expectRolloverValue(db, 'z');
		return 0;
	}
	if (op == "cycles") {
		auto old = db.beginReadOnlyTransaction();
		for (int cycle = 0; cycle < 12; ++cycle) {
			for (int i = 0; i < 2; ++i) {
				auto w = db.beginTransaction();
				query(w, "CREATE (:Epoch)");
				w.commit();
			}
			db.save();
			expect(!std::filesystem::exists(path + ".wal"), "Checkpoint retained WAL");
			expect(scalar(old, count) == 0 && scalar(db, count) == 2 * (cycle + 1), "Checkpoint changed snapshot view");
		}
		old.commit();
		db.close();
		db.open();
		expect(scalar(db, count) == 24, "Missing checkpoint state");
		return 0;
	}
	if (op == "exported") {
		expect(scalar(db, "MATCH (n:Person) WHERE n.v=7 RETURN id(n) AS id") == 1, "Export owner changed");
		expect(scalar(db, "MATCH ()-[r:LINK]->() RETURN id(r) AS id") == 1, "Export edge ID changed");
		expect(scalar(db, "MATCH ()-[r:LINK]->() RETURN r.w AS w") == 5, "Export edge value changed");
		expect(scalar(db, "MATCH (n:New) RETURN count(n) AS c") == 0, "Later creation leaked into export");
		return 0;
	}
	if (op == "count") {
		auto markers = scalar(db, "MATCH (n:Marker) RETURN count(n) AS c");
		auto echoes = scalar(db, "MATCH ()-[r:ECHO]->() RETURN count(r) AS c");
		expect(markers == echoes, "Partially replayed node/edge transaction");
		std::cout << scalar(db, count) << "\n";
		db.close();
		return 0;
	}
	if (op == "hold") {
		std::cout << "locked\n" << std::flush;
		char c;
		std::cin.get(c);
		return 0;
	}
	if (op == "checkpoint") {
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		db.save();
		db.close();
		return 0;
	}
	if (op == "columnar-chunks") {
		auto seed = db.beginBulkTransaction();
		const auto id = seed.createNode("Endpoint");
		seed.commit();
		const size_t rows = 90000;
		std::vector<int64_t> sources(rows, id), targets(rows, id);
		std::vector<zyx::PropertyColumn> columns{{"row", {}}};
		for (size_t row = 0; row < rows; ++row)
			columns.front().values.emplace_back(int64_t(row));
		targets.back() = INT64_MAX;
		try {
			(void) db.createEdgesColumnar("DIRECT", sources, targets, columns);
			throw std::runtime_error("Accepted invalid final columnar endpoint");
		} catch (const zyx::BulkInputError &e) {
			expect(e.rowIndex() == rows - 1 && e.fieldPath() == "target_id", "Lost global row ordinal");
		}
		targets.back() = id;
		const auto ids = db.createEdgesColumnar("DIRECT", sources, targets, columns);
		expect(ids.size() == rows, "Direct ID count");
		for (size_t row = 0; row < rows; ++row)
			expect(ids[row] == int64_t(row + 1), "Noncontiguous direct IDs after rejected input");
		std::filesystem::copy_file(path, path + ".copy");
		return 0;
	}
	if (op == "columnar-verify-chunks") {
		db.close();
		using namespace graph::storage;
		auto selected = StorageBackendFactory::create(path, 64U << 20, OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
		selected.backend->open();
		auto snapshot = dynamic_cast<LogicalGraphReadBackend &>(*selected.backend).openLogicalGraphReadSnapshot();
		size_t rows = 0;
		snapshot->visitEdges(90001, [&](const LogicalGraphEdge &edge) {
			expect(edge.type == "DIRECT" && edge.source_node_id == 1 && edge.target_node_id == 1 &&
							edge.properties.size() == 1 && edge.properties.front().key == "row" &&
							std::get<int64_t>(edge.properties.front().value.getVariant()) == int64_t(edge.edge_id - 1),
				   "Reopened chunk belongs to a different edge");
			++rows;
			return true;
		});
		expect(rows == 90000, "Lost direct edges on reopen");
		return 0;
	}
	if (op == "columnar-edges") {
		auto seed = db.beginBulkTransaction();
		const auto a = seed.createNode("Endpoint"), b = seed.createNode("Endpoint");
		seed.commit();
		for (bool readOnly: {true, false}) {
			auto active = readOnly ? db.beginReadOnlyTransaction() : db.beginTransaction();
			expect(db.createEdgesColumnar("DIRECT", {}, {}).empty(), "Empty direct batch must succeed");
			bool refused = false;
			try {
				(void) db.createEdgesColumnar("DIRECT", {a}, {b});
			} catch (const std::logic_error &) {
				refused = true;
			}
			expect(refused, "Direct write accepted with active transaction");
			active.rollback();
		}
		try {
			(void) db.createEdgesColumnar("DIRECT", {a, b}, {b, INT64_MAX});
			throw std::runtime_error("Invalid endpoint accepted");
		} catch (const zyx::BulkInputError &e) {
			expect(e.rowIndex() == 1 && e.fieldPath() == "target_id", "Wrong invalid endpoint diagnostic");
		}
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		auto ids = db.createEdgesColumnar("DIRECT", {a, a, b, a}, {b, b, a, a},
										  {{"i", {int64_t(0), int64_t(1), int64_t(2), int64_t(3)}}});
		expect(ids == std::vector<int64_t>({1, 2, 3, 4}), "Unexpected direct edge IDs");
		std::filesystem::copy_file(path, path + ".copy");
		std::cout << "ack\n" << std::flush;
		kill(getpid(), SIGKILL);
	}
	if (op == "direct-count") {
		std::cout << scalar(db, "MATCH ()-[e:DIRECT]->() RETURN count(e)") << "\n";
		return 0;
	}
	if (op == "bulk-publish" || op == "bulk-publish-clean") {
		auto prefix = db.beginTransaction();
		auto first = prefix.createNode("Prior");
		prefix.commit();
		if (op == "bulk-publish-clean")
			db.save();
		auto old = db.beginReadOnlyTransaction();
		auto bulk = db.beginBulkTransaction();
		const auto last = bulk.createNode("Marker", {{"v", int64_t(42)}});
		expect(bulk.createEdge(first, last, "ECHO", {{"w", int64_t(5)}}) > 0, "Invalid edge ID");
		expect(scalar(bulk, count) == 2 && scalar(old, count) == 1, "Bulk staging visibility");
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		bulk.commit();
		expect(scalar(old, count) == 1 && scalar(db, count) == 2, "Bulk snapshot isolation");
		expect(!std::filesystem::exists(path + ".wal") && !std::filesystem::exists(path + ".base"),
			   "Bulk acknowledgement still requires dependencies");
		std::filesystem::copy_file(path, path + ".copy");
		std::cout << "ack\n" << std::flush;
		kill(getpid(), SIGKILL);
	}
	if (op == "chain") {
		auto prefix = db.beginTransaction();
		query(prefix, "CREATE (:Prior {v: 1})");
		prefix.commit();
		std::filesystem::copy_file(path, path + ".prior");
		std::filesystem::copy_file(path + ".wal", path + ".prior-wal");
	}
	auto tx = db.beginTransaction();
	(void) query(tx, "CREATE (:Marker {v: 42})");
	(void) query(tx, "MATCH (n:Marker) CREATE (n)-[:ECHO {w: 5}]->(n)");
	if (op != "chain")
		db.save();
	if (argc > 3)
		setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
	tx.commit();
	if (op == "leave" || op == "chain") {
		std::cout << "ack\n" << std::flush;
		kill(getpid(), SIGKILL);
	}
	db.close();
	return 0;
} catch (const std::exception &e) {
	std::cerr << e.what() << "\n";
	return 2;
}
