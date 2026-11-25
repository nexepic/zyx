#include "confirmation_budget.hpp"
#include "projection_budget.hpp"
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
	// One checkpoint fits the old geometric reservations. The second must
	// still fit after retaining the original indexed snapshot and its posting list.
	void checkpointWithPinnedProjection(const std::string &path) {
		using namespace graph::storage;
		{
			zyx::Database seed(path);
			seed.open();
			for (int64_t first = 0; first < 10000; first += 250) {
				std::vector<std::unordered_map<std::string, zyx::Value>> rows;
				for (int64_t id = first; id < first + 250; ++id)
					rows.push_back({{"k", id}, {"v", int64_t(0)}});
				auto tx = seed.beginTransaction();
				(void) tx.createNodes("Item", rows);
				tx.commit();
			}
			execute(seed, ddl);
			seed.close();
		}
		auto backend = StorageBackendFactory::create(path, 6U << 20, OpenMode::OPEN_EXISTING_FILE).backend;
		backend->open();
		auto &reader = dynamic_cast<LogicalGraphReadBackend &>(*backend);
		auto &writer = dynamic_cast<LogicalGraphTransactionalBackend &>(*backend);
		auto old = reader.openLogicalGraphReadSnapshot();
		auto readKey = [](const auto &view) {
			auto row = view.readNode(1);
			auto key = std::ranges::find(row.properties, "k", &LogicalGraphPropertyEntry::key);
			if (key == row.properties.end())
				throw std::runtime_error("Checkpoint lost the indexed key");
			return key->value;
		};
		// After the numeric checkpoints an incomplete base must permit a
		// mixed key transition without retaining one canonical key per row.
		for (const auto &key : {graph::PropertyValue(int64_t(-1)), graph::PropertyValue(int64_t(-2)),
							graph::PropertyValue(std::string("mixed")), graph::PropertyValue(int64_t(-3)),
							graph::PropertyValue(std::string("mixed"))}) {
			LogicalGraphMutationEntityReference first;
			first.value = 1;
			LogicalGraphMutationBatch changes;
			changes.property_replacements.push_back({first, {{"k", key}}});
			auto tx = writer.beginLogicalGraphTransaction(false);
			(void) tx->stageMutationBatch(std::move(changes));
			(void) tx->commit();
			tx.reset();
			backend->flush();
			check(readKey(*old) == graph::PropertyValue(int64_t(0)) && readKey(*reader.openLogicalGraphReadSnapshot()) == key,
				  "Checkpoint changed a pinned or current indexed row");
		}
		old.reset();
		backend->close();
	}
} // namespace
int main(int argc, char **argv) try {
	std::string op = argv[1], p = argv[2];
	if (op == "pinned-projection") {
		checkpointWithPinnedProjection(p);
		return 0;
	}
	if (op == "projection-budget") {
		projection_test::exactBudget();
		return 0;
	}
	if (op == "deny-create" || op == "deny-drop") {
		confirmation_test::deny(p, op == "deny-create" ? ddl : "DROP CONSTRAINT unique_k", op == "deny-drop");
		return 0;
	}
	if (op == "unindexed") {
		std::cout << graph::storage::createNativeWalDatabase(p) << "\n";
		return 0;
	}
	if (op == "other-index") {
		std::cout << graph::storage::createIndexedNativeWalDatabase(p, "Other", "j") << "\n";
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
	if (op == "dynamic") {
		const std::string drop = "DROP CONSTRAINT unique_k";
		auto rollback = db.beginTransaction();
		check(!rollback.execute(ddl).isSuccess(), "transactional DDL was admitted");
		rollback.rollback();
		auto seed = db.beginTransaction();
		execute(seed, "CREATE (:Item {k:1})");
		seed.commit();
		auto old = db.beginReadOnlyTransaction();
		execute(db, ddl);
		check(!db.execute(ddl).isSuccess(), "duplicate declaration accepted");
		check(duplicate(db), "dynamic index absent");
		auto stale = db.beginTransaction();
		execute(stale, "CREATE (:Item {k:7})");
		execute(db, drop);
		check(!db.execute(drop).isSuccess(), "missing constraint dropped");
		check(!execute(db, drop + " IF EXISTS").hasNext(), "missing drop returned confirmation");
		execute(db, ddl);
		bool conflict = false;
		try {
			stale.commit();
		} catch (const std::exception &e) {
			conflict = std::string(e.what()).find("Stale") != std::string::npos;
		}
		check(conflict, "schema ABA did not fence the old writer");
		auto pendingWal = db.beginTransaction();
		execute(pendingWal, "CREATE (:Item {k:8})");
		pendingWal.commit();
		auto staged = db.beginTransaction();
		execute(staged, "CREATE (:Item {k:2})");
		db.save();
		staged.commit();
		count(old, "n.k=2", 0);
		old.commit();
		db.close();
		db.open();
		check(duplicate(db), "reopen lost uniqueness");
		execute(db, drop);
		check(!duplicate(db), "drop still constrained the key");
		auto rebuild = db.execute(ddl);
		check(!rebuild.isSuccess() && rebuild.getError().find("duplicate key") != std::string::npos,
			  "rebuild ignored actual duplicate");
		execute(db, "CREATE CONSTRAINT another FOR (n:Other) REQUIRE n.value IS UNIQUE");
		for (int i = 0; i < 2; ++i) {
			auto other = db.beginTransaction();
			execute(other, "CREATE (:Other {value:'str'})");
			bool rejected = false;
			try {
				other.commit();
			} catch (const std::exception &e) {
				rejected = std::string(e.what()).find("duplicate key") != std::string::npos;
				if (!rejected)
					throw;
			}
			check(rejected == (i == 1), "new property not protected");
		}
		db.close();
		return 0;
	}
	if (op == "several") {
		const std::string extra = "CREATE CONSTRAINT second FOR(n:Other) REQUIRE n.code IS UNIQUE";
		execute(db, ddl);
		execute(db, extra);
		check(!db.execute("CREATE CONSTRAINT third FOR(n:Other) REQUIRE n.code IS UNIQUE").isSuccess(),
			  "duplicate scope admitted");
		auto write = [&](int64_t key, bool accept) {
			auto tx = db.beginTransaction();
			execute(tx, "CREATE (:Other {code:" + std::to_string(key) + "})");
			bool rejected = false;
			try {
				tx.commit();
			} catch (const std::exception &e) {
				if (std::string(e.what()).find("duplicate key") == std::string::npos)
					throw;
				rejected = true;
			}
			check(rejected != accept, "secondary scope not enforced");
		};
		write(5, true);
		write(5, false);
		execute(db, "DROP CONSTRAINT unique_k");
		db.close();
		db.open();
		write(5, false);
		write(6, true);
		std::_Exit(0);
	}
	if (op == "long-schema-name") {
		const std::string name(300, 'x');
		execute(db, "CREATE CONSTRAINT " + name + " FOR(n:Item) REQUIRE n.k IS UNIQUE");
		auto tx = db.beginTransaction();
		execute(tx, "CREATE (:Item {k:1})");
		tx.commit();
		std::_Exit(0);
	}
	if (op == "recover-long-name") {
		check(duplicate(db), "long-name declaration absent after WAL recovery");
		execute(db, "DROP CONSTRAINT " + std::string(300, 'x'));
		check(!duplicate(db), "long-name drop failed");
		db.close();
		return 0;
	}
	if (op == "finish-several") {
		auto result = execute(db, "SHOW CONSTRAINT");
		check(result.hasNext(), "missing secondary");
		result.next();
		check(result.get(0) == zyx::Value(std::string("second")) && !result.hasNext(), "wrong declaration");
		execute(db, "DROP CONSTRAINT second");
		auto tx = db.beginTransaction();
		execute(tx, "CREATE (:Other {code:6})");
		tx.commit();
		check(!db.execute("CREATE CONSTRAINT second FOR(n:Other) REQUIRE n.code IS UNIQUE").isSuccess(),
			  "secondary duplicate survived rebuild");
		db.close();
		return 0;
	}
	if (op == "accept-drop") {
		count(db, "n.k=3", 1);
		check(!duplicate(db), "dropped constraint recovered");
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
	if (op == "mixed-seed") {
		auto tx = db.beginTransaction();
		execute(tx, "MATCH(n:Item) WHERE id(n)=1 SET n.k='base-owner'");
		tx.commit();
		db.save();
		db.close();
		return 0;
	}
	if (op == "mixed-prefix") {
		for (const auto *key: {"'free-owner'", "4"}) {
			auto tx = db.beginTransaction();
			execute(tx, std::string("MATCH(n:Item) WHERE id(n)=2 SET n.k=") + key);
			tx.commit();
		}
		std::_Exit(0);
	}
	if (op == "accept-mixed") {
		count(db, "n.k='base-owner'", 1);
		count(db, "n.k=4", 1);
		return 0;
	}
	if (op == "mixed-owners") {
		auto old = db.beginReadOnlyTransaction();
		auto swap = db.beginTransaction();
		execute(swap, "MATCH(n:Item) WHERE id(n)=2 SET n.k=1");
		execute(swap, "MATCH(n:Item) WHERE id(n)=1 SET n.k='overlay'");
		swap.commit();
		auto reject = [&](const std::string &value) {
			auto tx = db.beginTransaction();
			execute(tx, "CREATE (:Item {k:" + value + "})");
			try {
				tx.commit();
			} catch (const std::exception &e) {
				check(std::string(e.what()).find("duplicate key") != std::string::npos, e.what());
				return;
			}
			throw std::runtime_error("Missing mixed UNIQUE violation");
		};
		reject("'overlay'");
		count(old, "n.k=1", 1);
		count(old, "n.k='overlay'", 0);
		db.save();
		count(old, "n.k=2", 1);
		old.commit();
		db.close();
		db.open();
		reject("1");
		reject("'overlay'");
		auto replace = db.beginTransaction();
		execute(replace, "MATCH(n:Item) WHERE n.k='overlay' DELETE n");
		execute(replace, "CREATE (:Item {k:'overlay'})");
		replace.commit();
		db.save();
		count(db, "n.k='overlay'", 1);
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
	if (op == "ddl" || op == "drop") {
		if (argc > 3)
			setenv("ZYX_TEST_NATIVE_IO", argv[3], 1);
		auto r = db.execute(op == "ddl" ? ddl : "DROP CONSTRAINT unique_k");
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
