#include <csignal>
#include <cstdlib>
#include <iostream>
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
} // namespace
int main(int argc, char **argv) try {
	std::string op = argv[1], p = argv[2];
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
