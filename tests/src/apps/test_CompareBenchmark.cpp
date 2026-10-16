#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#define main zyx_compare_benchmark_main
#include "../../../apps/compare_benchmark/main.cpp"
#undef main

class CompareBenchmarkTest : public ::testing::Test {
protected:
	std::filesystem::path tempRoot;

	void SetUp() override {
		tempRoot = std::filesystem::temp_directory_path() / ("zyx_compare_benchmark_test_" + std::to_string(++counter));
		std::filesystem::remove_all(tempRoot);
		std::filesystem::create_directories(tempRoot);
	}

	void TearDown() override {
		graph::debug::PerfTrace::reset();
		graph::debug::PerfTrace::setEnabled(false);
		std::filesystem::remove_all(tempRoot);
	}

	std::filesystem::path writeSmallDataset() {
		const auto dataset = tempRoot / "dataset";
		std::filesystem::create_directories(dataset);
		writeFile(dataset / "users.csv", "id,age,country,score\n"
										 "user-000001,31,CN,901.5\n"
										 "user-000006,36,US,850.0\n"
										 "user-000007,42,CN,990.0\n");
		writeFile(dataset / "posts.csv", "id,created_at,score\n"
										 "post-000001,1,10.5\n");
		writeFile(dataset / "tags.csv", "id,rank\n"
										"tag-000001,1\n");
		writeFile(dataset / "follows.csv", "src,dst,weight\n"
										   "user-000001,user-000006,1\n"
										   "user-000006,user-000007,2\n");
		writeFile(dataset / "authored.csv", "src,dst,weight\n"
											"user-000001,post-000001,1\n");
		writeFile(dataset / "has_tag.csv", "src,dst,weight\n"
										   "post-000001,tag-000001,1\n");
		return dataset;
	}

	static void writeFile(const std::filesystem::path &path, const std::string &content) {
		std::ofstream out(path);
		out << content;
	}

	static inline int counter = 0;
};

TEST_F(CompareBenchmarkTest, CounterOnlyMeasurementDisablesTimersAndEmitsWorkValues) {
	Options options;
	options.scale = "smoke";
	options.emitCounters = true;
	std::ostringstream captured;
	int64_t result = 0;
	{
		struct RestoreStream {
			std::streambuf *previous;
			~RestoreStream() { std::cout.rdbuf(previous); }
		} restore{std::cout.rdbuf(captured.rdbuf())};
		result = measureOperation(options, "counted", 0, []() {
		EXPECT_FALSE(graph::debug::PerfTrace::isEnabled());
		EXPECT_TRUE(graph::debug::PerfTrace::areValuesEnabled());
		graph::debug::PerfTrace::addDuration("ignored.timer", 1000);
		graph::debug::PerfTrace::addValue("records_decoded", 7);
		return int64_t{7};
	}, [](int64_t value) { requireNonNegative("counted", value); });
	}
	const auto output = captured.str();
	EXPECT_EQ(result, 7);
	EXPECT_NE(output.find("\"event\":\"result\""), std::string::npos);
	EXPECT_NE(output.find("\"value\":7"), std::string::npos);
	EXPECT_NE(output.find("\"event\":\"sample\""), std::string::npos);
	EXPECT_NE(output.find("records_decoded"), std::string::npos);
	EXPECT_EQ(output.find("ignored.timer"), std::string::npos);
	EXPECT_NE(output.find("\"value_total\":7"), std::string::npos);
	EXPECT_FALSE(graph::debug::PerfTrace::areValuesEnabled());
	EXPECT_TRUE(graph::debug::PerfTrace::snapshotAndReset().empty());
}

TEST_F(CompareBenchmarkTest, CounterOnlyMeasurementCleansUpWhenOperationThrows) {
	Options options;
	options.emitCounters = true;
	testing::internal::CaptureStdout();
	EXPECT_THROW((void) measureOperation(options, "failed", 0, []() -> int64_t {
		graph::debug::PerfTrace::addValue("records_decoded", 1);
		throw std::runtime_error("operation failure");
	}, [](int64_t) {}), std::runtime_error);
	const auto output = testing::internal::GetCapturedStdout();
	EXPECT_TRUE(output.empty());
	EXPECT_FALSE(graph::debug::PerfTrace::isEnabled());
	EXPECT_FALSE(graph::debug::PerfTrace::areValuesEnabled());
	EXPECT_TRUE(graph::debug::PerfTrace::snapshotAndReset().empty());
}

TEST_F(CompareBenchmarkTest, MeasureOperationDoesNotEmitEventsWhenValidationFails) {
	Options options;
	options.scale = "smoke";
	options.profile = std::string(kProfileScan);
	options.emitProfile = true;

	testing::internal::CaptureStdout();
	EXPECT_THROW((void) measureOperation(
						 options, "bad_workload", 0,
						 []() {
							 graph::debug::PerfTrace::addDuration("parse", 1000);
							 return int64_t{-1};
						 },
						 [](const int64_t value) { requireNonNegative("bad_workload", value); }),
				 std::runtime_error);
	const std::string output = testing::internal::GetCapturedStdout();

	EXPECT_TRUE(output.empty());
	EXPECT_FALSE(graph::debug::PerfTrace::isEnabled());
	EXPECT_TRUE(graph::debug::PerfTrace::snapshotAndReset().empty());
}

TEST_F(CompareBenchmarkTest, RunMeasuredEmitsProfileForSuccessfulWarmupAndIteration) {
	Options options;
	options.scale = "smoke";
	options.profile = std::string(kProfileScan);
	options.emitProfile = true;
	options.warmup = 1;
	options.iterations = 1;

	int calls = 0;
	EXPECT_NO_THROW(runMeasured(options, "profiled_workload",
								[&]() {
									graph::debug::PerfTrace::addDuration("profiled.phase", 1000);
									return int64_t{++calls};
								}));

	EXPECT_EQ(calls, 2);
	EXPECT_FALSE(graph::debug::PerfTrace::isEnabled());
}

TEST_F(CompareBenchmarkTest, RunMeasuredOpenedModeSkipsExplicitQueryWarmup) {
	Options options;
	options.scale = "smoke";
	options.profile = std::string(kProfileScan);
	options.executionMode = std::string(kExecutionModeOpened);
	options.warmup = 5;
	options.iterations = 2;

	int calls = 0;
	EXPECT_NO_THROW(runMeasured(options, "opened_workload", [&]() { return int64_t{++calls}; }));

	EXPECT_EQ(calls, 2);
}

TEST_F(CompareBenchmarkTest, MutatingSamplesEachStartFromTheSameImmutableBaseline) {
	const auto baselinePath = tempRoot / "mutation-baseline.db";
	{
		zyx::Database baseline(baselinePath.string());
		baseline.open();
		baseline.close();
	}

	for (const auto &mode: {std::string(kExecutionModeWarm), std::string(kExecutionModeOpened),
			 std::string(kExecutionModeColdish)}) {
		Options options;
		options.dbPath = tempRoot / (mode + ".db");
		options.baselineDbPath = baselinePath;
		options.scale = "smoke";
		options.executionMode = mode;
		options.warmup = 2;
		options.iterations = 3;

		std::vector<int64_t> initialCounts;
		runMeasuredIsolatedWorkload(options, LoadedGraph{}, "point_create_node", [&](zyx::Database &db) {
			initialCounts.push_back(scalarInt(db.execute("MATCH (n:SampleIsolation) RETURN count(n)")));
			executeOk(db, "CREATE (:SampleIsolation {marker: 1})");
			return scalarInt(db.execute("MATCH (n:SampleIsolation) RETURN count(n)"));
		});

		const size_t expectedCalls = mode == kExecutionModeOpened ? 3U : 5U;
		EXPECT_EQ(initialCounts.size(), expectedCalls);
		EXPECT_TRUE(std::ranges::all_of(initialCounts, [](int64_t count) { return count == 0; }));
	}

	zyx::Database baseline(baselinePath.string());
	baseline.open();
	EXPECT_EQ(scalarInt(baseline.execute("MATCH (n:SampleIsolation) RETURN count(n)")), 0);
	baseline.close();
	for (const auto &entry: std::filesystem::directory_iterator(tempRoot)) {
		EXPECT_EQ(entry.path().filename().string().find(".workload-"), std::string::npos);
	}
}

TEST_F(CompareBenchmarkTest, MutatingSampleFailureStillRemovesItsDatabaseClone) {
	const auto baselinePath = tempRoot / "failure-baseline.db";
	{
		zyx::Database baseline(baselinePath.string());
		baseline.open();
		baseline.close();
	}

	Options options;
	options.dbPath = tempRoot / "failure.db";
	options.baselineDbPath = baselinePath;
	options.scale = "smoke";
	options.executionMode = std::string(kExecutionModeWarm);
	options.iterations = 1;

	EXPECT_THROW(runMeasuredIsolatedWorkload(options, LoadedGraph{}, "point_create_node", [](zyx::Database &) -> int64_t {
		throw std::runtime_error("expected operation failure");
	}), std::runtime_error);
	for (const auto &entry: std::filesystem::directory_iterator(tempRoot)) {
		EXPECT_EQ(entry.path().filename().string().find(".workload-"), std::string::npos);
	}
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsCompleteScanAndIndexedOptions) {
	std::vector<std::string> args = {"zyx-compare-bench",
									 "--dataset",
									 "data",
									 "--db-path",
									 "db",
									 "--scale",
									 "small",
									 "--profile",
									 "indexed",
									 "--emit-profile",
									 "--execution-mode",
									 "cold-ish",
									 "--warmup",
									 "2",
									 "--iterations",
									 "3"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	EXPECT_EQ(options.dataset, std::filesystem::path("data"));
	EXPECT_EQ(options.dbPath, std::filesystem::path("db"));
	EXPECT_EQ(options.scale, "small");
	EXPECT_EQ(options.profile, std::string(kProfileIndexed));
	EXPECT_TRUE(options.emitProfile);
	EXPECT_EQ(options.executionMode, std::string(kExecutionModeColdish));
	EXPECT_EQ(options.warmup, 2);
	EXPECT_EQ(options.iterations, 3);
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsCounterOnlyDiagnostics) {
    std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
                                     "--scale", "small", "--emit-counters"};
    std::vector<char *> argv;
    for (auto &arg: args) argv.push_back(arg.data());
    const auto options = parseArgs(static_cast<int>(argv.size()), argv.data());
    EXPECT_TRUE(options.emitCounters);
    EXPECT_FALSE(options.emitProfile);
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsThreadsOption) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
									 "--scale", "small", "--threads", "8"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	ASSERT_TRUE(options.threads.has_value());
	EXPECT_EQ(*options.threads, 8UL);
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsOpenedExecutionMode) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
									 "--scale", "small", "--execution-mode", "opened"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	EXPECT_EQ(options.executionMode, std::string(kExecutionModeOpened));
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsMultihopProfileAndComputesTargets) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
									 "--scale", "small", "--profile", "multihop"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	EXPECT_EQ(options.profile, std::string(kProfileMultihop));
	EXPECT_EQ(targetUserIdForDepth(6), "user-000031");
	EXPECT_EQ(targetUserIdForDepth(30), "user-000151");
	EXPECT_EQ(targetUserIdForDepth(6, "smoke"), "user-000019");
	EXPECT_EQ(targetUserIdForDepth(30, "smoke"), "user-000091");
}

TEST_F(CompareBenchmarkTest, ParseArgsValidatesTheSelectedWorkloadAgainstItsProfile) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
			"--scale", "small", "--profile", "multihop", "--workload", "varlength_frontier_count"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}
	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());
	ASSERT_TRUE(options.onlyWorkload.has_value());
	EXPECT_EQ(*options.onlyWorkload, "varlength_frontier_count");
	EXPECT_TRUE(isSupportedWorkload(kProfileMultihop, "varlength_frontier_count"));
	EXPECT_TRUE(isSupportedWorkload(kProfileMultihop, "load_nodes_edges"));
	EXPECT_FALSE(isSupportedWorkload(kProfileScan, "varlength_frontier_count"));

	args.back() = "not_registered";
	argv.clear();
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}
	EXPECT_THROW(static_cast<void>(parseArgs(static_cast<int>(argv.size()), argv.data())), std::invalid_argument);
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsWriteProfileAndComputesWriteTargets) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
									 "--scale", "smoke", "--profile", "write"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	EXPECT_EQ(options.profile, std::string(kProfileWrite));
	EXPECT_EQ(writeUpdateTargetUserId("smoke"), "user-000004");
	EXPECT_EQ(writeUpdateTargetUserId("small"), "user-000006");
	EXPECT_EQ(writeUpdateTargetUserId("tiny"), "user-000006");
	EXPECT_TRUE(isMutatingWorkload("point_create_node"));
	EXPECT_TRUE(isMutatingWorkload("batch_create_edges_10000_then_one_hop_expand"));
	EXPECT_TRUE(isMutatingWorkload("write_then_read_edge_durable"));
	EXPECT_FALSE(isMutatingWorkload("relationship_type_scan"));
	EXPECT_EQ(workloadPathToken("batch/create edges"), "batch_create_edges");
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsWriteDurableProfile) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
									 "--scale", "smoke", "--profile", "write_durable"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	EXPECT_EQ(options.profile, std::string(kProfileWriteDurable));
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsOperationalDynamicProfile) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
									 "--scale", "smoke", "--profile", "operational_dynamic"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	EXPECT_EQ(options.profile, std::string(kProfileOperationalDynamic));
}

TEST_F(CompareBenchmarkTest, ParseArgsAcceptsRetrievalProfile) {
	std::vector<std::string> args = {"zyx-compare-bench", "--dataset", "data", "--db-path", "db",
									 "--scale", "smoke", "--profile", "retrieval"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	const Options options = parseArgs(static_cast<int>(argv.size()), argv.data());

	EXPECT_EQ(options.profile, std::string(kProfileRetrieval));
	EXPECT_TRUE(isSupportedProfile(std::string(kProfileRetrieval)));
	EXPECT_FALSE(isSupportedProfile("not_a_profile"));
}

TEST_F(CompareBenchmarkTest, ReachableWithinUsesStrictBoundedPathSemantics) {
	zyx::Database db((tempRoot / "bounded_reachability.db").string());
	db.open();
	auto txn = db.beginTransaction();

	for (int64_t user = 1; user <= 32; ++user) {
		std::ostringstream query;
		query << "CREATE (:User {id: '" << userIdForIndex(user) << "'})";
		executeOk(txn, query.str());
	}
	for (const auto &[src, dst]: std::array<std::pair<int64_t, int64_t>, 8>{{{1, 6}, {6, 11}, {11, 16}, {16, 21},
																								 {21, 26}, {26, 31}, {26, 27}, {27, 32}}}) {
		std::ostringstream query;
		query << "MATCH (a:User {id: '" << userIdForIndex(src) << "'}), (b:User {id: '"
			  << userIdForIndex(dst) << "'}) CREATE (a)-[:FOLLOWS]->(b)";
		executeOk(txn, query.str());
	}
	txn.commit();

	EXPECT_EQ(reachableWithinTarget(db, 6, "user-000031"), 1);
	EXPECT_EQ(reachableWithinTarget(db, 6, "user-000032"), 0);
	EXPECT_EQ(reachableWithinTarget(db, 7, "user-000032"), 1);
	EXPECT_EQ(requireReachableWithinTarget(db, 6, "user-000031"), 1);
	EXPECT_THROW((void) requireReachableWithinTarget(db, 6, "user-000032"), std::runtime_error);

	db.close();
}

TEST_F(CompareBenchmarkTest, ParseArgsRejectsInvalidAndMissingValues) {
	auto parse = [](std::vector<std::string> args) {
		std::vector<char *> argv;
		for (auto &arg: args) {
			argv.push_back(arg.data());
		}
		return parseArgs(static_cast<int>(argv.size()), argv.data());
	};

	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset"}), std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--profile",
						"bad"}),
				 std::invalid_argument);
	EXPECT_THROW(
			parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--execution-mode",
				   "bad"}),
			std::invalid_argument);
	EXPECT_THROW(
			parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--warmup", "-1"}),
			std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--iterations",
						"0"}),
				 std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--threads",
						"-1"}),
				 std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--threads",
						"2x"}),
				 std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--unknown"}),
				 std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--db-path", "db", "--scale", "small"}), std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--scale", "small"}), std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db"}), std::invalid_argument);
}

TEST_F(CompareBenchmarkTest, CsvAndJsonHelpersHandleEscapingAndInvalidRows) {
	EXPECT_EQ(jsonEscape("a\\b\"c\n\t"), "a\\\\b\\\"c\\n\\t");
	EXPECT_EQ(jsonEscape(std::string("x\b\f\r") + static_cast<char>(0x01)), "x\\b\\f\\r\\u0001");
	EXPECT_EQ(parseCsvLine("a,\"b,c\",\"d\"\"e\""), (std::vector<std::string>{"a", "b,c", "d\"e"}));

	const auto csvPath = tempRoot / "rows.csv";
	writeFile(csvPath, "id,name,extra\r\n1,\"Alice, A\"\r\n\r\n2,Bob,ignored\r\n");
	const auto rows = readCsv(csvPath);
	ASSERT_EQ(rows.size(), 2U);
	EXPECT_EQ(field(rows[0], "name"), "Alice, A");
	EXPECT_FALSE(rows[0].values.contains("extra"));
	EXPECT_EQ(field(rows[1], "id"), "2");
	EXPECT_THROW((void) field(rows[0], "missing"), std::runtime_error);
	EXPECT_THROW((void) readCsv(tempRoot / "missing.csv"), std::runtime_error);
	writeFile(tempRoot / "empty.csv", "");
	EXPECT_TRUE(readCsv(tempRoot / "empty.csv").empty());
	EXPECT_EQ(toInt64("42"), 42);
	EXPECT_DOUBLE_EQ(toDouble("3.25"), 3.25);
}

TEST_F(CompareBenchmarkTest, ScalarHelpersHandleResultTypesAndFailures) {
	zyx::Database db((tempRoot / "scalar.db").string());
	db.open();

	EXPECT_EQ(scalarInt(db.execute("RETURN 7")), 7);
	EXPECT_EQ(scalarInt(db.execute("RETURN 3.5")), 3);
	EXPECT_EQ(scalarInt(db.execute("RETURN true")), 1);
	EXPECT_EQ(scalarInt(db.execute("RETURN false")), 0);
	EXPECT_EQ(scalarInt(db.execute("RETURN 'value'")), 0);
	EXPECT_EQ(scalarInt(db.execute("MATCH (n:Missing) RETURN n")), 0);
	EXPECT_THROW((void) scalarInt(db.execute("THIS IS NOT CYPHER")), std::runtime_error);

	EXPECT_TRUE(scalarTruthy(db.execute("RETURN true")));
	EXPECT_FALSE(scalarTruthy(db.execute("RETURN false")));
	EXPECT_TRUE(scalarTruthy(db.execute("RETURN 1")));
	EXPECT_FALSE(scalarTruthy(db.execute("RETURN 0")));
	EXPECT_TRUE(scalarTruthy(db.execute("RETURN 0.5")));
	EXPECT_FALSE(scalarTruthy(db.execute("RETURN 0.0")));
	EXPECT_TRUE(scalarTruthy(db.execute("RETURN 'value'")));
	EXPECT_FALSE(scalarTruthy(db.execute("RETURN null")));
	EXPECT_FALSE(scalarTruthy(db.execute("MATCH (n:Missing) RETURN n")));
	EXPECT_THROW((void) scalarTruthy(db.execute("THIS IS NOT CYPHER")), std::runtime_error);

	db.close();
}

TEST_F(CompareBenchmarkTest, RowCountAndValidationHelpersHandleAlternatePaths) {
	struct FakeRows {
		size_t rowCount() const { return 3; }
	};

	EXPECT_EQ(rowCount([]() { return FakeRows{}; }), 3);
	EXPECT_NO_THROW(requireNonNegative("ok", 0));
	EXPECT_THROW(requireNonNegative("bad", -1), std::runtime_error);
	EXPECT_EQ(requireExactlyOne("one", 1), 1);
	EXPECT_THROW((void) requireExactlyOne("not_one", 2), std::runtime_error);

	zyx::Database db((tempRoot / "execute_ok.db").string());
	db.open();
	EXPECT_NO_THROW(executeOk(db, "RETURN 1"));
	EXPECT_THROW(executeOk(db, "THIS IS NOT CYPHER"), std::runtime_error);
	EXPECT_EQ(materializedRowCount(db.execute("RETURN 1 AS a, 'x' AS b")), 1);
	EXPECT_EQ(materializedRowCount(db.execute("MATCH (n:Missing) RETURN n.id")), 0);
	EXPECT_THROW((void) materializedRowCount(db.execute("THIS IS NOT CYPHER")), std::runtime_error);
	db.close();

	Options options;
	options.scale = "tiny";
	options.iterations = 1;
	options.warmup = 1;
	options.emitProfile = false;
	EXPECT_NO_THROW(runMeasured(options, "allows_negative", []() { return int64_t{-5}; }, false));
	EXPECT_THROW(runMeasured(options, "rejects_negative", []() { return int64_t{-5}; }), std::runtime_error);
}

TEST_F(CompareBenchmarkTest, LoadGraphRejectsDanglingCsvReferences) {
	const auto dataset = writeSmallDataset();
	writeFile(dataset / "follows.csv", "src,dst,weight\n"
									   "user-000001,missing-user,1\n");

	zyx::Database db((tempRoot / "bad_refs.db").string());
	db.open();
	EXPECT_THROW((void) loadGraph(db, dataset), std::runtime_error);
	EXPECT_EQ(scalarInt(db.execute("MATCH (u:User) RETURN count(u)")), 0);
	db.close();
}

TEST_F(CompareBenchmarkTest, LoadGraphRejectsDanglingPostAndTagReferences) {
	{
		const auto dataset = writeSmallDataset();
		writeFile(dataset / "authored.csv", "src,dst,weight\n"
											"user-000001,missing-post,1\n");
		zyx::Database db((tempRoot / "bad_post_refs.db").string());
		db.open();
		EXPECT_THROW((void) loadGraph(db, dataset), std::runtime_error);
		EXPECT_EQ(scalarInt(db.execute("MATCH (u:User) RETURN count(u)")), 0);
		db.close();
	}

	{
		const auto dataset = writeSmallDataset();
		writeFile(dataset / "has_tag.csv", "src,dst,weight\n"
										   "post-000001,missing-tag,1\n");
		zyx::Database db((tempRoot / "bad_tag_refs.db").string());
		db.open();
		EXPECT_THROW((void) loadGraph(db, dataset), std::runtime_error);
		EXPECT_EQ(scalarInt(db.execute("MATCH (u:User) RETURN count(u)")), 0);
		db.close();
	}
}

TEST_F(CompareBenchmarkTest, LoadGraphCommitsDatasetInSingleWriteTransaction) {
	const auto dataset = writeSmallDataset();
	zyx::Database db((tempRoot / "batched_load.db").string());
	db.open();

	graph::debug::PerfTrace::setEnabled(true);
	graph::debug::PerfTrace::reset();
	const auto loaded = loadGraph(db, dataset);
	const auto trace = graph::debug::PerfTrace::snapshotAndReset();

	EXPECT_EQ(loaded.loadedRows, 9);
	ASSERT_TRUE(trace.contains("load.users.csv_read"));
	ASSERT_TRUE(trace.contains("load.users.create_nodes"));
	ASSERT_TRUE(trace.contains("load.follows.create_edges"));
	ASSERT_TRUE(trace.contains("load.commit"));
	EXPECT_EQ(trace.at("load.commit").calls, 1U);
	EXPECT_FALSE(trace.contains("wal.commit_sync"));
	EXPECT_EQ(scalarInt(db.execute("MATCH (u:User) RETURN count(u)")), 3);
	EXPECT_EQ(scalarInt(db.execute("MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) RETURN count(v)")), 1);
	EXPECT_EQ(scalarInt(db.execute("MATCH (:User {id: 'user-000001'})-[:AUTHORED]->(p:Post) RETURN count(p)")), 1);

	db.close();
}

TEST_F(CompareBenchmarkTest, LoadGraphUsesExistingWriteTransactionWhenPresent) {
	const auto dataset = writeSmallDataset();
	zyx::Database db((tempRoot / "external_txn_load.db").string());
	db.open();
	auto txn = db.beginTransaction();

	const auto loaded = loadGraph(txn, dataset);

	EXPECT_EQ(loaded.loadedRows, 9);
	EXPECT_TRUE(db.hasActiveTransaction());
	EXPECT_EQ(scalarInt(txn.execute("MATCH (u:User) RETURN count(u)")), 3);
	txn.commit();
	EXPECT_FALSE(db.hasActiveTransaction());
	EXPECT_EQ(scalarInt(db.execute("MATCH (u:User) RETURN count(u)")), 3);
	db.close();
}

TEST_F(CompareBenchmarkTest, LoadGraphRequiresExplicitActiveWriteTransaction) {
	const auto dataset = writeSmallDataset();
	zyx::Database db((tempRoot / "load_graph_transaction_contract.db").string());
	db.open();

	auto writeTransaction = db.beginTransaction();
	EXPECT_THROW((void) loadGraph(db, dataset), std::runtime_error);
	writeTransaction.rollback();
	EXPECT_THROW((void) loadGraph(writeTransaction, dataset), std::runtime_error);

	auto readOnlyTransaction = db.beginReadOnlyTransaction();
	EXPECT_THROW((void) loadGraph(readOnlyTransaction, dataset), std::runtime_error);
	readOnlyTransaction.rollback();
	db.close();
}

TEST_F(CompareBenchmarkTest, SelectedWorkloadEmitsOnlyItsOwnSamples) {
	const auto dataset = writeSmallDataset();

	Options query;
	query.dataset = dataset;
	query.dbPath = tempRoot / "selected-query.db";
	query.scale = "tiny";
	query.profile = std::string(kProfileScan);
	query.onlyWorkload = "label_scan_filter";
	query.iterations = 1;

	std::ostringstream queryStream;
	auto *originalStdout = std::cout.rdbuf(queryStream.rdbuf());
	const int queryStatus = run(query);
	std::cout.rdbuf(originalStdout);
	EXPECT_EQ(queryStatus, 0);
	const std::string queryOutput = queryStream.str();
	EXPECT_NE(queryOutput.find("\"workload\":\"label_scan_filter\""), std::string::npos);
	EXPECT_EQ(queryOutput.find("\"workload\":\"load_nodes_edges\""), std::string::npos);

	Options load = query;
	load.dbPath = tempRoot / "selected-load.db";
	load.onlyWorkload = "load_nodes_edges";
	std::ostringstream loadStream;
	originalStdout = std::cout.rdbuf(loadStream.rdbuf());
	const int loadStatus = run(load);
	std::cout.rdbuf(originalStdout);
	EXPECT_EQ(loadStatus, 0);
	const std::string loadOutput = loadStream.str();
	EXPECT_NE(loadOutput.find("\"workload\":\"load_nodes_edges\""), std::string::npos);
	EXPECT_EQ(loadOutput.find("\"workload\":\"label_scan_filter\""), std::string::npos);
}

TEST_F(CompareBenchmarkTest, RunExecutesSmallScanIndexedAndWriteProfiles) {
	const auto dataset = writeSmallDataset();

	Options scan;
	scan.dataset = dataset;
	scan.dbPath = tempRoot / "scan.db";
	scan.scale = "tiny";
	scan.profile = std::string(kProfileScan);
	scan.emitProfile = true;
	scan.iterations = 1;

	EXPECT_EQ(run(scan), 0);

	Options coldish = scan;
	coldish.dbPath = tempRoot / "coldish.db";
	coldish.executionMode = std::string(kExecutionModeColdish);
	EXPECT_EQ(run(coldish), 0);

	Options opened = scan;
	opened.dbPath = tempRoot / "opened.db";
	opened.executionMode = std::string(kExecutionModeOpened);
	EXPECT_EQ(run(opened), 0);

	Options indexed;
	indexed.dataset = dataset;
	indexed.dbPath = tempRoot / "indexed.db";
	indexed.scale = "tiny";
	indexed.profile = std::string(kProfileIndexed);
	indexed.iterations = 1;

	EXPECT_EQ(run(indexed), 0);

	Options retrieval;
	retrieval.dataset = dataset;
	retrieval.dbPath = tempRoot / "retrieval.db";
	retrieval.scale = "tiny";
	retrieval.profile = std::string(kProfileRetrieval);
	retrieval.iterations = 1;

	EXPECT_EQ(run(retrieval), 0);

	Options write;
	write.dataset = dataset;
	write.dbPath = tempRoot / "write.db";
	write.scale = "tiny";
	write.profile = std::string(kProfileWrite);
	write.iterations = 1;

	EXPECT_EQ(run(write), 0);

	Options writeDurable = write;
	writeDurable.dbPath = tempRoot / "write_durable.db";
	writeDurable.profile = std::string(kProfileWriteDurable);
	EXPECT_EQ(run(writeDurable), 0);
}

TEST_F(CompareBenchmarkTest, RunExecutesSmallOperationalDynamicProfile) {
	const auto dataset = tempRoot / "operational_dynamic_dataset";
	std::filesystem::create_directories(dataset);
	writeFile(dataset / "users.csv", "id,age,country,score\n"
									 "user-000001,31,CN,901.5\n"
									 "user-000006,36,US,850.0\n"
									 "user-000007,42,CN,990.0\n"
									 "user-000008,28,US,710.0\n");
	writeFile(dataset / "posts.csv", "id,created_at,score\n"
									 "post-000001,1,10.5\n");
	writeFile(dataset / "tags.csv", "id,rank\n"
									"tag-000001,1\n");
	writeFile(dataset / "follows.csv", "src,dst,weight\n"
									   "user-000001,user-000006,1\n"
									   "user-000006,user-000007,2\n"
									   "user-000007,user-000001,3\n"
									   "user-000008,user-000001,4\n");
	writeFile(dataset / "authored.csv", "src,dst,weight\n"
										"user-000001,post-000001,1\n");
	writeFile(dataset / "has_tag.csv", "src,dst,weight\n"
									   "post-000001,tag-000001,1\n");

	Options operational_dynamic;
	operational_dynamic.dataset = dataset;
	operational_dynamic.dbPath = tempRoot / "operational_dynamic.db";
	operational_dynamic.scale = "tiny";
	operational_dynamic.profile = std::string(kProfileOperationalDynamic);
	operational_dynamic.iterations = 1;
	operational_dynamic.includeLargeBatchWorkloads = false;

	EXPECT_EQ(run(operational_dynamic), 0);

	zyx::Database db(operational_dynamic.dbPath.string());
	db.open();
	EXPECT_EQ(scalarInt(db.execute("MATCH (u:User {id: 'post-persist-user-000001'}) RETURN count(u)")), 0);
	EXPECT_EQ(scalarInt(db.execute("MATCH (:User {id: 'user-000006'})-[r:FOLLOWS]->(:User {id: 'user-000001'}) "
						   "RETURN count(r)")),
		  0);
	db.close();

	for (const auto &entry: std::filesystem::directory_iterator(tempRoot)) {
		EXPECT_EQ(entry.path().filename().string().find(".workload-"), std::string::npos);
	}
}

TEST_F(CompareBenchmarkTest, MainReportsJsonErrorForInvalidArguments) {
	std::vector<std::string> args = {"zyx-compare-bench", "--scale", "tiny"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	EXPECT_EQ(zyx_compare_benchmark_main(static_cast<int>(argv.size()), argv.data()), 1);
}

TEST_F(CompareBenchmarkTest, MainAttributesSelectedWorkloadFailure) {
	std::vector<std::string> args = {
			"zyx-compare-bench", "--dataset", (tempRoot / "missing-dataset").string(), "--db-path",
			(tempRoot / "selected-failure.db").string(), "--scale", "tiny", "--profile", "scan", "--workload",
			"label_scan_filter"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	std::ostringstream errorStream;
	auto *originalStderr = std::cerr.rdbuf(errorStream.rdbuf());
	const int status = zyx_compare_benchmark_main(static_cast<int>(argv.size()), argv.data());
	std::cerr.rdbuf(originalStderr);
	EXPECT_EQ(status, 1);
	const std::string output = errorStream.str();
	EXPECT_NE(output.find("\"workload\":\"label_scan_filter\""), std::string::npos);
}

TEST_F(CompareBenchmarkTest, BoundedPreparationPreservesEveryNodeEdgeAndExternalIdAcrossCommits) {
    const auto dataset = writeSmallDataset();
    for (size_t batch : {1U, 2U}) {
        SCOPED_TRACE(batch);
        zyx::Database db((tempRoot / ("bounded-" + std::to_string(batch) + ".db")).string()); db.open();
        const auto loaded = loadGraph(db, dataset, batch);
        EXPECT_EQ(loaded.loadedRows, 9);
        EXPECT_EQ(loaded.usersByExternalId.size(), 3U);
        EXPECT_EQ(loaded.postsByExternalId.size(), 1U);
        EXPECT_EQ(loaded.tagsByExternalId.size(), 1U);
        EXPECT_FALSE(db.hasActiveTransaction());
        EXPECT_EQ(scalarInt(db.execute("MATCH (n:User) RETURN count(n)")), 3);
        EXPECT_EQ(scalarInt(db.execute("MATCH ()-[r:FOLLOWS]->() RETURN count(r)")), 2);
        EXPECT_EQ(scalarInt(db.execute("MATCH (:User {id:'user-000001'})-[:AUTHORED]->(:Post)-[:HAS_TAG]->(:Tag) RETURN count(*)")), 1);
        db.close(); db.open();
        EXPECT_EQ(scalarInt(db.execute("MATCH (n:User) WHERE n.country = 'CN' RETURN count(n)")), 2);
        db.close();
    }
}

TEST_F(CompareBenchmarkTest, BoundedPreparationRejectsInvalidColumnsBeforePublishingAChunk) {
    zyx::Database db((tempRoot / "invalid-bounded.db").string()); db.open();
    BoundedDatasetWriter writer(db, 1U);
    EXPECT_THROW(static_cast<void>(BoundedDatasetWriter(db, 0U)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(writer.createNodesColumnar("User", 2U, {{"id", {int64_t{1}}}})), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(writer.createEdgesColumnar("FOLLOWS", {1,2}, {1}, {})), std::invalid_argument);
    EXPECT_EQ(scalarInt(db.execute("MATCH (n) RETURN count(n)")), 0);
    auto transaction = db.beginTransaction();
    EXPECT_THROW(static_cast<void>(loadGraph(db, writeSmallDataset(), 1U)), std::runtime_error);
    transaction.rollback(); db.close();
}

TEST_F(CompareBenchmarkTest, SelectedQueryListPreparesOnceAndMeasuresOnlyRequestedQueries) {
    Options options; options.dataset=writeSmallDataset();options.dbPath=tempRoot/"profile-once.db";
    options.scale="smoke";options.warmup=0;options.iterations=1;options.loadBatchRows=2;
    options.selectedWorkloads={"label_scan_filter","aggregation_count_by_group"};
    options.emitResultRows=true;
    std::ostringstream captured;
    struct Restore {std::streambuf *previous;~Restore(){std::cout.rdbuf(previous);}} restore{std::cout.rdbuf(captured.rdbuf())};
    EXPECT_EQ(run(options),0);
    const auto output=captured.str();
    EXPECT_EQ(std::count(output.begin(),output.end(),'\n'),4);
    EXPECT_NE(output.find("label_scan_filter"),std::string::npos);
    EXPECT_NE(output.find("aggregation_count_by_group"),std::string::npos);
    EXPECT_EQ(output.find("load_nodes_edges"),std::string::npos);
    EXPECT_EQ(output.find("topk_property_sort"),std::string::npos);
}

TEST_F(CompareBenchmarkTest, QueryListRejectsAmbiguousUnknownAndImportSelections) {
    for(const std::string names:{"",",label_scan_filter","label_scan_filter,","label_scan_filter,label_scan_filter","unknown","load_nodes_edges"}) {
        std::vector<std::string> args={"bench","--dataset","data","--db-path","db","--scale","smoke","--workloads",names};
        std::vector<char *> argv;for(auto &arg:args)argv.push_back(arg.data());
        EXPECT_THROW(static_cast<void>(parseArgs(argv.size(),argv.data())),std::invalid_argument)<<names;
    }
    std::vector<std::string> args={"bench","--dataset","data","--db-path","db","--scale","smoke","--workloads","label_scan_filter","--workload","label_scan_filter"};
    std::vector<char *> argv;for(auto &arg:args)argv.push_back(arg.data());
    EXPECT_THROW(static_cast<void>(parseArgs(argv.size(),argv.data())),std::invalid_argument);
}

TEST_F(CompareBenchmarkTest, PreparedReadClonesPreserveTheSourceAndBuildRequestedIndexesPrivately) {
    const auto dataset=writeSmallDataset();
    const auto source=tempRoot/"prepared-source.db";
    Options original;original.dataset=dataset;original.scale="smoke";original.loadBatchRows=2;
    static_cast<void>(loadDatabase(original,source));
    const auto bytes=[](const std::filesystem::path &path) {
        std::ifstream input(path,std::ios::binary);
        return std::vector<char>(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    };
    struct RestorePermissions {
        std::filesystem::path path;
        std::filesystem::perms mode;
        ~RestorePermissions(){std::error_code error;std::filesystem::permissions(path,mode,error);}
    } restorePermissions{source,std::filesystem::status(source).permissions()};
    std::filesystem::permissions(source,std::filesystem::perms::owner_read,std::filesystem::perm_options::replace);
    const auto unchanged=bytes(source);
    for(const std::string profile:{"scan","indexed"}) {
        Options options;options.dataset=dataset;options.scale="smoke";options.profile=profile;
        options.dbPath=tempRoot/(profile+"-private.db");options.preparedDbPath=source;
        options.selectedWorkloads={profile=="scan"?"label_scan_filter":"point_lookup_indexed"};
        options.warmup=0;options.iterations=1;
        std::ostringstream captured;
        struct Restore {std::streambuf *previous;~Restore(){std::cout.rdbuf(previous);}} restore{std::cout.rdbuf(captured.rdbuf())};
        EXPECT_EQ(run(options),0);
        EXPECT_NE(captured.str().find(profile=="scan"?"\"value\":2":"\"value\":1"),std::string::npos);
        EXPECT_EQ(bytes(source),unchanged);
        EXPECT_THROW(static_cast<void>(loadDatabase(options,source)),std::invalid_argument);
        EXPECT_EQ(bytes(source),unchanged);
    }
}

TEST_F(CompareBenchmarkTest, PreparedInputRejectsImportAndMutationMeasurements) {
    for(const auto &selection:std::vector<std::pair<std::string,std::string>>{
        {"scan","load_nodes_edges"},{"write","point_create_node"},{"write_durable","point_create_node_durable"}}) {
        std::vector<std::string> args={"bench","--dataset","data","--db-path","db","--scale","smoke",
            "--profile",selection.first,"--workload",selection.second,"--prepared-db","source"};
        std::vector<char *> argv;for(auto &arg:args)argv.push_back(arg.data());
        EXPECT_THROW(static_cast<void>(parseArgs(argv.size(),argv.data())),std::invalid_argument);
    }
    Options options;options.preparedDbPath="source";
    EXPECT_THROW(validatePreparedInput(options),std::invalid_argument);
}
