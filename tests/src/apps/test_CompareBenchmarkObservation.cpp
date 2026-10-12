#include <gtest/gtest.h>

#define main zyx_compare_observer_benchmark_main
#include "../../../apps/compare_benchmark/main.cpp"
#undef main

TEST(CompareBenchmarkObserverTest, SemanticRowsKeepIntegerTypeAndEscapeStrings) {
	EXPECT_EQ(resultRowsJson({}), "[]");
	EXPECT_EQ(resultRowsJson({{std::string("CN"), int64_t{2}}, {std::string("US"), int64_t{1}}}),
			"[[\"CN\",2],[\"US\",1]]");
	EXPECT_EQ(resultRowsJson({{std::string("a\"\n")}}), "[[\"a\\\"\\n\"]]");
	EXPECT_THROW(resultRowsJson({{true}}), std::runtime_error);
	EXPECT_THROW(resultRowsJson({{2.0}}), std::runtime_error);
}

TEST(CompareBenchmarkObserverTest, SemanticObservationClearsWarmupAndPreviousIterationRows) {
	Options options;
	options.scale = "smoke";
	options.resultRows = std::vector<std::vector<zyx::Value>>{{std::string("stale")}};
	std::ostringstream captured;
	struct RestoreStream {
		std::streambuf *previous;
		~RestoreStream() { std::cout.rdbuf(previous); }
	} restore{std::cout.rdbuf(captured.rdbuf())};
	measureOperation(options, "groups", 0, [&]() {
		EXPECT_FALSE(options.resultRows.has_value());
		options.resultRows = std::vector<std::vector<zyx::Value>>{{std::string("CN"), int64_t{2}}};
		return int64_t{1};
	}, [](int64_t) {});
	EXPECT_FALSE(options.resultRows.has_value());
	EXPECT_NE(captured.str().find("\"rows\":[[\"CN\",2]]"), std::string::npos);
	EXPECT_EQ(captured.str().find("stale"), std::string::npos);
	captured.str("");
	measureOperation(options, "count-only", 1, []() { return int64_t{3}; }, [](int64_t) {});
	EXPECT_EQ(captured.str().find("\"rows\""), std::string::npos);
}

TEST(CompareBenchmarkObserverTest, UnsupportedSemanticCellFailsBeforeWritingAnyPartialEvent) {
	Options options;
	options.scale = "smoke";
	std::ostringstream captured;
	struct RestoreStream {
		std::streambuf *previous;
		~RestoreStream() { std::cout.rdbuf(previous); }
	} restore{std::cout.rdbuf(captured.rdbuf())};
	EXPECT_THROW(measureOperation(options, "bad", 0, [&]() {
		options.resultRows = std::vector<std::vector<zyx::Value>>{{true}};
		return int64_t{1};
	}, [](int64_t) {}), std::runtime_error);
	EXPECT_TRUE(captured.str().empty());
	EXPECT_FALSE(options.resultRows.has_value());
}
