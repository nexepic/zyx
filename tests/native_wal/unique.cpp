#include <csignal>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include "graph/storage/LogicalGraphSchema.hpp"
#include "graph/storage/NativeWalDatabase.hpp"
#include "graph/storage/StorageBackendFactory.hpp"
#include "zyx/zyx.hpp"

namespace {
	const std::string ddl = "CREATE CONSTRAINT unique_k FOR (n:Item) REQUIRE n.k IS UNIQUE";
	void check(bool ok, const std::string &reason) {
		if (!ok)
			throw std::runtime_error(reason);
	}
	auto execute(auto &db, const std::string &s) {
		auto r = db.execute(s);
		check(r.isSuccess(), s + " " + r.getError());
		return r;
	}
	void count(auto &db, const std::string &predicate, int64_t expected) {
		auto r = execute(db, "MATCH(n:Item) WHERE " + predicate + " RETURN count(n)");
		check(r.hasNext(), "missing count");
		r.next();
		check(r.get(0) == zyx::Value(expected) && !r.hasNext(), predicate);
	}
	bool duplicate(zyx::Database &db) {
		auto tx = db.beginTransaction();
		execute(tx, "CREATE (:Item {k:1})");
		try {
			tx.commit();
			return false;
		} catch (const std::exception &e) {
			check(std::string(e.what()).find("duplicate key") != std::string::npos, e.what());
			return true;
		}
	}

	auto labelledMembers(auto &db, const std::string &label) {
		auto result = execute(db, "MATCH(n:" + label + ") RETURN id(n),n.tag,n.k");
		std::set<std::pair<int64_t, std::string>> members;
		while (result.hasNext()) {
			result.next();
			check(result.get(2) == zyx::Value(int64_t(77)), "key changed");
			check(members.emplace(std::get<int64_t>(result.get(0)), std::get<std::string>(result.get(1))).second,
				  "duplicate output");
		}
		return members;
	}
	void rejectLabelAddition(zyx::Database &db, const std::string &tag) {
		auto tx = db.beginTransaction();
		execute(tx, "MATCH(n:Seed {tag:'" + tag + "'}) SET n:Item");
		check(labelledMembers(tx, "Item").size() == 2, "private label not visible");
		try {
			tx.commit();
		} catch (const std::exception &e) {
			check(std::string(e.what()).find("duplicate key") != std::string::npos, e.what());
			return;
		}
		throw std::runtime_error("duplicate label membership committed");
	}
	void checkLabelTransfer(zyx::Database &db) {
		auto seed = db.beginTransaction();
		execute(seed, "CREATE (:Seed:Item {k:77,tag:'A'}),(:Seed {k:77,tag:'B'})");
		seed.commit();
		execute(db, ddl);
		auto old = db.beginReadOnlyTransaction();
		const auto before = labelledMembers(old, "Item");
		const auto both = labelledMembers(old, "Seed");
		check(before.size() == 1 && before.begin()->second == "A" && both.size() == 2, "invalid initial graph");
		auto after = both;
		after.erase(*before.begin());
		check(after.size() == 1 && after.begin()->second == "B", "missing B");

		rejectLabelAddition(db, "B");
		check(labelledMembers(db, "Item") == before, "rejection changed graph");
		auto tx = db.beginTransaction();
		execute(tx, "MATCH(n:Seed {tag:'B'}) SET n:Item");
		check(labelledMembers(tx, "Item") == both, "private add");
		execute(tx, "MATCH(n:Seed {tag:'A'}) REMOVE n:Item");
		check(labelledMembers(tx, "Item") == after, "private remove");
		tx.commit();
		check(labelledMembers(old, "Item") == before && labelledMembers(db, "Item") == after, "old/current mismatch");
		db.save();
		check(labelledMembers(old, "Item") == before && labelledMembers(db, "Item") == after, "checkpoint mismatch");
		old.commit();
		db.close();
		db.open();
		check(labelledMembers(db, "Item") == after && labelledMembers(db, "Seed") == both, "reopen mismatch");
		rejectLabelAddition(db, "A");
		check(labelledMembers(db, "Item") == after, "second rejection changed graph");
	}
} // namespace
int main(int argc, char **argv) try {
	std::string op = argv[1], p = argv[2];
	if (op == "unindexed") {
		std::cout << graph::storage::createNativeWalDatabase(p) << "\n";
		return 0;
	}
	if (op == "init") {
		std::cout << graph::storage::createIndexedNativeWalDatabase(p, "Item", "k") << "\n";
		return 0;
	}
	if (op == "cancel") {
		using namespace graph::storage;
		auto backend = StorageBackendFactory::create(p, 8 << 20, OpenMode::OPEN_EXISTING_FILE).backend;
		backend->open();
		auto schema = std::dynamic_pointer_cast<LogicalGraphSchemaBackend>(backend);
		check(bool(schema), "missing schema service");
		LogicalGraphConstraintCreateRequest req;
		req.name = "unique_k";
		req.label = "Item";
		req.property_keys = {"k"};
		for (int failure: {2, 6}) {
			int calls = 0;
			bool interrupted = false;
			try {
				(void) schema->createLogicalConstraintWithCheckpoint(req, [&] {
					if (++calls == failure)
						throw std::length_error("test cancellation");
				});
			} catch (const std::length_error &e) {
				interrupted = std::string(e.what()) == "test cancellation";
			}
			check(calls == failure && interrupted && schema->listLogicalConstraints(1).empty(),
				  "cancelled declaration published");
		}
		auto d = schema->createLogicalConstraint(req);
		check(d.constraint_id > 0, "missing durable revision");
		return 0;
	}
	zyx::Database db(p);
	db.open();
	if (op == "labels") {
		checkLabelTransfer(db);
		return 0;
	}
	if (op == "undeclared") {
		auto r = db.execute(ddl);
		check(!r.isSuccess() && r.getError().find("requires its declared") != std::string::npos,
			  "installed constraint without the declared index");
		auto tx = db.beginTransaction();
		execute(tx, "CREATE (:Item {k:1}),(:Item {k:1})");
		tx.commit();
		db.close();
		return 0;
	}
	if (op == "duplicate-in-base") {
		auto w = db.beginTransaction();
		execute(w, "CREATE (:Item {k:1}),(:Item {k:1})");
		w.commit();
		db.close();
		return 0;
	}
	if (op == "large-delta") {
		// Exceed sparse admission, with a duplicate among late incoming keys.
		auto tx = db.beginTransaction();
		std::vector<std::unordered_map<std::string, zyx::Value>> rows;
		for (int64_t i = 0; i < 4097; ++i)
			rows.push_back({{"k", i == 4096 ? int64_t{100} : 100 + i}});
		(void) tx.createNodes("Item", rows);
		bool rejected = false;
		try {
			tx.commit();
		} catch (const std::exception &e) {
			rejected = std::string(e.what()).find("duplicate key") != std::string::npos;
		}
		check(rejected, "large duplicate batch escaped the full validator");
		count(db, "n.k=100", 0);
		check(duplicate(db), "failed batch changed uniqueness");
		db.close();
		return 0;
	}
	if (op == "prefix") {
		for (auto key: {"4328719365", "4"}) {
			auto w = db.beginTransaction();
			execute(w, std::string("MATCH(n:Item) WHERE id(n)=2 SET n.k=") + key);
			w.commit();
		}
		std::_Exit(0);
	}
	if (op == "accept-prefix") {
		count(db, "n.k=4", 1);
		check(duplicate(db), "restored constraint absent");
		db.close();
		return 0;
	}
	if (op == "owner-limit") {
		for (int64_t base: {100, 2200}) {
			auto tx = db.beginTransaction();
			std::vector<std::unordered_map<std::string, zyx::Value>> rows;
			for (int64_t i = base; i < base + 2100; ++i)
				rows.push_back({{"k", i}});
			(void) tx.createNodes("Item", rows);
			tx.commit();
		}
		auto conflict = db.beginTransaction();
		execute(conflict, "CREATE (:Item {k:4299})");
		bool duplicateKey = false;
		try {
			conflict.commit();
		} catch (const std::exception &e) {
			duplicateKey = std::string(e.what()).find("duplicate key") != std::string::npos;
		}
		check(duplicateKey, "owner admission limit hid a committed claim");
		db.save();
		count(db, "n.k=4299", 1);
		return 0;
	}
	if (op == "overlay") {
		auto tx = db.beginTransaction();
		execute(tx, "MATCH(n:Item) WHERE n.k=1 SET n.k=91");
		tx.commit();
		auto bad = db.beginTransaction();
		execute(bad, "CREATE (:Item {k:91})");
		bool rejected = false;
		try {
			bad.commit();
		} catch (const std::exception &e) {
			rejected = std::string(e.what()).find("duplicate key") != std::string::npos;
		}
		check(rejected, "empty base hid confirmed owner");
		auto swap = db.beginTransaction();
		execute(swap, "MATCH(n:Item) WHERE id(n)=2 SET n.k=91");
		db.save();
		execute(swap, "MATCH(n:Item) WHERE id(n)=1 SET n.k=2");
		swap.commit();
		auto del = db.beginTransaction();
		execute(del, "MATCH(n:Item) WHERE n.k=91 DETACH DELETE n");
		execute(del, "CREATE (:Item {k:91})");
		del.commit();
		count(db, "n.k=91", 1);
		db.close();
		return 0;
	}
	if (op == "seed" || op == "dirty-seed") {
		auto w = db.beginTransaction();
		execute(w, "CREATE (:Item {k:1,v:0}),(:Item {k:2}),(:Item),(:Item {k:null})");
		w.commit();
		if (op == "dirty-seed")
			std::_Exit(0);
		db.close();
		return 0;
	}
	if (op == "ddl") {
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		auto r = db.execute(ddl);
		if (!r.isSuccess()) {
			std::cerr << r.getError() << std::endl;
			std::_Exit(2);
		}
		std::_Exit(0);
	}
	if (op == "checkpoint") {
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		try {
			db.save();
		} catch (const std::exception &e) {
			std::cerr << e.what() << std::endl;
			std::_Exit(2);
		}
		std::_Exit(0);
	}
	if (op == "change") {
		auto w = db.beginTransaction();
		execute(w, "MATCH(n:Item) WHERE n.k=2 SET n.k=3");
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		try {
			w.commit();
		} catch (const std::exception &e) {
			std::cerr << e.what() << std::endl;
			std::_Exit(2);
		}
		std::_Exit(0);
	}
	if (op == "old" || op == "defined" || op == "changed") {
		count(db, "n.k=1", 1);
		count(db, "n.k=2", op == "changed" ? 0 : 1);
		count(db, "n.k=3", op == "changed" ? 1 : 0);
		check(duplicate(db) == (op != "old"), "wrong durable unique");
		db.close();
		return 0;
	}
	if (op == "semantics") {
		auto prior = db.beginReadOnlyTransaction();
		auto a = db.beginTransaction();
		execute(a, "CREATE (:Item {k:1})");
		a.commit();
		auto rejected = db.execute(ddl);
		check(!rejected.isSuccess() && rejected.getError().find("duplicate key") != std::string::npos,
			  "bad DDL accepted");
		auto cleanup = db.beginTransaction();
		execute(cleanup, "MATCH(n:Item) WHERE id(n)=5 DETACH DELETE n");
		cleanup.commit();
		auto staged = db.beginTransaction();
		execute(staged, "CREATE (:Item {k:1})");
		execute(db, ddl);
		bool stale = false;
		try {
			staged.commit();
		} catch (const std::exception &e) {
			stale = true;
		}
		check(stale, "predating writer committed");
		count(prior, "n.k=1", 1);
		auto w = db.beginTransaction();
		execute(w, "MATCH(n:Item) WHERE id(n)=1 SET n.k=2");
		count(w, "n.k=2", 2);
		execute(w, "MATCH(n:Item) WHERE id(n)=2 SET n.k=1");
		w.commit();
		auto redo = db.beginTransaction();
		execute(redo, "MATCH(n:Item) WHERE n.k=1 DETACH DELETE n");
		execute(redo, "CREATE (:Item {k:1}),(:Item),(:Item {k:null})");
		redo.commit();
		count(db, "n.k IS NULL", 4);
		auto l = db.beginTransaction(), r = db.beginTransaction();
		execute(l, "CREATE (:Item {k:7})");
		execute(r, "CREATE (:Item {k:7})");
		l.commit();
		stale = false;
		try {
			r.commit();
		} catch (const std::exception &) {
			stale = true;
		}
		check(stale, "competing writer committed");
		auto obsolete = db.beginTransaction();
		execute(obsolete, "MATCH(n:Item) WHERE n.k=2 SET n.v=9");
		for (auto v: {7, 0}) {
			auto tx = db.beginTransaction();
			execute(tx, "MATCH(n:Item) WHERE n.k=2 SET n.v=" + std::to_string(v));
			tx.commit();
		}
		db.save();
		stale = false;
		try {
			obsolete.commit();
		} catch (const std::exception &) {
			stale = true;
		}
		check(stale, "ABA admitted");
		count(prior, "n.k IS NULL", 2);
		prior.commit();
		check(duplicate(db), "duplicate accepted");
		auto tx = db.beginTransaction();
		execute(tx, "CREATE (:Item {k:12})");
		tx.commit();
		std::cout << "ack" << std::endl;
		std::raise(SIGKILL);
	}
	if (op == "replay") {
		count(db, "n.k=12", 1);
		count(db, "n.k IS NULL", 4);
		check(duplicate(db), "constraint missing after WAL");
		db.save();
		db.close();
		db.open();
		check(duplicate(db), "constraint missing after clean reopen");
		return 0;
	}
	throw std::runtime_error("unknown test");
} catch (const std::exception &e) {
	std::cerr << e.what() << "\n";
	return 1;
}
