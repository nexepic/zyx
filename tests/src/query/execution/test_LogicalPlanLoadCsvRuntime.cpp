/**
 * @file test_LogicalPlanLoadCsvRuntime.cpp
 * @brief Security and semantic tests for logical LOAD CSV execution.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::query::execution {

	namespace {

		namespace fs = std::filesystem;

		class CsvOnlySnapshot final : public storage::LogicalGraphReadSnapshot {
		public:
			[[nodiscard]] storage::LogicalGraphNode readNode(uint64_t) const override {
				throw std::logic_error("CSV-only snapshot must not read Nodes");
			}
			[[nodiscard]] storage::LogicalGraphEdge readEdge(uint64_t) const override {
				throw std::logic_error("CSV-only snapshot must not read Edges");
			}
			void visitNodes(size_t, const std::function<bool(const storage::LogicalGraphNode &)> &) const override {
				throw std::logic_error("CSV-only snapshot must not scan Nodes");
			}
			[[nodiscard]] storage::LogicalGraphNodePage scanNodesAfter(uint64_t, size_t, size_t) const override {
				throw std::logic_error("CSV-only snapshot must not scan Nodes");
			}
			[[nodiscard]] storage::LogicalGraphAdjacencyPage scanAdjacentEdges(
					uint64_t, storage::LogicalGraphReadDirection,
					std::optional<storage::LogicalGraphAdjacencyContinuation>, size_t) const override {
				throw std::logic_error("CSV-only snapshot must not scan adjacency");
			}
			void visitEdges(size_t, const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("CSV-only snapshot must not scan Edges");
			}
			void visitAdjacentEdges(uint64_t, storage::LogicalGraphReadDirection, size_t,
								const std::function<bool(const storage::LogicalGraphEdge &)> &) const override {
				throw std::logic_error("CSV-only snapshot must not scan adjacency");
			}
		};

		class CsvOnlyBackend final : public storage::LogicalGraphReadBackend {
		public:
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				return std::make_unique<CsvOnlySnapshot>();
			}
		};

		[[nodiscard]] LogicalPlanReadRuntimeBounds bounds() {
			return {.maximum_node_records = 16U,
					.maximum_node_page_records = 2U,
					.maximum_metadata_pages_per_node_page = 4U,
					.maximum_edges_per_node = 16U,
					.maximum_adjacency_page_records = 2U,
					.maximum_property_entries = 16U,
					.maximum_rows = 16U,
					.maximum_distinct_rows = 16U};
		}

		[[nodiscard]] QueryPlan parse(const std::string &query) {
			graph::parser::cypher::CypherParserImpl parser;
			return parser.parseToLogical(query);
		}

		[[nodiscard]] std::string fileUri(const fs::path &path) {
			const std::string value = path.generic_string();
			return std::string("file://") + (value.starts_with('/') ? "" : "/") + value;
		}

		class LogicalPlanLoadCsvRuntimeTest : public ::testing::Test {
		protected:
			void SetUp() override {
				const auto uuid = boost::uuids::random_generator()();
				root_ = fs::temp_directory_path() / ("zyx_logical_csv_" + boost::uuids::to_string(uuid));
				fs::create_directories(root_);
			}

			void TearDown() override {
				std::error_code error;
				fs::remove_all(root_, error);
			}

			[[nodiscard]] fs::path writeFile(const std::string &name, const std::string &content) const {
				const fs::path path = root_ / name;
				std::ofstream output(path, std::ios::binary);
				if (!output.is_open()) {
					throw std::runtime_error("test could not create a CSV file");
				}
				output << content;
				output.close();
				return path;
			}

			[[nodiscard]] QueryContext context() const {
				QueryContext result;
				result.csv_import_policy.import_root = root_;
				return result;
			}

			fs::path root_;
		};

	} // namespace

	TEST_F(LogicalPlanLoadCsvRuntimeTest, ReadsBoundedHeadersAndQuotedFieldsWithoutOpeningGraphRecords) {
		const fs::path csv = writeFile("people.csv", "name,bio\nAlice,\"Has a, comma\"\nBob,\"Line1\nLine2\"\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(csv) +
				"' AS row RETURN row.name AS name, row.bio AS bio ORDER BY name");
		ASSERT_NE(plan.root, nullptr);
		EXPECT_FALSE(plan.mutatesData);

		const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context());
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("name").asPrimitive(), PropertyValue("Alice"));
		EXPECT_EQ(result.getRows()[0].at("bio").asPrimitive(), PropertyValue("Has a, comma"));
		EXPECT_EQ(result.getRows()[1].at("bio").asPrimitive(), PropertyValue("Line1\nLine2"));
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, ReadsAnInRootFileThroughAValidPercentEncodedUri) {
		static_cast<void>(writeFile("encoded value~~.csv", "value\nencoded\n"));
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string uri = fileUri(root_) + "/encoded%20value%7e%7E.csv";
		QueryPlan plan = parse("LOAD CSV WITH HEADERS FROM '" + uri + "' AS row RETURN row.value AS value");
		ASSERT_NE(plan.root, nullptr);

		const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("value").asPrimitive(), PropertyValue("encoded"));
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, RejectsRemoteUrisAndFilesOutsideTheConfiguredRoot) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan remote = parse("LOAD CSV FROM 'https://example.invalid/input.csv' AS row RETURN row");
		ASSERT_NE(remote.root, nullptr);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*remote.root, bounds(), context())),
				 std::invalid_argument);

		const fs::path outside = fs::temp_directory_path() / "zyx_logical_csv_outside.csv";
		{
			std::ofstream output(outside, std::ios::binary);
			output << "value\n";
		}
		QueryPlan escaped = parse("LOAD CSV FROM '" + fileUri(outside) + "' AS row RETURN row");
		ASSERT_NE(escaped.root, nullptr);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*escaped.root, bounds(), context())),
				 std::runtime_error);
		const std::string encodedEscape = "file://" + root_.generic_string() + "/%2e%2e/" + outside.filename().string();
		QueryPlan encoded = parse("LOAD CSV FROM '" + encodedEscape + "' AS row RETURN row");
		ASSERT_NE(encoded.root, nullptr);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*encoded.root, bounds(), context())),
				 std::runtime_error);
		std::error_code error;
		fs::remove(outside, error);
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, FailsClosedForInvalidUriPoliciesAndFileBounds) {
		const fs::path csv = writeFile("bounded.csv", "value\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM $uri AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		const auto execute = [&runtime, &plan](const QueryContext &queryContext) {
			return runtime.executeToQueryResult(*plan.root, bounds(), queryContext);
		};

		QueryContext disabled;
		disabled.parameters.emplace("uri", PropertyValue(fileUri(csv)));
		EXPECT_THROW(static_cast<void>(execute(disabled)), std::runtime_error);

		QueryContext invalidLimits = context();
		invalidLimits.parameters.emplace("uri", PropertyValue(fileUri(csv)));
		invalidLimits.csv_import_policy.maximum_rows = 0U;
		EXPECT_THROW(static_cast<void>(execute(invalidLimits)), std::invalid_argument);

		QueryContext nonStringUri = context();
		nonStringUri.parameters.emplace("uri", PropertyValue(7));
		EXPECT_THROW(static_cast<void>(execute(nonStringUri)), std::invalid_argument);

		for (const std::string &uri: {"file:///broken%", "file:///broken%0", "file:///broken%GG", "file:///broken%00"}) {
			QueryContext malformed = context();
			malformed.parameters.emplace("uri", PropertyValue(uri));
			EXPECT_THROW(static_cast<void>(execute(malformed)), std::invalid_argument) << uri;
		}

		QueryContext missingFile = context();
		missingFile.parameters.emplace("uri", PropertyValue(fileUri(root_ / "missing.csv")));
		EXPECT_THROW(static_cast<void>(execute(missingFile)), std::runtime_error);

		QueryContext smallFileLimit = context();
		smallFileLimit.parameters.emplace("uri", PropertyValue(fileUri(csv)));
		smallFileLimit.csv_import_policy.maximum_file_bytes = 1U;
		smallFileLimit.csv_import_policy.maximum_record_bytes = 1U;
		smallFileLimit.csv_import_policy.maximum_field_bytes = 1U;
		EXPECT_THROW(static_cast<void>(execute(smallFileLimit)), std::runtime_error);
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, RejectsAmbiguousLocalUrisAndInvalidImportRoots) {
		const fs::path csv = writeFile("valid.csv", "value\nentry\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM $uri AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		const auto execute = [&runtime, &plan](const QueryContext &queryContext) {
			return runtime.executeToQueryResult(*plan.root, bounds(), queryContext);
		};

		for (const std::string &uri: {"file://relative.csv", "file:///valid.csv?query", "file:///valid.csv#fragment"}) {
			QueryContext invalidUri = context();
			invalidUri.parameters.emplace("uri", PropertyValue(uri));
			EXPECT_THROW(static_cast<void>(execute(invalidUri)), std::invalid_argument) << uri;
		}

		QueryContext rootIsFile = context();
		rootIsFile.csv_import_policy.import_root = csv;
		rootIsFile.parameters.emplace("uri", PropertyValue(fileUri(csv)));
		EXPECT_THROW(static_cast<void>(execute(rootIsFile)), std::runtime_error);

		QueryContext directoryIsNotFile = context();
		directoryIsNotFile.parameters.emplace("uri", PropertyValue(fileUri(root_)));
		EXPECT_THROW(static_cast<void>(execute(directoryIsNotFile)), std::runtime_error);
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, RejectsEveryInconsistentImportLimitBeforeOpeningTheFile) {
		const fs::path csv = writeFile("limits.csv", "value\nentry\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM $uri AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		const auto execute = [&runtime, &plan](const QueryContext &queryContext) {
			return runtime.executeToQueryResult(*plan.root, bounds(), queryContext);
		};

		const auto reject = [&](const std::function<void(QueryContext &)> &configure) {
			QueryContext invalid = context();
			invalid.parameters.emplace("uri", PropertyValue(fileUri(csv)));
			configure(invalid);
			EXPECT_THROW(static_cast<void>(execute(invalid)), std::invalid_argument);
		};

		reject([](QueryContext &value) { value.csv_import_policy.maximum_file_bytes = 0U; });
		reject([](QueryContext &value) { value.csv_import_policy.maximum_record_bytes = 0U; });
		reject([](QueryContext &value) { value.csv_import_policy.maximum_record_bytes = 64U; value.csv_import_policy.maximum_file_bytes = 63U; });
		reject([](QueryContext &value) { value.csv_import_policy.maximum_columns = 0U; });
		reject([](QueryContext &value) { value.csv_import_policy.maximum_field_bytes = 0U; });
		reject([](QueryContext &value) { value.csv_import_policy.maximum_field_bytes = 64U; value.csv_import_policy.maximum_file_bytes = 63U; });
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, RejectsResourceExhaustionAndAmbiguousHeaders) {
		const fs::path rows = writeFile("rows.csv", "A\nB\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan rowPlan = parse("LOAD CSV FROM '" + fileUri(rows) + "' AS row RETURN row");
		QueryContext rowContext = context();
		rowContext.csv_import_policy.maximum_rows = 1U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*rowPlan.root, bounds(), rowContext)),
				 std::runtime_error);

		const fs::path duplicateHeaders = writeFile("duplicate.csv", "name,name\nAda,Grace\n");
		QueryPlan headerPlan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(duplicateHeaders) + "' AS row RETURN row");
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*headerPlan.root, bounds(), context())),
				 std::runtime_error);
	}

} // namespace graph::query::execution
