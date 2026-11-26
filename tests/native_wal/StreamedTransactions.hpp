#pragma once

#include "LongProperties.hpp"
#include <chrono>
#include <algorithm>
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/StorageBackendFactory.hpp"
#include <filesystem>
#include <iostream>
#include <set>

namespace streamed_transaction_test {
	using long_property_test::require;
	using long_property_test::run;
	inline std::string payload(size_t i) {
		std::string data(24 * 1024, char('a' + i % 26));
		data.replace(0, 12, std::string(12, char('A' + i % 26)));
		return data;
	}
	inline void verify(auto &view, size_t count) {
		auto meta = run(view, "MATCH (h:Head)-[r:LINK]->() RETURN h.v,r.v,h.bin,r.bin");
		require(meta.hasNext(), "Missing markers");
		meta.next();
		const int64_t value = count ? 1 : 0;
		const zyx::Bytes binary{0, 255, uint8_t(value)};
		require(std::get<int64_t>(meta.get(0)) == value && std::get<int64_t>(meta.get(1)) == value &&
				std::get<zyx::Bytes>(meta.get(2)) == binary && std::get<zyx::Bytes>(meta.get(3)) == zyx::Bytes{255, uint8_t(value), 0, 127} &&
				!meta.hasNext(), "Wrong node or edge revision");
		auto counts = run(view, "MATCH(n:Part) RETURN count(n)");
		require(counts.hasNext(), "Missing count");
		counts.next();
		require(std::get<int64_t>(counts.get(0)) == int64_t(count), "Partial commit");
		for (size_t start = 0; start < count; start += 48) {
			auto rows = run(view, "MATCH(n:Part) WHERE n.i >= $lo AND n.i < $hi RETURN n.i,n.p",
							{{"lo", int64_t(start)}, {"hi", int64_t(start + 48)}});
			std::set<int64_t> seen;
			while (rows.hasNext()) {
				rows.next();
				auto i = std::get<int64_t>(rows.get(0));
				require(i >= int64_t(start) && size_t(i) < std::min(start + 48, count) && seen.insert(i).second &&
						std::get<std::string>(rows.get(1)) == payload(i), "Wrong replayed property");
			}
			require(seen.size() == std::min(size_t(48), count - start), "Missing replayed property");
		}
	}
	// At the first budget that admits this commit, a fresh process using
	// the same cap must also be able to open and read every acknowledged row.
	inline void exerciseBudget(const std::string &path, size_t cap, bool read, bool small) {
		using namespace graph::storage;
		const size_t rows = small ? 3 : 90;
		auto bytes = [small](size_t i) { return small ? std::string(16, char('a' + i % 26)) : payload(i); };
		auto selected = StorageBackendFactory::create(path, cap,
				read ? OpenMode::OPEN_READ_ONLY_EXISTING_FILE : OpenMode::OPEN_EXISTING_FILE);
		selected.backend->open();
		if (read) {
			auto view = dynamic_cast<LogicalGraphReadBackend &>(*selected.backend).openLogicalGraphReadSnapshot();
			for (size_t i = 0; i < rows; ++i) {
				auto node = view->readNode(i + 3);
				auto it = std::ranges::find(node.properties, "p", &LogicalGraphPropertyEntry::key);
				require(it != node.properties.end() &&
						   std::get<std::string>(it->value.getVariant()) == bytes(i), "Wrong tight-budget row");
			}
			return;
		}
		auto tx = dynamic_cast<LogicalGraphTransactionalBackend &>(*selected.backend).beginLogicalGraphTransaction(false);
		for (size_t i = 0; i < rows; ++i) {
			LogicalGraphMutationBatch batch;
			LogicalGraphNodeCreateRequest row;
			row.labels = {"Part"};
			row.properties = {{"i", graph::PropertyValue(int64_t(i))}, {"p", graph::PropertyValue(bytes(i))}};
			batch.node_creates.push_back(std::move(row));
			(void) tx->stageMutationBatch(std::move(batch));
		}
		(void) tx->commit();
		std::_Exit(0); // Leave the receipt and WAL for the independent reader.
	}
	inline void exercise(const std::string &op, const std::string &path, size_t count, const char *fault) {
		using Clock = std::chrono::steady_clock;
		auto elapsed = [](auto start) { return std::chrono::duration<double,std::micro>(Clock::now()-start).count(); };
		zyx::Database db(path);
		auto started = Clock::now();
		db.open();
		std::cout << "open_us " << elapsed(started) << std::endl;
		if (op == "stream-seed") {
			run(db, "CREATE CONSTRAINT part_i FOR(n:Part) REQUIRE n.i IS UNIQUE");
			auto seed = db.beginTransaction();
			run(seed, "CREATE (:Head {v:0,bin:$b})-[:LINK {v:0,bin:$e}]->(:Tail)", {{"b", zyx::Bytes{0,255,0}}, {"e", zyx::Bytes{255,0,0,127}}});
			seed.commit();
		} else if (op == "stream-read") {
			verify(db, count);
		} else {
			auto pin = db.beginReadOnlyTransaction();
			if (op == "stream-append") {
				auto prefix = db.beginTransaction();
				run(prefix, "CREATE (:Prefix)");
				prefix.commit();
			}
			started = Clock::now();
			auto tx = db.beginTransaction();
			run(tx, "MATCH(h:Head)-[r:LINK]->() SET h.v=1,r.v=1,h.bin=$b,r.bin=$e", {{"b",zyx::Bytes{0,255,1}}, {"e",zyx::Bytes{255,1,0,127}}});
			const size_t group = op == "stream-small-statements" ? 10 : 90;
			for (size_t begin = 0; begin < count; begin += group) {
				std::vector<std::unordered_map<std::string,zyx::Value>> rows;
				for (size_t i = begin; i < std::min(begin + group, count); ++i)
					rows.push_back({{"i", int64_t(i)}, {"p",payload(i)}});
				require(tx.createNodes("Part", rows).size() == rows.size(), "Missing private rows");
			}
			std::cout << "stage_us " << elapsed(started) << std::endl;
			if (op == "stream-duplicate") {
				(void) tx.createNode("Part", {{"i", int64_t(0)}});
				try {
					tx.commit();
				} catch (const std::exception &e) {
					require(std::string_view(e.what()).find("duplicate key") != std::string_view::npos,
							"Expected duplicate across WAL windows");
					if (tx.isActive()) tx.rollback();
					verify(db, 0);
					return;
				}
				require(false, "Accepted duplicate key");
			}
			if (op == "stream-life") {
				verify(tx, count);
				verify(pin, 0);
			}
			if (fault)
				setenv("ZYX_TEST_NATIVE_IO", fault, 1);
			const auto start = std::chrono::steady_clock::now();
			tx.commit();
			std::cout << "ACK_us " << std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()
					<< " wal_bytes " << (std::filesystem::exists(path+".wal") ? std::filesystem::file_size(path+".wal") : 0) << std::endl;
			if (op == "stream-roll") {
				const auto length = std::filesystem::file_size(path + ".wal");
				auto step = db.beginTransaction();
				run(step, "CREATE (:After)");
				step.commit();
				require(std::filesystem::file_size(path+".wal") < length, "Large generation did not rotate");
			}
			if (op != "stream-life")
				raise(SIGKILL);
			started = Clock::now();
			db.save();
			std::cout << "checkpoint_us " << elapsed(started) << std::endl;
			verify(pin, 0);
			pin.commit();
			verify(db, count);
		}
		db.close();
	}
}
