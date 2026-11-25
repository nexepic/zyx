#pragma once
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include "zyx/zyx.hpp"

namespace relocation_test {
	using Id = int64_t;
	struct Row {
		std::string text;
		Id rank, from = 0, to = 0;
	};
	struct Model {
		std::map<Id, Row> nodes, edges;
	};
	inline void check(bool ok, const std::string &message) {
		if (!ok)
			throw std::runtime_error(message);
	}
	inline std::string text(size_t size, int variant) {
		std::string value;
		if (variant % 2)
			while (value.size() + 3 <= size)
				value += "猫";
		value.resize(size, static_cast<char>('A' + variant % 26));
		return value;
	}
	template<class Handle>
	void verify(Handle &handle, const Model &expected) {
		for (bool edges : {false, true}) {
			auto &rows = edges ? expected.edges : expected.nodes;
			auto r = handle.execute(edges ? "MATCH (a:N)-[r:R]->(b:N) RETURN id(r), r.s, r.rank, id(a), id(b)"
										  : "MATCH (n:N) RETURN id(n), n.s, n.rank");
			check(r.isSuccess(), r.getError());
			std::set<Id> seen;
			while (r.hasNext()) {
				r.next();
				auto id = std::get<Id>(r.get(0));
				const auto &row = rows.at(id);
				check(seen.insert(id).second && std::get<std::string>(r.get(1)) == row.text &&
							  std::get<Id>(r.get(2)) == row.rank,
					  "Relocated row has incorrect identity, type, or value");
				if (edges)
					check(std::get<Id>(r.get(3)) == row.from && std::get<Id>(r.get(4)) == row.to,
						  "Wrong relocated relationship endpoints");
			}
			check(seen.size() == rows.size(), "Incomplete relocated graph");
		}
	}
	inline void exercise(const std::string &path) {
		Model model;
		zyx::Database db(path);
		db.open();
		{
			auto writer = db.beginTransaction();
			for (Id id = 1; id <= 80; ++id) {
				auto &node = model.nodes[id] = {text(4000, id), id};
				check(writer.createNode("N", {{"s", node.text}, {"rank", node.rank}}) == id, "Unexpected node ID");
			}
			for (Id id = 1; id <= 80; ++id) {
				auto &edge = model.edges[id] = {text(4000, id + 1), -id, 1 + id % 20,
												1 + (id % 5 == 0 ? id : id + 1) % 20};
				check(writer.createEdge(edge.from, edge.to, "R", {{"s", edge.text}, {"rank", edge.rank}}) == id,
					  "Unexpected relationship ID");
			}
			writer.commit();
		}
		db.close();
		db.open();
		std::optional<zyx::Transaction> old, middle;
		Model pinned, middleVersion;
		for (int step = 0; step < 12; ++step) {
			if (step == 2) {
				old.emplace(db.beginReadOnlyTransaction());
				pinned = model;
				std::ofstream output(path + ".relocation.cp003", std::ios::binary);
				db.exportSegmentedSnapshot(output);
				output.close();
				check(output.good(), "Could not finish standalone relocation export");
			}
			if (step == 5) {
				middle.emplace(db.beginReadOnlyTransaction());
				middleVersion = model;
			}
			if (step == 10) {
				middle->commit();
				middle.reset();
			}
			if (step == 8) {
				old->commit();
				old.reset();
			}
			auto writer = db.beginTransaction();
			for (bool edges : {false, true}) {
				auto &rows = edges ? model.edges : model.nodes;
				for (Id id : std::set<Id>{1, 40, 79, 2 + (step * 17 + (edges ? 9 : 3)) % 75}) {
					auto &row = rows.at(id);
					row.text = text(step % 2 ? 8192 : 43, step + static_cast<int>(id));
					auto r = writer.execute(edges ? "MATCH ()-[r:R]->() WHERE id(r)=$id SET r.s=$s"
												  : "MATCH (n:N) WHERE id(n)=$id SET n.s=$s",
											{{"id", id}, {"s", row.text}});
					check(r.isSuccess(), r.getError());
				}
			}
			auto removeLast = [&](auto &rows, const char *statement) {
				auto id = rows.rbegin()->first;
				auto r = writer.execute(statement, {{"id", id}});
				check(r.isSuccess(), r.getError());
				rows.erase(id);
			};
			removeLast(model.edges, "MATCH ()-[r:R]->() WHERE id(r)=$id DELETE r");
			removeLast(model.nodes, "MATCH (n:N) WHERE id(n)=$id DELETE n");
			auto value = text(101 + (step % 2) * 91, step);
			Id rank = step;
			auto node = writer.createNode("N", {{"s", value}, {"rank", rank}});
			auto edge = writer.createEdge(node, 1, "R", {{"s", value}, {"rank", -rank}});
			check(node == 81 + step && edge == 81 + step, "Reused deleted identity");
			model.nodes[node] = {value, rank};
			model.edges[edge] = {value, -rank, node, 1};
			if (step == 3)
				verify(writer, model);
			writer.commit();
			db.save();
			verify(db, model);
			if (old)
				verify(*old, pinned);
			if (middle)
				verify(*middle, middleVersion);
		}
		auto setRank = [](zyx::Transaction &tx, Id rank) {
			auto r = tx.execute("MATCH (n:N) WHERE id(n)=77 SET n.rank=$v", {{"v", rank}});
			check(r.isSuccess(), r.getError());
		};
		// Make an earlier row longer, then stage a write to a later ID while
		// its published file location is still that of the previous checkpoint.
		{
			auto earlier = db.beginTransaction();
			model.nodes.at(1).text = text(10000, 1);
			auto r = earlier.execute("MATCH (n:N) WHERE id(n)=1 SET n.s=$s", {{"s", model.nodes.at(1).text}});
			check(r.isSuccess(), r.getError());
			earlier.commit();
			auto pending = db.beginTransaction();
			setRank(pending, 0);
			db.save();
			pending.commit();
			model.nodes.at(77).rank = 0;
		}
		verify(db, model);
		{
			auto stale = db.beginTransaction();
			setRank(stale, 42);
			for (Id rank : {7, 0}) {
				auto other = db.beginTransaction();
				setRank(other, rank);
				other.commit();
			}
			db.save();
			bool conflicted = false;
			try {
				stale.commit();
			} catch (const std::exception &e) {
				conflicted = std::string(e.what()).find("Stale native graph write snapshot") != std::string::npos;
				if (!conflicted)
					throw;
			}
			check(conflicted, "ABA value equality allowed a stale transaction to commit");
		}
		db.close();
		db.open();
		verify(db, model);
		db.close();
		zyx::Database exported(path + ".relocation.cp003");
		exported.open();
		verify(exported, pinned);
		exported.close();
	}
	inline void verifyIndependentExport(const std::string &path) {
		zyx::Database exported(path);
		exported.open();
		auto r = exported.execute("MATCH (n:N) WHERE id(n)=1 RETURN n.s, n.rank");
		check(r.isSuccess() && r.hasNext(), "Export lacks pinned row");
		r.next();
		check(std::get<std::string>(r.get(0)) == text(8192, 2) && std::get<Id>(r.get(1)) == 1 && !r.hasNext(),
			  "Export contains a later row version");
		auto newer = exported.execute("MATCH (n:N) WHERE id(n)=92 RETURN id(n)");
		check(newer.isSuccess() && !newer.hasNext(), "Later committed node appeared in export");
		exported.close();
	}
} // namespace relocation_test
