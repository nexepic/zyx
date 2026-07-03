/**
 * @file test_Leiden.cpp
 * @author Nexepic
 * @date 2026/7/3
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

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <filesystem>
#include <gtest/gtest.h>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/query/algorithm/CsrProjection.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/query/algorithm/LeidenEngine.hpp"

namespace fs = std::filesystem;

namespace {

	class LeidenTest : public ::testing::Test {
	protected:
		void SetUp() override {
			auto uuid = boost::uuids::random_generator()();
			testFilePath = fs::temp_directory_path() / ("test_leiden_" + to_string(uuid) + ".dat");
			database = std::make_unique<graph::Database>(testFilePath.string());
			database->open();
			dataManager = database->getStorage()->getDataManager();
		}

		void TearDown() override {
			if (database) database->close();
			database.reset();
			std::error_code ec;
			if (fs::exists(testFilePath)) fs::remove(testFilePath, ec);
		}

		int64_t addNode(const std::string &label = "N") {
			int64_t id = dataManager->getIdAllocator(graph::EntityType::Node)->allocate();
			graph::Node node(id, dataManager->getOrCreateTokenId(label));
			dataManager->addNode(node);
			return id;
		}

		void addEdge(int64_t src, int64_t dst, const std::string &type = "LINK") {
			int64_t eid = dataManager->getIdAllocator(graph::EntityType::Edge)->allocate();
			graph::Edge edge(eid, src, dst, dataManager->getOrCreateTokenId(type));
			dataManager->addEdge(edge);
		}

		void reload() {
			database->getStorage()->flush();
			database->close();
			database->open();
			dataManager = database->getStorage()->getDataManager();
		}

		// Helper: build N cliques of size K, each clique connected to the next by a
		// single bridge edge. With Lifting, Leiden should merge cliques into fewer
		// super-communities and reach a higher modularity than single-level.
		void buildCliqueChain(int cliques, int cliqueSize, std::vector<int64_t> &firstNodes) {
			std::vector<int64_t> prevLast;
			for (int c = 0; c < cliques; ++c) {
				std::vector<int64_t> nodes;
				for (int i = 0; i < cliqueSize; ++i) nodes.push_back(addNode());
				firstNodes.push_back(nodes[0]);
				for (int i = 0; i < cliqueSize; ++i)
					for (int j = i + 1; j < cliqueSize; ++j) addEdge(nodes[i], nodes[j]);
				if (!prevLast.empty()) addEdge(prevLast.back(), nodes[0]); // bridge
				prevLast = nodes;
			}
		}

		fs::path testFilePath;
		std::unique_ptr<graph::Database> database;
		std::shared_ptr<graph::storage::DataManager> dataManager;
	};

	// Build a string identifier "n1,cX" set for comparing community partitions.
	std::unordered_map<int64_t, int64_t> toMap(const std::vector<graph::query::algorithm::NodeCommunity> &v) {
		std::unordered_map<int64_t, int64_t> m;
		for (const auto &nc : v) m[nc.nodeId] = nc.communityId;
		return m;
	}

	size_t distinctCommunities(const std::vector<graph::query::algorithm::NodeCommunity> &v) {
		std::set<int64_t> s;
		for (const auto &nc : v) s.insert(nc.communityId);
		return s.size();
	}

	// Map NodeCommunity results back to a communityOf vector indexed by CSR order.
	std::vector<int64_t> toMapRes(const std::vector<graph::query::algorithm::NodeCommunity> &v,
								  const graph::query::algorithm::CsrProjection &csr) {
		std::unordered_map<int64_t, int64_t> m = toMap(v);
		std::vector<int64_t> out(csr.nodeCount());
		for (size_t i = 0; i < csr.nodeCount(); ++i) out[i] = m[csr.nodeIdAt(i)];
		return out;
	}

	TEST_F(LeidenTest, TwoTrianglesYieldTwoCommunities) {
		int64_t a = addNode(), b = addNode(), c = addNode();
		int64_t d = addNode(), e = addNode(), f = addNode();
		addEdge(a, b); addEdge(b, c); addEdge(c, a);
		addEdge(d, e); addEdge(e, f); addEdge(f, d);
		// One bridge edge so the graph is connected but clusterable.
		addEdge(a, d);
		reload();

		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);
		auto res = graph::query::algorithm::LeidenEngine::run(*csr);

		ASSERT_EQ(res.size(), 6u);
		auto m = toMap(res);
		EXPECT_EQ(distinctCommunities(res), 2u);
		EXPECT_EQ(m[a], m[b]);
		EXPECT_EQ(m[b], m[c]);
		EXPECT_EQ(m[d], m[e]);
		EXPECT_EQ(m[e], m[f]);
		EXPECT_NE(m[a], m[d]);
	}

	TEST_F(LeidenTest, EmptyGraphReturnsEmpty) {
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);
		auto res = graph::query::algorithm::LeidenEngine::run(*csr);
		EXPECT_TRUE(res.empty());
	}

	TEST_F(LeidenTest, SingleNodeOwnCommunity) {
		int64_t a = addNode();
		reload();
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);
		auto res = graph::query::algorithm::LeidenEngine::run(*csr);
		ASSERT_EQ(res.size(), 1u);
		EXPECT_EQ(res[0].nodeId, a);
		EXPECT_EQ(res[0].communityId, 0);
	}

	TEST_F(LeidenTest, IsolatedNodesFormOwnCommunities) {
		(void) addNode(); (void) addNode(); (void) addNode(); // no edges
		reload();
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);
		auto res = graph::query::algorithm::LeidenEngine::run(*csr);
		ASSERT_EQ(res.size(), 3u);
		EXPECT_EQ(distinctCommunities(res), 3u);
	}

	TEST_F(LeidenTest, ModularityNonDecreasingAcrossIterations) {
		int64_t a = addNode(), b = addNode(), c = addNode();
		int64_t d = addNode(), e = addNode(), f = addNode();
		addEdge(a, b); addEdge(b, c); addEdge(c, a);
		addEdge(d, e); addEdge(e, f); addEdge(f, d);
		addEdge(a, d);
		reload();
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);

		const size_t n = csr->nodeCount();
		std::vector<int64_t> init(n);
		for (size_t i = 0; i < n; ++i) init[i] = static_cast<int64_t>(i);
		double q0 = graph::query::algorithm::LeidenEngine::modularity(*csr, init);

		auto res = graph::query::algorithm::LeidenEngine::run(*csr);
		double qFinal = graph::query::algorithm::LeidenEngine::modularity(*csr, toMapRes(res, *csr));
		EXPECT_GE(qFinal, q0 - 1e-9);
	}

	TEST_F(LeidenTest, ParallelMatchesSerialCommunityCount) {
		// Two dense clusters bridged by one edge.
		std::vector<int64_t> left, right;
		for (int i = 0; i < 20; ++i) left.push_back(addNode());
		for (int i = 0; i < 20; ++i) right.push_back(addNode());
		for (int i = 0; i < 20; ++i)
			for (int j = i + 1; j < 20; ++j) {
				addEdge(left[i], left[j]);
				addEdge(right[i], right[j]);
			}
		addEdge(left[0], right[0]);
		reload();

		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csrSerial = graph::query::algorithm::CsrProjection::build(proj, nullptr);
		auto resSerial = graph::query::algorithm::LeidenEngine::run(*csrSerial);

		graph::concurrent::ThreadPool pool(4);
		auto csrPar = graph::query::algorithm::CsrProjection::build(proj, &pool);
		auto resPar = graph::query::algorithm::LeidenEngine::run(*csrPar, {}, &pool);

		ASSERT_EQ(resSerial.size(), resPar.size());
		EXPECT_EQ(distinctCommunities(resSerial), 2u);
		EXPECT_EQ(distinctCommunities(resPar), 2u);

		double qS = graph::query::algorithm::LeidenEngine::modularity(*csrSerial, toMapRes(resSerial, *csrSerial));
		double qP = graph::query::algorithm::LeidenEngine::modularity(*csrPar, toMapRes(resPar, *csrPar));
		EXPECT_NEAR(qS, qP, 1e-3);
	}

	// Helper moved into the fixture (needs addNode/addEdge).

	TEST_F(LeidenTest, LiftingImprovesModularity) {
		std::vector<int64_t> firsts;
		buildCliqueChain(6, 6, firsts); // 6 cliques of 6 nodes, chained
		reload();

		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);

		// Single-level (maxLevels = 1, no lifting).
		graph::query::algorithm::LeidenOptions single;
		single.maxLevels = 1;
		auto resSingle = graph::query::algorithm::LeidenEngine::run(*csr, single);
		double qSingle = graph::query::algorithm::LeidenEngine::modularity(*csr, toMapRes(resSingle, *csr));

		// With lifting (default maxLevels = 10).
		auto resLifted = graph::query::algorithm::LeidenEngine::run(*csr);
		double qLifted = graph::query::algorithm::LeidenEngine::modularity(*csr, toMapRes(resLifted, *csr));

		EXPECT_GT(qLifted, qSingle - 1e-9)
			<< "Lifting should reach modularity >= single-level";
	}

	TEST_F(LeidenTest, LiftingProducesFewerCommunitiesOrEqual) {
		std::vector<int64_t> firsts;
		buildCliqueChain(4, 8, firsts);
		reload();

		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);

		graph::query::algorithm::LeidenOptions single;
		single.maxLevels = 1;
		auto resSingle = graph::query::algorithm::LeidenEngine::run(*csr, single);
		auto resLifted = graph::query::algorithm::LeidenEngine::run(*csr);

		EXPECT_LE(distinctCommunities(resLifted), distinctCommunities(resSingle));
	}

	TEST_F(LeidenTest, MaxLevelsOneDegradesToSingleLevel) {
		std::vector<int64_t> firsts;
		buildCliqueChain(4, 6, firsts);
		reload();
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);

		graph::query::algorithm::LeidenOptions single;
		single.maxLevels = 1;
		auto a = graph::query::algorithm::LeidenEngine::run(*csr, single);
		// Two single-level runs are deterministic (same node order).
		auto b = graph::query::algorithm::LeidenEngine::run(*csr, single);
		ASSERT_EQ(a.size(), b.size());
		auto ma = toMap(a), mb = toMap(b);
		for (const auto &kv : ma) EXPECT_EQ(ma[kv.first], mb[kv.first]);
	}

	TEST_F(LeidenTest, TwoNodesNoEdgeLiftingSafe) {
		(void) addNode(); (void) addNode(); // isolated
		reload();
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);
		auto res = graph::query::algorithm::LeidenEngine::run(*csr);
		ASSERT_EQ(res.size(), 2u);
		EXPECT_EQ(distinctCommunities(res), 2u);
	}

	TEST_F(LeidenTest, LiftingParallelConsistentCommunityCount) {
		std::vector<int64_t> firsts;
		buildCliqueChain(6, 6, firsts);
		reload();
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);

		auto csrSerial = graph::query::algorithm::CsrProjection::build(proj, nullptr);
		auto resSerial = graph::query::algorithm::LeidenEngine::run(*csrSerial);

		graph::concurrent::ThreadPool pool(4);
		auto csrPar = graph::query::algorithm::CsrProjection::build(proj, &pool);
		auto resPar = graph::query::algorithm::LeidenEngine::run(*csrPar, {}, &pool);

		ASSERT_EQ(resSerial.size(), resPar.size());
		EXPECT_EQ(distinctCommunities(resSerial), distinctCommunities(resPar));
	}

	TEST_F(LeidenTest, LiftingMapsResultToOriginalNodeIds) {
		// Sanity: every returned nodeId must be one of the original node ids.
		std::vector<int64_t> firsts;
		buildCliqueChain(3, 5, firsts);
		reload();
		std::set<int64_t> origIds;
		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		for (size_t i = 0; i < proj.getNodeIds().size(); ++i)
			origIds.insert(proj.getNodeIds().begin(), proj.getNodeIds().end());

		auto csr = graph::query::algorithm::CsrProjection::build(proj);
		auto res = graph::query::algorithm::LeidenEngine::run(*csr);
		ASSERT_EQ(res.size(), origIds.size());
		for (const auto &nc : res) {
			EXPECT_EQ(origIds.count(nc.nodeId), 1u) << "unknown nodeId in result";
			EXPECT_GE(nc.communityId, 0);
		}
	}

	TEST_F(LeidenTest, CommunitiesAreInternallyConnected) {
		// Leiden's refinement guarantees every community is connected on the
		// level-0 graph. Build a graph that is prone to disconnected (broken)
		// communities under plain Louvain: two separate cliques weakly coupled to
		// a third clique, which greedy local-moving may absorb into one big but
		// disconnected community.
		std::vector<int64_t> c1, c2, c3;
		for (int i = 0; i < 8; ++i) c1.push_back(addNode());
		for (int i = 0; i < 8; ++i) c2.push_back(addNode());
		for (int i = 0; i < 8; ++i) c3.push_back(addNode());
		for (int i = 0; i < 8; ++i)
			for (int j = i + 1; j < 8; ++j) {
				addEdge(c1[i], c1[j]);
				addEdge(c2[i], c2[j]);
				addEdge(c3[i], c3[j]);
			}
		// Single weak bridges: c1—c2 and c2—c3 (c1 and c3 NOT directly connected).
		addEdge(c1[0], c2[0]);
		addEdge(c2[1], c3[0]);
		reload();

		auto proj = graph::query::algorithm::GraphProjection::build(dataManager);
		auto csr = graph::query::algorithm::CsrProjection::build(proj);
		auto res = graph::query::algorithm::LeidenEngine::run(*csr);

		ASSERT_FALSE(res.empty());

		// Build nodeId -> communityId map and an adjacency set on original node ids.
		std::unordered_map<int64_t, int64_t> commOf = toMap(res);
		std::unordered_map<int64_t, std::vector<int64_t>> adj;
		for (size_t i = 0; i < csr->nodeCount(); ++i) {
			int64_t u = csr->nodeIdAt(i);
			for (int64_t v : csr->neighbors(i)) adj[u].push_back(v);
		}

		// For each community, verify all its nodes form a single connected component
		// using only intra-community edges (BFS). This is the Leiden guarantee.
		std::unordered_map<int64_t, std::vector<int64_t>> byComm;
		for (const auto &nc : res) byComm[nc.communityId].push_back(nc.nodeId);

		for (const auto &kv : byComm) {
			const auto &members = kv.second;
			if (members.size() < 2) continue;
			std::unordered_set<int64_t> memberSet(members.begin(), members.end());
			std::unordered_set<int64_t> visited;
			std::vector<int64_t> frontier{members[0]};
			visited.insert(members[0]);
			while (!frontier.empty()) {
				int64_t u = frontier.back();
				frontier.pop_back();
				for (int64_t v : adj[u]) {
					if (memberSet.count(v) && !visited.count(v)) {
						visited.insert(v);
						frontier.push_back(v);
					}
				}
			}
			EXPECT_EQ(visited.size(), members.size())
				<< "community " << kv.first << " is internally disconnected";
		}
	}

} // namespace