/**
 * @file test_DriverAbiAgentMemorySigsegvRepro.cpp
 * @brief Driver ABI regression workload for a full-conversation agent-memory case.
 *
 * The corresponding Python benchmark can fail in a multiprocessing worker.
 * This test keeps the database-side workload in C++ so Driver ABI regressions
 * are caught without depending on a Python runtime or benchmark dialogue.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "api/AgentMemoryBlobChainBgeVectors.hpp"
#include "api/AgentMemorySigsegvVectors.hpp"
#include "zyx/zyx_driver_abi.h"

namespace {

constexpr size_t kEmbeddingDimension = 256;
constexpr size_t kSessionCount = agent_memory_sigsegv_repro::kSessionTextLengths.size();
constexpr size_t kNodesPerSession = 4;
constexpr size_t kBlobChainEmbeddingDimension = 384;
constexpr size_t kBlobChainSessionCount = agent_memory_blob_chain_repro::kSessionTextLengths.size();

static_assert(agent_memory_sigsegv_repro::kHashVectorCounts.size() == kSessionCount);
static_assert(agent_memory_blob_chain_repro::kBgeVectors.size() == kBlobChainSessionCount);
static_assert(agent_memory_blob_chain_repro::kBgeVectors.front().size() == kBlobChainEmbeddingDimension);

const std::filesystem::path &databasePath() {
	static const std::filesystem::path path = [] {
		std::error_code error;
		const std::filesystem::path temporaryDirectory = std::filesystem::temp_directory_path(error);
		if (error) {
			throw std::runtime_error("could not resolve the test temporary directory: " + error.message());
		}
		return temporaryDirectory / "zyx_agent_memory_sigsegv_repro.zyx";
	}();
	return path;
}

const std::filesystem::path &blobChainDatabasePath() {
	static const std::filesystem::path path = [] {
		std::error_code error;
		const std::filesystem::path temporaryDirectory = std::filesystem::temp_directory_path(error);
		if (error) {
			throw std::runtime_error("could not resolve the test temporary directory: " + error.message());
		}
		return temporaryDirectory / "zyx_agent_memory_blob_chain_repro.zyx";
	}();
	return path;
}

void removeDatabaseArtifacts() {
	const std::filesystem::path &path = databasePath();
	std::error_code error;
	for (const std::string &suffix: {"", ".wal", ".migration", ".cutover", ".legacy-backup", "-wal"}) {
		std::filesystem::remove_all(path.string() + suffix, error);
		error.clear();
	}
}

void removeBlobChainDatabaseArtifacts() {
	const std::filesystem::path &path = blobChainDatabasePath();
	std::error_code error;
	for (const std::string &suffix: {"", ".wal", ".migration", ".cutover", ".legacy-backup", "-wal"}) {
		std::filesystem::remove_all(path.string() + suffix, error);
		error.clear();
	}
}

void requireDriverSuccess(zyx_driver_status_t status, zyx_driver_error_t *&error, const std::string &operation) {
	if (status == ZYX_DRIVER_OK)
		return;
	const std::string message = error == nullptr ? "unknown driver error" : zyx_driver_error_message(error);
	zyx_driver_error_free(error);
	error = nullptr;
	throw std::runtime_error(operation + " failed: " + message);
}

void freeParams(zyx_driver_params_t *params) {
	zyx_driver_error_t *error = nullptr;
	zyx_driver_params_free(params, &error);
	if (error != nullptr) {
		const std::string message = zyx_driver_error_message(error);
		zyx_driver_error_free(error);
		throw std::runtime_error("free driver parameters failed: " + message);
	}
}

void executeDatabase(zyx_driver_db_t *database, const char *cypher, const std::string &operation) {
	zyx_driver_error_t *error = nullptr;
	zyx_driver_result_t *result = nullptr;
	requireDriverSuccess(zyx_driver_db_execute(database, cypher, nullptr, &result, &error), error, operation);
	zyx_driver_result_free(result);
}

void scanStoredSources(zyx_driver_db_t *database) {
	zyx_driver_error_t *error = nullptr;
	zyx_driver_result_t *result = nullptr;
	requireDriverSuccess(
		zyx_driver_db_execute(
			database,
			"MATCH (n:Memory) WHERE n.tenantId = 'longmemeval' AND n.bankId = 's' "
			"AND n.subjectId = 'gpt4_2f91af09' AND n.kind = 'source' AND n.status = 'active' "
			"RETURN n.text AS text, n.canonicalKey AS canonicalKey, n.embedding AS embedding "
			"ORDER BY n.recordedAt DESC LIMIT 2000",
			nullptr,
			&result,
			&error),
		error,
		"scan retained source memories");

	size_t expectedTextBytes = 0;
	for (const size_t length: agent_memory_sigsegv_repro::kSessionTextLengths)
		expectedTextBytes += length;

	uint32_t rows = 0;
	size_t textBytes = 0;
	size_t canonicalKeyBytes = 0;
	for (;;) {
		const auto status = zyx_driver_result_next(result, &error);
		if (status == ZYX_DRIVER_DONE)
			break;
		if (status != ZYX_DRIVER_ROW) {
			const std::string message = error == nullptr ? "unknown driver error" : zyx_driver_error_message(error);
			if (error != nullptr)
				zyx_driver_error_free(error);
			zyx_driver_result_free(result);
			throw std::runtime_error("advance retained source result failed: " + message);
		}

		const char *text = nullptr;
		const char *canonicalKey = nullptr;
		uint32_t embeddingSize = 0;
		requireDriverSuccess(zyx_driver_result_get_string(result, 0, &text, &error), error,
			"read retained source text");
		requireDriverSuccess(zyx_driver_result_get_string(result, 1, &canonicalKey, &error), error,
			"read retained source canonical key");
		requireDriverSuccess(zyx_driver_result_get_list_count(result, 2, &embeddingSize, &error), error,
			"read retained source embedding");
		if (text == nullptr || canonicalKey == nullptr || embeddingSize != kEmbeddingDimension) {
			zyx_driver_result_free(result);
			throw std::runtime_error("retained source result has an unexpected shape");
		}
		textBytes += std::char_traits<char>::length(text);
		canonicalKeyBytes += std::char_traits<char>::length(canonicalKey);
		++rows;
	}
	zyx_driver_result_free(result);
	if (rows != kSessionCount || textBytes != expectedTextBytes || canonicalKeyBytes != expectedTextBytes)
		throw std::runtime_error("retained source scan returned an unexpected row count");
}

void executeTransaction(zyx_driver_txn_t *transaction, const char *cypher, zyx_driver_params_t *params,
						const std::string &operation) {
	zyx_driver_error_t *error = nullptr;
	zyx_driver_result_t *result = nullptr;
	requireDriverSuccess(zyx_driver_txn_execute(transaction, cypher, params, &result, &error), error, operation);
	zyx_driver_result_free(result);
}

std::string fullConversation(size_t session) {
	const std::string prefix =
		"user: I am tracking a writing challenge and need the agent to retain every detail.\n\n"
		"assistant: I will preserve this conversation as durable memory with its supporting evidence. ";
	const std::string fragment = "session-" + std::to_string(session) +
		" writing progress, short stories, poems, draft revisions, daily notes, and follow-up plans. ";
	const size_t targetLength = agent_memory_sigsegv_repro::kSessionTextLengths.at(session);

	std::string text = prefix;
	text.reserve(targetLength);
	while (text.size() < targetLength)
		text.append(fragment);
	text.resize(targetLength);
	return text;
}

std::vector<float> embeddingForSession(size_t session) {
	const auto &counts = agent_memory_sigsegv_repro::kHashVectorCounts.at(session);
	std::vector<float> vector(kEmbeddingDimension);
	float norm = 0.0F;
	for (size_t dimension = 0; dimension < vector.size(); ++dimension) {
		vector[dimension] = static_cast<float>(counts[dimension]);
		norm += vector[dimension] * vector[dimension];
	}
	if (norm == 0.0F)
		throw std::runtime_error("agent-memory embedding fixture must not be a zero vector");
	const float scale = 1.0F / std::sqrt(norm);
	for (float &value: vector)
		value *= scale;
	return vector;
}

void initializeMemorySchema(zyx_driver_db_t *database) {
	executeDatabase(database,
		"CREATE (s:MemorySchema {name: 'cognitive-graph-memory', version: 1, "
		"embeddingDimension: 256, embeddingModel: 'benchmark-hash-256'})",
		"create memory schema");
	executeDatabase(database, "CREATE CONSTRAINT cgm_memory_id_unique FOR (n:Memory) REQUIRE n.id IS UNIQUE",
		"create memory id constraint");
	executeDatabase(database,
		"CREATE INDEX cgm_memory_scope_kind_status_idx FOR (n:Memory) "
		"ON (n.tenantId, n.bankId, n.subjectId, n.kind, n.status)",
		"create memory scope index");
	executeDatabase(database, "CREATE INDEX cgm_memory_event_time_idx FOR (n:Memory) ON (n.eventTime)",
		"create memory event-time index");
	executeDatabase(database, "CREATE INDEX cgm_memory_canonical_key_idx FOR (n:Memory) ON (n.canonicalKey)",
		"create memory canonical-key index");
	executeDatabase(database,
		"CREATE VECTOR INDEX agent_memory_embedding_idx ON :Memory(embedding) "
		"OPTIONS {dimension: 256, metric: 'COSINE'}",
		"create agent-memory vector index");
}

const char *memoryWriteQuery(size_t node) {
	static constexpr std::array<const char *, kNodesPerSession> queries{
		"CREATE (n:Memory:Source {id: $id}) SET "
		"n.tenantId = $tenantId, n.bankId = $bankId, n.subjectId = $subjectId, n.kind = $kind, "
		"n.text = $text, n.canonicalKey = $canonicalKey, n.embedding = $embedding, "
		"n.embeddingModel = $embeddingModel, n.status = $status, n.recordedAt = $recordedAt, "
		"n.eventTime = $eventTime, n.validFrom = $validFrom, n.validTo = $validTo, "
		"n.importance = $importance, n.confidence = $confidence, n.sourceReliability = $sourceReliability, "
		"n.accessCount = $accessCount, n.lastAccessedAt = $lastAccessedAt, n.successCount = $successCount, "
		"n.failureCount = $failureCount, n.reinforcementTimesJson = $reinforcementTimesJson, "
		"n.metadataJson = $metadataJson",
		"CREATE (n:Memory:Episode {id: $id}) SET "
		"n.tenantId = $tenantId, n.bankId = $bankId, n.subjectId = $subjectId, n.kind = $kind, "
		"n.text = $text, n.canonicalKey = $canonicalKey, n.embedding = $embedding, "
		"n.embeddingModel = $embeddingModel, n.status = $status, n.recordedAt = $recordedAt, "
		"n.eventTime = $eventTime, n.validFrom = $validFrom, n.validTo = $validTo, "
		"n.importance = $importance, n.confidence = $confidence, n.sourceReliability = $sourceReliability, "
		"n.accessCount = $accessCount, n.lastAccessedAt = $lastAccessedAt, n.successCount = $successCount, "
		"n.failureCount = $failureCount, n.reinforcementTimesJson = $reinforcementTimesJson, "
		"n.metadataJson = $metadataJson",
		"CREATE (n:Memory:Event {id: $id}) SET "
		"n.tenantId = $tenantId, n.bankId = $bankId, n.subjectId = $subjectId, n.kind = $kind, "
		"n.text = $text, n.canonicalKey = $canonicalKey, n.embedding = $embedding, "
		"n.embeddingModel = $embeddingModel, n.status = $status, n.recordedAt = $recordedAt, "
		"n.eventTime = $eventTime, n.validFrom = $validFrom, n.validTo = $validTo, "
		"n.importance = $importance, n.confidence = $confidence, n.sourceReliability = $sourceReliability, "
		"n.accessCount = $accessCount, n.lastAccessedAt = $lastAccessedAt, n.successCount = $successCount, "
		"n.failureCount = $failureCount, n.reinforcementTimesJson = $reinforcementTimesJson, "
		"n.metadataJson = $metadataJson",
		"CREATE (n:Memory:Claim {id: $id}) SET "
		"n.tenantId = $tenantId, n.bankId = $bankId, n.subjectId = $subjectId, n.kind = $kind, "
		"n.text = $text, n.canonicalKey = $canonicalKey, n.embedding = $embedding, "
		"n.embeddingModel = $embeddingModel, n.status = $status, n.recordedAt = $recordedAt, "
		"n.eventTime = $eventTime, n.validFrom = $validFrom, n.validTo = $validTo, "
		"n.importance = $importance, n.confidence = $confidence, n.sourceReliability = $sourceReliability, "
		"n.accessCount = $accessCount, n.lastAccessedAt = $lastAccessedAt, n.successCount = $successCount, "
		"n.failureCount = $failureCount, n.reinforcementTimesJson = $reinforcementTimesJson, "
		"n.metadataJson = $metadataJson",
	};
	return queries.at(node);
}

void setMemoryParameters(zyx_driver_params_t *params, const std::string &id, const std::string &kind,
						 const std::string &conversation, const std::vector<float> &embedding) {
	zyx_driver_error_t *error = nullptr;
	const auto setString = [&](const char *key, const std::string &value) {
		requireDriverSuccess(zyx_driver_params_set_string(params, key, value.c_str(), &error), error,
			"set memory string parameter");
	};
	const auto setInteger = [&](const char *key, int64_t value) {
		requireDriverSuccess(zyx_driver_params_set_int64(params, key, value, &error), error,
			"set memory integer parameter");
	};
	const auto setDouble = [&](const char *key, double value) {
		requireDriverSuccess(zyx_driver_params_set_double(params, key, value, &error), error,
			"set memory double parameter");
	};

	setString("id", id);
	setString("kind", kind);
	setString("text", conversation);
	setString("canonicalKey", conversation);
	setString("tenantId", "longmemeval");
	setString("bankId", "s");
	setString("subjectId", "gpt4_2f91af09");
	setString("embeddingModel", "hash-embedding-256");
	setString("status", "active");
	setString("reinforcementTimesJson", "[1721000000000]");
	setString("metadataJson", "{\"sourceType\":\"longmemeval-session\"}");
	setInteger("recordedAt", 1721000000000LL);
	setInteger("eventTime", 1721000000000LL);
	setInteger("accessCount", 0);
	setInteger("successCount", 0);
	setInteger("failureCount", 0);
	setDouble("importance", 0.5);
	setDouble("confidence", kind == "claim" ? 0.7 : 1.0);
	setDouble("sourceReliability", 0.7);
	requireDriverSuccess(zyx_driver_params_set_null(params, "validFrom", &error), error,
		"set memory valid-from parameter");
	requireDriverSuccess(zyx_driver_params_set_null(params, "validTo", &error), error,
		"set memory valid-to parameter");
	requireDriverSuccess(zyx_driver_params_set_null(params, "lastAccessedAt", &error), error,
		"set memory last-accessed parameter");
	requireDriverSuccess(zyx_driver_params_set_float_list(
		params, "embedding", embedding.data(), static_cast<uint32_t>(embedding.size()), &error),
		error, "set memory embedding parameter");
}

void createRelationship(zyx_driver_txn_t *transaction, const char *cypher, const std::string &source,
					const std::string &target) {
	zyx_driver_params_t *params = nullptr;
	zyx_driver_error_t *error = nullptr;
	requireDriverSuccess(zyx_driver_params_create(&params, &error), error, "create relationship parameters");
	requireDriverSuccess(zyx_driver_params_set_string(params, "sourceId", source.c_str(), &error), error,
		"set relationship source parameter");
	requireDriverSuccess(zyx_driver_params_set_string(params, "targetId", target.c_str(), &error), error,
		"set relationship target parameter");
	executeTransaction(transaction, cypher, params, "create retained-memory relationship");
	freeParams(params);
}

void retainFullConversationWorkload() {
	removeDatabaseArtifacts();

	zyx_driver_db_t *database = nullptr;
	zyx_driver_error_t *error = nullptr;
	requireDriverSuccess(zyx_driver_db_open(databasePath().string().c_str(), &database, &error), error,
		"open agent-memory database");
	initializeMemorySchema(database);

	for (size_t session = 0; session < kSessionCount; ++session) {
		const std::string conversation = fullConversation(session);
		const std::vector<float> embedding = embeddingForSession(session);
		zyx_driver_txn_t *transaction = nullptr;
		requireDriverSuccess(zyx_driver_txn_begin(database, &transaction, &error), error,
			"begin retained-memory transaction");

		std::array<std::string, kNodesPerSession> ids;
		for (size_t node = 0; node < kNodesPerSession; ++node) {
			ids[node] = "session-" + std::to_string(session) + "-memory-" + std::to_string(node);
			const std::string kind = node == 0 ? "source" : node == 1 ? "episode" : node == 2 ? "event" : "claim";
			zyx_driver_params_t *params = nullptr;
			requireDriverSuccess(zyx_driver_params_create(&params, &error), error, "create memory parameters");
			setMemoryParameters(params, ids[node], kind, conversation, embedding);
			executeTransaction(transaction, memoryWriteQuery(node), params, "retain full-conversation memory");
			freeParams(params);
		}

		for (const auto &[source, target, query]: std::array{
				std::tuple{0U, 1U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:RECORDED_AS]->(target)"},
				std::tuple{1U, 2U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:CONTAINS]->(target)"},
				std::tuple{2U, 3U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:ASSERTS]->(target)"},
				std::tuple{0U, 3U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:EVIDENCE_FOR]->(target)"},
		}) {
			createRelationship(transaction, query, ids[source], ids[target]);
		}

		requireDriverSuccess(zyx_driver_txn_commit(transaction, &error), error,
			"commit retained-memory transaction");
		requireDriverSuccess(zyx_driver_txn_close(transaction, &error), error,
			"close retained-memory transaction");
	}

	executeDatabase(database, "CALL db.index.vector.train('agent_memory_embedding_idx')",
		"train agent-memory vector index");
	scanStoredSources(database);
	requireDriverSuccess(zyx_driver_db_close(database, &error), error, "close agent-memory database");
	removeDatabaseArtifacts();
}

std::string blobChainConversation(size_t session) {
	const size_t targetLength = agent_memory_blob_chain_repro::kSessionTextLengths.at(session);
	std::string text;
	text.reserve(targetLength);
	uint64_t state = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(session);
	while (text.size() < targetLength) {
		state ^= state << 13U;
		state ^= state >> 7U;
		state ^= state << 17U;
		text += "session memory event ";
		text += std::to_string(session);
		text += " detail ";
		text += std::to_string(state);
		text += '\n';
	}
	text.resize(targetLength);
	return text;
}

std::vector<float> blobChainEmbedding(size_t session) {
	const auto &fixture = agent_memory_blob_chain_repro::kBgeVectors.at(session);
	return {fixture.begin(), fixture.end()};
}

void initializeBlobChainMemorySchema(zyx_driver_db_t *database) {
	executeDatabase(database,
		"CREATE (s:MemorySchema {name: 'cognitive-graph-memory', version: 1, "
		"embeddingDimension: 384, embeddingModel: 'BAAI/bge-small-en-v1.5'})",
		"create blob-chain memory schema");
	executeDatabase(database, "CREATE CONSTRAINT cgm_memory_id_unique FOR (n:Memory) REQUIRE n.id IS UNIQUE",
		"create blob-chain memory id constraint");
	executeDatabase(database,
		"CREATE INDEX cgm_memory_scope_kind_status_idx FOR (n:Memory) "
		"ON (n.tenantId, n.bankId, n.subjectId, n.kind, n.status)",
		"create blob-chain memory scope index");
	executeDatabase(database, "CREATE INDEX cgm_memory_event_time_idx FOR (n:Memory) ON (n.eventTime)",
		"create blob-chain memory event-time index");
	executeDatabase(database, "CREATE INDEX cgm_memory_canonical_key_idx FOR (n:Memory) ON (n.canonicalKey)",
		"create blob-chain memory canonical-key index");
	executeDatabase(database,
		"CREATE VECTOR INDEX cgm_memory_embedding_idx ON :Memory(embedding) "
		"OPTIONS {dimension: 384, metric: 'COSINE'}",
		"create blob-chain vector index");
}

void setBlobChainMemoryParameters(zyx_driver_params_t *params, const std::string &id, const std::string &kind,
	const std::string &conversation, const std::vector<float> &embedding, size_t session) {
	zyx_driver_error_t *error = nullptr;
	const auto setString = [&](const char *key, const std::string &value) {
		requireDriverSuccess(zyx_driver_params_set_string(params, key, value.c_str(), &error), error,
			"set blob-chain memory string parameter");
	};
	const auto setInteger = [&](const char *key, int64_t value) {
		requireDriverSuccess(zyx_driver_params_set_int64(params, key, value, &error), error,
			"set blob-chain memory integer parameter");
	};

	setString("id", id);
	setString("kind", kind);
	setString("text", conversation);
	setString("canonicalKey", conversation);
	setString("tenantId", "longmemeval");
	setString("bankId", "s");
	setString("subjectId", "01493427");
	setString("embeddingModel", "BAAI/bge-small-en-v1.5");
	setString("status", "active");
	setString("reinforcementTimesJson", "[1721000000000]");
	setString("metadataJson", "{\"sourceType\":\"longmemeval-session\",\"fixtureSession\":" + std::to_string(session) + "}");
	setInteger("recordedAt", 1721000000000LL + static_cast<int64_t>(session));
	setInteger("eventTime", 1721000000000LL + static_cast<int64_t>(session));
	setInteger("accessCount", 0);
	setInteger("successCount", 0);
	setInteger("failureCount", 0);
	requireDriverSuccess(zyx_driver_params_set_double(params, "importance", 0.5, &error), error,
		"set blob-chain memory importance parameter");
	requireDriverSuccess(zyx_driver_params_set_double(params, "confidence", kind == "claim" ? 0.7 : 1.0, &error), error,
		"set blob-chain memory confidence parameter");
	requireDriverSuccess(zyx_driver_params_set_double(params, "sourceReliability", 0.7, &error), error,
		"set blob-chain memory reliability parameter");
	requireDriverSuccess(zyx_driver_params_set_null(params, "validFrom", &error), error,
		"set blob-chain memory valid-from parameter");
	requireDriverSuccess(zyx_driver_params_set_null(params, "validTo", &error), error,
		"set blob-chain memory valid-to parameter");
	requireDriverSuccess(zyx_driver_params_set_null(params, "lastAccessedAt", &error), error,
		"set blob-chain memory last-accessed parameter");
	requireDriverSuccess(zyx_driver_params_set_float_list(
		params, "embedding", embedding.data(), static_cast<uint32_t>(embedding.size()), &error),
		error, "set blob-chain memory embedding parameter");
}

void retainBlobChainWorkload() {
	removeBlobChainDatabaseArtifacts();
	zyx_driver_db_t *database = nullptr;
	zyx_driver_error_t *error = nullptr;
	requireDriverSuccess(zyx_driver_db_open(blobChainDatabasePath().string().c_str(), &database, &error), error,
		"open blob-chain agent-memory database");
	initializeBlobChainMemorySchema(database);

	for (size_t session = 0; session < kBlobChainSessionCount; ++session) {
		const std::string conversation = blobChainConversation(session);
		const std::vector<float> embedding = blobChainEmbedding(session);
		zyx_driver_txn_t *transaction = nullptr;
		requireDriverSuccess(zyx_driver_txn_begin(database, &transaction, &error), error,
			"begin blob-chain retained-memory transaction");

		std::array<std::string, kNodesPerSession> ids;
		for (size_t node = 0; node < kNodesPerSession; ++node) {
			ids[node] = "blob-session-" + std::to_string(session) + "-memory-" + std::to_string(node);
			const std::string kind = node == 0 ? "source" : node == 1 ? "episode" : node == 2 ? "event" : "claim";
			zyx_driver_params_t *params = nullptr;
			requireDriverSuccess(zyx_driver_params_create(&params, &error), error,
				"create blob-chain memory parameters");
			setBlobChainMemoryParameters(params, ids[node], kind, conversation, embedding, session);
			executeTransaction(transaction, memoryWriteQuery(node), params, "retain blob-chain memory");
			freeParams(params);
		}

		for (const auto &[source, target, query]: std::array{
				std::tuple{0U, 1U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:RECORDED_AS]->(target)"},
				std::tuple{1U, 2U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:CONTAINS]->(target)"},
				std::tuple{2U, 3U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:ASSERTS]->(target)"},
				std::tuple{0U, 3U, "MATCH (source:Memory {id: $sourceId}), (target:Memory {id: $targetId}) CREATE (source)-[:EVIDENCE_FOR]->(target)"},
		}) {
			createRelationship(transaction, query, ids[source], ids[target]);
		}
		requireDriverSuccess(zyx_driver_txn_commit(transaction, &error), error,
			"commit blob-chain retained-memory transaction");
		requireDriverSuccess(zyx_driver_txn_close(transaction, &error), error,
			"close blob-chain retained-memory transaction");
	}

	requireDriverSuccess(zyx_driver_db_close(database, &error), error, "close blob-chain agent-memory database");
	removeBlobChainDatabaseArtifacts();
}

class CerrCapture {
public:
	CerrCapture() : original_(std::cerr.rdbuf(buffer_.rdbuf())) {}
	~CerrCapture() { std::cerr.rdbuf(original_); }
	std::string str() const { return buffer_.str(); }

private:
	std::ostringstream buffer_;
	std::streambuf *original_;
};

TEST(DriverAbiAgentMemoryLongConversationTest, FullConversationRetentionAndPQTrainingCompletesWithoutNativeCrash) {
	removeDatabaseArtifacts();

	// The suite-level supervisor already isolates timeout and crash failures.
	// A fork-based death test is unsafe here because vector training can create
	// worker threads before the death-test child starts the full workload.
	EXPECT_NO_THROW(retainFullConversationWorkload());
	removeDatabaseArtifacts();
}

TEST(DriverAbiAgentMemoryBlobChainTest, BgeRetentionDoesNotLogBlobChainCorruption) {
	removeBlobChainDatabaseArtifacts();
	CerrCapture capture;
	retainBlobChainWorkload();
	EXPECT_EQ(capture.str().find("Blob chain corrupted"), std::string::npos);
	removeBlobChainDatabaseArtifacts();
}

} // namespace
