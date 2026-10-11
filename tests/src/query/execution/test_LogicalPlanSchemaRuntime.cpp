/**
 * @file test_LogicalPlanSchemaRuntime.cpp
 * @brief Tests for the fail-closed logical schema runtime.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <memory>
#include <chrono>
#include <thread>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/LogicalPlanSchemaRuntime.hpp"
#include "graph/query/logical/operators/LogicalCreateConstraint.hpp"
#include "graph/query/logical/operators/LogicalCreateIndex.hpp"
#include "graph/query/logical/operators/LogicalCreateVectorIndex.hpp"
#include "graph/query/logical/operators/LogicalDropConstraint.hpp"
#include "graph/query/logical/operators/LogicalDropIndex.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalShowConstraints.hpp"
#include "graph/query/logical/operators/LogicalShowIndexes.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"

namespace graph::query::execution {

	namespace {

		class SchemaBackend final : public storage::LogicalGraphReadBackend, public storage::LogicalGraphSchemaBackend {
		public:
			std::function<void()> construction_hook;
			storage::LogicalGraphIndexDefinition createLogicalNodeIndexWithCheckpoint(
				const storage::LogicalGraphNodeIndexCreateRequest &request, const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) override {
				if (construction_hook) construction_hook();
				if (checkpoint) checkpoint();
				return createLogicalNodeIndex(request);
			}
			storage::LogicalGraphIndexDefinition createLogicalNodeVectorIndexWithCheckpoint(
				const storage::LogicalGraphNodeVectorIndexCreateRequest &request, const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) override {
				if (construction_hook) construction_hook();
				if (checkpoint) checkpoint();
				return createLogicalNodeVectorIndex(request);
			}
			storage::LogicalGraphConstraintDefinition createLogicalConstraintWithCheckpoint(
				const storage::LogicalGraphConstraintCreateRequest &request, const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) override {
				if (construction_hook) construction_hook();
				if (checkpoint) checkpoint();
				return createLogicalConstraint(request);
			}
			[[nodiscard]] std::unique_ptr<storage::LogicalGraphReadSnapshot>
			openLogicalGraphReadSnapshot() const override {
				throw std::logic_error("schema test does not execute graph reads");
			}

			[[nodiscard]] storage::LogicalGraphIndexDefinition
			createLogicalNodeIndex(const storage::LogicalGraphNodeIndexCreateRequest &request) override {
				++calls;
				create_requests.push_back(request);
				storage::LogicalGraphIndexDefinition definition;
				definition.index_id = next_id++;
				definition.name = request.name.empty() ? "generated" : request.name;
				definition.kind = request.property_keys.size() == 1U ? storage::LogicalGraphIndexKind::LGIK_PROPERTY
																	 : storage::LogicalGraphIndexKind::LGIK_COMPOSITE;
				definition.label = request.label;
				definition.property_keys = request.property_keys;
				if (property_result_override.has_value()) {
					definition = *property_result_override;
				}
				definitions.push_back(definition);
				return definition;
			}

			[[nodiscard]] std::vector<storage::LogicalGraphIndexDefinition>
			createLogicalNodeIndexes(std::span<const storage::LogicalGraphNodeIndexCreateRequest> requests) override {
				std::vector<storage::LogicalGraphIndexDefinition> definitions;
				definitions.reserve(requests.size());
				for (const storage::LogicalGraphNodeIndexCreateRequest &request: requests) {
					definitions.push_back(createLogicalNodeIndex(request));
				}
				return definitions;
			}

			[[nodiscard]] storage::LogicalGraphIndexDefinition
			createLogicalNodeVectorIndex(const storage::LogicalGraphNodeVectorIndexCreateRequest &request) override {
				++calls;
				vector_create_requests.push_back(request);
				storage::LogicalGraphIndexDefinition definition;
				definition.index_id = next_id++;
				definition.name = request.name.empty() ? "generated_vector" : request.name;
				definition.kind = storage::LogicalGraphIndexKind::LGIK_VECTOR;
				definition.label = request.label;
				definition.property_keys = {request.property_key};
				definition.vector_dimensions = request.dimensions;
				definition.vector_metric = request.metric;
				if (vector_result_override.has_value()) {
					definition = *vector_result_override;
				}
				definitions.push_back(definition);
				return definition;
			}

			[[nodiscard]] bool dropLogicalIndexByName(std::string_view name) override {
				++calls;
				dropped_name = std::string(name);
				return drop_by_name_result;
			}

			[[nodiscard]] bool dropLogicalNodeIndexBySchema(std::string_view label,
														std::string_view propertyKey) override {
				++calls;
				dropped_label = std::string(label);
				dropped_property = std::string(propertyKey);
				return drop_by_schema_result;
			}

			[[nodiscard]] std::vector<storage::LogicalGraphIndexDefinition>
			listLogicalIndexes(size_t maximumIndexes) const override {
				++calls;
				if (maximumIndexes < definitions.size()) {
					throw std::runtime_error("test schema list bound is too small");
				}
				return definitions;
			}

			[[nodiscard]] storage::LogicalGraphConstraintDefinition
			createLogicalConstraint(const storage::LogicalGraphConstraintCreateRequest &request) override {
				++calls;
				constraint_create_requests.push_back(request);
				storage::LogicalGraphConstraintDefinition definition;
				definition.constraint_id = next_constraint_id++;
				definition.name = request.name;
				definition.entity_kind = request.entity_kind;
				definition.kind = request.kind;
				definition.label = request.label;
				definition.property_keys = request.property_keys;
				definition.property_type = request.property_type;
				if (constraint_result_override.has_value()) {
					definition = *constraint_result_override;
				}
				constraint_definitions.push_back(definition);
				return definition;
			}

			[[nodiscard]] bool dropLogicalConstraintByName(std::string_view name) override {
				++calls;
				dropped_constraint_name = std::string(name);
				return drop_constraint_result;
			}

			[[nodiscard]] std::vector<storage::LogicalGraphConstraintDefinition>
			listLogicalConstraints(size_t maximumConstraints) const override {
				++calls;
				if (maximumConstraints < constraint_definitions.size()) {
					throw std::runtime_error("test constraint list bound is too small");
				}
				return constraint_definitions;
			}

			uint64_t next_id = 1U;
			mutable size_t calls = 0U;
			uint64_t next_constraint_id = 1U;
			bool drop_by_name_result = true;
			bool drop_by_schema_result = true;
			std::vector<storage::LogicalGraphNodeIndexCreateRequest> create_requests;
			std::vector<storage::LogicalGraphNodeVectorIndexCreateRequest> vector_create_requests;
			std::vector<storage::LogicalGraphIndexDefinition> definitions;
			std::vector<storage::LogicalGraphConstraintCreateRequest> constraint_create_requests;
			std::vector<storage::LogicalGraphConstraintDefinition> constraint_definitions;
			std::optional<storage::LogicalGraphIndexDefinition> property_result_override;
			std::optional<storage::LogicalGraphIndexDefinition> vector_result_override;
			std::optional<storage::LogicalGraphConstraintDefinition> constraint_result_override;
			std::string dropped_name;
			std::string dropped_label;
			std::string dropped_property;
			bool drop_constraint_result = true;
			std::string dropped_constraint_name;
		};

	} // namespace

	TEST(LogicalPlanSchemaRuntimeTest, CreationForwardsGuardIntoBackendConstructionBeforePublication) {
		logical::LogicalCreateIndex scalar("name", "Person", "name");
		logical::LogicalCreateVectorIndex vector("embedding", "Person", "embedding", 3, "l2");
		logical::LogicalCreateConstraint constraint("required", "node", "not_null", "Person", {"name"}, "");
		for (const logical::LogicalOperator *operation: std::vector<const logical::LogicalOperator *>{&scalar, &vector, &constraint}) {
			SchemaBackend backend;
			LogicalPlanSchemaRuntime runtime(backend);
			size_t resident = 0U;
			QueryGuard guard(0, 1, [&] { return resident; });
			QueryContext context; context.guard = &guard;
			backend.construction_hook = [&] { resident = 2U * 1024U * 1024U; };
			EXPECT_THROW(static_cast<void>(runtime.execute(*operation, 8U, context)), QueryMemoryExceededException);
			EXPECT_EQ(backend.calls, 0U);
			EXPECT_TRUE(backend.definitions.empty());
			EXPECT_TRUE(backend.constraint_definitions.empty());
			backend.construction_hook = {}; resident = 0U;
			EXPECT_NO_THROW(static_cast<void>(runtime.execute(*operation, 8U, context)));
			EXPECT_EQ(backend.calls, 1U);
		}
	}

	TEST(LogicalPlanSchemaRuntimeTest, EntryChecksExpiredAndMemoryGuardsBeforeEverySchemaBackendOperation) {
		std::vector<std::unique_ptr<logical::LogicalOperator>> operations;
		operations.push_back(std::make_unique<logical::LogicalCreateIndex>("name", "Person", "name"));
		operations.push_back(std::make_unique<logical::LogicalCreateVectorIndex>("embedding", "Person", "embedding", 3, "l2"));
		operations.push_back(std::make_unique<logical::LogicalCreateConstraint>("required", "node", "not_null", "Person", std::vector<std::string>{"name"}, ""));
		operations.push_back(std::make_unique<logical::LogicalDropIndex>("name", "", ""));
		operations.push_back(std::make_unique<logical::LogicalDropIndex>("", "Person", "name"));
		operations.push_back(std::make_unique<logical::LogicalDropConstraint>("required", false));
		operations.push_back(std::make_unique<logical::LogicalShowIndexes>());
		operations.push_back(std::make_unique<logical::LogicalShowConstraints>());
		for (const auto &operation: operations) {
			SCOPED_TRACE(operation->toString());
			SchemaBackend backend;
			LogicalPlanSchemaRuntime runtime(backend);
			QueryGuard expired(1, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			QueryContext context; context.guard = &expired;
			EXPECT_THROW(static_cast<void>(runtime.execute(*operation, 8U, context)), QueryTimeoutException);
			EXPECT_EQ(backend.calls, 0U);
			size_t resident = 0U;
			QueryGuard memory(0, 1, [&] { return resident; });
			context.guard = &memory; resident = 2U * 1024U * 1024U;
			EXPECT_THROW(static_cast<void>(runtime.execute(*operation, 8U, context)), QueryMemoryExceededException);
			EXPECT_EQ(backend.calls, 0U);
			resident = 0U;
			for (size_t retry = 0U; retry < 2U; ++retry) {
				EXPECT_NO_THROW(static_cast<void>(runtime.execute(*operation, 8U, context)));
				EXPECT_EQ(backend.calls, retry + 1U);
			}
		}
	}

	TEST(LogicalPlanSchemaRuntimeTest, CreatesIndexesAndReturnsTheLegacyCompatibleStatusRow) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalCreateIndex create("person_name", "Person", "name");

		const QueryResult result = runtime.execute(create, 8U, QueryContext{});
		ASSERT_EQ(backend.create_requests.size(), 1U);
		EXPECT_EQ(backend.create_requests.front(),
				  (storage::LogicalGraphNodeIndexCreateRequest{"person_name", "Person", {"name"}}));
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("result").toString(), "Index created");
	}

	TEST(LogicalPlanSchemaRuntimeTest, CreatesVectorIndexesThroughTheDedicatedSchemaBoundary) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalCreateVectorIndex create("person_embedding", "Person", "embedding", 3, "cosine");

		const QueryResult result = runtime.execute(create, 8U, QueryContext{});
		ASSERT_EQ(backend.vector_create_requests.size(), 1U);
		EXPECT_EQ(backend.vector_create_requests.front(),
				  (storage::LogicalGraphNodeVectorIndexCreateRequest{"person_embedding", "Person", "embedding", 3U,
																	 "cosine"}));
		ASSERT_EQ(result.rowCount(), 1U);
		EXPECT_EQ(result.getRows().front().at("result").toString(), "Index created");
	}

	TEST(LogicalPlanSchemaRuntimeTest, AllowsBackendGeneratedVectorIndexNames) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalCreateVectorIndex create("", "Person", "embedding", 3, "l2");

		const QueryResult result = runtime.execute(create, 8U, QueryContext{});
		ASSERT_EQ(backend.vector_create_requests.size(), 1U);
		EXPECT_TRUE(backend.vector_create_requests.front().name.empty());
		ASSERT_EQ(result.rowCount(), 1U);
	}

	TEST(LogicalPlanSchemaRuntimeTest, DropsByNameOrSchemaAndPreservesNoOpDropBehavior) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalDropIndex byName("person_name", "", "");
		const QueryResult byNameResult = runtime.execute(byName, 8U, QueryContext{});
		EXPECT_EQ(backend.dropped_name, "person_name");
		ASSERT_EQ(byNameResult.rowCount(), 1U);
		EXPECT_EQ(byNameResult.getRows().front().at("result").toString(), "Index dropped");

		logical::LogicalDropIndex bySchema("", "Person", "name");
		const QueryResult bySchemaResult = runtime.execute(bySchema, 8U, QueryContext{});
		EXPECT_EQ(backend.dropped_label, "Person");
		EXPECT_EQ(backend.dropped_property, "name");
		ASSERT_EQ(bySchemaResult.rowCount(), 1U);

		backend.drop_by_name_result = false;
		EXPECT_TRUE(runtime.execute(byName, 8U, QueryContext{}).isEmpty());
	}

	TEST(LogicalPlanSchemaRuntimeTest, ShowsTypedDefinitionsAndRejectsUnsupportedOperators) {
		SchemaBackend backend;
		backend.definitions = {
				{1U, "person_name", storage::LogicalGraphIndexKind::LGIK_PROPERTY, "Person", {"name"}, 0U, {}},
				{2U,
				 "person_location",
				 storage::LogicalGraphIndexKind::LGIK_COMPOSITE,
				 "Person",
				 {"city", "zip"},
				 0U,
				 {}},
				{3U,
				 "person_embedding",
				 storage::LogicalGraphIndexKind::LGIK_VECTOR,
				 "Person",
				 {"embedding"},
				 3U,
				 "l2"}};
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalShowIndexes show;

		const QueryResult result = runtime.execute(show, 8U, QueryContext{});
		ASSERT_EQ(result.rowCount(), 3U);
		EXPECT_EQ(result.getRows()[0].at("type").toString(), "property");
		EXPECT_EQ(result.getRows()[1].at("properties").toString(), "city,zip");
		EXPECT_EQ(result.getRows()[2].at("type").toString(), "vector");

		logical::LogicalNodeScan unsupported("n");
		EXPECT_THROW(static_cast<void>(runtime.execute(unsupported, 8U, QueryContext{})), std::runtime_error);
	}

	TEST(LogicalPlanSchemaRuntimeTest, QueryEngineUsesTheSchemaCapabilityWithoutLegacyFallback) {
		auto backend = std::make_shared<SchemaBackend>();
		std::shared_ptr<storage::LogicalGraphReadBackend> read = backend;
		QueryEngine engine(std::move(read));

		const QueryResult create = engine.execute("CREATE INDEX person_name FOR (n:Person) ON (n.name)");
		ASSERT_EQ(create.rowCount(), 1U);
		EXPECT_EQ(backend->create_requests.front().label, "Person");

		const QueryResult show = engine.execute("SHOW INDEXES");
		ASSERT_EQ(show.rowCount(), 1U);
		EXPECT_EQ(show.getRows().front().at("name").toString(), "person_name");

		const QueryResult drop = engine.execute("DROP INDEX person_name");
		ASSERT_EQ(drop.rowCount(), 1U);
		EXPECT_EQ(backend->dropped_name, "person_name");

		const QueryResult vectorCreate = engine.execute(
				"CREATE VECTOR INDEX ON :Person(embedding) OPTIONS {dimension: 3, metric: 'INNER_PRODUCT'}");
		ASSERT_EQ(vectorCreate.rowCount(), 1U);
		ASSERT_EQ(backend->vector_create_requests.size(), 1U);
		EXPECT_TRUE(backend->vector_create_requests.front().name.empty());
		EXPECT_EQ(backend->vector_create_requests.front().label, "Person");
		EXPECT_EQ(backend->vector_create_requests.front().property_key, "embedding");
		EXPECT_EQ(backend->vector_create_requests.front().dimensions, 3U);
		EXPECT_EQ(backend->vector_create_requests.front().metric, "INNER_PRODUCT");

		const QueryResult constraintCreate =
				engine.execute("CREATE CONSTRAINT unique_person_name FOR (n:Person) REQUIRE n.name IS UNIQUE");
		ASSERT_EQ(constraintCreate.rowCount(), 1U);
		ASSERT_EQ(backend->constraint_create_requests.size(), 1U);
		EXPECT_EQ(backend->constraint_create_requests.front(),
				  (storage::LogicalGraphConstraintCreateRequest{
						  "unique_person_name",
						  storage::LogicalGraphConstraintEntityKind::LGCEK_NODE,
						  storage::LogicalGraphConstraintKind::LGCK_UNIQUE,
						  "Person",
						  {"name"},
						  storage::LogicalGraphConstraintPropertyType::LGCPT_NONE}));

		const QueryResult constraints = engine.execute("SHOW CONSTRAINT");
		ASSERT_EQ(constraints.rowCount(), 1U);
		EXPECT_EQ(constraints.getRows().front().at("name").toString(), "unique_person_name");

		const QueryResult constraintDrop = engine.execute("DROP CONSTRAINT unique_person_name");
		ASSERT_EQ(constraintDrop.rowCount(), 1U);
		EXPECT_EQ(backend->dropped_constraint_name, "unique_person_name");
	}

	TEST(LogicalPlanSchemaRuntimeTest, RejectsIncompleteSchemasAndInvalidBackendDefinitions) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalCreateIndex validProperty("person_name", "Person", "name");
		logical::LogicalCreateVectorIndex validVector("person_embedding", "Person", "embedding", 3, "cosine");

		EXPECT_THROW(static_cast<void>(runtime.execute(validProperty, 0U, QueryContext{})), std::invalid_argument);
		logical::LogicalCreateIndex emptyLabel("person_name", "", "name");
		EXPECT_THROW(static_cast<void>(runtime.execute(emptyLabel, 8U, QueryContext{})), std::logic_error);
		logical::LogicalCreateIndex emptyProperties("person_name", "Person", std::vector<std::string>{});
		EXPECT_THROW(static_cast<void>(runtime.execute(emptyProperties, 8U, QueryContext{})), std::logic_error);

		backend.property_result_override = storage::LogicalGraphIndexDefinition{
				0U, "person_name", storage::LogicalGraphIndexKind::LGIK_PROPERTY, "Person", {"name"}, 0U, {}};
		EXPECT_THROW(static_cast<void>(runtime.execute(validProperty, 8U, QueryContext{})), std::logic_error);
		backend.property_result_override = storage::LogicalGraphIndexDefinition{
				7U, "", storage::LogicalGraphIndexKind::LGIK_PROPERTY, "Person", {"name"}, 0U, {}};
		EXPECT_THROW(static_cast<void>(runtime.execute(validProperty, 8U, QueryContext{})), std::logic_error);

		logical::LogicalCreateVectorIndex emptyVectorLabel("person_embedding", "", "embedding", 3, "cosine");
		logical::LogicalCreateVectorIndex emptyVectorProperty("person_embedding", "Person", "", 3, "cosine");
		logical::LogicalCreateVectorIndex zeroVectorDimension("person_embedding", "Person", "embedding", 0, "cosine");
		logical::LogicalCreateVectorIndex emptyVectorMetric("person_embedding", "Person", "embedding", 3, "");
		EXPECT_THROW(static_cast<void>(runtime.execute(emptyVectorLabel, 8U, QueryContext{})), std::logic_error);
		EXPECT_THROW(static_cast<void>(runtime.execute(emptyVectorProperty, 8U, QueryContext{})), std::logic_error);
		EXPECT_THROW(static_cast<void>(runtime.execute(zeroVectorDimension, 8U, QueryContext{})), std::logic_error);
		EXPECT_THROW(static_cast<void>(runtime.execute(emptyVectorMetric, 8U, QueryContext{})), std::logic_error);

		backend.vector_result_override = storage::LogicalGraphIndexDefinition{
				0U,		 "person_embedding", storage::LogicalGraphIndexKind::LGIK_VECTOR, "Person", {"embedding"}, 3U,
				"cosine"};
		EXPECT_THROW(static_cast<void>(runtime.execute(validVector, 8U, QueryContext{})), std::logic_error);
		backend.vector_result_override = storage::LogicalGraphIndexDefinition{
				8U, "", storage::LogicalGraphIndexKind::LGIK_VECTOR, "Person", {"embedding"}, 3U, "cosine"};
		EXPECT_THROW(static_cast<void>(runtime.execute(validVector, 8U, QueryContext{})), std::logic_error);
		backend.vector_result_override = storage::LogicalGraphIndexDefinition{
				9U,		 "person_embedding", storage::LogicalGraphIndexKind::LGIK_PROPERTY, "Person", {"embedding"}, 3U,
				"cosine"};
		EXPECT_THROW(static_cast<void>(runtime.execute(validVector, 8U, QueryContext{})), std::logic_error);
	}

	TEST(LogicalPlanSchemaRuntimeTest, RejectsUnknownIndexKindsAndNoOpSchemaDrops) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalDropIndex bySchema("", "Person", "name");
		backend.drop_by_schema_result = false;
		EXPECT_TRUE(runtime.execute(bySchema, 8U, QueryContext{}).isEmpty());

		backend.definitions = {
				{1U, "corrupt", static_cast<storage::LogicalGraphIndexKind>(99U), "Person", {"name"}, 0U, {}}};
		logical::LogicalShowIndexes show;
		EXPECT_THROW(static_cast<void>(runtime.execute(show, 8U, QueryContext{})), std::runtime_error);
	}

	TEST(LogicalPlanSchemaRuntimeTest, CreatesTypedNodeAndEdgeConstraintsThroughTheSchemaBoundary) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		const std::vector<logical::LogicalCreateConstraint> constraints = {
				{"unique_email", "node", "unique", "User", {"email"}, ""},
				{"required_since", "edge", "not_null", "KNOWS", {"since"}, ""},
				{"typed_active", "node", "property_type", "User", {"active"}, "BOOLEAN"},
				{"typed_score", "node", "property_type", "User", {"score"}, "FLOAT"},
				{"typed_name", "node", "property_type", "User", {"name"}, "STRING"},
				{"typed_tags", "node", "property_type", "User", {"tags"}, "LIST"},
				{"typed_metadata", "node", "property_type", "User", {"metadata"}, "MAP"},
				{"user_key", "node", "node_key", "User", {"tenant", "email"}, ""}};

		for (const logical::LogicalCreateConstraint &create: constraints) {
			const QueryResult result = runtime.execute(create, 16U, QueryContext{});
			ASSERT_EQ(result.rowCount(), 1U);
			EXPECT_EQ(result.getRows().front().at("result").toString(), "Constraint created");
		}

		ASSERT_EQ(backend.constraint_create_requests.size(), constraints.size());
		EXPECT_EQ(backend.constraint_create_requests[0U].entity_kind,
				  storage::LogicalGraphConstraintEntityKind::LGCEK_NODE);
		EXPECT_EQ(backend.constraint_create_requests[1U].entity_kind,
				  storage::LogicalGraphConstraintEntityKind::LGCEK_EDGE);
		EXPECT_EQ(backend.constraint_create_requests[0U].kind, storage::LogicalGraphConstraintKind::LGCK_UNIQUE);
		EXPECT_EQ(backend.constraint_create_requests[1U].kind, storage::LogicalGraphConstraintKind::LGCK_NOT_NULL);
		EXPECT_EQ(backend.constraint_create_requests[2U].property_type,
				  storage::LogicalGraphConstraintPropertyType::LGCPT_BOOLEAN);
		EXPECT_EQ(backend.constraint_create_requests[3U].property_type,
				  storage::LogicalGraphConstraintPropertyType::LGCPT_FLOAT);
		EXPECT_EQ(backend.constraint_create_requests[4U].property_type,
				  storage::LogicalGraphConstraintPropertyType::LGCPT_STRING);
		EXPECT_EQ(backend.constraint_create_requests[5U].property_type,
				  storage::LogicalGraphConstraintPropertyType::LGCPT_LIST);
		EXPECT_EQ(backend.constraint_create_requests[6U].property_type,
				  storage::LogicalGraphConstraintPropertyType::LGCPT_MAP);
		EXPECT_EQ(backend.constraint_create_requests[7U].kind, storage::LogicalGraphConstraintKind::LGCK_NODE_KEY);
		EXPECT_EQ(backend.constraint_create_requests[7U].property_keys, (std::vector<std::string>{"tenant", "email"}));
	}

	TEST(LogicalPlanSchemaRuntimeTest, ListsAndDropsConstraintsWithExplicitNotFoundSemantics) {
		SchemaBackend backend;
		backend.constraint_definitions = {{1U,
										   "unique_email",
										   storage::LogicalGraphConstraintEntityKind::LGCEK_NODE,
										   storage::LogicalGraphConstraintKind::LGCK_UNIQUE,
										   "User",
										   {"email"},
										   storage::LogicalGraphConstraintPropertyType::LGCPT_NONE},
										  {2U,
										   "required_since",
										   storage::LogicalGraphConstraintEntityKind::LGCEK_EDGE,
										   storage::LogicalGraphConstraintKind::LGCK_NOT_NULL,
										   "KNOWS",
										   {"since"},
										   storage::LogicalGraphConstraintPropertyType::LGCPT_NONE},
										  {3U,
										   "typed_score",
										   storage::LogicalGraphConstraintEntityKind::LGCEK_NODE,
										   storage::LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
										   "User",
										   {"score"},
										   storage::LogicalGraphConstraintPropertyType::LGCPT_FLOAT},
										  {4U,
										   "user_key",
										   storage::LogicalGraphConstraintEntityKind::LGCEK_NODE,
										   storage::LogicalGraphConstraintKind::LGCK_NODE_KEY,
										   "User",
										   {"tenant", "email"},
										   storage::LogicalGraphConstraintPropertyType::LGCPT_NONE}};
		LogicalPlanSchemaRuntime runtime(backend);
		logical::LogicalShowConstraints show;

		const QueryResult listed = runtime.execute(show, 8U, QueryContext{});
		ASSERT_EQ(listed.rowCount(), 4U);
		EXPECT_EQ(listed.getRows()[0U].at("type").toString(), "unique");
		EXPECT_EQ(listed.getRows()[1U].at("entity").toString(), "edge");
		EXPECT_EQ(listed.getRows()[2U].at("type").toString(), "property_type");
		EXPECT_EQ(listed.getRows()[3U].at("properties").toString(), "tenant,email");

		logical::LogicalDropConstraint drop("unique_email", false);
		const QueryResult dropped = runtime.execute(drop, 8U, QueryContext{});
		EXPECT_EQ(backend.dropped_constraint_name, "unique_email");
		ASSERT_EQ(dropped.rowCount(), 1U);
		EXPECT_EQ(dropped.getRows().front().at("result").toString(), "Constraint dropped");

		backend.drop_constraint_result = false;
		EXPECT_THROW(static_cast<void>(runtime.execute(drop, 8U, QueryContext{})), std::runtime_error);
		logical::LogicalDropConstraint ifExists("unknown", true);
		EXPECT_TRUE(runtime.execute(ifExists, 8U, QueryContext{}).isEmpty());
	}

	TEST(LogicalPlanSchemaRuntimeTest, RejectsMalformedAndCorruptConstraintDefinitions) {
		SchemaBackend backend;
		LogicalPlanSchemaRuntime runtime(backend);
		const logical::LogicalCreateConstraint valid("typed_age", "node", "property_type", "User", {"age"}, "INTEGER");

		EXPECT_THROW(static_cast<void>(runtime.execute(
							 logical::LogicalCreateConstraint("", "node", "unique", "User", {"email"}, ""), 8U,
							 QueryContext{})),
					 std::logic_error);
		EXPECT_THROW(
				static_cast<void>(runtime.execute(
						logical::LogicalCreateConstraint("invalid_entity", "invalid", "unique", "User", {"email"}, ""),
						8U, QueryContext{})),
				std::logic_error);
		EXPECT_THROW(static_cast<void>(runtime.execute(
							 logical::LogicalCreateConstraint("invalid_kind", "node", "invalid", "User", {"email"}, ""),
							 8U, QueryContext{})),
					 std::logic_error);
		EXPECT_THROW(static_cast<void>(
							 runtime.execute(logical::LogicalCreateConstraint("invalid_type", "node", "property_type",
																			  "User", {"age"}, "INVALID"),
											 8U, QueryContext{})),
					 std::logic_error);
		EXPECT_THROW(static_cast<void>(runtime.execute(
							 logical::LogicalCreateConstraint("no_label", "node", "unique", "", {"email"}, ""), 8U,
							 QueryContext{})),
					 std::logic_error);
		EXPECT_THROW(static_cast<void>(runtime.execute(
							 logical::LogicalCreateConstraint("no_properties", "node", "unique", "User", {}, ""), 8U,
							 QueryContext{})),
					 std::logic_error);

		backend.constraint_result_override =
				storage::LogicalGraphConstraintDefinition{0U,
														  "typed_age",
														  storage::LogicalGraphConstraintEntityKind::LGCEK_NODE,
														  storage::LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
														  "User",
														  {"age"},
														  storage::LogicalGraphConstraintPropertyType::LGCPT_INTEGER};
		EXPECT_THROW(static_cast<void>(runtime.execute(valid, 8U, QueryContext{})), std::logic_error);
		backend.constraint_result_override =
				storage::LogicalGraphConstraintDefinition{1U,
														  "",
														  storage::LogicalGraphConstraintEntityKind::LGCEK_NODE,
														  storage::LogicalGraphConstraintKind::LGCK_PROPERTY_TYPE,
														  "User",
														  {"age"},
														  storage::LogicalGraphConstraintPropertyType::LGCPT_INTEGER};
		EXPECT_THROW(static_cast<void>(runtime.execute(valid, 8U, QueryContext{})), std::logic_error);

		backend.constraint_definitions = {{1U,
										   "corrupt",
										   storage::LogicalGraphConstraintEntityKind::LGCEK_NODE,
										   static_cast<storage::LogicalGraphConstraintKind>(99U),
										   "User",
										   {"age"},
										   storage::LogicalGraphConstraintPropertyType::LGCPT_INTEGER}};
		logical::LogicalShowConstraints show;
		EXPECT_THROW(static_cast<void>(runtime.execute(show, 8U, QueryContext{})), std::runtime_error);
		backend.constraint_definitions = {{1U,
										   "corrupt",
										   static_cast<storage::LogicalGraphConstraintEntityKind>(99U),
										   storage::LogicalGraphConstraintKind::LGCK_UNIQUE,
										   "User",
										   {"age"},
										   storage::LogicalGraphConstraintPropertyType::LGCPT_NONE}};
		EXPECT_THROW(static_cast<void>(runtime.execute(show, 8U, QueryContext{})), std::runtime_error);
	}

} // namespace graph::query::execution
