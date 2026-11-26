#pragma once

#include <sstream>
#include "LongProperties.hpp"
#include "graph/storage/NativeWalDatabase.hpp"

namespace snapshot_property_test {
	using long_property_test::require;
	using long_property_test::run;

	inline std::string text(size_t bytes) { return std::string(bytes - 3, 'x') + "树"; }

	// Stop while the large authenticated segment is being written.
	class ShortOutput : public std::filebuf {
		std::streamsize remaining = 128 * 1024;
		std::streamsize xsputn(const char *s, std::streamsize n) override {
			auto sent = std::filebuf::xsputn(s, std::min(n, remaining));
			remaining -= sent;
			return sent;
		}
	};

	inline void rejectReader(zyx::Database &db, const zyx::Transaction &reader) {
		std::ostringstream out;
		try {
			db.exportSegmentedSnapshot(reader, out);
		} catch (const std::logic_error &) {
			require(out.str().empty(), "Invalid snapshot wrote an export prefix");
			return;
		}
		throw std::runtime_error("Invalid export reader accepted");
	}

	inline void verify(auto &view, size_t bytes, bool edge, bool updated) {
		auto row = run(view, "MATCH (n:Item)-[e:LINK]->(b:End) RETURN n.k,e.v,id(n),id(e),id(b)");
		require(row.hasNext(), "Missing snapshot relationship");
		row.next();
		require(std::get<int64_t>(row.get(0)) == 7 && std::get<int64_t>(row.get(2)) == 1 &&
				std::get<int64_t>(row.get(3)) == 1 && std::get<int64_t>(row.get(4)) == 2, "Invalid snapshot identity");
		auto nullable = std::get<std::shared_ptr<zyx::ValueList>>(zyx::detail::getTypedResultValue(row, 1));
		require(nullable->elements.size() == 2 && std::holds_alternative<std::monostate>(nullable->elements[0]) &&
				std::get<std::string>(nullable->elements[1]) == "水" && !row.hasNext(), "Invalid nullable list");

		auto r = run(view, edge ? "MATCH ()-[e:LINK]->() RETURN e.p" : "MATCH(n:Item) RETURN n.p");
		require(r.hasNext(), "Missing snapshot property");
		r.next();
		auto list = std::get<std::shared_ptr<zyx::ValueList>>(zyx::detail::getTypedResultValue(r, 0));
		require(list->elements.size() == 1 && std::get<std::string>(list->elements.front()) ==
					(updated ? "B" : text(bytes)) && !r.hasNext(), "Wrong snapshot property");
	}

	inline void exercise(const std::string &op, const std::string &path, size_t bytes, bool edge) {
		if (op != "snapshot-property-export") {
			zyx::Database copy(path);
			copy.open();
			verify(copy, bytes, edge, op == "snapshot-property-current");
			return;
		}
		const auto main = graph::storage::createIndexedNativeWalDatabase(path, "Item", "k");
		zyx::Database db(main);
		db.open();
		{
			auto list = std::make_shared<zyx::ValueList>();
			list->elements.emplace_back(text(bytes));
			auto tx = db.beginTransaction();
			run(tx, edge ? "CREATE (:Item {k:7})-[:LINK {v:[NULL,'水'],p:$p}]->(:End)"
						 : "CREATE (:Item {k:7,p:$p})-[:LINK {v:[NULL,'水']}]->(:End)", {{"p", list}});
			tx.commit();
		}
		db.save();
		db.close();
		db.open();
		verify(db, bytes, edge, false);
		auto old = db.beginReadOnlyTransaction();
		{
			zyx::Database other(graph::storage::createNativeWalDatabase(path + "-other"));
			other.open();
			rejectReader(other, old);
		}
		auto writer = db.beginTransaction();
		rejectReader(db, writer);
		run(writer, edge ? "MATCH ()-[e:LINK]->() SET e.p=['B']" : "MATCH(n:Item) SET n.p=['B']");
		writer.commit();
		db.save();
		{
			ShortOutput buf;
			require(buf.open(path + ".partial", std::ios::out | std::ios::binary) != nullptr, "Cannot open test sink");
			std::ostream failing(&buf);
			bool rejected = false;
			try {
				db.exportSegmentedSnapshot(old, failing);
			} catch (const std::runtime_error &) {
				rejected = !failing.good();
			}
			require(rejected, "Short output was accepted");
		}
		std::ofstream out(path + ".zyx", std::ios::binary);
		db.exportSegmentedSnapshot(old, out);
		out.close();
		require(out.good(), "Snapshot write failed");
		old.commit();
		rejectReader(db, old);
		verify(db, bytes, edge, true);
		db.close();
	}
}
