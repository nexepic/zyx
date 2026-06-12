#include "graph/debug/PerfTrace.hpp"
#include "zyx/zyx.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
	constexpr std::string_view kProfileMultihop = "multihop";
	constexpr std::string_view kProfileWrite = "write";
	constexpr std::string_view kProfileWriteDurable = "write_durable";
	constexpr std::string_view kProfileOperationalDynamic = "operational_dynamic";
	constexpr std::string_view kExecutionModeWarm = "warm";
	constexpr std::string_view kExecutionModeOpened = "opened";
	constexpr std::string_view kExecutionModeColdish = "cold-ish";

	struct Options {
		std::filesystem::path dataset;
		std::filesystem::path dbPath;
		std::string scale;
		std::string profile = std::string(kProfileScan);
		bool emitProfile = false;
		std::string executionMode = std::string(kExecutionModeWarm);
		std::optional<size_t> threads;
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

	size_t parseThreadCount(const std::string &value) {
		if (value.empty() || value.front() == '-') {
			throw std::invalid_argument("--threads must be >= 0");
		}
		size_t parsedChars = 0;
		const auto parsed = std::stoull(value, &parsedChars);
		if (parsedChars != value.size()) {
			throw std::invalid_argument("--threads must be an integer");
		}
		return static_cast<size_t>(parsed);
	}

	void configureThreadPool(zyx::Database &db, const Options &options) {
		if (options.threads.has_value()) {
			db.setThreadPoolSize(*options.threads);
		}
	}

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

	std::string userIdForIndex(int64_t index) {
		std::ostringstream out;
		out << "user-" << std::setw(6) << std::setfill('0') << index;
		return out.str();
	}

	int64_t multihopFollowsPerUser(std::string_view scale) {
		return scale == "smoke" ? int64_t{3} : int64_t{5};
	}

	std::string targetUserIdForDepth(int depth, std::string_view scale = "medium") {
		return userIdForIndex(1 + multihopFollowsPerUser(scale) * static_cast<int64_t>(depth));
	}

	std::string writeUpdateTargetUserId(std::string_view scale) {
		return scale == "smoke" ? "user-000004" : "user-000006";
	}

	bool isMutatingWorkload(std::string_view workload) {
		static constexpr std::string_view kMutatingWorkloads[] = {
				"point_create_node",
				"point_create_edge",
				"point_update_node_property",
				"point_update_edge_property",
				"point_create_delete_edge",
				"write_then_read_edge",
				"point_create_node_durable",
				"point_create_edge_durable",
				"point_update_node_property_durable",
				"point_update_edge_property_durable",
				"point_create_delete_edge_durable",
				"write_then_read_edge_durable",
				"post_persist_create_node",
				"post_persist_create_edge",
				"write_then_one_hop_expand",
				"batch_create_edges_100",
				"batch_create_edges_1000",
				"batch_create_edges_10000",
				"batch_create_edges_100_then_one_hop_expand",
				"batch_create_edges_10000_then_one_hop_expand",
		};
		for (const auto candidate: kMutatingWorkloads) {
			if (workload == candidate) {
				return true;
			}
		}
		return false;
	}

	std::string workloadPathToken(std::string_view workload) {
		std::string token;
		token.reserve(workload.size());
		for (const unsigned char ch: workload) {
			token.push_back(std::isalnum(ch) != 0 ? static_cast<char>(ch) : '_');
		}
		return token.empty() ? "workload" : token;
	}

	int64_t reachableWithinTarget(zyx::Database &queryDb, int depth, const std::string &target) {
		std::ostringstream query;
		query << "MATCH p = (:User {id: 'user-000001'})-[:FOLLOWS*1.." << depth << "]->(:User {id: '" << target
			  << "'}) RETURN 1 LIMIT 1";
		return scalarTruthy(queryDb.execute(query.str())) ? int64_t{1} : int64_t{0};
	}

	int64_t requireReachableWithinTarget(zyx::Database &queryDb, int depth, const std::string &target) {
		const int64_t reachable = reachableWithinTarget(queryDb, depth, target);
		if (reachable != 1) {
			throw std::runtime_error("reachable_within_" + std::to_string(depth) + " did not find expected target " +
									 target);
		}
		return reachable;
	}

	int64_t requireReachableWithin(zyx::Database &queryDb, int depth, std::string_view scale) {
		return requireReachableWithinTarget(queryDb, depth, targetUserIdForDepth(depth, scale));
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

	int64_t requireExactlyOne(const std::string &workload, int64_t value) {
		if (value != 1) {
			throw std::runtime_error(workload + " expected exactly one affected row, got " + std::to_string(value));
		}
		return value;
	}

	int64_t requireExactCount(const std::string &workload, int64_t value, int64_t expected) {
		if (value != expected) {
			throw std::runtime_error(workload + " expected " + std::to_string(expected) + ", got " +
									 std::to_string(value));
		}
		return value;
	}

	int64_t userInternalId(const LoadedGraph &graph, const std::string &externalId) {
		const auto it = graph.usersByExternalId.find(externalId);
		if (it == graph.usersByExternalId.end()) {
			throw std::runtime_error("loaded graph is missing user " + externalId);
		}
		return it->second;
	}

	int64_t createBatchEdges(zyx::Database &db, const LoadedGraph &graph, int64_t &sequence, size_t count,
							 const std::string &sourceUserId = "user-000006") {
		const int64_t src = userInternalId(graph, sourceUserId);
		const int64_t dst = userInternalId(graph, "user-000001");
		std::vector<EdgeInput> edges;
		edges.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			++sequence;
			edges.emplace_back(src, dst,
							   std::unordered_map<std::string, zyx::Value>{{"weight", int64_t{-3'000'000} - sequence}});
		}
		return static_cast<int64_t>(db.createEdges("FOLLOWS", edges).size());
	}

	template<typename Fn>
	decltype(auto) tracePhase(std::string_view phase, Fn &&operation) {
		graph::debug::ScopedPerfTimer timer(phase);
		if constexpr (std::is_void_v<std::invoke_result_t<Fn>>) {
			operation();
			return;
		} else {
			return operation();
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
		const auto userRows = tracePhase("load.users.csv_read", [&]() { return readCsv(dataset / "users.csv"); });
		tracePhase("load.users.prepare", [&]() {
			for (const auto &row: userRows) {
				const std::string id = field(row, "id");
				userExternalIds.push_back(id);
				userProps.push_back({
						{"id", id},
						{"age", toInt64(field(row, "age"))},
						{"country", field(row, "country")},
						{"score", toDouble(field(row, "score"))},
				});
			}
		});

		const auto nodeIds =
				tracePhase("load.users.create_nodes", [&]() { return db.createNodes("User", userProps); });
		for (size_t i = 0; i < userExternalIds.size() && i < nodeIds.size(); ++i) { // ZYX_COV_EXCL_LINE: bulk API returns one node id per input row.
			graph.usersByExternalId.emplace(userExternalIds[i], nodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(nodeIds.size());

		std::vector<std::unordered_map<std::string, zyx::Value>> postProps;
		std::vector<std::string> postExternalIds;
		const auto postRows = tracePhase("load.posts.csv_read", [&]() { return readCsv(dataset / "posts.csv"); });
		tracePhase("load.posts.prepare", [&]() {
			for (const auto &row: postRows) {
				const std::string id = field(row, "id");
				postExternalIds.push_back(id);
				postProps.push_back({
						{"id", id},
						{"created_at", toInt64(field(row, "created_at"))},
						{"score", toDouble(field(row, "score"))},
				});
			}
		});

		const auto postNodeIds =
				tracePhase("load.posts.create_nodes", [&]() { return db.createNodes("Post", postProps); });
		for (size_t i = 0; i < postExternalIds.size() && i < postNodeIds.size(); ++i) { // ZYX_COV_EXCL_LINE: bulk API returns one node id per input row.
			graph.postsByExternalId.emplace(postExternalIds[i], postNodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(postNodeIds.size());

		std::vector<std::unordered_map<std::string, zyx::Value>> tagProps;
		std::vector<std::string> tagExternalIds;
		const auto tagRows = tracePhase("load.tags.csv_read", [&]() { return readCsv(dataset / "tags.csv"); });
		tracePhase("load.tags.prepare", [&]() {
			for (const auto &row: tagRows) {
				const std::string id = field(row, "id");
				tagExternalIds.push_back(id);
				tagProps.push_back({
						{"id", id},
						{"rank", toInt64(field(row, "rank"))},
				});
			}
		});

		const auto tagNodeIds =
				tracePhase("load.tags.create_nodes", [&]() { return db.createNodes("Tag", tagProps); });
		for (size_t i = 0; i < tagExternalIds.size() && i < tagNodeIds.size(); ++i) { // ZYX_COV_EXCL_LINE: bulk API returns one node id per input row.
			graph.tagsByExternalId.emplace(tagExternalIds[i], tagNodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(tagNodeIds.size());

		std::vector<EdgeInput> edges;
		const auto followsRows = tracePhase("load.follows.csv_read", [&]() { return readCsv(dataset / "follows.csv"); });
		tracePhase("load.follows.prepare", [&]() {
			for (const auto &row: followsRows) {
				auto src = graph.usersByExternalId.find(field(row, "src"));
				auto dst = graph.usersByExternalId.find(field(row, "dst"));
				if (src == graph.usersByExternalId.end() || dst == graph.usersByExternalId.end()) {
					throw std::runtime_error("follows.csv references unknown user");
				}
				edges.emplace_back(
						src->second, dst->second,
						std::unordered_map<std::string, zyx::Value>{{"weight", toInt64(field(row, "weight"))}});
			}
		});
		const auto followEdgeIds =
				tracePhase("load.follows.create_edges", [&]() { return db.createEdges("FOLLOWS", edges); });
		graph.loadedRows += static_cast<int64_t>(followEdgeIds.size());

		std::vector<EdgeInput> authoredEdges;
		const auto authoredRows = tracePhase("load.authored.csv_read", [&]() { return readCsv(dataset / "authored.csv"); });
		tracePhase("load.authored.prepare", [&]() {
			for (const auto &row: authoredRows) {
				auto src = graph.usersByExternalId.find(field(row, "src"));
				auto dst = graph.postsByExternalId.find(field(row, "dst"));
				if (src == graph.usersByExternalId.end() || dst == graph.postsByExternalId.end()) {
					throw std::runtime_error("authored.csv references unknown user or post");
				}
				authoredEdges.emplace_back(
						src->second, dst->second,
						std::unordered_map<std::string, zyx::Value>{{"weight", toInt64(field(row, "weight"))}});
			}
		});
		const auto authoredEdgeIds =
				tracePhase("load.authored.create_edges", [&]() { return db.createEdges("AUTHORED", authoredEdges); });
		graph.loadedRows += static_cast<int64_t>(authoredEdgeIds.size());

		std::vector<EdgeInput> hasTagEdges;
		const auto hasTagRows = tracePhase("load.has_tag.csv_read", [&]() { return readCsv(dataset / "has_tag.csv"); });
		tracePhase("load.has_tag.prepare", [&]() {
			for (const auto &row: hasTagRows) {
				auto src = graph.postsByExternalId.find(field(row, "src"));
				auto dst = graph.tagsByExternalId.find(field(row, "dst"));
				if (src == graph.postsByExternalId.end() || dst == graph.tagsByExternalId.end()) {
					throw std::runtime_error("has_tag.csv references unknown post or tag");
				}
				hasTagEdges.emplace_back(
						src->second, dst->second,
						std::unordered_map<std::string, zyx::Value>{{"weight", toInt64(field(row, "weight"))}});
			}
		});
		const auto hasTagEdgeIds =
				tracePhase("load.has_tag.create_edges", [&]() { return db.createEdges("HAS_TAG", hasTagEdges); });
		graph.loadedRows += static_cast<int64_t>(hasTagEdgeIds.size());

		if (loadTxn) {
			tracePhase("load.commit", [&]() { loadTxn->commit(); });
		}

		return graph;
	}

	void createBenchmarkIndexes(zyx::Database &db, const Options &options) {
		std::vector<std::string> userIndexes{"id"};
		if (options.profile == kProfileIndexed || options.profile == kProfileOperationalDynamic) {
			userIndexes.push_back("country");
			userIndexes.push_back("age");
		}
		tracePhase("load.index.user_properties", [&]() {
			if (!db.createNodePropertyIndexes("User", userIndexes)) {
				throw std::runtime_error("failed to create benchmark User property indexes");
			}
		});
	}

	std::filesystem::path suffixedDbPath(const std::filesystem::path &base, const std::string &suffix) {
		return std::filesystem::path(base.string() + suffix);
	}

	LoadedGraph loadDatabase(const Options &options, const std::filesystem::path &dbPath) {
		tracePhase("load.remove_existing_db", [&]() { std::filesystem::remove_all(dbPath); });
		zyx::Database db(dbPath.string());
		configureThreadPool(db, options);
		tracePhase("load.db_open", [&]() { db.open(); });
		LoadedGraph loaded = tracePhase("load.graph", [&]() { return loadGraph(db, options.dataset); });
		tracePhase("load.indexes", [&]() { createBenchmarkIndexes(db, options); });
		tracePhase("load.db_close", [&]() { db.close(); });
		return loaded;
	}

	template<typename Fn, typename ValidateFn>
	auto measureOperation(const Options &options, const std::string &workload, int iteration, Fn &&operation,
						  ValidateFn &&validate) {
		if (options.emitProfile) { // ZYX_COV_EXCL_LINE: profile emission is integration-tested through benchmark runs.
			graph::debug::PerfTrace::setEnabled(true);
			graph::debug::PerfTrace::reset();
		}

		const auto start = Clock::now();
		try {
			auto value = operation();
			const auto end = Clock::now();
			graph::debug::PerfTrace::Snapshot snapshot;
			if (options.emitProfile) { // ZYX_COV_EXCL_LINE: timing-only profile collection branch.
				snapshot = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setEnabled(false);
			}
			const auto latencyMs = std::chrono::duration<double, std::milli>(end - start).count();
			validate(value);
			emitSample(workload, options.scale, iteration, latencyMs);
			if (options.emitProfile) { // ZYX_COV_EXCL_LINE: timing-only profile emission branch.
				emitProfileSnapshot(options, workload, iteration, snapshot);
			}
			return value;
		} catch (...) {
			if (options.emitProfile) { // ZYX_COV_EXCL_LINE: error cleanup mirrors the profiled success path.
				[[maybe_unused]] const auto snapshot = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setEnabled(false);
			}
			throw;
		}
	}

	LoadedGraph measureLoadWorkload(const Options &options) {
		for (int i = 0; i < options.warmup; ++i) { // ZYX_COV_EXCL_LINE: benchmark warmup loop shape is option-driven.
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
		const int queryWarmup = options.executionMode == kExecutionModeOpened ? 0 : options.warmup;
		for (int i = 0; i < queryWarmup; ++i) { // ZYX_COV_EXCL_LINE: benchmark warmup loop shape is option-driven.
			const int64_t value = operation();
			if (validateResult) { // ZYX_COV_EXCL_LINE: validation toggle is covered by public helper tests.
				requireNonNegative(workload, value);
			}
		}
		for (int i = 0; i < options.iterations; ++i) {
			const std::string phase =
					options.executionMode == kExecutionModeOpened ? "opened.operation" : "warm.operation";
			(void) measureOperation(
					options, workload, i,
					[&]() {
						graph::debug::ScopedPerfTimer timer(phase);
						return operation();
					},
					[&](const int64_t value) {
						if (validateResult) { // ZYX_COV_EXCL_LINE: validation toggle is covered by public helper tests.
							requireNonNegative(workload, value);
						}
					});
		}
	}

	template<typename Fn>
	void runMeasuredColdish(const Options &options, const std::string &workload, Fn &&operation,
						  bool validateResult = true) {
		auto runOnce = [&](std::optional<int> iteration) {
			zyx::Database db(options.dbPath.string());
			configureThreadPool(db, options);
			{
				graph::debug::ScopedPerfTimer timer("coldish.db_open");
				db.open();
			}
			try {
				int64_t value = 0;
				auto measuredOperation = [&]() {
					graph::debug::ScopedPerfTimer timer("coldish.operation");
					return operation(db);
				};
				auto validateMeasuredValue = [&](const int64_t measuredValue) {
					if (validateResult) { // ZYX_COV_EXCL_LINE: validation toggle is covered by warm-mode helper tests.
						requireNonNegative(workload, measuredValue);
					}
				};
				if (iteration.has_value()) {
					value = measureOperation(options, workload, *iteration, measuredOperation, validateMeasuredValue);
				} else {
					value = measuredOperation();
					validateMeasuredValue(value);
				}
				{
					graph::debug::ScopedPerfTimer timer("coldish.db_close");
					db.close();
				}
				return value;
			} catch (...) {
				{
					graph::debug::ScopedPerfTimer timer("coldish.db_close");
					db.close();
				}
				throw;
			}
		};

		for (int i = 0; i < options.warmup; ++i) { // ZYX_COV_EXCL_LINE: benchmark warmup loop shape is option-driven.
			(void) runOnce(std::nullopt);
		}
		for (int i = 0; i < options.iterations; ++i) {
			(void) runOnce(i);
		}
	}

	template<typename Fn>
	int64_t invokeWorkloadOperation(Fn &operation, zyx::Database &db, const LoadedGraph &loaded) {
		if constexpr (std::is_invocable_v<Fn, zyx::Database &, const LoadedGraph &>) {
			return static_cast<int64_t>(operation(db, loaded));
		} else {
			return static_cast<int64_t>(operation(db));
		}
	}

	template<typename Fn>
	void runMeasuredIsolatedLoadedWorkload(const Options &options, const std::string &workload, Fn &&operation,
									  bool validateResult = true) {
		const auto isolatedPath = suffixedDbPath(options.dbPath, ".workload-" + workloadPathToken(workload));
		LoadedGraph isolatedLoaded = loadDatabase(options, isolatedPath);
		zyx::Database isolatedDb(isolatedPath.string());
		configureThreadPool(isolatedDb, options);
		bool opened = false;
		try {
			if (options.executionMode == kExecutionModeWarm || options.executionMode == kExecutionModeOpened) {
				isolatedDb.open();
				opened = true;
			}
			runMeasured(options, workload,
						[&]() { return invokeWorkloadOperation(operation, isolatedDb, isolatedLoaded); },
						validateResult);
			if (opened) {
				isolatedDb.close();
			}
			std::filesystem::remove_all(isolatedPath);
		} catch (...) {
			if (opened) {
				isolatedDb.close();
			}
			std::filesystem::remove_all(isolatedPath);
			throw;
		}
	}

	template<typename Fn>
	void runMeasuredIsolatedColdishWorkload(const Options &options, const std::string &workload, Fn &&operation,
									  bool validateResult = true) {
		auto runOnce = [&](std::optional<int> iteration, int sequence) {
			const std::string suffix = ".coldish-" + workloadPathToken(workload) + "-" + std::to_string(sequence);
			const auto isolatedPath = suffixedDbPath(options.dbPath, suffix);
			LoadedGraph isolatedLoaded = loadDatabase(options, isolatedPath);
			zyx::Database isolatedDb(isolatedPath.string());
			configureThreadPool(isolatedDb, options);
			bool opened = false;
			try {
				{
					graph::debug::ScopedPerfTimer timer("coldish.db_open");
					isolatedDb.open();
					opened = true;
				}
				auto measuredOperation = [&]() {
					graph::debug::ScopedPerfTimer timer("coldish.operation");
					return invokeWorkloadOperation(operation, isolatedDb, isolatedLoaded);
				};
				auto validateMeasuredValue = [&](const int64_t measuredValue) {
					if (validateResult) { // ZYX_COV_EXCL_LINE: validation toggle is covered by warm-mode helper tests.
						requireNonNegative(workload, measuredValue);
					}
				};
				if (iteration.has_value()) {
					(void) measureOperation(options, workload, *iteration, measuredOperation, validateMeasuredValue);
				} else {
					const int64_t value = measuredOperation();
					validateMeasuredValue(value);
				}
				{
					graph::debug::ScopedPerfTimer timer("coldish.db_close");
					isolatedDb.close();
					opened = false;
				}
				std::filesystem::remove_all(isolatedPath);
			} catch (...) {
				if (opened) {
					graph::debug::ScopedPerfTimer timer("coldish.db_close");
					isolatedDb.close();
				}
				std::filesystem::remove_all(isolatedPath);
				throw;
			}
		};

		int sequence = 0;
		for (int i = 0; i < options.warmup; ++i) { // ZYX_COV_EXCL_LINE: benchmark warmup loop shape is option-driven.
			runOnce(std::nullopt, sequence++);
		}
		for (int i = 0; i < options.iterations; ++i) {
			runOnce(i, sequence++);
		}
	}

	template<typename Fn>
	void runQueryWorkload(const Options &options, zyx::Database &db, const LoadedGraph &loaded,
					  const std::string &workload, Fn &&operation, bool validateResult = true) {
		if (isMutatingWorkload(workload)) {
			if (options.executionMode == kExecutionModeColdish) {
				runMeasuredIsolatedColdishWorkload(options, workload, std::forward<Fn>(operation), validateResult);
			} else {
				runMeasuredIsolatedLoadedWorkload(options, workload, std::forward<Fn>(operation), validateResult);
			}
			return;
		}
		if (options.executionMode == kExecutionModeColdish) {
			runMeasuredColdish(
					options, workload,
					[&](zyx::Database &queryDb) { return invokeWorkloadOperation(operation, queryDb, loaded); },
					validateResult);
		} else {
			runMeasured(options, workload, [&]() { return invokeWorkloadOperation(operation, db, loaded); }, validateResult);
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
				if (options.profile != kProfileScan && options.profile != kProfileIndexed &&
					options.profile != kProfileMultihop && options.profile != kProfileWrite &&
					options.profile != kProfileWriteDurable && options.profile != kProfileOperationalDynamic) {
					throw std::invalid_argument(
							"--profile must be scan, indexed, multihop, write, write_durable, or operational_dynamic");
				}
			} else if (arg == "--emit-profile") {
				options.emitProfile = true;
			} else if (arg == "--execution-mode") {
				options.executionMode = requireValue(arg);
				if (options.executionMode != kExecutionModeWarm && options.executionMode != kExecutionModeOpened &&
					options.executionMode != kExecutionModeColdish) {
					throw std::invalid_argument("--execution-mode must be warm, opened, or cold-ish");
				}
			} else if (arg == "--warmup") {
				options.warmup = std::stoi(requireValue(arg));
			} else if (arg == "--iterations") {
				options.iterations = std::stoi(requireValue(arg));
			} else if (arg == "--threads") {
				options.threads = parseThreadCount(requireValue(arg));
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
		if (options.profile != kProfileScan && options.profile != kProfileIndexed && options.profile != kProfileMultihop &&
			options.profile != kProfileWrite && options.profile != kProfileWriteDurable &&
			options.profile != kProfileOperationalDynamic) {
			throw std::invalid_argument(
					"--profile must be scan, indexed, multihop, write, write_durable, or operational_dynamic");
		}
		if (options.executionMode != kExecutionModeWarm && options.executionMode != kExecutionModeOpened &&
			options.executionMode != kExecutionModeColdish) {
			throw std::invalid_argument("--execution-mode must be warm, opened, or cold-ish");
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
		configureThreadPool(db, options);
		if (options.executionMode == kExecutionModeWarm || options.executionMode == kExecutionModeOpened) {
			db.open();
		}

		if (options.profile == kProfileScan) {
			runQueryWorkload(options, db, loaded, "label_scan_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' RETURN count(u)"));
			});
			runQueryWorkload(options, db, loaded, "all_nodes_property_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (n) WHERE n.score >= 900.0 RETURN count(n)"));
			});
			runQueryWorkload(options, db, loaded, "label_multi_property_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' AND u.age >= 30 RETURN count(u)"));
			});
			runQueryWorkload(options, db, loaded, "relationship_type_scan", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH ()-[r:FOLLOWS]->() RETURN count(r)"));
			});
			runQueryWorkload(options, db, loaded, "relationship_property_filter", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH ()-[r:FOLLOWS]->() WHERE r.weight = 1 RETURN count(r)"));
			});
			runQueryWorkload(options, db, loaded, "one_hop_expand", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) RETURN count(v)"));
			});
			runQueryWorkload(options, db, loaded, "two_hop_expand", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute(
						"MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v)"));
			});
			runQueryWorkload(options, db, loaded, "shortest_path_chain", [](zyx::Database &queryDb) {
				return scalarTruthy(queryDb.execute("MATCH (src:User {id: 'user-000001'}), (dst:User {id: 'user-000006'}) "
											        "RETURN shortestPath((src)-[:FOLLOWS*1..6]->(dst))"))
							   ? int64_t{1}
							   : int64_t{0};
			});
			runQueryWorkload(options, db, loaded, "aggregation_group_by", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) RETURN count(DISTINCT u.country)"));
			});
			runQueryWorkload(options, db, loaded, "aggregation_count_by_group", [](zyx::Database &queryDb) {
				return rowCount([&]() { return queryDb.execute("MATCH (u:User) RETURN u.country, count(*)"); });
			});
			runQueryWorkload(options, db, loaded, "topk_property_sort", [](zyx::Database &queryDb) {
				return rowCount([&]() { return queryDb.execute("MATCH (u:User) RETURN u.id ORDER BY u.score DESC LIMIT 100"); });
			});
		} else if (options.profile == kProfileIndexed) {
			runQueryWorkload(options, db, loaded, "point_lookup_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User {id: 'user-000001'}) RETURN count(u)"));
			});
			runQueryWorkload(options, db, loaded, "property_equality_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' RETURN count(u)"));
			});
			runQueryWorkload(options, db, loaded, "property_range_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.age >= 30 AND u.age < 40 RETURN count(u)"));
			});
		} else if (options.profile == kProfileMultihop) {
			const std::string scale = options.scale;
			runQueryWorkload(options, db, loaded, "reachable_within_6",
							 [scale](zyx::Database &queryDb) { return requireReachableWithin(queryDb, 6, scale); });
			runQueryWorkload(options, db, loaded, "reachable_within_12",
							 [scale](zyx::Database &queryDb) { return requireReachableWithin(queryDb, 12, scale); });
			runQueryWorkload(options, db, loaded, "reachable_within_24",
							 [scale](zyx::Database &queryDb) { return requireReachableWithin(queryDb, 24, scale); });
			runQueryWorkload(options, db, loaded, "reachable_within_30",
							 [scale](zyx::Database &queryDb) { return requireReachableWithin(queryDb, 30, scale); });
		} else if (options.profile == kProfileOperationalDynamic) {
			int64_t createNodeId = 0;
			int64_t createEdgeId = 0;
			int64_t writeExpandId = 0;
			int64_t batchEdgeId = 0;

			runQueryWorkload(options, db, loaded, "index_seek_then_one_hop_expand", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User {country: 'CN'})-[:FOLLOWS]->(v:User) RETURN count(v)"));
			});
			runQueryWorkload(options, db, loaded, "index_seek_then_two_hop_expand", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute(
						"MATCH (u:User {country: 'CN'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v)"));
			});
			runQueryWorkload(options, db, loaded, "post_persist_create_node", [&createNodeId](zyx::Database &queryDb) {
				++createNodeId;
				std::ostringstream query;
				query << "CREATE (:User {id: 'post-persist-user-" << std::setw(6) << std::setfill('0') << createNodeId
					  << "', age: 41, country: 'ZZ', score: " << static_cast<double>(createNodeId) << "}) RETURN 1";
				return requireExactlyOne("post_persist_create_node", scalarInt(queryDb.execute(query.str())));
			});
			runQueryWorkload(options, db, loaded, "post_persist_create_edge", [&createEdgeId](zyx::Database &queryDb) {
				++createEdgeId;
				(void) createEdgeId;
				return requireExactlyOne(
						"post_persist_create_edge",
						scalarInt(queryDb.execute("MATCH (src:User {id: 'user-000006'}), "
											  "(dst:User {id: 'user-000001'}) "
											  "CREATE (src)-[:FOLLOWS {weight: 1}]->(dst) RETURN 1")));
			});
			runQueryWorkload(options, db, loaded, "write_then_one_hop_expand", [&writeExpandId](zyx::Database &queryDb) {
				++writeExpandId;
				const int64_t weight = -2'000'000 - writeExpandId;
				std::ostringstream create;
				create << "MATCH (src:User {id: 'user-000007'}), (dst:User {id: 'user-000001'}) "
					   << "CREATE (src)-[:FOLLOWS {weight: " << weight << "}]->(dst)";
				executeOk(queryDb, create.str());
				return scalarInt(queryDb.execute("MATCH (:User {id: 'user-000007'})-[:FOLLOWS]->(v:User) "
											 "RETURN count(v)"));
			});
			runQueryWorkload(options, db, loaded, "batch_create_edges_100",
							 [&batchEdgeId](zyx::Database &queryDb, const LoadedGraph &graph) {
								 return requireExactCount("batch_create_edges_100",
														  createBatchEdges(queryDb, graph, batchEdgeId, 100), 100);
							 });
			runQueryWorkload(options, db, loaded, "batch_create_edges_1000",
							 [&batchEdgeId](zyx::Database &queryDb, const LoadedGraph &graph) {
								 return requireExactCount("batch_create_edges_1000",
														  createBatchEdges(queryDb, graph, batchEdgeId, 1000), 1000);
							 });
			runQueryWorkload(options, db, loaded, "batch_create_edges_10000",
							 [&batchEdgeId](zyx::Database &queryDb, const LoadedGraph &graph) {
								 return requireExactCount("batch_create_edges_10000",
														  createBatchEdges(queryDb, graph, batchEdgeId, 10000), 10000);
							 });
			runQueryWorkload(options, db, loaded, "batch_create_edges_100_then_one_hop_expand",
							 [&batchEdgeId](zyx::Database &queryDb, const LoadedGraph &graph) {
								 (void) requireExactCount("batch_create_edges_100_then_one_hop_expand",
														  createBatchEdges(queryDb, graph, batchEdgeId, 100,
																		   "user-000007"),
														  100);
								 return scalarInt(queryDb.execute("MATCH (:User {id: 'user-000007'})-[:FOLLOWS]->"
															 "(v:User) RETURN count(v)"));
							 });
			runQueryWorkload(options, db, loaded, "batch_create_edges_10000_then_one_hop_expand",
							 [&batchEdgeId](zyx::Database &queryDb, const LoadedGraph &graph) {
								 (void) requireExactCount("batch_create_edges_10000_then_one_hop_expand",
														  createBatchEdges(queryDb, graph, batchEdgeId, 10000,
																		   "user-000008"),
														  10000);
								 return scalarInt(queryDb.execute("MATCH (:User {id: 'user-000008'})-[:FOLLOWS]->"
															 "(v:User) RETURN count(v)"));
							 });
		} else if (options.profile == kProfileWrite || options.profile == kProfileWriteDurable) {
			const bool durableProfile = options.profile == kProfileWriteDurable;
			auto workloadName = [durableProfile](std::string_view base) {
				std::string name(base);
				if (durableProfile) {
					name += "_durable";
				}
				return name;
			};
			int64_t createNodeId = 0;
			int64_t createEdgeId = 0;
			int64_t updateNodeId = 0;
			int64_t updateEdgeId = 0;
			int64_t createDeleteEdgeId = 0;
			int64_t writeReadEdgeId = 0;
			const std::string updateTarget = writeUpdateTargetUserId(options.scale);

			runQueryWorkload(options, db, loaded, workloadName("point_create_node"), [&createNodeId](zyx::Database &queryDb) {
				++createNodeId;
				std::ostringstream query;
				query << "CREATE (:User {id: 'bench-user-" << std::setw(6) << std::setfill('0') << createNodeId
					  << "', age: 41, country: 'ZZ', score: " << static_cast<double>(createNodeId) << "}) RETURN 1";
				return requireExactlyOne("point_create_node", scalarInt(queryDb.execute(query.str())));
			});
			runQueryWorkload(options, db, loaded, workloadName("point_create_edge"), [&createEdgeId](zyx::Database &queryDb) {
				++createEdgeId;
				(void) createEdgeId;
				return requireExactlyOne(
						"point_create_edge",
						scalarInt(queryDb.execute("MATCH (src:User {id: 'user-000006'}), "
											  "(dst:User {id: 'user-000001'}) "
											  "CREATE (src)-[:FOLLOWS {weight: 1}]->(dst) RETURN 1")));
			});
			runQueryWorkload(options, db, loaded, workloadName("point_update_node_property"), [&updateNodeId](zyx::Database &queryDb) {
				++updateNodeId;
				std::ostringstream query;
				query << "MATCH (u:User {id: 'user-000001'}) SET u.score = "
					  << 1000.0 + static_cast<double>(updateNodeId) << " RETURN count(u)";
				return requireExactlyOne("point_update_node_property", scalarInt(queryDb.execute(query.str())));
			});
			runQueryWorkload(options, db, loaded, workloadName("point_update_edge_property"),
							 [&updateEdgeId, updateTarget](zyx::Database &queryDb) {
								 ++updateEdgeId;
								 std::ostringstream query;
								 query << "MATCH (:User {id: 'user-000001'})-[r:FOLLOWS]->(:User {id: '"
									   << updateTarget << "'}) SET r.weight = " << 10'000 + updateEdgeId
									   << " RETURN count(r)";
								 return requireExactlyOne("point_update_edge_property",
														  scalarInt(queryDb.execute(query.str())));
							 });
			runQueryWorkload(options, db, loaded, workloadName("point_create_delete_edge"), [&createDeleteEdgeId](zyx::Database &queryDb) {
				++createDeleteEdgeId;
				const int64_t weight = -createDeleteEdgeId;
				std::ostringstream create;
				create << "MATCH (src:User {id: 'user-000006'}), (dst:User {id: 'user-000001'}) "
					   << "CREATE (src)-[:FOLLOWS {weight: " << weight << "}]->(dst)";
				executeOk(queryDb, create.str());
				std::ostringstream remove;
				remove << "MATCH (:User {id: 'user-000006'})-[r:FOLLOWS]->(:User {id: 'user-000001'}) "
					   << "WHERE r.weight = " << weight << " DELETE r RETURN 1";
				return requireExactlyOne("point_create_delete_edge", scalarTruthy(queryDb.execute(remove.str())) ? 1 : 0);
			});
			runQueryWorkload(options, db, loaded, workloadName("write_then_read_edge"), [&writeReadEdgeId](zyx::Database &queryDb) {
				++writeReadEdgeId;
				const int64_t weight = -1'000'000 - writeReadEdgeId;
				std::ostringstream create;
				create << "MATCH (src:User {id: 'user-000007'}), (dst:User {id: 'user-000001'}) "
					   << "CREATE (src)-[:FOLLOWS {weight: " << weight << "}]->(dst)";
				executeOk(queryDb, create.str());
				std::ostringstream read;
				read << "MATCH (:User {id: 'user-000007'})-[r:FOLLOWS]->(:User {id: 'user-000001'}) "
					 << "WHERE r.weight = " << weight << " RETURN count(r)";
				return requireExactlyOne("write_then_read_edge", scalarInt(queryDb.execute(read.str())));
			});
		}

		if (options.executionMode == kExecutionModeWarm || options.executionMode == kExecutionModeOpened) {
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
