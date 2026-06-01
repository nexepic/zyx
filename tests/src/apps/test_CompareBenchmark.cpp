#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
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
	EXPECT_EQ(options.warmup, 2);
	EXPECT_EQ(options.iterations, 3);
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
			parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--warmup", "-1"}),
			std::invalid_argument);
	EXPECT_THROW(parse({"zyx-compare-bench", "--dataset", "data", "--db-path", "db", "--scale", "small", "--iterations",
						"0"}),
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
	ASSERT_TRUE(trace.contains("txn.save"));
	ASSERT_TRUE(trace.contains("wal.commit_sync"));
	EXPECT_EQ(trace.at("txn.save").calls, 1U);
	EXPECT_EQ(trace.at("wal.commit_sync").calls, 1U);
	EXPECT_EQ(scalarInt(db.execute("MATCH (u:User) RETURN count(u)")), 3);

	db.close();
}

TEST_F(CompareBenchmarkTest, RunExecutesSmallScanAndIndexedProfiles) {
	const auto dataset = writeSmallDataset();

	Options scan;
	scan.dataset = dataset;
	scan.dbPath = tempRoot / "scan.db";
	scan.scale = "tiny";
	scan.profile = std::string(kProfileScan);
	scan.emitProfile = true;
	scan.iterations = 1;

	EXPECT_EQ(run(scan), 0);

	Options indexed;
	indexed.dataset = dataset;
	indexed.dbPath = tempRoot / "indexed.db";
	indexed.scale = "tiny";
	indexed.profile = std::string(kProfileIndexed);
	indexed.iterations = 1;

	EXPECT_EQ(run(indexed), 0);
}

TEST_F(CompareBenchmarkTest, MainReportsJsonErrorForInvalidArguments) {
	std::vector<std::string> args = {"zyx-compare-bench", "--scale", "tiny"};
	std::vector<char *> argv;
	for (auto &arg: args) {
		argv.push_back(arg.data());
	}

	EXPECT_EQ(zyx_compare_benchmark_main(static_cast<int>(argv.size()), argv.data()), 1);
}
