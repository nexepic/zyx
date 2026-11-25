#pragma once
#include <filesystem>
#include "zyx/zyx.hpp"
#include "../../src/storage/native_wal/Journal.hpp"

// Construct enough independent shifts that their optional locator table costs
// more than the mandatory checkpoint encoder. Keep the two admissions separate.
inline void exerciseLocatorBudget(const std::string &path) {
	using namespace graph::storage::nativewal;
	constexpr uint64_t count = 3000;
	auto check = [](bool ok, const char *message) {
		if (!ok)
			throw std::runtime_error(message);
	};
	{
		zyx::Database db(path);
		db.open();
		for (size_t block = 0; block < 10; ++block) {
			auto tx = db.beginTransaction();
			std::vector<std::unordered_map<std::string, zyx::Value>> rows(count / 10, {{"s", std::string(128, 'a')}});
			auto ids = tx.createNodes("Budget", rows);
			check(ids.size() == rows.size(), "Missing budget fixture rows");
			tx.commit();
		}
		db.close();
	}
	auto orphans = [&] {
		size_t count = 0;
		for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
			std::error_code error;
			auto link = std::filesystem::read_symlink(entry.path(), error).string();
			count += !error && link.starts_with(path) && link.ends_with(" (deleted)");
		}
		return count;
	};
	{
		Journal journal(path, false, 16U << 20);
		auto change = [&](bool many) {
			auto prior = journal.snapshot();
			auto next = std::make_shared<State>(*prior);
			Changes changes;
			for (uint64_t id = 2; id <= (many ? count : 2); id += 2) {
				auto row = next->nodes.at(id);
				row.properties.at(0).value = graph::PropertyValue(std::string(129, many ? 'b' : 'c'));
				next->nodes.insert_or_assign(id, std::move(row));
				changes.nodes.insert(id);
			}
			journal.commit(prior, std::move(next), changes);
		};
		change(true);
		auto budget = journal.snapshot()->budget;
		size_t lo = 0, hi = budget->capacity();
		while (lo < hi) {
			auto probe = lo + (hi - lo + 1) / 2;
			try {
				VersionCharge measured(budget, probe);
				lo = probe;
			} catch (const std::length_error &) {
				hi = probe - 1;
			}
		}
		check(lo > 20000, "Insufficient fixture headroom");
		{
			VersionCharge pressure(budget, lo - 20000);
			journal.checkpoint();
			check(!std::filesystem::exists(path + ".wal") && orphans() == 1,
				  "Optional locator denial failed to publish a standalone checkpoint");
		}
		auto value = [&](uint64_t id) {
			return std::get<std::string>(journal.snapshot()->nodes.at(id).properties.at(0).value.getVariant());
		};
		check(value(2) == std::string(129, 'b') && value(count - 1) == std::string(128, 'a'),
			  "Locator denial lost an acknowledged or unchanged row");
		change(false);
		journal.checkpoint();
		check(orphans() == 0 && value(count - 1) == std::string(128, 'a'), "Locator retry retained the old file");
	}
	Journal reopened(path, true, 16U << 20);
	auto view = reopened.snapshot();
	check(view->nodes.size() == count &&
				  std::get<std::string>(view->nodes.at(2).properties.at(0).value.getVariant()) == std::string(129, 'c'),
		  "Wrong durable contents after optional locator denial");
}
