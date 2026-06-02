#include "graph/debug/PerfTrace.hpp"
#include "zyx/zyx.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

	constexpr std::string_view kDatabase = "zyx";
	constexpr std::string_view kEquivalentMode = "api";
	constexpr std::string_view kProfileScan = "scan";
	constexpr std::string_view kProfileIndexed = "indexed";
	constexpr std::string_view kExecutionModeWarm = "warm";
	constexpr std::string_view kExecutionModeColdish = "cold-ish";

	struct Options {
		std::filesystem::path dataset;
		std::filesystem::path dbPath;
		std::string scale;
		std::string profile = std::string(kProfileScan);
		bool emitProfile = false;
		std::string executionMode = std::string(kExecutionModeWarm);
		int warmup = 0;
		int iterations = 1;
	};

	struct LoadedGraph {
		std::unordered_map<std::string, int64_t> usersByExternalId;
		std::unordered_map<std::string, int64_t> postsByExternalId;
		std::unordered_map<std::string, int64_t> tagsByExternalId;
		int64_t loadedRows = 0;
	};

	struct CsvRow {
		std::unordered_map<std::string, std::string> values;
	};

	using Clock = std::chrono::steady_clock;
	using EdgeInput = std::tuple<int64_t, int64_t, std::unordered_map<std::string, zyx::Value>>;

	std::string jsonEscape(const std::string &value) {
		std::ostringstream out;
		for (char ch: value) {
			switch (ch) {
				case '\\':
					out << "\\\\";
					break;
				case '"':
					out << "\\\"";
					break;
				case '\b':
					out << "\\b";
					break;
				case '\f':
					out << "\\f";
					break;
				case '\n':
					out << "\\n";
					break;
				case '\r':
					out << "\\r";
					break;
				case '\t':
					out << "\\t";
					break;
				default:
					if (static_cast<unsigned char>(ch) < 0x20) {
						out << "\\u00";
						constexpr char hex[] = "0123456789abcdef";
						out << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
					} else {
						out << ch;
					}
			}
		}
		return out.str();
	}

	void emitSample(const std::string &workload, const std::string &scale, int iteration, double latencyMs) {
		std::cout << "{\"database\":\"" << kDatabase << "\",\"equivalent_mode\":\"" << kEquivalentMode
				  << "\",\"event\":\"sample\",\"iteration\":" << iteration << ",\"latency_ms\":" << latencyMs
				  << ",\"scale\":\"" << jsonEscape(scale) << "\",\"status\":\"ok\",\"workload\":\"" << workload
				  << "\"}\n";
	}

	void emitProfileEvent(const std::string &workload, const std::string &scale, const std::string &profile,
						  int iteration, const std::string &phase, double totalTimeMs, uint64_t calls) {
		std::cout << "{\"database\":\"" << kDatabase << "\",\"equivalent_mode\":\"" << kEquivalentMode
				  << "\",\"event\":\"profile\",\"iteration\":" << iteration << ",\"phase\":\"" << jsonEscape(phase)
				  << "\",\"profile\":\"" << jsonEscape(profile) << "\",\"scale\":\"" << jsonEscape(scale)
				  << "\",\"total_time_ms\":" << totalTimeMs << ",\"calls\":" << calls << ",\"workload\":\""
				  << jsonEscape(workload) << "\"}\n";
	}

	void emitProfileSnapshot(const Options &options, const std::string &workload, int iteration,
							 const graph::debug::PerfTrace::Snapshot &snapshot) {
		for (const auto &[phase, entry]: snapshot) {
			const double totalMs = static_cast<double>(entry.totalNs) / 1e6;
			emitProfileEvent(workload, options.scale, options.profile, iteration, phase, totalMs, entry.calls);
		}
	}

	void emitError(const std::string &workload, const std::string &scale, const std::string &error) {
		std::cerr << "{\"database\":\"" << kDatabase << "\",\"equivalent_mode\":\"" << kEquivalentMode
				  << "\",\"error\":\"" << jsonEscape(error) << "\",\"event\":\"error\",\"scale\":\""
				  << jsonEscape(scale) << "\",\"status\":\"failed\",\"workload\":\"" << workload << "\"}\n";
	}

	std::vector<std::string> parseCsvLine(const std::string &line) {
		std::vector<std::string> fields;
		std::string field;
		bool inQuotes = false;
		for (size_t i = 0; i < line.size(); ++i) {
			const char ch = line[i];
			if (inQuotes) {
				if (ch == '"') {
					if (i + 1 < line.size() && line[i + 1] == '"') {
						field.push_back('"');
						++i;
					} else {
						inQuotes = false;
					}
				} else {
					field.push_back(ch);
				}
			} else if (ch == '"') {
				inQuotes = true;
			} else if (ch == ',') {
				fields.push_back(field);
				field.clear();
			} else {
				field.push_back(ch);
			}
		}
		fields.push_back(field);
		return fields;
	}

	std::vector<CsvRow> readCsv(const std::filesystem::path &path) {
		std::ifstream file(path);
		if (!file) {
			throw std::runtime_error("failed to open CSV: " + path.string());
		}

		std::string headerLine;
		if (!std::getline(file, headerLine)) {
			return {};
		}
		if (!headerLine.empty() && headerLine.back() == '\r') {
			headerLine.pop_back();
		}
		const auto headers = parseCsvLine(headerLine);
		std::vector<CsvRow> rows;
		std::string line;
		while (std::getline(file, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty()) {
				continue;
			}
			const auto fields = parseCsvLine(line);
			CsvRow row;
			for (size_t i = 0; i < headers.size() && i < fields.size(); ++i) {
				row.values.emplace(headers[i], fields[i]);
			}
			rows.push_back(std::move(row));
		}
		return rows;
	}

	const std::string &field(const CsvRow &row, const std::string &name) {
		auto it = row.values.find(name);
		if (it == row.values.end()) {
			throw std::runtime_error("missing CSV field: " + name);
		}
		return it->second;
	}

	int64_t toInt64(const std::string &value) { return std::stoll(value); }

	double toDouble(const std::string &value) { return std::stod(value); }

	int64_t scalarInt(zyx::Result result) {
		if (!result.isSuccess()) {
			throw std::runtime_error(result.getError());
		}
		if (!result.hasNext()) {
			return 0;
		}
		result.next();
		zyx::Value value = result.get(0);
		if (const auto *intValue = std::get_if<int64_t>(&value)) {
			return *intValue;
		}
		if (const auto *doubleValue = std::get_if<double>(&value)) {
			return static_cast<int64_t>(*doubleValue);
		}
		if (const auto *boolValue = std::get_if<bool>(&value)) {
			return *boolValue ? 1 : 0;
		}
		return 0;
	}

	bool scalarTruthy(zyx::Result result) {
		if (!result.isSuccess()) {
			throw std::runtime_error(result.getError());
		}
		if (!result.hasNext()) {
			return false;
		}
		result.next();
		zyx::Value value = result.get(0);
		if (std::holds_alternative<std::monostate>(value)) {
			return false;
		}
		if (const auto *boolValue = std::get_if<bool>(&value)) {
			return *boolValue;
		}
		if (const auto *intValue = std::get_if<int64_t>(&value)) {
			return *intValue != 0;
		}
		if (const auto *doubleValue = std::get_if<double>(&value)) {
			return *doubleValue != 0.0;
		}
		return true;
	}

	void executeOk(zyx::Database &db, const std::string &query) {
		auto result = db.execute(query);
		if (!result.isSuccess()) {
			throw std::runtime_error(result.getError());
		}
	}

	void requireNonNegative(const std::string &workload, int64_t value) {
		if (value < 0) {
			throw std::runtime_error(workload + " returned a negative count");
		}
	}

	LoadedGraph loadGraph(zyx::Database &db, const std::filesystem::path &dataset) {
		std::optional<zyx::Transaction> loadTxn;
		if (!db.hasActiveTransaction()) {
			loadTxn.emplace(db.beginTransaction());
		}

		LoadedGraph graph;
		std::vector<std::unordered_map<std::string, zyx::Value>> userProps;
		std::vector<std::string> userExternalIds;
		for (const auto &row: readCsv(dataset / "users.csv")) {
			const std::string id = field(row, "id");
			userExternalIds.push_back(id);
			userProps.push_back({
					{"id", id},
					{"age", toInt64(field(row, "age"))},
					{"country", field(row, "country")},
					{"score", toDouble(field(row, "score"))},
			});
		}

		const auto nodeIds = db.createNodes("User", userProps);
		for (size_t i = 0; i < userExternalIds.size() && i < nodeIds.size(); ++i) {
			graph.usersByExternalId.emplace(userExternalIds[i], nodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(nodeIds.size());

		std::vector<std::unordered_map<std::string, zyx::Value>> postProps;
		std::vector<std::string> postExternalIds;
		for (const auto &row: readCsv(dataset / "posts.csv")) {
			const std::string id = field(row, "id");
			postExternalIds.push_back(id);
			postProps.push_back({
					{"id", id},
					{"created_at", toInt64(field(row, "created_at"))},
					{"score", toDouble(field(row, "score"))},
			});
		}

		const auto postNodeIds = db.createNodes("Post", postProps);
		for (size_t i = 0; i < postExternalIds.size() && i < postNodeIds.size(); ++i) {
			graph.postsByExternalId.emplace(postExternalIds[i], postNodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(postNodeIds.size());

		std::vector<std::unordered_map<std::string, zyx::Value>> tagProps;
		std::vector<std::string> tagExternalIds;
		for (const auto &row: readCsv(dataset / "tags.csv")) {
			const std::string id = field(row, "id");
			tagExternalIds.push_back(id);
			tagProps.push_back({
					{"id", id},
					{"rank", toInt64(field(row, "rank"))},
			});
		}

		const auto tagNodeIds = db.createNodes("Tag", tagProps);
		for (size_t i = 0; i < tagExternalIds.size() && i < tagNodeIds.size(); ++i) {
			graph.tagsByExternalId.emplace(tagExternalIds[i], tagNodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(tagNodeIds.size());

		std::vector<EdgeInput> edges;
		for (const auto &row: readCsv(dataset / "follows.csv")) {
			auto src = graph.usersByExternalId.find(field(row, "src"));
			auto dst = graph.usersByExternalId.find(field(row, "dst"));
			if (src == graph.usersByExternalId.end() || dst == graph.usersByExternalId.end()) {
				throw std::runtime_error("follows.csv references unknown user");
			}
			edges.emplace_back(src->second, dst->second,
							   std::unordered_map<std::string, zyx::Value>{{"weight", toInt64(field(row, "weight"))}});
		}
		graph.loadedRows += static_cast<int64_t>(db.createEdges("FOLLOWS", edges).size());

		std::vector<EdgeInput> authoredEdges;
		for (const auto &row: readCsv(dataset / "authored.csv")) {
			auto src = graph.usersByExternalId.find(field(row, "src"));
			auto dst = graph.postsByExternalId.find(field(row, "dst"));
			if (src == graph.usersByExternalId.end() || dst == graph.postsByExternalId.end()) {
				throw std::runtime_error("authored.csv references unknown user or post");
			}
			authoredEdges.emplace_back(
					src->second, dst->second,
					std::unordered_map<std::string, zyx::Value>{{"weight", toInt64(field(row, "weight"))}});
		}
		graph.loadedRows += static_cast<int64_t>(db.createEdges("AUTHORED", authoredEdges).size());

		std::vector<EdgeInput> hasTagEdges;
		for (const auto &row: readCsv(dataset / "has_tag.csv")) {
			auto src = graph.postsByExternalId.find(field(row, "src"));
			auto dst = graph.tagsByExternalId.find(field(row, "dst"));
			if (src == graph.postsByExternalId.end() || dst == graph.tagsByExternalId.end()) {
				throw std::runtime_error("has_tag.csv references unknown post or tag");
			}
			hasTagEdges.emplace_back(
					src->second, dst->second,
					std::unordered_map<std::string, zyx::Value>{{"weight", toInt64(field(row, "weight"))}});
		}
		graph.loadedRows += static_cast<int64_t>(db.createEdges("HAS_TAG", hasTagEdges).size());

		if (loadTxn) {
			loadTxn->commit();
		}

		return graph;
	}

	void createBenchmarkIndexes(zyx::Database &db, const Options &options) {
		executeOk(db, "CREATE INDEX ON :User(id)");
		if (options.profile == kProfileIndexed) {
			executeOk(db, "CREATE INDEX ON :User(country)");
			executeOk(db, "CREATE INDEX ON :User(age)");
		}
	}

	std::filesystem::path suffixedDbPath(const std::filesystem::path &base, const std::string &suffix) {
		return std::filesystem::path(base.string() + suffix);
	}

	LoadedGraph loadDatabase(const Options &options, const std::filesystem::path &dbPath) {
		std::filesystem::remove_all(dbPath);
		zyx::Database db(dbPath.string());
		db.open();
		LoadedGraph loaded = loadGraph(db, options.dataset);
		createBenchmarkIndexes(db, options);
		db.close();
		return loaded;
	}

	template<typename Fn, typename ValidateFn>
	auto measureOperation(const Options &options, const std::string &workload, int iteration, Fn &&operation,
						  ValidateFn &&validate) {
		if (options.emitProfile) {
			graph::debug::PerfTrace::setEnabled(true);
			graph::debug::PerfTrace::reset();
		}

		const auto start = Clock::now();
		try {
			auto value = operation();
			const auto end = Clock::now();
			graph::debug::PerfTrace::Snapshot snapshot;
			if (options.emitProfile) {
				snapshot = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setEnabled(false);
			}
			const auto latencyMs = std::chrono::duration<double, std::milli>(end - start).count();
			validate(value);
			emitSample(workload, options.scale, iteration, latencyMs);
			if (options.emitProfile) {
				emitProfileSnapshot(options, workload, iteration, snapshot);
			}
			return value;
		} catch (...) {
			if (options.emitProfile) {
				[[maybe_unused]] const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setEnabled(false);
			}
			throw;
		}
	}

	LoadedGraph measureLoadWorkload(const Options &options) {
		for (int i = 0; i < options.warmup; ++i) {
			const auto warmupPath = suffixedDbPath(options.dbPath, ".load-warmup-" + std::to_string(i));
			const LoadedGraph loaded = loadDatabase(options, warmupPath);
			requireNonNegative("load_nodes_edges", loaded.loadedRows);
			std::filesystem::remove_all(warmupPath);
		}

		for (int i = 0; i < options.iterations; ++i) {
			const auto iterationPath = suffixedDbPath(options.dbPath, ".load-iteration-" + std::to_string(i));
			const LoadedGraph loaded = measureOperation(
					options, "load_nodes_edges", i, [&]() { return loadDatabase(options, iterationPath); },
					[](const LoadedGraph &loadedGraph) {
						requireNonNegative("load_nodes_edges", loadedGraph.loadedRows);
					});
			std::filesystem::remove_all(iterationPath);
		}

		return loadDatabase(options, options.dbPath);
	}

	template<typename Fn>
	void runMeasured(const Options &options, const std::string &workload, Fn &&operation, bool validateResult = true) {
		for (int i = 0; i < options.warmup; ++i) {
			const int64_t value = operation();
			if (validateResult) {
				requireNonNegative(workload, value);
			}
		}
		for (int i = 0; i < options.iterations; ++i) {
			(void) measureOperation(
					options, workload, i, [&]() { return operation(); },
					[&](const int64_t value) {
						if (validateResult) {
							requireNonNegative(workload, value);
						}
					});
		}
	}

	template<typename Fn>
	void runMeasuredColdish(const Options &options, const std::string &workload, Fn &&operation,
						  bool validateResult = true) {
		auto runOnce = [&]() {
			zyx::Database db(options.dbPath.string());
			db.open();
			try {
				const int64_t value = operation(db);
				db.close();
				return value;
			} catch (...) {
				db.close();
				throw;
			}
		};

		for (int i = 0; i < options.warmup; ++i) {
			const int64_t value = runOnce();
			if (validateResult) {
				requireNonNegative(workload, value);
			}
		}
		for (int i = 0; i < options.iterations; ++i) {
			(void) measureOperation(
					options, workload, i, runOnce,
					[&](const int64_t value) {
						if (validateResult) {
							requireNonNegative(workload, value);
						}
					});
		}
	}

	template<typename Fn>
	void runQueryWorkload(const Options &options, zyx::Database &db, const std::string &workload, Fn &&operation,
					  bool validateResult = true) {
		if (options.executionMode == kExecutionModeColdish) {
			runMeasuredColdish(options, workload, std::forward<Fn>(operation), validateResult);
		} else {
			runMeasured(options, workload, [&]() { return operation(db); }, validateResult);
		}
	}

	template<typename Fn>
	int64_t rowCount(Fn &&operation) {
		auto result = operation();
		if constexpr (std::is_convertible_v<decltype(result), zyx::Result>) {
			if (!result.isSuccess()) {
				throw std::runtime_error(result.getError());
			}
			int64_t count = 0;
			while (result.hasNext()) {
				result.next();
				++count;
			}
			return count;
		} else {
			return static_cast<int64_t>(result.rowCount());
		}
	}

	Options parseArgs(int argc, char **argv) {
		Options options;
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			auto requireValue = [&](const std::string &name) -> std::string {
				if (i + 1 >= argc) {
					throw std::invalid_argument("missing value for " + name);
				}
				return argv[++i];
			};
			if (arg == "--dataset") {
				options.dataset = requireValue(arg);
			} else if (arg == "--db-path") {
				options.dbPath = requireValue(arg);
			} else if (arg == "--scale") {
				options.scale = requireValue(arg);
			} else if (arg == "--profile") {
				options.profile = requireValue(arg);
				if (options.profile != kProfileScan && options.profile != kProfileIndexed) {
					throw std::invalid_argument("--profile must be scan or indexed");
				}
			} else if (arg == "--emit-profile") {
				options.emitProfile = true;
			} else if (arg == "--execution-mode") {
				options.executionMode = requireValue(arg);
				if (options.executionMode != kExecutionModeWarm && options.executionMode != kExecutionModeColdish) {
					throw std::invalid_argument("--execution-mode must be warm or cold-ish");
				}
			} else if (arg == "--warmup") {
				options.warmup = std::stoi(requireValue(arg));
			} else if (arg == "--iterations") {
				options.iterations = std::stoi(requireValue(arg));
			} else {
				throw std::invalid_argument("unknown argument: " + arg);
			}
		}
		if (options.dataset.empty()) {
			throw std::invalid_argument("--dataset is required");
		}
		if (options.dbPath.empty()) {
			throw std::invalid_argument("--db-path is required");
		}
		if (options.scale.empty()) {
			throw std::invalid_argument("--scale is required");
		}
		if (options.profile != kProfileScan && options.profile != kProfileIndexed) {
			throw std::invalid_argument("--profile must be scan or indexed");
		}
		if (options.executionMode != kExecutionModeWarm && options.executionMode != kExecutionModeColdish) {
			throw std::invalid_argument("--execution-mode must be warm or cold-ish");
		}
		if (options.warmup < 0) {
			throw std::invalid_argument("--warmup must be >= 0");
		}
		if (options.iterations <= 0) {
			throw std::invalid_argument("--iterations must be > 0");
		}
		return options;
	}

	int run(const Options &options) {
		LoadedGraph loaded = measureLoadWorkload(options);

		zyx::Database db(options.dbPath.string());
		if (options.executionMode == kExecutionModeWarm) {
			db.open();
		}

		if (options.profile == kProfileScan) {
			runQueryWorkload(options, db, "label_scan_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' RETURN count(u)"));
			});
			runQueryWorkload(options, db, "all_nodes_property_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (n) WHERE n.score >= 900.0 RETURN count(n)"));
			});
			runQueryWorkload(options, db, "label_multi_property_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' AND u.age >= 30 RETURN count(u)"));
			});
			runQueryWorkload(options, db, "relationship_type_scan", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH ()-[r:FOLLOWS]->() RETURN count(r)"));
			});
			runQueryWorkload(options, db, "relationship_property_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH ()-[r:FOLLOWS]->() WHERE r.weight = 1 RETURN count(r)"));
			});
			runQueryWorkload(options, db, "one_hop_expand", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) RETURN count(v)"));
			});
			runQueryWorkload(options, db, "two_hop_expand", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute(
						"MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v)"));
			});
			runQueryWorkload(options, db, "shortest_path_chain", [](zyx::Database &queryDb) {
				return scalarTruthy(queryDb.execute("MATCH (src:User {id: 'user-000001'}), (dst:User {id: 'user-000006'}) "
											        "RETURN shortestPath((src)-[:FOLLOWS*1..6]->(dst))"))
							   ? int64_t{1}
							   : int64_t{0};
			});
			runQueryWorkload(options, db, "aggregation_group_by", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) RETURN count(DISTINCT u.country)"));
			});
			runQueryWorkload(options, db, "aggregation_count_by_group", [](zyx::Database &queryDb) {
				return rowCount([&]() { return queryDb.execute("MATCH (u:User) RETURN u.country, count(*)"); });
			});
			runQueryWorkload(options, db, "topk_property_sort", [](zyx::Database &queryDb) {
				return rowCount([&]() { return queryDb.execute("MATCH (u:User) RETURN u.id ORDER BY u.score DESC LIMIT 100"); });
			});
		} else {
			runQueryWorkload(options, db, "point_lookup_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User {id: 'user-000001'}) RETURN count(u)"));
			});
			runQueryWorkload(options, db, "property_equality_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' RETURN count(u)"));
			});
			runQueryWorkload(options, db, "property_range_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.age >= 30 AND u.age < 40 RETURN count(u)"));
			});
		}

		if (options.executionMode == kExecutionModeWarm) {
			db.close();
		}
		return 0;
	}

} // namespace

int main(int argc, char **argv) {
	std::string scale;
	try {
		const Options options = parseArgs(argc, argv);
		scale = options.scale;
		return run(options);
	} catch (const std::exception &exc) {
		emitError("run_all", scale, exc.what());
		return 1;
	}
}
