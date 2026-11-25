#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/NativeWalDatabase.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/StorageBackendFactory.hpp"
#include "zyx/zyx.hpp"

namespace {
	void expect(bool ok, const std::string &message) {
		if (!ok)
			throw std::runtime_error(message);
	}
	auto query(auto &connection, const std::string &statement) {
		auto result = connection.execute(statement);
		expect(result.isSuccess(), statement + ": " + result.getError());
		return result;
	}
	void scalar(auto &connection, const std::string &statement, int64_t expected) {
		auto r = query(connection, statement);
		expect(r.hasNext(), statement + " missing");
		r.next();
		expect(r.get(0) == zyx::Value(expected) && !r.hasNext(), statement + " wrong value");
	}
	void write(zyx::Database &db, const std::string &q) {
		auto tx = db.beginTransaction();
		query(tx, q);
		tx.commit();
	}
	const std::string oldKey = "MATCH(n:K) WHERE n.k=17 RETURN id(n)";
	const std::string newKey = "MATCH(n:K) WHERE n.k=23 RETURN id(n)";
	void empty(auto &c, const std::string &q) {
		auto r = query(c, q);
		expect(!r.hasNext(), q + " surplus row");
	}
	void mutate(auto &tx) {
		query(tx, "MATCH(n:K) WHERE n.k=17 SET n.k=23, n.future=87");
		scalar(tx, "MATCH(n:K) WHERE n.k=23 RETURN n.future", 87);
		empty(tx, oldKey);
	}
} // namespace
int main(int argc, char **argv) try {
	expect(argc == 3 || argc == 4, "indexed OP PATH");
	std::string op = argv[1], path = argv[2];
	if (op == "create") {
		std::cout << graph::storage::createIndexedNativeWalDatabase(path, "K", "k") << "\n";
		return 0;
	}
	if (op == "resident-budget") {
		using namespace graph::storage;
		auto backend = StorageBackendFactory::create(path, 8 << 20, OpenMode::OPEN_EXISTING_FILE).backend;
		backend->open();
		auto snapshot = std::dynamic_pointer_cast<LogicalGraphReadBackend>(backend)->openLogicalGraphReadSnapshot();
		size_t calls = 0;
		auto probe = [&](int64_t key, size_t nodes, size_t fields, size_t bytes, size_t expected, bool consume) {
			calls = 0;
			return snapshot->visitNodeCandidatesByExactProperties(
				{"K"}, {{"k", graph::PropertyValue(key)}}, nodes, fields, bytes,
				[&](const IndexCandidateBatch &batch) {
					++calls;
					expect(batch.candidates.size() == expected, "resident candidate count");
					return consume;
				});
		};
		expect(!probe(-17, 700, 2048, 4096, 0, true).has_value() && !calls, "node budget");
		expect(!probe(-17, 701, 701, 4096, 0, true).has_value() && !calls, "field budget");
		expect(!probe(-17, 701, 702, 0, 0, true).has_value() && !calls, "byte budget");
		expect(probe(-17, 701, 702, 4096, 2, false) == false && calls == 1, "consumer stop");
		expect(probe(-701, 701, 702, 4096, 0, true) == true && calls == 1, "complete empty");
		return 0;
	}
	zyx::Database db(path);
	db.open();
	if (op == "seed") {
		write(db, "CREATE (:K {k:17})-[:T {p:5}]->(:K {k:19})");
		write(db, "CREATE (:K), (:K {k:null})");
		db.save();
	} else if (op == "resident-seed") {
		for (int64_t first = 0; first < 2048; first += 128) {
			auto tx = db.beginTransaction();
			for (auto i = first; i < first + 128; ++i)
				(void) tx.createNode("K", {{"k", i}, {"padding", std::string(240, 'p')}});
			tx.commit();
		}
		db.save();
	} else if (op == "resident-deletions") {
		auto old = db.beginReadOnlyTransaction();
		write(db, "MATCH(n:K) WHERE n.k>=0 AND n.k<75 DETACH DELETE n");
		graph::debug::PerfTrace::setEnabled(true);
		graph::debug::PerfTrace::reset();
		scalar(db, "MATCH(n:K) WHERE n.k=17 RETURN count(n)", 0);
		auto trace = graph::debug::PerfTrace::snapshotAndReset();
		expect(trace["native_wal.index.probes"].totalValue == 1, "deletion-only index route");
		expect(trace["native_wal.index.resident_rows"].totalValue == 0, "deletions became resident");
		scalar(old, "MATCH(n:K) WHERE n.k=17 RETURN count(n)", 1);
		old.commit();
		db.save();
		db.close();
		db.open();
		scalar(db, "MATCH(n:K) WHERE n.k=17 RETURN count(n)", 0);
	} else if (op == "resident-kill" || op == "resident-replay") {
		const std::string key = "MATCH(n:K) WHERE n.k=-17 RETURN count(n)";
		const std::string gone = "MATCH(n:K) WHERE n.k=42 RETURN count(n)";
		auto verify = [&] {
			graph::debug::PerfTrace::setEnabled(true);
			graph::debug::PerfTrace::reset();
			scalar(db, key, 2);
			auto trace = graph::debug::PerfTrace::snapshotAndReset();
			expect(trace["native_wal.index.resident_rows"].totalValue == 701, "delta route lost or repeated a row");
			scalar(db, gone, 0);
			scalar(db, "MATCH(n:K) WHERE n.k>=-19 AND n.k<=-17 RETURN count(n)", 4);
			scalar(db, "MATCH(n:K) WHERE n.k=43 RETURN count(n)", 0);
			scalar(db, "MATCH(n:K) WHERE n.k=1001 RETURN id(n)", 1002);
		};
		if (op == "resident-kill") {
			auto old = db.beginReadOnlyTransaction();
			auto change = [&](auto &tx) {
				query(tx, "MATCH(n:K) WHERE n.k=42 SET n.k=-17");
				query(tx, "MATCH(n:K) WHERE n.k=43 DETACH DELETE n");
				std::vector<std::unordered_map<std::string, zyx::Value>> rows;
				for (int64_t k = -700; k < 0; ++k)
					rows.push_back({{"k", k}});
				(void) tx.createNodes("K", rows);
				scalar(tx, key, 2);
				scalar(tx, gone, 0);
			};
			auto discarded = db.beginTransaction();
			change(discarded);
			discarded.rollback();
			scalar(db, key, 0);
			auto tx = db.beginTransaction();
			change(tx);
			tx.commit();
			verify();
			scalar(old, key, 0);
			scalar(old, gone, 1);
			std::cout << "ack" << std::endl;
			std::raise(SIGKILL);
		}
		verify();
		auto before = db.beginReadOnlyTransaction();
		write(db, "CREATE (:K {k:'unindexed'})");
		graph::debug::PerfTrace::reset();
		scalar(db, key, 2);
		expect(graph::debug::PerfTrace::snapshotAndReset()["native_wal.index.probes"].totalValue == 0,
			   "incompatible resident value did not fall back");
		scalar(before, key, 2);
		db.save();
		scalar(before, key, 2);
		before.commit();
		db.close();
		db.open();
		scalar(db, key, 2);
	} else if (op == "fault-commit" || op == "fault-checkpoint") {
		expect(argc == 4, "fault event required");
		auto tx = db.beginTransaction();
		mutate(tx);
		if (op == "fault-checkpoint")
			tx.commit();
		setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		try {
			if (op == "fault-checkpoint")
				db.save();
			else
				tx.commit();
		} catch (const std::exception &e) {
			std::cerr << e.what() << "\n";
			std::_Exit(2);
		}
		std::_Exit(0);
	} else if (op == "old") {
		scalar(db, oldKey, 1);
		empty(db, newKey);
	} else if (op == "new") {
		empty(db, oldKey);
		scalar(db, newKey, 1);
		scalar(db, "MATCH(n:K) WHERE n.k=23 RETURN n.future", 87);
	} else if (op == "mixed") {
		auto mixed = db.beginTransaction();
		(void) mixed.createNode("K", {{"k", 17.0}});
		mixed.commit();
		for (int i = 0; i < 2; ++i) {
			scalar(db, "MATCH(n:K) WHERE n.k=17 RETURN count(n)", 1);
			db.save();
		}
		auto big = db.beginTransaction();
		(void) big.createNode("K", {{"k", int64_t(9007199254740993LL)}});
		big.commit();
		db.save();
		auto q = db.execute("MATCH(n:K) WHERE id(n)=6 RETURN n.k");
		expect(q.isSuccess() && q.hasNext(), "large value absent");
		q.next();
		expect(q.get(0) == zyx::Value(int64_t(9007199254740993LL)), "large value rounded");
	} else if (op == "merge-crash" || op == "merge-reopen") {
		const std::string matches = "MATCH(n:K) WHERE n.k=71 RETURN id(n) ORDER BY id(n)";
		auto verify = [&](auto &handle) {
			auto result = query(handle, matches);
			for (int64_t id: {1, 5}) {
				expect(result.hasNext(), "overlay omitted new key");
				result.next();
				expect(result.get(0) == zyx::Value(id), "overlay repeated or stale owner");
			}
			expect(!result.hasNext(), "overlay surplus");
			empty(handle, oldKey);
			empty(handle, "MATCH(n:K) WHERE n.k=19 RETURN id(n)");
			auto first = query(handle, "MATCH(n:K) WHERE n.k=71 RETURN id(n) LIMIT 1");
			expect(first.hasNext(), "prematurely stopped overlay");
			first.next();
			expect(!first.hasNext(), "consumer limit ignored");
		};
		if (op == "merge-crash") {
			auto old = db.beginReadOnlyTransaction();
			empty(old, matches);
			auto tx = db.beginTransaction();
			query(tx, "MATCH(n:K) WHERE n.k=17 SET n.k=71");
			query(tx, "MATCH(n:K) WHERE n.k=19 DETACH DELETE n");
			query(tx, "CREATE (:K {k:71})");
			verify(tx);
			empty(old, matches);
			scalar(old, oldKey, 1);
			tx.commit();
			verify(db);
			empty(old, matches);
			scalar(old, oldKey, 1);
			std::cout << "ack" << std::endl;
			std::raise(SIGKILL);
		}
		verify(db);
		db.save();
		verify(db);
	} else if (op == "change") {
		auto old = db.beginReadOnlyTransaction();
		auto w = db.beginTransaction();
		mutate(w);
		w.rollback();
		scalar(db, oldKey, 1);
		empty(db, newKey);
		auto w2 = db.beginTransaction();
		mutate(w2);
		w2.commit();
		scalar(old, oldKey, 1);
		empty(old, newKey);
		scalar(db, newKey, 1);
		empty(db, oldKey);
		{
			std::ofstream out(path + ".A", std::ios::binary);
			(void) db.publishColumnSnapshot(old, out);
			out.close();
			expect(bool(out), "export");
		}
		db.save();
		scalar(old, oldKey, 1);
		old.commit();
		write(db, "MATCH(n:K) WHERE n.k=19 DETACH DELETE n");
		write(db, "CREATE (:K {k:19})");
		scalar(db, "MATCH(n:K) WHERE n.k=19 RETURN id(n)", 5);
		std::_Exit(0); // recover the final two confirmed deltas in a new process
	} else if (op == "verify") {
		empty(db, oldKey);
		scalar(db, newKey, 1);
		scalar(db, "MATCH(n:K) WHERE n.k=19 RETURN id(n)", 5);
		scalar(db, "MATCH(n:K) WHERE n.k=23 RETURN n.future", 87);
		scalar(db, "MATCH ()-[r:T]->() RETURN count(r)", 0);
		scalar(db, "MATCH(n:K) WHERE n.k IS NULL RETURN count(n)", 2);
	} else if (op == "export") {
		scalar(db, oldKey, 1);
		empty(db, newKey);
		scalar(db, "MATCH ()-[r:T]->() RETURN r.p", 5);
	} else
		throw std::runtime_error("unknown operation");
	db.close();
	return 0;
} catch (const std::exception &e) {
	std::cerr << e.what() << "\n";
	return 1;
}
