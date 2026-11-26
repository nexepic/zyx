#pragma once

#include "LongProperties.hpp"

namespace large_row_test {
	using long_property_test::require;
	using long_property_test::run;

	inline auto value(size_t bytes, char digit) {
		auto list = std::make_shared<zyx::ValueList>();
		list->elements.emplace_back(std::string(bytes, digit));
		return list;
	}

	inline void verify(auto &view, size_t bytes, char digit) {
		auto rows = run(view, "MATCH (n:Huge)-[r:LINK]->(m:Small) RETURN n.p,r.v,m.v");
		if (!digit) {
			require(!rows.hasNext(), "Uncommitted relationship visible");
			require(!run(view, "MATCH(n:Huge) RETURN id(n)").hasNext(), "Partial large row visible");
			require(!run(view, "MATCH(n:Small) RETURN id(n)").hasNext(), "Partial small row visible");
			return;
		}
		require(rows.hasNext(), "Missing large graph");
		rows.next();
		auto list = std::get<std::shared_ptr<zyx::ValueList>>(zyx::detail::getTypedResultValue(rows, 0));
		require(list->elements.size() == 1 && std::get<std::string>(list->elements.front()) == std::string(bytes, digit),
				"Incorrect nested text");
		require(std::get<int64_t>(rows.get(1)) == 7 && std::get<int64_t>(rows.get(2)) == 9 && !rows.hasNext(),
				"Incorrect large transaction membership");
	}

	inline void exercise(const std::string &op, const std::string &path, size_t bytes, const char *fault) {
		zyx::Database db(path);
		db.open();
		if (op == "large-row-schema") {
			run(db, "CREATE CONSTRAINT huge_key FOR(n:Huge) REQUIRE n.k IS UNIQUE");
		} else if (op == "large-row-duplicate") {
			auto writer = db.beginTransaction();
			run(writer, "CREATE (:Huge {k:'short'})");
			bool rejected = false;
			try {
				writer.commit();
			} catch (const std::exception &error) {
				require(std::string(error.what()).find("duplicate key") != std::string::npos, error.what());
				rejected = true;
			}
			require(rejected, "Reopened UNIQUE accepted a duplicate");
		} else if (op == "large-row-write" || op == "large-row-append") {
			if (op == "large-row-append") {
				auto prefix = db.beginTransaction();
				run(prefix, "CREATE (:Prefix)");
				prefix.commit();
			}
			auto tx = db.beginTransaction();
			run(tx, "CREATE (:Huge {p:$p,k:'short'})-[:LINK {v:7}]->(:Small {v:9})", {{"p", value(bytes, 'a')}});
			if (fault)
				setenv("ZYX_TEST_NATIVE_IO", fault, 1);
			tx.commit();
			raise(SIGKILL);
		} else if (op == "large-row-life") {
			auto old = db.beginReadOnlyTransaction();
			for (bool commit : {false, true}) {
				auto writer = db.beginTransaction();
				run(writer, "MATCH(n:Huge) SET n.p=$p", {{"p", value(bytes, 'b')}});
				verify(writer, bytes, 'b');
				if (commit)
					writer.commit();
				else
					writer.rollback();
				verify(old, bytes, 'a');
			}
			db.save();
			verify(old, bytes, 'a');
			old.commit();
			verify(db, bytes, 'b');
		} else if (op == "large-row-delete") {
			auto tx = db.beginTransaction();
			run(tx, "MATCH(n:Huge) DETACH DELETE n");
			run(tx, "MATCH(n:Small) DELETE n");
			tx.commit();
			verify(db, bytes, 0);
		} else if (op == "large-row-read" || op == "large-row-updated" || op == "large-row-empty") {
			verify(db, bytes, op == "large-row-read" ? 'a' : op == "large-row-updated" ? 'b' : 0);
		} else {
			throw std::runtime_error("Unknown large-row fixture");
		}
		db.close();
	}
}
