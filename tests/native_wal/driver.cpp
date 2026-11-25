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
	if (op == "init") {
		std::cout << graph::storage::createNativeWalDatabase(path) << "\n";
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
		bool refused = false;
		try {
			std::vector<std::unordered_map<std::string, zyx::Value>> wide(256, {{"s", std::string(12000, 'x')}});
			(void) tx.createNodes("Wide", wide);
		} catch (const std::exception &) {
			refused = true;
		}
		expect(refused, "Entity cap was ignored");
		tx.rollback();
		db.close();
		db.open();
		expect(scalar(db, count) == 4097, "Oversized statement leaked");
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
		expect(!ddl.isSuccess(), "Claimed an unsupported persistent constraint");
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
