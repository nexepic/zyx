#include "graph/debug/PerfTrace.hpp"
#include "zyx/zyx.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <type_traits>
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
	constexpr std::string_view kProfileRetrieval = "retrieval";
	constexpr std::string_view kExecutionModeWarm = "warm";
	constexpr std::string_view kExecutionModeOpened = "opened";
	constexpr std::string_view kExecutionModeColdish = "cold-ish";

	struct Options {
		std::filesystem::path dataset;
		std::filesystem::path dbPath;
		std::string scale;
		std::string profile = std::string(kProfileScan);
		bool emitProfile = false;
		bool emitCounters = false;
		bool emitResultRows = false;
        std::vector<std::string> selectedWorkloads;
		// Serial benchmark execution retains only this operation's owning output.
		// Row retention is opt-in and is a separate measurement contract.
		mutable std::optional<std::vector<std::vector<zyx::Value>>> resultRows;
		// Scale workloads stay enabled for the standalone benchmark; unit tests can
		// disable them while retaining the small operational correctness checks.
		bool includeLargeBatchWorkloads = true;
		std::string executionMode = std::string(kExecutionModeWarm);
		std::optional<std::string> onlyWorkload;
		std::filesystem::path baselineDbPath;
        std::filesystem::path preparedDbPath;
		std::optional<size_t> threads;
		int warmup = 0;
		int iterations = 1;
        size_t loadBatchRows = 0U;
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
						  int iteration, const std::string &phase, double totalTimeMs, uint64_t calls,
						  int64_t totalValue = 0, uint64_t valueCalls = 0, int64_t maximumValue = 0) {
		std::cout << "{\"database\":\"" << kDatabase << "\",\"equivalent_mode\":\"" << kEquivalentMode
				  << "\",\"event\":\"profile\",\"iteration\":" << iteration << ",\"phase\":\"" << jsonEscape(phase)
				  << "\",\"profile\":\"" << jsonEscape(profile) << "\",\"scale\":\"" << jsonEscape(scale)
				  << "\",\"total_time_ms\":" << totalTimeMs << ",\"calls\":" << calls;
		if (valueCalls != 0) {
			const double averageValue = static_cast<double>(totalValue) / static_cast<double>(valueCalls);
			std::cout << ",\"value_total\":" << totalValue << ",\"value_calls\":" << valueCalls
					  << ",\"value_avg\":" << averageValue << ",\"value_max\":" << maximumValue;
		}
		std::cout << ",\"workload\":\"" << jsonEscape(workload) << "\"}\n";
	}

	void emitProfileSnapshot(const Options &options, const std::string &workload, int iteration,
							 const graph::debug::PerfTrace::Snapshot &snapshot) {
		for (const auto &[phase, entry]: snapshot) {
			const double totalMs = static_cast<double>(entry.totalNs) / 1e6;
			emitProfileEvent(workload, options.scale, options.profile, iteration, phase, totalMs, entry.calls,
							 entry.totalValue, entry.valueCalls, entry.maximumValue);
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

	[[maybe_unused]] std::vector<CsvRow> readCsv(const std::filesystem::path &path) {
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

	[[maybe_unused]] const std::string &field(const CsvRow &row, const std::string &name) {
		auto it = row.values.find(name);
		if (it == row.values.end()) {
			throw std::runtime_error("missing CSV field: " + name);
		}
		return it->second;
	}

	class CsvStreamReader {
	public:
		explicit CsvStreamReader(const std::filesystem::path &path) : file_(path) {
			if (!file_) {
				throw std::runtime_error("failed to open CSV: " + path.string());
			}

			std::string headerLine;
			if (!std::getline(file_, headerLine)) {
				return;
			}
			if (!headerLine.empty() && headerLine.back() == '\r') {
				headerLine.pop_back();
			}
			headers_ = parseCsvLine(headerLine);
			headerIndexes_.reserve(headers_.size());
			for (size_t i = 0; i < headers_.size(); ++i) {
				headerIndexes_.emplace(headers_[i], i);
			}
		}

		[[nodiscard]] bool hasHeader() const noexcept { return !headers_.empty(); }

		[[nodiscard]] size_t requireColumn(const std::string &name) const {
			const auto it = headerIndexes_.find(name);
			if (it == headerIndexes_.end()) {
				throw std::runtime_error("missing CSV field: " + name);
			}
			return it->second;
		}

		[[nodiscard]] const std::string &fieldAt(
				const std::vector<std::string> &fields,
				size_t column,
				const std::string &name) const {
			if (column >= fields.size()) {
				throw std::runtime_error("missing CSV field: " + name);
			}
			return fields[column];
		}

		bool next(std::vector<std::string> &fields) {
			std::string line;
			while (std::getline(file_, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (line.empty()) {
					continue;
				}
				fields = parseCsvLine(line);
				return true;
			}
			return false;
		}

	private:
		std::ifstream file_;
		std::vector<std::string> headers_;
		std::unordered_map<std::string, size_t> headerIndexes_;
	};

	size_t estimateCsvRows(const std::filesystem::path &path, size_t averageBytesPerRow) {
		std::error_code ec;
		const auto bytes = std::filesystem::file_size(path, ec);
		if (ec || averageBytesPerRow == 0) {
			return 0;
		}
		return static_cast<size_t>((std::max<uintmax_t>)(16, bytes / averageBytesPerRow));
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

	template<typename DatabaseT>
	void configureThreadPool(DatabaseT &db, const Options &options) {
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

	std::string anchoredNeighborUserId(std::string_view scale) {
		return scale == "smoke" ? "user-000004" : "user-000006";
	}

	std::string writeUpdateTargetUserId(std::string_view scale) { return anchoredNeighborUserId(scale); }

	bool isSupportedProfile(const std::string &profile) {
		return profile == kProfileScan || profile == kProfileIndexed || profile == kProfileMultihop ||
			   profile == kProfileWrite || profile == kProfileWriteDurable ||
			   profile == kProfileOperationalDynamic || profile == kProfileRetrieval;
	}

	const char *supportedProfileMessage() {
		return "--profile must be scan, indexed, multihop, write, write_durable, operational_dynamic, or retrieval";
	}

	bool isSupportedWorkload(const std::string_view profile, const std::string_view workload) {
		if (workload == "load_nodes_edges") {
			return true;
		}
		static constexpr std::pair<std::string_view, std::string_view> kWorkloads[] = {
				{kProfileScan, "label_scan_filter"},
				{kProfileScan, "all_nodes_property_filter"},
				{kProfileScan, "label_multi_property_filter"},
				{kProfileScan, "relationship_type_scan"},
				{kProfileScan, "relationship_property_filter"},
				{kProfileScan, "one_hop_expand"},
				{kProfileScan, "two_hop_expand"},
				{kProfileScan, "shortest_path_chain"},
				{kProfileScan, "aggregation_group_by"},
				{kProfileScan, "aggregation_count_by_group"},
				{kProfileScan, "topk_property_sort"},
				{kProfileIndexed, "point_lookup_indexed"},
				{kProfileIndexed, "point_lookup_missing"},
				{kProfileIndexed, "property_equality_indexed"},
				{kProfileIndexed, "property_range_indexed"},
				{kProfileRetrieval, "point_node_fetch_by_id"},
				{kProfileRetrieval, "point_edge_fetch_by_endpoints"},
				{kProfileRetrieval, "batch_node_fetch_100"},
				{kProfileRetrieval, "one_hop_fetch_neighbor_ids"},
				{kProfileRetrieval, "one_hop_fetch_neighbor_records"},
				{kProfileRetrieval, "property_index_fetch_users_by_country"},
				{kProfileRetrieval, "range_index_fetch_user_projection"},
				{kProfileRetrieval, "relationship_property_fetch"},
				{kProfileMultihop, "reachable_within_6"},
				{kProfileMultihop, "reachable_within_12"},
				{kProfileMultihop, "reachable_within_24"},
				{kProfileMultihop, "reachable_within_30"},
				{kProfileMultihop, "varlength_frontier_count"},
				{kProfileOperationalDynamic, "index_seek_then_one_hop_expand"},
				{kProfileOperationalDynamic, "index_seek_then_two_hop_expand"},
				{kProfileOperationalDynamic, "post_persist_create_node"},
				{kProfileOperationalDynamic, "post_persist_create_edge"},
				{kProfileOperationalDynamic, "write_then_one_hop_expand"},
				{kProfileOperationalDynamic, "batch_create_edges_100"},
				{kProfileOperationalDynamic, "batch_create_edges_1000"},
				{kProfileOperationalDynamic, "batch_create_edges_10000"},
				{kProfileOperationalDynamic, "batch_create_edges_100_then_one_hop_expand"},
				{kProfileOperationalDynamic, "batch_create_edges_10000_then_one_hop_expand"},
				{kProfileWrite, "point_create_node"},
				{kProfileWrite, "point_create_edge"},
				{kProfileWrite, "point_update_node_property"},
				{kProfileWrite, "point_update_edge_property"},
				{kProfileWrite, "point_create_delete_edge"},
				{kProfileWrite, "write_then_read_edge"},
				{kProfileWriteDurable, "point_create_node_durable"},
				{kProfileWriteDurable, "point_create_edge_durable"},
				{kProfileWriteDurable, "point_update_node_property_durable"},
				{kProfileWriteDurable, "point_update_edge_property_durable"},
				{kProfileWriteDurable, "point_create_delete_edge_durable"},
				{kProfileWriteDurable, "write_then_read_edge_durable"},
		};
		return std::ranges::any_of(kWorkloads, [profile, workload](const auto &candidate) {
			return candidate.first == profile && candidate.second == workload;
		});
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

	template<typename Executor>
	void executeOk(Executor &executor, const std::string &query) {
		auto result = executor.execute(query);
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

	template<typename Column>
	void reserveColumnValues(std::vector<Column> &columns, size_t count) {
		for (auto &column: columns) {
			column.values.reserve(count);
		}
	}

	template<class Writer>
    LoadedGraph loadGraphRows(Writer &writer, const std::filesystem::path &dataset) {
		if (!writer.isActive()) {
			throw std::runtime_error("loadGraph requires an active write transaction");
		}
		if (writer.isReadOnly()) {
			throw std::runtime_error("loadGraph cannot write through a read-only transaction");
		}

		LoadedGraph graph;
		std::vector<zyx::PropertyColumn> userColumns{{"id", {}}, {"age", {}}, {"country", {}}, {"score", {}}};
		std::vector<std::string> userExternalIds;
		const auto usersPath = dataset / "users.csv";
		tracePhase("load.users.prepare", [&]() {
			const size_t estimate = estimateCsvRows(usersPath, 48);
			reserveColumnValues(userColumns, estimate);
			userExternalIds.reserve(estimate);
			graph.usersByExternalId.reserve(estimate);
		});
		tracePhase("load.users.csv_read", [&]() {
			CsvStreamReader reader(usersPath);
			if (!reader.hasHeader()) {
				return;
			}
			const size_t idColumn = reader.requireColumn("id");
			const size_t ageColumn = reader.requireColumn("age");
			const size_t countryColumn = reader.requireColumn("country");
			const size_t scoreColumn = reader.requireColumn("score");
			std::vector<std::string> fields;
			while (reader.next(fields)) {
				const std::string id = reader.fieldAt(fields, idColumn, "id");
				userExternalIds.push_back(id);
				userColumns[0].values.emplace_back(id);
				userColumns[1].values.emplace_back(toInt64(reader.fieldAt(fields, ageColumn, "age")));
				userColumns[2].values.emplace_back(reader.fieldAt(fields, countryColumn, "country"));
				userColumns[3].values.emplace_back(toDouble(reader.fieldAt(fields, scoreColumn, "score")));
			}
		});

		const auto nodeIds = tracePhase("load.users.create_nodes", [&]() {
			return writer.createNodesColumnar("User", userExternalIds.size(), userColumns);
		});
		for (size_t i = 0; i < userExternalIds.size() && i < nodeIds.size(); ++i) { // ZYX_COV_EXCL_LINE: bulk API returns one node id per input row.
			graph.usersByExternalId.emplace(userExternalIds[i], nodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(nodeIds.size());

		std::vector<zyx::PropertyColumn> postColumns{{"id", {}}, {"created_at", {}}, {"score", {}}};
		std::vector<std::string> postExternalIds;
		const auto postsPath = dataset / "posts.csv";
		tracePhase("load.posts.prepare", [&]() {
			const size_t estimate = estimateCsvRows(postsPath, 40);
			reserveColumnValues(postColumns, estimate);
			postExternalIds.reserve(estimate);
			graph.postsByExternalId.reserve(estimate);
		});
		tracePhase("load.posts.csv_read", [&]() {
			CsvStreamReader reader(postsPath);
			if (!reader.hasHeader()) {
				return;
			}
			const size_t idColumn = reader.requireColumn("id");
			const size_t createdAtColumn = reader.requireColumn("created_at");
			const size_t scoreColumn = reader.requireColumn("score");
			std::vector<std::string> fields;
			while (reader.next(fields)) {
				const std::string id = reader.fieldAt(fields, idColumn, "id");
				postExternalIds.push_back(id);
				postColumns[0].values.emplace_back(id);
				postColumns[1].values.emplace_back(toInt64(reader.fieldAt(fields, createdAtColumn, "created_at")));
				postColumns[2].values.emplace_back(toDouble(reader.fieldAt(fields, scoreColumn, "score")));
			}
		});

		const auto postNodeIds = tracePhase("load.posts.create_nodes", [&]() {
			return writer.createNodesColumnar("Post", postExternalIds.size(), postColumns);
		});
		for (size_t i = 0; i < postExternalIds.size() && i < postNodeIds.size(); ++i) { // ZYX_COV_EXCL_LINE: bulk API returns one node id per input row.
			graph.postsByExternalId.emplace(postExternalIds[i], postNodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(postNodeIds.size());

		std::vector<zyx::PropertyColumn> tagColumns{{"id", {}}, {"rank", {}}};
		std::vector<std::string> tagExternalIds;
		const auto tagsPath = dataset / "tags.csv";
		tracePhase("load.tags.prepare", [&]() {
			const size_t estimate = estimateCsvRows(tagsPath, 32);
			reserveColumnValues(tagColumns, estimate);
			tagExternalIds.reserve(estimate);
			graph.tagsByExternalId.reserve(estimate);
		});
		tracePhase("load.tags.csv_read", [&]() {
			CsvStreamReader reader(tagsPath);
			if (!reader.hasHeader()) {
				return;
			}
			const size_t idColumn = reader.requireColumn("id");
			const size_t rankColumn = reader.requireColumn("rank");
			std::vector<std::string> fields;
			while (reader.next(fields)) {
				const std::string id = reader.fieldAt(fields, idColumn, "id");
				tagExternalIds.push_back(id);
				tagColumns[0].values.emplace_back(id);
				tagColumns[1].values.emplace_back(toInt64(reader.fieldAt(fields, rankColumn, "rank")));
			}
		});

		const auto tagNodeIds = tracePhase("load.tags.create_nodes", [&]() {
			return writer.createNodesColumnar("Tag", tagExternalIds.size(), tagColumns);
		});
		for (size_t i = 0; i < tagExternalIds.size() && i < tagNodeIds.size(); ++i) { // ZYX_COV_EXCL_LINE: bulk API returns one node id per input row.
			graph.tagsByExternalId.emplace(tagExternalIds[i], tagNodeIds[i]);
		}
		graph.loadedRows += static_cast<int64_t>(tagNodeIds.size());

		std::vector<int64_t> followSources;
		std::vector<int64_t> followTargets;
		std::vector<zyx::PropertyColumn> followColumns{{"weight", {}}};
		const auto followsPath = dataset / "follows.csv";
		tracePhase("load.follows.prepare", [&]() {
			const size_t estimate = estimateCsvRows(followsPath, 40);
			followSources.reserve(estimate);
			followTargets.reserve(estimate);
			reserveColumnValues(followColumns, estimate);
		});
		tracePhase("load.follows.csv_read", [&]() {
			CsvStreamReader reader(followsPath);
			if (!reader.hasHeader()) {
				return;
			}
			const size_t srcColumn = reader.requireColumn("src");
			const size_t dstColumn = reader.requireColumn("dst");
			const size_t weightColumn = reader.requireColumn("weight");
			std::vector<std::string> fields;
			while (reader.next(fields)) {
				auto src = graph.usersByExternalId.find(reader.fieldAt(fields, srcColumn, "src"));
				auto dst = graph.usersByExternalId.find(reader.fieldAt(fields, dstColumn, "dst"));
				if (src == graph.usersByExternalId.end() || dst == graph.usersByExternalId.end()) {
					throw std::runtime_error("follows.csv references unknown user");
				}
				followSources.push_back(src->second);
				followTargets.push_back(dst->second);
				followColumns[0].values.emplace_back(toInt64(reader.fieldAt(fields, weightColumn, "weight")));
			}
		});
		const auto followEdgeIds = tracePhase("load.follows.create_edges", [&]() {
			return writer.createEdgesColumnar("FOLLOWS", followSources, followTargets, followColumns);
		});
		graph.loadedRows += static_cast<int64_t>(followEdgeIds.size());

		std::vector<int64_t> authoredSources;
		std::vector<int64_t> authoredTargets;
		std::vector<zyx::PropertyColumn> authoredColumns{{"weight", {}}};
		const auto authoredPath = dataset / "authored.csv";
		tracePhase("load.authored.prepare", [&]() {
			const size_t estimate = estimateCsvRows(authoredPath, 40);
			authoredSources.reserve(estimate);
			authoredTargets.reserve(estimate);
			reserveColumnValues(authoredColumns, estimate);
		});
		tracePhase("load.authored.csv_read", [&]() {
			CsvStreamReader reader(authoredPath);
			if (!reader.hasHeader()) {
				return;
			}
			const size_t srcColumn = reader.requireColumn("src");
			const size_t dstColumn = reader.requireColumn("dst");
			const size_t weightColumn = reader.requireColumn("weight");
			std::vector<std::string> fields;
			while (reader.next(fields)) {
				auto src = graph.usersByExternalId.find(reader.fieldAt(fields, srcColumn, "src"));
				auto dst = graph.postsByExternalId.find(reader.fieldAt(fields, dstColumn, "dst"));
				if (src == graph.usersByExternalId.end() || dst == graph.postsByExternalId.end()) {
					throw std::runtime_error("authored.csv references unknown user or post");
				}
				authoredSources.push_back(src->second);
				authoredTargets.push_back(dst->second);
				authoredColumns[0].values.emplace_back(toInt64(reader.fieldAt(fields, weightColumn, "weight")));
			}
		});
		const auto authoredEdgeIds = tracePhase("load.authored.create_edges", [&]() {
			return writer.createEdgesColumnar("AUTHORED", authoredSources, authoredTargets, authoredColumns);
		});
		graph.loadedRows += static_cast<int64_t>(authoredEdgeIds.size());

		std::vector<int64_t> hasTagSources;
		std::vector<int64_t> hasTagTargets;
		std::vector<zyx::PropertyColumn> hasTagColumns{{"weight", {}}};
		const auto hasTagPath = dataset / "has_tag.csv";
		tracePhase("load.has_tag.prepare", [&]() {
			const size_t estimate = estimateCsvRows(hasTagPath, 40);
			hasTagSources.reserve(estimate);
			hasTagTargets.reserve(estimate);
			reserveColumnValues(hasTagColumns, estimate);
		});
		tracePhase("load.has_tag.csv_read", [&]() {
			CsvStreamReader reader(hasTagPath);
			if (!reader.hasHeader()) {
				return;
			}
			const size_t srcColumn = reader.requireColumn("src");
			const size_t dstColumn = reader.requireColumn("dst");
			const size_t weightColumn = reader.requireColumn("weight");
			std::vector<std::string> fields;
			while (reader.next(fields)) {
				auto src = graph.postsByExternalId.find(reader.fieldAt(fields, srcColumn, "src"));
				auto dst = graph.tagsByExternalId.find(reader.fieldAt(fields, dstColumn, "dst"));
				if (src == graph.postsByExternalId.end() || dst == graph.tagsByExternalId.end()) {
					throw std::runtime_error("has_tag.csv references unknown post or tag");
				}
				hasTagSources.push_back(src->second);
				hasTagTargets.push_back(dst->second);
				hasTagColumns[0].values.emplace_back(toInt64(reader.fieldAt(fields, weightColumn, "weight")));
			}
		});
		const auto hasTagEdgeIds = tracePhase("load.has_tag.create_edges", [&]() {
			return writer.createEdgesColumnar("HAS_TAG", hasTagSources, hasTagTargets, hasTagColumns);
		});
		graph.loadedRows += static_cast<int64_t>(hasTagEdgeIds.size());

		return graph;
	}

    LoadedGraph loadGraph(zyx::Transaction &writer, const std::filesystem::path &dataset) {
        return loadGraphRows(writer, dataset);
    }

    // Preparation-only adapter. Each chunk is committed before staging the next
    // one, so a medium input cannot multiply retained WRITE workspace beyond the
    // same production quota. Query operations and their timing are unchanged.
    class BoundedDatasetWriter final {
    public:
        BoundedDatasetWriter(zyx::Database &database, size_t rows) : database_(database), rows_(rows) {
            if (!rows) throw std::invalid_argument("bounded dataset load requires positive rows");
        }
        bool isActive() const { return true; }
        bool isReadOnly() const { return false; }
        std::vector<int64_t> createNodesColumnar(const std::string &label, size_t count,
                const std::vector<zyx::PropertyColumn> &columns) {
            validateColumns(columns, count);
            std::vector<int64_t> result; result.reserve(count);
            for (size_t offset = 0U; offset < count;) {
                const size_t amount = (std::min)(rows_, count - offset);
                auto part = sliceColumns(columns, offset, amount);
                auto transaction = database_.beginBulkTransaction();
                auto ids = transaction.createNodesColumnar(label, amount, part);
                transaction.commit(); result.insert(result.end(), ids.begin(), ids.end()); offset += amount;
            }
            return result;
        }
        std::vector<int64_t> createEdgesColumnar(const std::string &type,
                const std::vector<int64_t> &sources, const std::vector<int64_t> &targets,
                const std::vector<zyx::PropertyColumn> &columns) {
            if (sources.size() != targets.size()) throw std::invalid_argument("bounded edge load changed endpoint cardinality");
            validateColumns(columns, sources.size());
            std::vector<int64_t> result; result.reserve(sources.size());
            for (size_t offset = 0U; offset < sources.size();) {
                const size_t amount = (std::min)(rows_, sources.size() - offset);
                auto part = sliceColumns(columns, offset, amount);
                std::vector<int64_t> source(sources.begin() + offset, sources.begin() + offset + amount);
                std::vector<int64_t> target(targets.begin() + offset, targets.begin() + offset + amount);
                auto transaction = database_.beginBulkTransaction();
                auto ids = transaction.createEdgesColumnar(type, source, target, part);
                transaction.commit(); result.insert(result.end(), ids.begin(), ids.end()); offset += amount;
            }
            return result;
        }
    private:
        static void validateColumns(const std::vector<zyx::PropertyColumn> &columns, size_t count) {
            for (const auto &column : columns)
                if (column.values.size() != count) throw std::invalid_argument("bounded load changed column cardinality");
        }
        static std::vector<zyx::PropertyColumn> sliceColumns(const std::vector<zyx::PropertyColumn> &columns,
                size_t offset, size_t count) {
            std::vector<zyx::PropertyColumn> result; result.reserve(columns.size());
            for (const auto &column : columns) result.push_back({column.name,
                {column.values.begin() + offset, column.values.begin() + offset + count}});
            return result;
        }
        zyx::Database &database_;
        size_t rows_;
    };

    LoadedGraph loadGraph(zyx::Database &db, const std::filesystem::path &dataset, size_t batchRows = 0U) {
        if (db.hasActiveTransaction()) throw std::runtime_error("loadGraph requires an explicit transaction when one is already active");
        if (batchRows) { BoundedDatasetWriter writer(db, batchRows); return loadGraphRows(writer, dataset); }
        auto transaction = db.beginBulkTransaction();
        LoadedGraph graph = loadGraph(transaction, dataset);
        tracePhase("load.commit", [&]() { transaction.commit(); });
        return graph;
    }

	void createBenchmarkIndexes(zyx::Database &db, const Options &options) {
		std::vector<std::string> userIndexes{"id"};
		if (options.profile == kProfileIndexed || options.profile == kProfileOperationalDynamic ||
			options.profile == kProfileRetrieval) {
			userIndexes.push_back("country");
			userIndexes.push_back("age");
		}
        if (!options.preparedDbPath.empty()) {
            auto existing = db.execute("SHOW INDEXES");
            if (!existing.isSuccess()) throw std::runtime_error(existing.getError());
            while (existing.hasNext()) {
                existing.next();
                const auto label = existing.get("label"), kind = existing.get("type"), properties = existing.get("properties");
                const auto *labelText = std::get_if<std::string>(&label);
                const auto *kindText = std::get_if<std::string>(&kind);
                const auto *propertyText = std::get_if<std::string>(&properties);
                if (labelText && kindText && propertyText && *labelText == "User" && *kindText == "property")
                    std::erase(userIndexes, *propertyText);
            }
        }

		tracePhase("load.index.user_properties", [&]() {
			if (!userIndexes.empty() && !db.createNodePropertyIndexes("User", userIndexes)) {
				throw std::runtime_error("failed to create benchmark User property indexes");
			}
		});
	}

	std::filesystem::path suffixedDbPath(const std::filesystem::path &base, const std::string &suffix) {
		return std::filesystem::path(base.string() + suffix);
	}

	void removeDatabaseArtifacts(const std::filesystem::path &dbPath) {
		std::error_code ignored;
		if (std::filesystem::is_directory(dbPath, ignored)) {
			std::filesystem::remove_all(dbPath, ignored);
			return;
		}
		std::filesystem::remove(dbPath, ignored);
	}

	void cloneDatabaseArtifact(const std::filesystem::path &source, const std::filesystem::path &destination) {
		std::error_code error;
		removeDatabaseArtifacts(destination);
		if (!std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error)) {
			throw std::runtime_error("failed to clone benchmark database: " + error.message());
		}
	}

	struct ScopedArtifactCleanup {
		std::filesystem::path path;
		~ScopedArtifactCleanup() { removeDatabaseArtifacts(path); }
	};

    void validatePreparedInput(const Options &options) {
        if (options.preparedDbPath.empty()) return;
        if ((options.profile != kProfileScan && options.profile != kProfileIndexed && options.profile != kProfileRetrieval) ||
            (options.onlyWorkload && *options.onlyWorkload == "load_nodes_edges") ||
            (!options.onlyWorkload && options.selectedWorkloads.empty()))
            throw std::invalid_argument("prepared database requires explicitly selected read queries");
    }

	LoadedGraph loadDatabase(const Options &options, const std::filesystem::path &dbPath) {
        validatePreparedInput(options);
        if (!options.preparedDbPath.empty()) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(options.preparedDbPath) ||
                std::filesystem::equivalent(options.preparedDbPath, dbPath, error))
                throw std::invalid_argument("prepared source must be a regular file distinct from the private database");
            cloneDatabaseArtifact(options.preparedDbPath, dbPath);
            std::filesystem::permissions(dbPath, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        } else tracePhase("load.remove_existing_db", [&]() { removeDatabaseArtifacts(dbPath); });
		zyx::Database db(dbPath.string());
		configureThreadPool(db, options);
		tracePhase("load.db_open", [&]() { db.open(); });
		// External-to-native ID maps are needed by import/write workloads only.
        // Read profiles use their unchanged SQL and receive a private file clone.
        LoadedGraph loaded;
        if (options.preparedDbPath.empty())
            loaded = tracePhase("load.graph", [&]() { return loadGraph(db, options.dataset, options.loadBatchRows); });
		tracePhase("load.indexes", [&]() { createBenchmarkIndexes(db, options); });
		tracePhase("load.db_close", [&]() { db.close(); });
		return loaded;
	}

	std::string resultRowsJson(const std::vector<std::vector<zyx::Value>> &rows) {
		std::ostringstream output;
		output << '[';
		bool firstRow = true;
		for (const auto &row : rows) {
			if (!firstRow) output << ',';
			firstRow = false;
			output << '[';
			bool firstValue = true;
			for (const auto &cell : row) {
				if (!firstValue) output << ',';
				firstValue = false;
				if (const auto *text = std::get_if<std::string>(&cell)) {
					output << '\"' << jsonEscape(*text) << '\"';
				} else if (const auto *number = std::get_if<int64_t>(&cell)) {
					output << *number;
				} else {
					throw std::runtime_error("benchmark semantic rows require string or integer cells");
				}
			}
			output << ']';
		}
		output << ']';
		return output.str();
	}

	template<typename Fn, typename ValidateFn>
	auto measureOperation(const Options &options, const std::string &workload, int iteration, Fn &&operation,
						  ValidateFn &&validate) {
		if (options.emitProfile || options.emitCounters) {
			graph::debug::PerfTrace::setEnabled(options.emitProfile);
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			graph::debug::PerfTrace::reset();
		}

		options.resultRows.reset();
		const auto start = Clock::now();
		try {
			auto value = operation();
			const auto end = Clock::now();
			graph::debug::PerfTrace::Snapshot snapshot;
			if (options.emitProfile || options.emitCounters) {
				snapshot = graph::debug::PerfTrace::snapshotAndReset();
				graph::debug::PerfTrace::setEnabled(false);
			}
			const auto latencyMs = std::chrono::duration<double, std::milli>(end - start).count();
			validate(value);
			const auto rowsJson = options.resultRows.has_value() ? resultRowsJson(*options.resultRows) : std::string{};
			emitSample(workload, options.scale, iteration, latencyMs);
			if constexpr (std::is_integral_v<decltype(value)>) {
					std::cout << "{\"event\":\"result\",\"workload\":\"" << jsonEscape(workload)
					<< "\",\"iteration\":" << iteration << ",\"value\":" << value;
				if (options.resultRows.has_value()) std::cout << ",\"rows\":" << rowsJson;
				std::cout << "}\n";
				options.resultRows.reset();
			}
			if (options.emitProfile || options.emitCounters) {
				emitProfileSnapshot(options, workload, iteration, snapshot);
			}
			return value;
		} catch (...) {
			options.resultRows.reset();
			if (options.emitProfile || options.emitCounters) {
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
			removeDatabaseArtifacts(warmupPath);
		}

		for (int i = 0; i < options.iterations; ++i) {
			const auto iterationPath = suffixedDbPath(options.dbPath, ".load-iteration-" + std::to_string(i));
			const LoadedGraph loaded = measureOperation(
					options, "load_nodes_edges", i, [&]() { return loadDatabase(options, iterationPath); },
					[](const LoadedGraph &loadedGraph) {
						requireNonNegative("load_nodes_edges", loadedGraph.loadedRows);
					});
			removeDatabaseArtifacts(iterationPath);
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
	void runMeasuredIsolatedWorkload(const Options &options, const LoadedGraph &loaded,
									 const std::string &workload, Fn &&operation,
									 bool validateResult = true) {
		auto runOnce = [&](std::optional<int> iteration, int sequence) {
			const std::string suffix = ".workload-" + workloadPathToken(workload) + "-" + std::to_string(sequence);
			const auto isolatedPath = suffixedDbPath(options.dbPath, suffix);
			const auto &baselinePath = options.baselineDbPath.empty() ? options.dbPath : options.baselineDbPath;
			cloneDatabaseArtifact(baselinePath, isolatedPath);
			ScopedArtifactCleanup cleanup{isolatedPath};
			zyx::Database isolatedDb(isolatedPath.string());
			configureThreadPool(isolatedDb, options);
			bool opened = false;
			try {
				if (options.executionMode == kExecutionModeColdish) {
					graph::debug::ScopedPerfTimer timer("coldish.db_open");
					isolatedDb.open();
				} else {
					isolatedDb.open();
				}
				opened = true;
				std::string phase = "warm.operation";
				if (options.executionMode == kExecutionModeColdish) {
					phase = "coldish.operation";
				} else if (options.executionMode == kExecutionModeOpened) {
					phase = "opened.operation";
				}
				auto measuredOperation = [&]() {
					graph::debug::ScopedPerfTimer timer(phase);
					return invokeWorkloadOperation(operation, isolatedDb, loaded);
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
				if (options.executionMode == kExecutionModeColdish) {
					graph::debug::ScopedPerfTimer timer("coldish.db_close");
					isolatedDb.close();
				} else {
					isolatedDb.close();
				}
				opened = false;
			} catch (...) {
				if (opened) {
					try {
						isolatedDb.close();
					} catch (...) {
						// Preserve the operation failure; artifact cleanup is still guaranteed.
					}
				}
				throw;
			}
		};

		const int queryWarmup = options.executionMode == kExecutionModeOpened ? 0 : options.warmup;
		int sequence = 0;
		for (int i = 0; i < queryWarmup; ++i) { // ZYX_COV_EXCL_LINE: benchmark warmup loop shape is option-driven.
			runOnce(std::nullopt, sequence++);
		}
		for (int i = 0; i < options.iterations; ++i) {
			runOnce(i, sequence++);
		}
	}

	template<typename Fn>
	void runQueryWorkload(const Options &options, zyx::Database &db, const LoadedGraph &loaded,
					  const std::string &workload, Fn &&operation, bool validateResult = true) {
		if ((options.onlyWorkload.has_value() && *options.onlyWorkload != workload) ||
            (!options.selectedWorkloads.empty() && std::ranges::find(options.selectedWorkloads, workload) == options.selectedWorkloads.end())) {
			return;
		}
		if (isMutatingWorkload(workload)) {
			runMeasuredIsolatedWorkload(options, loaded, workload, std::forward<Fn>(operation), validateResult);
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
	int64_t rowCount(Fn &&operation,
			std::optional<std::vector<std::vector<zyx::Value>>> *observedRows = nullptr) {
		auto result = operation();
		if constexpr (std::is_convertible_v<decltype(result), zyx::Result>) {
			if (!result.isSuccess()) {
				throw std::runtime_error(result.getError());
			}
			int64_t count = 0;
			if (observedRows != nullptr) observedRows->emplace();
			while (result.hasNext()) {
				result.next();
				if (observedRows != nullptr) {
					std::vector<zyx::Value> row;
					row.reserve(static_cast<size_t>(result.getColumnCount()));
					for (int column = 0; column < result.getColumnCount(); ++column) row.push_back(result.get(column));
					(*observedRows)->push_back(std::move(row));
				}
				++count;
			}
			return count;
		} else {
			return static_cast<int64_t>(result.rowCount());
		}
	}

	size_t valueDigest(const zyx::Value &value) {
		if (const auto *boolValue = std::get_if<bool>(&value)) {
			return *boolValue ? 0x9e3779b97f4a7c15ULL : 0x85ebca6bU;
		}
		if (const auto *intValue = std::get_if<int64_t>(&value)) {
			return std::hash<int64_t>{}(*intValue);
		}
		if (const auto *doubleValue = std::get_if<double>(&value)) {
			return std::hash<double>{}(*doubleValue);
		}
		if (const auto *stringValue = std::get_if<std::string>(&value)) {
			return std::hash<std::string>{}(*stringValue);
		}
		if (const auto *nodeValue = std::get_if<std::shared_ptr<zyx::Node>>(&value)) {
			return *nodeValue ? std::hash<int64_t>{}((*nodeValue)->id) ^ (*nodeValue)->properties.size() : 0;
		}
		if (const auto *edgeValue = std::get_if<std::shared_ptr<zyx::Edge>>(&value)) {
			return *edgeValue ? std::hash<int64_t>{}((*edgeValue)->id) ^ (*edgeValue)->properties.size() : 0;
		}
		if (const auto *vectorValue = std::get_if<std::vector<float>>(&value)) {
			return vectorValue->size();
		}
		if (const auto *stringsValue = std::get_if<std::vector<std::string>>(&value)) {
			return stringsValue->size();
		}
		if (const auto *listValue = std::get_if<std::shared_ptr<zyx::ValueList>>(&value)) {
			return *listValue ? (*listValue)->elements.size() : 0;
		}
		if (const auto *mapValue = std::get_if<std::shared_ptr<zyx::ValueMap>>(&value)) {
			return *mapValue ? (*mapValue)->entries.size() : 0;
		}
		return 0;
	}

	int64_t materializedRowCount(zyx::Result result) {
		if (!result.isSuccess()) {
			throw std::runtime_error(result.getError());
		}
		const int columns = result.getColumnCount();
		int64_t count = 0;
		size_t checksum = 0;
		while (result.hasNext()) {
			result.next();
			for (int column = 0; column < columns; ++column) {
				checksum ^= valueDigest(result.get(column)) + 0x9e3779b97f4a7c15ULL + (checksum << 6U) + (checksum >> 2U);
			}
			++count;
		}
		if (checksum == (std::numeric_limits<size_t>::max)()) { // Keep projected values observable to the optimizer.
			std::cerr << "";
		}
		return count;
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
				if (!isSupportedProfile(options.profile)) {
					throw std::invalid_argument(supportedProfileMessage());
				}
			} else if (arg == "--emit-profile") {
				options.emitProfile = true;
			} else if (arg == "--emit-counters") {
				options.emitCounters = true;
			} else if (arg == "--emit-result-rows") {
				options.emitResultRows = true;
			} else if (arg == "--execution-mode") {
				options.executionMode = requireValue(arg);
				if (options.executionMode != kExecutionModeWarm && options.executionMode != kExecutionModeOpened &&
					options.executionMode != kExecutionModeColdish) {
					throw std::invalid_argument("--execution-mode must be warm, opened, or cold-ish");
				}
			} else if (arg == "--workload") {
				options.onlyWorkload = requireValue(arg);
			} else if (arg == "--prepared-db") {
                options.preparedDbPath = requireValue(arg);
                if (options.preparedDbPath.empty()) throw std::invalid_argument("prepared database path must be nonempty");
            } else if (arg == "--workloads") {
                const auto list = requireValue(arg);
                if (!options.selectedWorkloads.empty() || list.empty() || list.back() == ',')
                    throw std::invalid_argument("workload list must be supplied once with nonempty names");
                std::stringstream names(list);
                std::string name;
                while (std::getline(names, name, ',')) options.selectedWorkloads.push_back(name);
                if (options.selectedWorkloads.empty() || names.str().back() == ',')
                    throw std::invalid_argument("workload list must contain nonempty unique query names");
            } else if (arg == "--warmup") {
				options.warmup = std::stoi(requireValue(arg));
			} else if (arg == "--iterations") {
				options.iterations = std::stoi(requireValue(arg));
			} else if (arg == "--load-batch-rows") {
                const auto text = requireValue(arg);
                if (text.empty() || text.front() == '-') throw std::invalid_argument("load batch rows must be positive");
                size_t parsed = 0U;
                const auto count = std::stoull(text, &parsed);
                if (!count || parsed != text.size() || count > (std::numeric_limits<size_t>::max)())
                    throw std::invalid_argument("load batch rows must be a positive size");
                options.loadBatchRows = static_cast<size_t>(count);
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
		if (!isSupportedProfile(options.profile)) {
			throw std::invalid_argument(supportedProfileMessage());
		}
		if (options.onlyWorkload.has_value() &&
			!isSupportedWorkload(options.profile, *options.onlyWorkload)) {
			throw std::invalid_argument("--workload is not registered for the selected profile");
		}
        if (!options.selectedWorkloads.empty()) {
            if (options.onlyWorkload) throw std::invalid_argument("--workloads and --workload are mutually exclusive");
            for (size_t index = 0U; index < options.selectedWorkloads.size(); ++index) {
                const auto &name = options.selectedWorkloads[index];
                if (name == "load_nodes_edges" || !isSupportedWorkload(options.profile, name) ||
                    std::find(options.selectedWorkloads.begin(), options.selectedWorkloads.begin() + index, name) != options.selectedWorkloads.begin() + index)
                    throw std::invalid_argument("workload list must contain registered unique query names");
            }
        }

        validatePreparedInput(options);
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

	int run(const Options &inputOptions) {
		Options options = inputOptions;
		LoadedGraph loaded;
		if ((!options.onlyWorkload.has_value() && options.selectedWorkloads.empty()) ||
            (options.onlyWorkload.has_value() && *options.onlyWorkload == "load_nodes_edges")) {
			loaded = measureLoadWorkload(options);
			if (options.onlyWorkload.has_value()) {
				return 0;
			}
		} else {
			// A selected query measures only that workload; database preparation is not a benchmark sample.
			loaded = loadDatabase(options, options.dbPath);
		}
		options.baselineDbPath = suffixedDbPath(options.dbPath, ".benchmark-baseline");
		cloneDatabaseArtifact(options.dbPath, options.baselineDbPath);
		ScopedArtifactCleanup baselineCleanup{options.baselineDbPath};

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
			runQueryWorkload(options, db, loaded, "aggregation_count_by_group", [&options](zyx::Database &queryDb) {
				return rowCount([&]() { return queryDb.execute("MATCH (u:User) RETURN u.country, count(*)"); },
						options.emitResultRows ? &options.resultRows : nullptr);
			});
			runQueryWorkload(options, db, loaded, "topk_property_sort", [&options](zyx::Database &queryDb) {
				return rowCount([&]() { return queryDb.execute("MATCH (u:User) RETURN u.id ORDER BY u.score DESC LIMIT 100"); },
						options.emitResultRows ? &options.resultRows : nullptr);
			});
		} else if (options.profile == kProfileIndexed) {
			runQueryWorkload(options, db, loaded, "point_lookup_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User {id: 'user-000001'}) RETURN count(u)"));
			});
			runQueryWorkload(options, db, loaded, "point_lookup_missing", [](zyx::Database &queryDb) {
				return requireExactCount("point_lookup_missing",
					scalarInt(queryDb.execute("MATCH (u:User {id: 'missing-user'}) RETURN COUNT(u)")), 0);
			});
			runQueryWorkload(options, db, loaded, "property_equality_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' RETURN count(u)"));
			});
			runQueryWorkload(options, db, loaded, "property_range_indexed", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute("MATCH (u:User) WHERE u.age >= 30 AND u.age < 40 RETURN count(u)"));
			});
		} else if (options.profile == kProfileRetrieval) {
			const std::string endpointTarget = anchoredNeighborUserId(options.scale);
			runQueryWorkload(options, db, loaded, "point_node_fetch_by_id", [](zyx::Database &queryDb) {
				return materializedRowCount(
						queryDb.execute("MATCH (u:User {id: 'user-000001'}) RETURN u.id, u.age, u.country, u.score"));
			});
			runQueryWorkload(options, db, loaded, "point_edge_fetch_by_endpoints",
							 [endpointTarget](zyx::Database &queryDb) {
								 std::ostringstream query;
								 query << "MATCH (:User {id: 'user-000001'})-[r:FOLLOWS]->(:User {id: '"
									   << endpointTarget << "'}) RETURN r.weight";
								 return materializedRowCount(queryDb.execute(query.str()));
							 });
			runQueryWorkload(options, db, loaded, "batch_node_fetch_100", [](zyx::Database &queryDb) {
				return materializedRowCount(
						queryDb.execute("MATCH (u:User) RETURN u.id, u.age, u.country, u.score LIMIT 100"));
			});
			runQueryWorkload(options, db, loaded, "one_hop_fetch_neighbor_ids", [](zyx::Database &queryDb) {
				return materializedRowCount(
						queryDb.execute("MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) RETURN v.id"));
			});
			runQueryWorkload(options, db, loaded, "one_hop_fetch_neighbor_records", [](zyx::Database &queryDb) {
				return materializedRowCount(queryDb.execute(
						"MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) "
						"RETURN v.id, v.age, v.country, v.score"));
			});
			runQueryWorkload(options, db, loaded, "property_index_fetch_users_by_country", [](zyx::Database &queryDb) {
				return materializedRowCount(
						queryDb.execute("MATCH (u:User) WHERE u.country = 'CN' RETURN u.id, u.age, u.score LIMIT 100"));
			});
			runQueryWorkload(options, db, loaded, "range_index_fetch_user_projection", [](zyx::Database &queryDb) {
				return materializedRowCount(queryDb.execute(
						"MATCH (u:User) WHERE u.age >= 30 AND u.age < 40 "
						"RETURN u.id, u.age, u.country, u.score LIMIT 100"));
			});
			runQueryWorkload(options, db, loaded, "relationship_property_fetch", [](zyx::Database &queryDb) {
				return materializedRowCount(queryDb.execute(
						"MATCH ()-[r:FOLLOWS]->(v:User) WHERE r.weight = 1 RETURN r.weight, v.id LIMIT 100"));
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
			runQueryWorkload(options, db, loaded, "varlength_frontier_count", [](zyx::Database &queryDb) {
				return scalarInt(queryDb.execute(
						"MATCH (u:User {country: 'CN'})-[:FOLLOWS*1..2]->(v:User) RETURN count(v)"));
			});
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
			if (options.includeLargeBatchWorkloads) {
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
			}
			runQueryWorkload(options, db, loaded, "batch_create_edges_100_then_one_hop_expand",
							 [&batchEdgeId](zyx::Database &queryDb, const LoadedGraph &graph) {
								 (void) requireExactCount("batch_create_edges_100_then_one_hop_expand",
														  createBatchEdges(queryDb, graph, batchEdgeId, 100,
																		   "user-000007"),
														  100);
								 return scalarInt(queryDb.execute("MATCH (:User {id: 'user-000007'})-[:FOLLOWS]->"
															 "(v:User) RETURN count(v)"));
							 });
			if (options.includeLargeBatchWorkloads) {
				runQueryWorkload(options, db, loaded, "batch_create_edges_10000_then_one_hop_expand",
								 [&batchEdgeId](zyx::Database &queryDb, const LoadedGraph &graph) {
									 (void) requireExactCount("batch_create_edges_10000_then_one_hop_expand",
															 createBatchEdges(queryDb, graph, batchEdgeId, 10000,
																									   "user-000008"),
															 10000);
									 return scalarInt(queryDb.execute("MATCH (:User {id: 'user-000008'})-[:FOLLOWS]->"
																													 "(v:User) RETURN count(v)"));
								 });
			}
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
					  << 1000.0 + static_cast<double>(updateNodeId) << " RETURN u.score AS score";
				return requireExactlyOne("point_update_node_property",
									 materializedRowCount(queryDb.execute(query.str())));
			});
			runQueryWorkload(options, db, loaded, workloadName("point_update_edge_property"),
							 [&updateEdgeId, updateTarget](zyx::Database &queryDb) {
								 ++updateEdgeId;
								 std::ostringstream query;
								 query << "MATCH (:User {id: 'user-000001'})-[r:FOLLOWS]->(:User {id: '"
									   << updateTarget << "'}) SET r.weight = " << 10'000 + updateEdgeId
									   << " RETURN r.weight AS weight";
								 return requireExactlyOne("point_update_edge_property",
																	 materializedRowCount(queryDb.execute(query.str())));
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
	std::string workload = "run_all";
	try {
		const Options options = parseArgs(argc, argv);
		scale = options.scale;
		if (options.onlyWorkload.has_value()) {
			workload = *options.onlyWorkload;
		}
		return run(options);
	} catch (const std::exception &exc) {
		emitError(workload, scale, exc.what());
		return 1;
	}
}
