/**
 * @file test_PQDecompressionRepro.cpp
 * @date 2026/07/14
 *
 * @copyright Copyright (c) 2026 Nexepic
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

// Reproduction of the JS repro at bindings/nodejs/test/vector-pq-repro.test.js.
// It drives the DiskANN vector index through the exact storage path the JS test
// exercises (VectorIndexRegistry -> BlobChainManager -> zlib compressed blobs),
// crossing the auto-PQ-training boundary (2000 inserts), and then searches.
// If any blob chain becomes unreadable after training, loadRawVector /
// loadAdjacency will throw std::runtime_error("Decompression failed") — the
// same error the JS repro surfaces as "Insert failed: Decompression failed".

#include <algorithm>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "graph/core/Database.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/vector/VectorIndexRegistry.hpp"
#include "graph/vector/index/DiskANNIndex.hpp"

namespace {

constexpr size_t kDimension = 64;
constexpr size_t kAutoTrainThreshold = 2000;

// Deterministic hashing of text into a unit vector, mirroring the JS repro's
// hashEmbedding(). Keeps the test hermetic (no JS), while producing vectors of
// the same shape and cardinality distribution that triggered the original bug.
std::vector<float> hashEmbedding(const std::string &text) {
	std::vector<float> v(kDimension, 0.0f);
	auto fnv1a = [](const std::string &s) -> uint32_t {
		uint32_t h = 0x811c9dc5u;
		for (char c: s) {
			h ^= static_cast<uint8_t>(c);
			h *= 0x01000193u;
		}
		return h;
	};
	auto mix32 = [](uint32_t x) -> uint32_t {
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	};

	// Minimal tokenizer (alphanumeric runs), case-folded.
	std::string cur;
	auto flush = [&](const std::string &tok) {
		if (tok.empty())
			return;
		uint32_t first = fnv1a(tok);
		uint32_t second = mix32(first ^ 0x9e3779b9u);
		v[first % kDimension] += (second & 1u) ? -1.0f : 1.0f;
	};
	for (char c: text) {
		if (std::isalnum(static_cast<unsigned char>(c))) {
			cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		} else {
			flush(cur);
			cur.clear();
		}
	}
	flush(cur);

	float mag = 0.0f;
	for (float x: v)
		mag += x * x;
	mag = std::sqrt(mag);
	if (mag > 0.0f)
		for (float &x: v)
			x /= mag;
	return v;
}

class PQDecompressionReproTest : public ::testing::Test {
protected:
	void SetUp() override {
		boost::uuids::uuid uuid = boost::uuids::random_generator()();
		dbPath = std::filesystem::temp_directory_path() / ("pq_repro_" + boost::uuids::to_string(uuid) + ".zyx");
		if (std::filesystem::exists(dbPath))
			std::filesystem::remove_all(dbPath);

		database = std::make_unique<graph::Database>(dbPath.string());
		database->open();
		dataManager = database->getStorage()->getDataManager();
		stateManager = database->getStorage()->getSystemStateManager();
	}

	void TearDown() override {
		dataManager.reset();
		stateManager.reset();
		database->close();
		database.reset();
		std::error_code ec;
		std::filesystem::remove_all(dbPath, ec);
	}

	std::filesystem::path dbPath;
	std::unique_ptr<graph::Database> database;
	std::shared_ptr<graph::storage::DataManager> dataManager;
	std::shared_ptr<graph::storage::state::SystemStateManager> stateManager;
};

// Mirrors bindings/nodejs/test/vector-pq-repro.test.js::runWorkload.
// The JS repro asserts "Insert failed: Decompression failed" is logged. In C++,
// the same failure surfaces as an exception out of loadRawVector/loadAdjacency.
// After the auto-PQ-training fix, no insert/search here may throw — the test
// pins that contract. If a regression reintroduces a decompression failure,
// the EXPECT_* below (or an uncaught exception) will fail.
TEST_F(PQDecompressionReproTest, InsertAcrossPQTrainingDoesNotThrowDecompression) {
	auto registry = std::make_shared<graph::vector::VectorIndexRegistry>(dataManager, stateManager, "cgm_memory_embedding_idx");

	graph::vector::VectorIndexConfig regCfg;
	regCfg.dimension = kDimension;
	// Cosine metric in JS -> metricType=1.
	regCfg.metricType = 1;
	registry->updateConfig(regCfg);

	graph::vector::DiskANNConfig daCfg;
	daCfg.dim = kDimension;
	daCfg.metric = "IP"; // registry maps Cosine/IP to metricType=1
	daCfg.maxDegree = 64;
	daCfg.beamWidth = 100;
	daCfg.autoTrainThreshold = kAutoTrainThreshold;

	graph::vector::DiskANNIndex index(registry, daCfg);

	// Insert enough vectors to cross autoTrainThreshold == 2000.
	// JS repro used (34 fixtures + 250*4 performance rows ~= 1034) entries but
	// observed training at count 2000 — the DiskANN index counts each insert
	// call, and the runtime there accumulated to 2000. We drive 2000 direct
	// inserts deterministically to force the same boundary.
	constexpr int kTotalInserts = 2100;
	std::mt19937 rng(42);
	std::vector<std::vector<float>> inserted;
	inserted.reserve(kTotalInserts);

	for (int i = 0; i < kTotalInserts; ++i) {
		std::string text = "performance fixture " + std::to_string(i) + " marker-" + std::to_string(i % 19);
		std::vector<float> vec = hashEmbedding(text);
		inserted.push_back(vec);

		EXPECT_NO_THROW({
			// node ids start at 1.
			index.insert(static_cast<int64_t>(i + 1), vec);
		}) << "insert #" << i << " threw (likely Decompression failed after PQ training)";
	}

	// Guard against the test silently passing because training never happened.
	// PQ auto-training must have triggered by the time we cross autoTrainThreshold.
	EXPECT_TRUE(index.isPQTrained())
		<< "Test did not exercise the post-PQ-training path; autoTrainThreshold not reached";

	// Post-training search must also be able to reload every raw vector blob —
	// this is where a corrupted compressed blob chain would surface.
	EXPECT_NO_THROW({
		auto results = index.search(inserted[0], 10);
		// At minimum, the queried node should be reachable.
		EXPECT_GE(results.size(), 1u);
	});

	// Sanity: search a few more queries to exercise additional blob-chain reads.
	for (int q = 1; q <= 5 && q < static_cast<int>(inserted.size()); ++q) {
		EXPECT_NO_THROW({
			auto r = index.search(inserted[q], 10);
			EXPECT_GE(r.size(), 1u);
		}) << "search q=" << q << " threw";
	}
}

} // namespace