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
#include <string_view>
#include <utility>
#include <vector>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "CypherParserImpl.hpp"
#include "query/execution/LogicalPlanDirectEdgeRuntimeTestFixture.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/logical/operators/LogicalLoadCsv.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
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
					.maximum_edge_records = 32U,
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

	TEST_F(LogicalPlanLoadCsvRuntimeTest, ReadsHeadersThroughTheConfiguredTabFieldTerminator) {
		const fs::path tsv = writeFile("people.tsv", "name\trole\nAda\tengineer\nGrace\tcompiler\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(tsv) +
				"' AS row FIELDTERMINATOR '\\t' RETURN row.name AS name, row.role AS role ORDER BY name");
		ASSERT_NE(plan.root, nullptr);

		const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context());
		ASSERT_EQ(result.rowCount(), 2U);
		EXPECT_EQ(result.getRows()[0].at("name").asPrimitive(), PropertyValue("Ada"));
		EXPECT_EQ(result.getRows()[0].at("role").asPrimitive(), PropertyValue("engineer"));
		EXPECT_EQ(result.getRows()[1].at("name").asPrimitive(), PropertyValue("Grace"));
		EXPECT_EQ(result.getRows()[1].at("role").asPrimitive(), PropertyValue("compiler"));
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, StopsLoadCsvAfterThePublicRowVisitorCancels) {
		const fs::path csv = writeFile("cancel.csv", "first\nsecond\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM '" + fileUri(csv) + "' AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);

		std::vector<PropertyValue> delivered;
		const LogicalPlanReadRuntimeStats stats = runtime.execute(
				*plan.root, bounds(), [&delivered](const LogicalPlanReadRow &row) {
					delivered.push_back(std::get<PropertyValue>(row.values.at("row")));
					return false;
				},
				context());
		EXPECT_EQ(stats, (LogicalPlanReadRuntimeStats{0U, 0U, 0U, 1U}));
		ASSERT_EQ(delivered.size(), 1U);
		EXPECT_EQ(delivered.front().getList(), (std::vector<PropertyValue>{PropertyValue("first")}));
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, ChecksGuardDuringRecordReadingBeforeDeliveringRows) {
		const std::string field(16384U, 'x');
		const fs::path csv = writeFile("guard-during-record.csv", field + "\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM '" + fileUri(csv) + "' AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		size_t samples = 0U;
		QueryGuard guard(0, 1, [&] { return ++samples < 3U ? size_t(0U) : size_t(2U * 1024U * 1024U); });
		QueryContext guarded = context();
		guarded.guard = &guard;
		size_t delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*plan.root, bounds(), [&](const LogicalPlanReadRow &) {
			++delivered;
			return true;
		}, guarded)), QueryMemoryExceededException);
		EXPECT_EQ(delivered, 0U);
		EXPECT_EQ(samples, 3U);
		const auto retry = runtime.executeToQueryResult(*plan.root, bounds(), context());
		ASSERT_EQ(retry.rowCount(), 1U);
		EXPECT_EQ(retry.getRows().front().at("row").asPrimitive(),
				PropertyValue(std::vector<PropertyValue>{PropertyValue(field)}));
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, LimitStopsBeforeParsingTheNextCsvRecord) {
		const fs::path csv = writeFile("limit-before-malformed-row.csv", "first\n\"unterminated\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM '" + fileUri(csv) + "' AS row RETURN row LIMIT 1");
		ASSERT_NE(plan.root, nullptr);

		const QueryResult result = runtime.executeToQueryResult(*plan.root, bounds(), context());

		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("row").asPrimitive(),
				PropertyValue(std::vector<PropertyValue>{PropertyValue("first")}));
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

		for (const std::string &uri: {"file://", "file:///broken%", "file:///broken%0", "file:///broken%GG",
									   "file:///broken%g0", "file:///broken%0G", "file:///broken%00"}) {
			QueryContext malformed = context();
			malformed.parameters.emplace("uri", PropertyValue(uri));
			EXPECT_THROW(static_cast<void>(execute(malformed)), std::invalid_argument) << uri;
		}
		QueryContext embeddedNul = context();
		std::string nulUri = fileUri(root_ / "nul.csv");
		nulUri.push_back('\0');
		nulUri += "tail";
		embeddedNul.parameters.emplace("uri", PropertyValue(std::move(nulUri)));
		EXPECT_THROW(static_cast<void>(execute(embeddedNul)), std::invalid_argument);

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

	TEST_F(LogicalPlanLoadCsvRuntimeTest, RowLimitRejectsBeforeParsingTheForbiddenNextRecord) {
		const fs::path csv = writeFile("row-limit-before-parse.csv", "accepted\n\"unterminated\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM '" + fileUri(csv) + "' AS row RETURN row");
		QueryContext queryContext = context();
		queryContext.csv_import_policy.maximum_rows = 1U;
		try {
			static_cast<void>(runtime.executeToQueryResult(*plan.root, bounds(), queryContext));
			FAIL() << "CSV source should exceed its row bound";
		} catch (const std::runtime_error &error) {
			EXPECT_NE(std::string(error.what()).find("exceeded its row bound"), std::string::npos);
			EXPECT_EQ(std::string(error.what()).find("unterminated quoted field"), std::string::npos);
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, ExecutionBudgetRejectsCsvSourceBuffersBeforeRowDelivery) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		for (const bool withHeaders: {false, true}) {
			SCOPED_TRACE(withHeaders);
			const std::string field(1000U, 'x');
			const fs::path csv = writeFile(withHeaders ? "source-budget-headers.csv" : "source-budget-list.csv",
					(withHeaders ? "value\n" : "") + field + "\n");
			QueryPlan plan = parse(std::string("LOAD CSV ") + (withHeaders ? "WITH HEADERS " : "") +
					"FROM '" + fileUri(csv) + "' AS row RETURN row");
			ASSERT_NE(plan.root, nullptr);
			auto constrained = bounds();
			constrained.maximum_materialized_bytes = 1024U;
			size_t delivered = 0U;
			try {
				static_cast<void>(runtime.execute(*plan.root, constrained, [&](const LogicalPlanReadRow &) {
					++delivered;
					return true;
				}, context()));
				FAIL() << "CSV source should exceed its execution byte allowance";
			} catch (const std::runtime_error &error) {
				EXPECT_NE(std::string(error.what()).find("CsvReader:"), std::string::npos);
			}
			EXPECT_EQ(delivered, 0U);
			constrained.maximum_materialized_bytes = 8192U;
			EXPECT_EQ(runtime.executeToQueryResult(*plan.root, constrained, context()).rowCount(), 1U);
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, EscapedQuoteRecordsDoNotRequireTwoRawBuffers) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string decoded(1000U, '"');
		const std::string raw = "\"" + std::string(2000U, '"') + "\"";
		for (const bool withHeaders: {false, true}) {
			SCOPED_TRACE(withHeaders);
			const fs::path csv = writeFile(withHeaders ? "escaped-map.csv" : "escaped-list.csv",
					(withHeaders ? "value\r\n" : "") + raw + "\r\n");
			QueryPlan plan = parse(std::string("LOAD CSV ") + (withHeaders ? "WITH HEADERS " : "") +
					"FROM '" + fileUri(csv) + "' AS row RETURN row");
			ASSERT_NE(plan.root, nullptr);
			auto constrained = bounds();
			// One raw record plus decoded allocation capacity fits; two raw records do not.
			constrained.maximum_materialized_bytes = 7000U;
			const auto result = runtime.executeToQueryResult(*plan.root, constrained, context());
			ASSERT_EQ(result.rowCount(), 1U);
			const auto value = result.getRows().front().at("row").asPrimitive();
			if (withHeaders) EXPECT_EQ(value.getMap().at("value"), PropertyValue(decoded));
			else {
				ASSERT_EQ(value.getList().size(), 1U);
				EXPECT_EQ(value.getList().front(), PropertyValue(decoded));
			}
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, BoundsHeaderParsingAndPreservesColumnAssociation) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		std::string headers;
		std::vector<std::string> expectedHeaders;
		constexpr size_t columns = 50U;
		for (size_t index = 0U; index < columns; ++index) {
			const std::string name = "k" + std::to_string(columns - index);
			if (index != 0U) headers += ',';
			headers += name;
			expectedHeaders.push_back(name);
		}
		size_t headerBytes = expectedHeaders.capacity() * sizeof(std::string);
		for (const auto &header: expectedHeaders) {
			if (header.capacity() > sizeof(std::string)) headerBytes += header.capacity() + 1U;
		}
		std::vector<std::string_view> expectedValidationSlots;
		expectedValidationSlots.reserve(columns);
		const fs::path csv = writeFile("headers-only.csv", headers + "\n");
		QueryPlan plan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(csv) + "' AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		auto constrained = bounds();
		constrained.maximum_materialized_bytes = 2U * sizeof(LogicalPlanReadRow) + headerBytes +
				expectedValidationSlots.capacity() * sizeof(std::string_view) - 1U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained, context())), std::runtime_error);
		constrained.maximum_materialized_bytes *= 4U;
		EXPECT_EQ(runtime.executeToQueryResult(*plan.root, constrained, context()).rowCount(), 0U);

		const fs::path ordered = writeFile("unsorted-headers.csv", "z,a,m\nlast,first,middle\n");
		QueryPlan orderedPlan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(ordered) + "' AS row RETURN row");
		const auto result = runtime.executeToQueryResult(*orderedPlan.root, bounds(), context());
		ASSERT_EQ(result.rowCount(), 1U);
		const auto value = result.getRows().front().at("row").asPrimitive();
		EXPECT_EQ(value.getMap().at("z"), PropertyValue("last"));
		EXPECT_EQ(value.getMap().at("a"), PropertyValue("first"));
		EXPECT_EQ(value.getMap().at("m"), PropertyValue("middle"));
		const fs::path duplicate = writeFile("separated-duplicate.csv", "z,a,z\n");
		QueryPlan duplicatePlan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(duplicate) + "' AS row RETURN row");
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*duplicatePlan.root, bounds(), context())), std::runtime_error);
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, PreflightsListAndMapSlotsBeforeConstructingWideCsvRows) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		for (const bool withHeaders: {false, true}) {
			SCOPED_TRACE(withHeaders);
			const size_t columns = withHeaders ? 50U : 80U;
			std::string headers, record;
			for (size_t index = 0U; index < columns; ++index) {
				if (index != 0U) { headers += ','; record += ','; }
				headers += "k" + std::to_string(index);
				record += 'a';
			}
			const fs::path csv = writeFile(withHeaders ? "wide-map.csv" : "wide-list.csv",
					(withHeaders ? headers + "\n" : "") + record + "\n");
			QueryPlan plan = parse(std::string("LOAD CSV ") + (withHeaders ? "WITH HEADERS " : "") +
					"FROM '" + fileUri(csv) + "' AS row RETURN row");
			ASSERT_NE(plan.root, nullptr);
			auto constrained = bounds();
			// String slots, vector growth and retained headers vary by standard
			// library. Locate the conversion boundary without fixing a platform's
			// parser allowance; reaching this exact rejection remains mandatory.
			bool conversionRejected = false;
			for (size_t budget = columns * sizeof(std::string); budget < 20000U; budget += 128U) {
				constrained.maximum_materialized_bytes = budget;
				size_t delivered = 0U;
				try {
					static_cast<void>(runtime.execute(*plan.root, constrained, [&](const LogicalPlanReadRow &) {
						++delivered;
						return true;
					}, context()));
					break;
				} catch (const std::runtime_error &error) {
					EXPECT_EQ(delivered, 0U);
					if (std::string_view(error.what()).find("logical LOAD CSV conversion") != std::string_view::npos) {
						conversionRejected = true;
						break;
					}
				}
			}
			EXPECT_TRUE(conversionRejected) << "no conversion rejection before successful row delivery";
			constrained.maximum_materialized_bytes = 20000U;
			const auto result = runtime.executeToQueryResult(*plan.root, constrained, context());
			ASSERT_EQ(result.rowCount(), 1U);
			const auto value = result.getRows().front().at("row").asPrimitive();
			if (withHeaders) EXPECT_EQ(value.getMap().size(), columns);
			else EXPECT_EQ(value.getList().size(), columns);
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, ChargesHeaderMapLinksAndBucketsBeforeConstructingRows) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		std::string headers, record;
		std::vector<std::string> names;
		for (size_t index = 0U; index < 10U; ++index) {
			if (index != 0U) { headers += ','; record += ','; }
			names.push_back("k" + std::to_string(index));
			headers += names.back();
			record += 'a';
		}
		const fs::path csv = writeFile("map-structure.csv", headers + "\n" + record + "\n");
		QueryPlan plan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(csv) + "' AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		auto constrained = bounds();
		size_t headerBytes = 0U;
		size_t constructionWithoutMapStructure = sizeof(PropertyValue) +
			names.size() * sizeof(PropertyValue::MapType::value_type);
		for (const std::string &name: names) {
			headerBytes += sizeof(std::string) + name.size();
			constructionWithoutMapStructure += sizeof(std::string) + 1U + name.size();
		}
		const size_t mapStructureBytes = names.size() * 2U * sizeof(void *);
		// The old preflight admits this record; the structure-aware preflight
		// rejects it. Calculate the boundary from this platform's object sizes.
		constrained.maximum_materialized_bytes = sizeof(LogicalPlanReadRow) + headerBytes +
			constructionWithoutMapStructure + mapStructureBytes - 1U;
		size_t delivered = 0U;
		try {
			static_cast<void>(runtime.execute(*plan.root, constrained, [&](const LogicalPlanReadRow &) {
				++delivered;
				return true;
			}, context()));
			FAIL() << "Map structure should exceed the conversion allowance";
		} catch (const std::runtime_error &error) {
			EXPECT_NE(std::string(error.what()).find("logical LOAD CSV conversion"), std::string::npos);
		}
		EXPECT_EQ(delivered, 0U);
		constrained.maximum_materialized_bytes *= 4U;
		const QueryResult result = runtime.executeToQueryResult(*plan.root, constrained, context());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("row").asPrimitive().getMap().size(), 10U);
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, SortAndCsvRetainedHeadersShareTheExecutionBudget) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string key(2000U, 'h');
		const fs::path csv = writeFile("sort-shared.csv", key + "\nx\nw\nv\nu\nt\ns\nr\nq\np\no\n");
		const std::string query = "LOAD CSV WITH HEADERS FROM '" + fileUri(csv) +
				"' AS row WITH row." + key + " AS value RETURN value";
		QueryPlan streaming = parse(query);
		QueryPlan sorted = parse(query + " ORDER BY value");
		ASSERT_NE(streaming.root, nullptr);
		ASSERT_NE(sorted.root, nullptr);
		auto constrained = bounds();
		constrained.maximum_materialized_bytes = 8000U;
		size_t delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*streaming.root, constrained,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context())));
		EXPECT_EQ(delivered, 10U);
		// The same scalar rows fit Sort by themselves; only simultaneous CSV
		// headers/input and the buffered rows make this pipeline exceed the cap.
		QueryPlan scalarOnly = parse("UNWIND ['x','w','v','u','t','s','r','q','p','o'] AS value RETURN value ORDER BY value");
		delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*scalarOnly.root, constrained,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context())));
		EXPECT_EQ(delivered, 10U);
		delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*sorted.root, constrained,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context())), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		constrained.maximum_materialized_bytes = 16000U;
		const auto result = runtime.executeToQueryResult(*sorted.root, constrained, context());
		ASSERT_EQ(result.rowCount(), 10U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("value").asPrimitive(), PropertyValue(std::string(1U, static_cast<char>('o' + index))));
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, OutputBatchSharesCsvSourceBudgetWhileReadingAhead) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string field(100U, 'v');
		std::string content;
		for (size_t index = 0U; index < 10U; ++index) content += field + "\n";
		const fs::path csv = writeFile("output-batch-budget.csv", content);
		QueryPlan plan = parse("LOAD CSV FROM '" + fileUri(csv) + "' AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		auto constrained = bounds();
		constrained.maximum_materialized_bytes = 4000U;
		size_t streamed = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*plan.root, constrained,
			[&](const LogicalPlanReadRow &) { ++streamed; return true; }, context())));
		EXPECT_EQ(streamed, 10U);
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained, context())),
			std::runtime_error);
		constrained.maximum_materialized_bytes = 12000U;
		const QueryResult result = runtime.executeToQueryResult(*plan.root, constrained, context());
		ASSERT_EQ(result.rowCount(), 10U);
		for (const auto &row: result.getRows()) {
			EXPECT_EQ(row.at("row").asPrimitive().getList().front(), PropertyValue(field));
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, TransfersAnUnwindRowReservationIntoLoadCsvWithoutChargingItTwice) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string retained(10'000U, 'r');
		const fs::path csv = writeFile("unwind-shared.csv", "value\nx\n");
		auto input = std::make_unique<logical::LogicalUnwind>(std::make_unique<logical::LogicalSingleRow>(),
				"input", std::make_shared<expressions::ParameterExpression>("inputs"));
		auto source = std::make_unique<logical::LogicalLoadCsv>(std::move(input),
				std::make_shared<expressions::ParameterExpression>("uri"), "row", true);
		QueryContext queryContext = context();
		queryContext.parameters.emplace("inputs", PropertyValue(std::vector<PropertyValue>{PropertyValue(retained)}));
		queryContext.parameters.emplace("uri", PropertyValue(fileUri(csv)));
		auto constrained = bounds();
		constrained.maximum_materialized_bytes = 23'000U;
		size_t delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*source, constrained, [&](const LogicalPlanReadRow &row) {
			++delivered;
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("input")), PropertyValue(retained));
			EXPECT_EQ(std::get<PropertyValue>(row.values.at("row")).getMap().at("value"), PropertyValue("x"));
			return true;
		}, queryContext)));
		EXPECT_EQ(delivered, 1U);
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, CountsOverlappingProjectInputAndOutputBeforeLoadCsv) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string retained(10'000U, 'r');
		const fs::path csv = writeFile("project-shared.csv", "value\nx\n");
		QueryPlan plan = parse("UNWIND $inputs AS input WITH input AS carried "
				"LOAD CSV WITH HEADERS FROM $uri AS row RETURN carried, row");
		ASSERT_NE(plan.root, nullptr);
		QueryContext queryContext = context();
		queryContext.parameters.emplace("inputs", PropertyValue(std::vector<PropertyValue>{PropertyValue(retained)}));
		queryContext.parameters.emplace("uri", PropertyValue(fileUri(csv)));
		auto constrained = bounds();
		constrained.maximum_materialized_bytes = 23'000U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained, queryContext)),
			std::runtime_error);
		constrained.maximum_materialized_bytes = 36'000U;
		const QueryResult result = runtime.executeToQueryResult(*plan.root, constrained, queryContext);
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("carried").asPrimitive(), PropertyValue(retained));
		EXPECT_EQ(result.getRows().front().at("row").asPrimitive().getMap().at("value"), PropertyValue("x"));
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, NestedSortTransfersRowsWithoutDuplicatingSharedCharges) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("UNWIND ['d','b','a','c'] AS value WITH value ORDER BY value RETURN value ORDER BY 0");
		ASSERT_NE(plan.root, nullptr);
		auto constrained = bounds();
		// Leave room for the old and new QueryResult row-vector allocations to
		// coexist during growth while remaining below the duplicated-row-charge case.
		constrained.maximum_materialized_bytes =
				5U * sizeof(LogicalPlanReadRow) + 8U * sizeof(PropertyValue) + 664U;
		EXPECT_LT(constrained.maximum_materialized_bytes,
				8U * (sizeof(LogicalPlanReadRow) + sizeof(LogicalPlanReadValue) + 6U));
		const auto result = runtime.executeToQueryResult(*plan.root, constrained, context());
		ASSERT_EQ(result.rowCount(), 4U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("value").asPrimitive(), PropertyValue(std::string(1U, static_cast<char>('a' + index))));
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, CsvHeadersAndStreamingWindowsShareTheExecutionBudget) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string key(2000U, 'h');
		const fs::path csv = writeFile("pagination-shared.csv", key + "\nx\nw\nv\nu\nt\ns\nr\nq\np\no\n\"unterminated\n");
		for (const bool withSkip: {false, true}) {
			SCOPED_TRACE(withSkip);
			QueryPlan plan = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(csv) +
					"' AS row WITH row." + key + " AS value RETURN value" +
					(withSkip ? " SKIP 8 LIMIT 2" : " LIMIT 10"));
			ASSERT_NE(plan.root, nullptr);
			auto constrained = bounds();
			constrained.maximum_materialized_bytes = 2000U;
			size_t delivered = 0U;
			std::vector<PropertyValue> streamed;
			const auto stream = [&] {
				static_cast<void>(runtime.execute(*plan.root, constrained,
					[&](const LogicalPlanReadRow &row) {
						++delivered;
						streamed.push_back(std::get<PropertyValue>(row.values.at("value")));
						return true;
					}, context()));
			};
			EXPECT_THROW(stream(), std::runtime_error);
			EXPECT_EQ(delivered, 0U);
			constrained.maximum_materialized_bytes = 9000U;
			// Neither window retains a pagination batch; only the header, source
			// conversion and rows admitted by downstream consumers share the cap.
			EXPECT_NO_THROW(stream());
			EXPECT_EQ(delivered, withSkip ? 2U : 10U);
			ASSERT_EQ(streamed.size(), delivered);
			for (size_t index = 0U; index < streamed.size(); ++index) {
				EXPECT_EQ(streamed[index],
					PropertyValue(std::string(1U, static_cast<char>((withSkip ? 'p' : 'x') - index))));
			}
			constrained.maximum_materialized_bytes = 16000U;
			const auto result = runtime.executeToQueryResult(*plan.root, constrained, context());
			ASSERT_EQ(result.rowCount(), withSkip ? 2U : 10U);
			for (size_t index = 0U; index < result.rowCount(); ++index) {
				EXPECT_EQ(result.getRows()[index].at("value").asPrimitive(),
						PropertyValue(std::string(1U, static_cast<char>((withSkip ? 'p' : 'x') - index))));
			}
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, AggregateAndCsvRetainedHeadersShareTheExecutionBudget) {
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const std::string key(2000U, 'h');
		const fs::path csv = writeFile("aggregate-shared.csv", key + "\nx\nw\nv\nu\nt\ns\nr\nq\np\no\n");
		const std::string query = "LOAD CSV WITH HEADERS FROM '" + fileUri(csv) +
				"' AS row WITH row." + key + " AS value RETURN value, count(*) AS count";
		QueryPlan source = parse(query);
		QueryPlan independent = parse("UNWIND ['x','w','v','u','t','s','r','q','p','o'] AS value RETURN value, count(*) AS count");
		auto constrained = bounds();
		constrained.maximum_materialized_bytes = 5000U;
		size_t delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*independent.root, constrained,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context())));
		EXPECT_EQ(delivered, 10U);
		delivered = 0U;
		EXPECT_THROW(static_cast<void>(runtime.execute(*source.root, constrained,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context())), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		constrained.maximum_materialized_bytes = 16000U;
		QueryPlan sorted = parse(query + " ORDER BY value");
		const auto result = runtime.executeToQueryResult(*sorted.root, constrained, context());
		ASSERT_EQ(result.rowCount(), 10U);
		for (size_t index = 0U; index < result.rowCount(); ++index) {
			EXPECT_EQ(result.getRows()[index].at("value").asPrimitive(), PropertyValue(std::string(1U, static_cast<char>('o' + index))));
			EXPECT_EQ(result.getRows()[index].at("count").asPrimitive(), PropertyValue(1));
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, PropertyPrefetchAndLiveCsvHeadersShareTheExecutionBudget) {
		using namespace direct_edge_test;
		DirectEdgeGraph graph = directGraph();
		graph.enable_property_tokens = true;
		graph.nodes.erase(2U);
		graph.nodes.erase(3U);
		const PropertyValue payload(std::string(6000U, 'p'));
		graph.nodes.at(1U).properties.emplace_back("payload", payload);
		DirectEdgeBackend backend(graph);
		LogicalPlanReadRuntime runtime(backend);
		const std::string key(2000U, 'h');
		const fs::path csv = writeFile("prefetch-shared.csv", key + "\nx\n");
		const auto makeProject = [](std::unique_ptr<logical::LogicalOperator> child) {
			std::vector<logical::LogicalProjectItem> items;
			items.emplace_back(std::make_shared<expressions::VariableReferenceExpression>("n", "payload"), "payload");
			return std::make_unique<logical::LogicalProject>(std::move(child), std::move(items));
		};
		auto independent = makeProject(std::make_unique<logical::LogicalNodeScan>("n"));
		auto combined = makeProject(std::make_unique<logical::LogicalLoadCsv>(
				std::make_unique<logical::LogicalNodeScan>("n"),
				std::make_shared<expressions::LiteralExpression>(fileUri(csv)), "row", true));
		auto constrained = bounds();
		// Flush within the CSV callback, while its input/header owners are live.
		constrained.maximum_rows = 1U;
		// The execution retains one output row slot while CSV headers and selected
		// Property values coexist, so include that storage in the same allowance.
		constrained.maximum_materialized_bytes = 10000U + sizeof(LogicalPlanReadRow);
		size_t delivered = 0U;
		EXPECT_NO_THROW(static_cast<void>(runtime.execute(*independent, constrained,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; })));
		EXPECT_EQ(delivered, 1U);
		QueryPlan csvOnly = parse("LOAD CSV WITH HEADERS FROM '" + fileUri(csv) + "' AS row RETURN row." + key + " AS value");
		EXPECT_EQ(runtime.executeToQueryResult(*csvOnly.root, constrained, context()).rowCount(), 1U);
		delivered = 0U;
		const size_t readsBeforeCombined = graph.node_property_column_batch_reads;
		EXPECT_THROW(static_cast<void>(runtime.execute(*combined, constrained,
				[&](const LogicalPlanReadRow &) { ++delivered; return true; }, context())), std::runtime_error);
		EXPECT_EQ(delivered, 0U);
		EXPECT_EQ(graph.node_property_column_batch_reads, readsBeforeCombined + 1U);
		constrained.maximum_materialized_bytes = 24000U;
		const auto result = runtime.executeToQueryResult(*combined, constrained, context());
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows()[0].at("payload").asPrimitive(), payload);
		EXPECT_EQ(graph.node_property_point_reads, 0U);
		EXPECT_TRUE(graph.node_read_ids.empty());
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, BoundsMaterializedCsvRowsIndependentlyFromParserLimits) {
		const fs::path csv = writeFile("large-row.csv", "value\n" + std::string(256U, 'x') + "\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		QueryPlan plan = parse("LOAD CSV FROM '" + fileUri(csv) + "' AS row RETURN row");
		ASSERT_NE(plan.root, nullptr);
		QueryContext queryContext = context();
		queryContext.csv_import_policy.maximum_field_bytes = 1024U;
		LogicalPlanReadRuntimeBounds constrained = bounds();
		constrained.maximum_materialized_bytes = 64U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(*plan.root, constrained, queryContext)),
				 std::runtime_error);
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, RejectsMalformedLogicalLoadCsvContractsAndRowVariableRebinding) {
		const fs::path csv = writeFile("contract.csv", "value\nentry\n");
		CsvOnlyBackend backend;
		LogicalPlanReadRuntime runtime(backend);
		const auto uri = std::make_shared<expressions::LiteralExpression>(fileUri(csv));

		logical::LogicalLoadCsv missingUrl(nullptr, {}, "row");
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(missingUrl, bounds(), context())), std::invalid_argument);

		logical::LogicalLoadCsv missingRow(nullptr, uri, "");
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(missingRow, bounds(), context())), std::invalid_argument);

		logical::LogicalLoadCsv noContext(nullptr, uri, "row");
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(noContext, bounds())), std::runtime_error);

		for (const std::string &delimiter: {"", "\n", "\"", "123456789"}) {
			logical::LogicalLoadCsv invalidDelimiter(nullptr, uri, "row", false, delimiter);
			EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(invalidDelimiter, bounds(), context())),
						 std::invalid_argument);
		}

		logical::LogicalLoadCsv bareInput(nullptr, uri, "row");
		const QueryResult bareResult = runtime.executeToQueryResult(bareInput, bounds(), context());
		ASSERT_EQ(bareResult.rowCount(), 2U);
		EXPECT_EQ(bareResult.getRows().front().at("row").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("value")}));
		EXPECT_EQ(bareResult.getRows().back().at("row").asPrimitive(),
				  PropertyValue(std::vector<PropertyValue>{PropertyValue("entry")}));

		std::vector<logical::LogicalProjectItem> items;
		items.emplace_back(std::make_shared<expressions::LiteralExpression>("already-bound"), "row");
		auto input = std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalSingleRow>(), std::move(items));
		logical::LogicalLoadCsv rebindsRow(std::move(input), uri, "row");
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(rebindsRow, bounds(), context())), std::runtime_error);

		// A value-equal existing binding is still an illegal LOAD CSV alias.
		// The generic bindValue helper permits equal values for path matching.
		const fs::path single = writeFile("equal-rebind.csv", "value\n");
		std::vector<logical::LogicalProjectItem> equalItems;
		equalItems.emplace_back(std::make_shared<expressions::LiteralExpression>(
			PropertyValue(std::vector<PropertyValue>{PropertyValue("value")})), "row");
		auto equalInput = std::make_unique<logical::LogicalProject>(
			std::make_unique<logical::LogicalSingleRow>(), std::move(equalItems));
		logical::LogicalLoadCsv equalRebind(std::move(equalInput),
			std::make_shared<expressions::LiteralExpression>(fileUri(single)), "row");
		try {
			static_cast<void>(runtime.executeToQueryResult(equalRebind, bounds(), context()));
			FAIL() << "LOAD CSV must reject an existing alias even when the values match";
		} catch (const std::runtime_error &error) {
			EXPECT_STREQ(error.what(), "logical LOAD CSV attempted to rebind its row variable");
		}
	}

	TEST_F(LogicalPlanLoadCsvRuntimeTest, WideHeaderConversionRejectsSmallBudgetAndRetriesCompleteMap) {
		std::string headers, fields;
		for (size_t index = 0U; index < 512U; ++index) {
			if (index != 0U) { headers += ','; fields += ','; }
			headers += "column" + std::to_string(index); fields += std::string(40U, 'v');
		}
		const fs::path csv = writeFile("wide-conversion.csv", headers + "\n" + fields + "\n");
		CsvOnlyBackend backend; LogicalPlanReadRuntime runtime(backend);
		logical::LogicalLoadCsv source(std::make_unique<logical::LogicalSingleRow>(),
			std::make_shared<expressions::LiteralExpression>(fileUri(csv)), "row", true);
		auto constrained = bounds(); constrained.maximum_materialized_bytes = 16384U;
		EXPECT_THROW(static_cast<void>(runtime.executeToQueryResult(source, constrained, context())), std::runtime_error);
		constrained.maximum_materialized_bytes = 2U * 1024U * 1024U;
		const auto result = runtime.executeToQueryResult(source, constrained, context());
		ASSERT_EQ(result.rowCount(), 1U);
		const auto &map = result.getRows()[0].at("row").asPrimitive().getMap();
		ASSERT_EQ(map.size(), 512U);
		for (size_t index = 0U; index < 512U; ++index)
			EXPECT_EQ(map.at("column" + std::to_string(index)), PropertyValue(std::string(40U, 'v')));
	}

} // namespace graph::query::execution
