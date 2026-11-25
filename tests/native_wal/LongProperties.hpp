#pragma once

#include <csignal>
#include <cstdlib>
#include <fstream>
#include "zyx/zyx.hpp"

namespace long_property_test {
	using Parameters = std::unordered_map<std::string, zyx::Value>;

	inline void require(bool ok, const std::string &message) {
		if (!ok)
			throw std::runtime_error(message);
	}

	inline std::string text(int revision) {
		std::string out;
		out.reserve(120000);
		for (size_t i = 0; i < 40000; ++i)
			out += revision == 1 ? "树" : "水";
		return out;
	}

	inline zyx::Bytes binary(int revision) {
		zyx::Bytes out(123457);
		for (size_t i = 0; i < out.size(); ++i)
			out[i] = static_cast<uint8_t>(i * 17 + revision);
		return out;
	}

	inline Parameters parameters(int revision) {
		auto list = std::make_shared<zyx::ValueList>();
		list->elements = {std::monostate{}, int64_t(revision), text(revision)};
		auto doc = std::make_shared<zyx::ValueMap>();
		doc->entries.emplace("items", list);
		return {{"txt", text(revision)}, {"doc", doc}, {"bin", binary(revision)}};
	}

	inline zyx::Result run(auto &handle, const std::string &stmt, const Parameters &params = {}) {
		auto r = handle.execute(stmt, params);
		require(r.isSuccess(), stmt + ": " + r.getError());
		return r;
	}

	inline void verify(auto &handle, int revision) {
		auto r = run(handle, "MATCH (n:Long)-[e:PAY]->(b) RETURN id(n),id(e),n.txt,n.doc,e.bin");
		require(r.hasNext(), "Missing long graph");
		r.next();
		require(std::get<int64_t>(r.get(0)) == 1 && std::get<int64_t>(r.get(1)) == 1, "Wrong graph identity");
		require(std::get<std::string>(r.get(2)) == text(revision) && std::get<zyx::Bytes>(r.get(4)) == binary(revision),
				"Wrong long scalar");
		auto map = std::get<std::shared_ptr<zyx::ValueMap>>(zyx::detail::getTypedResultValue(r, 3));
		auto list = std::get<std::shared_ptr<zyx::ValueList>>(map->entries.at("items"));
		require(list->elements.size() == 3 && std::holds_alternative<std::monostate>(list->elements[0]) &&
						std::get<int64_t>(list->elements[1]) == revision &&
						std::get<std::string>(list->elements[2]) == text(revision) && !r.hasNext(),
				"Wrong nested long value");
	}

	inline void update(auto &writer) {
		auto params = parameters(2);
		run(writer, "MATCH(n:Long) SET n.txt=$txt,n.doc=$doc", params);
		run(writer, "MATCH()-[e:PAY]->() SET e.bin=$bin", params);
	}

	inline void exercise(const std::string &op, const std::string &path, const char *fault) {
		zyx::Database db(path);
		db.open();
		if (op == "long-limits") {
			for (size_t length : {16373, 16374, 16375}) {
				auto tx = db.beginTransaction();
				run(tx, "CREATE (:Boundary {p:$v})", {{"v", std::string(length, 'x')}});
				tx.commit();
			}
			auto before = db.beginReadOnlyTransaction();
			bool refused = false;
			try {
				auto tx = db.beginTransaction();
				const Parameters params{{"v", std::string(1100000, 'y')}};
				run(tx, "CREATE (:TooBig {p:$v})", params);
				run(tx, "CREATE (:TooBig {p:$v})", params);
				tx.commit();
			} catch (const std::exception &e) {
				refused = std::string_view(e.what()).find("transaction exceeds record limit") != std::string_view::npos;
			}
			require(refused, "Oversized combined transaction was not rejected at commit");
			auto check = [](auto &view) {
				auto r = run(view, "MATCH(n:Boundary) RETURN n.p ORDER BY id(n)");
				for (size_t length : {16373, 16374, 16375}) {
					require(r.hasNext(), "Missing boundary result");
					r.next();
					require(std::get<std::string>(r.get(0)) == std::string(length, 'x'), "Boundary value changed");
				}
				require(!r.hasNext(), "Duplicate boundary result");
				require(!run(view, "MATCH(n:TooBig) RETURN id(n)").hasNext(), "Partial oversized transaction");
			};
			check(before);
			before.commit();
			db.close();
			db.open();
			check(db);
		} else if (op == "long-seed") {
			auto tx = db.beginTransaction();
			run(tx, "CREATE (:Long {txt:$txt,doc:$doc})-[:PAY {bin:$bin}]->(:End)", parameters(1));
			verify(tx, 1);
			tx.commit();
		} else if (op == "long-life" || op == "long-fault" || op == "long-append-fault") {
			auto old = db.beginReadOnlyTransaction();
			auto rollback = db.beginTransaction();
			update(rollback);
			verify(rollback, 2);
			rollback.rollback();
			verify(old, 1);
			{
				std::ofstream out(path + ".view", std::ios::binary);
				db.exportSegmentedSnapshot(out);
				out.close();
				require(out.good(), "Long export failed");
			}
			if (op == "long-append-fault") {
				auto prefix = db.beginTransaction();
				run(prefix, "CREATE (:Prefix)");
				prefix.commit();
			}
			auto writer = db.beginTransaction();
			update(writer);
			if (fault)
				setenv("ZYX_TEST_NATIVE_IO", fault, 1);
			writer.commit();
			if (op != "long-life")
				raise(SIGKILL);
			verify(db, 2);
			db.save();
			verify(old, 1);
			old.commit();
		} else if (op == "long-delete") {
			auto writer = db.beginTransaction();
			run(writer, "MATCH(n:Long) DETACH DELETE n");
			writer.commit();
			db.close();
			db.open();
			auto r = run(db, "MATCH(n:Long) RETURN count(n)");
			require(r.hasNext(), "Missing deletion count");
			r.next();
			require(std::get<int64_t>(r.get(0)) == 0, "Long owner not deleted");
		} else {
			require(op == "long-A" || op == "long-B", "Unknown long test");
			verify(db, op == "long-A" ? 1 : 2);
		}
		db.close();
	}
}
