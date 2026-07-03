/**
 * @file leiden_bench.cpp
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

// Leiden algorithm benchmark.
//
// Builds a synthetic graph (clique-chain or Erdős-Rényi random) directly via
// DataManager, builds the CSR projection, runs Leiden, and reports per-phase
// timings, peak RSS, final modularity and community count.
//
// Capped at 100k nodes so repeated runs stay cheap. Temp database files are
// cleaned up automatically (RAII + glob), unless --keep is passed.

#include <algorithm>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <sys/resource.h>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Database.hpp"
#include "graph/query/algorithm/CsrProjection.hpp"
#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/query/algorithm/LeidenEngine.hpp"

namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;

namespace {

// ---- CLI parsing (minimal, no external deps) ----

struct Args {
	std::string scale = "small";
	std::string graphType = "clique-chain"; // "clique-chain" | "random"
	size_t threadCount = 0;				  // 0 = auto
	bool keep = false;					  // keep temp db files
	bool cleanupOnly = false;			  // clean leftover files and exit
	bool help = false;
};

void printHelp() {
	std::cerr << "Usage: leiden-bench [options]\n"
			  << "  --scale <s>        smoke|small|medium|large (default small)\n"
			  << "  --graph <g>        clique-chain|random (default clique-chain)\n"
			  << "  --threads <n>      worker threads (default auto)\n"
			  << "  --keep             keep temp database files (default: clean up)\n"
			  << "  --cleanup-only     remove leftover leiden-bench temp files and exit\n"
			  << "  -h, --help         show this help\n";
}

Args parseArgs(int argc, char **argv) {
	Args a;
	for (int i = 1; i < argc; ++i) {
		std::string s = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) throw std::runtime_error("missing value for " + s);
			return argv[++i];
		};
		if (s == "-h" || s == "--help") a.help = true;
		else if (s == "--scale") a.scale = next();
		else if (s == "--graph") a.graphType = next();
		else if (s == "--threads") a.threadCount = static_cast<size_t>(std::stoll(next()));
		else if (s == "--keep") a.keep = true;
		else if (s == "--cleanup-only") a.cleanupOnly = true;
		else throw std::runtime_error("unknown argument: " + s);
	}
	return a;
}

size_t nodeCountForScale(const std::string &scale) {
	if (scale == "smoke") return 1'000;
	if (scale == "small") return 10'000;
	if (scale == "medium") return 50'000;
	if (scale == "large") return 100'000;
	throw std::runtime_error("unknown scale: " + scale + " (smoke|small|medium|large)");
}

// ---- peak RSS via getrusage (BSD/macOS: ru_maxrss in KiB) ----

size_t peakRssKb() {
	rusage ru {};
	getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
	// macOS: ru_maxrss is in bytes; convert to KiB.
	return static_cast<size_t>(ru.ru_maxrss) / 1024;
#else
	// Linux: ru_maxrss is already in KiB.
	return static_cast<size_t>(ru.ru_maxrss);
#endif
}

double nowSec() {
	return std::chrono::duration<double>(clock_type::now().time_since_epoch()).count();
}

// ---- Temp database with RAII cleanup ----

constexpr const char *TEMP_PREFIX = "leiden_bench_";

std::string tempDbPath() {
	auto uuid = boost::uuids::random_generator()();
	auto name = std::string(TEMP_PREFIX) + to_string(uuid) + ".dat";
	return (fs::temp_directory_path() / name).string();
}

// Remove the main .dat file plus any sibling artifacts (.dat-wal, etc.).
void cleanupFiles(const std::string &dbPath) {
	if (dbPath.empty()) return;
	fs::path base(dbPath);
	fs::path dir = base.parent_path();
	std::string stem = base.filename().string(); // e.g. leiden_bench_xxx.dat
	std::error_code ec;
	for (auto &entry : fs::directory_iterator(dir)) {
		auto fname = entry.path().filename().string();
		if (fname.rfind(stem, 0) == 0) fs::remove(entry.path(), ec);
	}
}

// Sweep the temp dir for any leftover leiden_bench_*.dat* files (from crashed
// runs) and remove them. Used by --cleanup-only and as a pre-run safety sweep.
size_t sweepLeftovers() {
	size_t removed = 0;
	std::error_code ec;
	for (auto &entry : fs::directory_iterator(fs::temp_directory_path())) {
		auto fname = entry.path().filename().string();
		if (fname.rfind(TEMP_PREFIX, 0) == 0) {
			if (fs::remove(entry.path(), ec)) ++removed;
		}
	}
	return removed;
}

struct TempDatabase {
	std::string path;
	graph::Database db;
	bool keep;

	TempDatabase(bool keep) : path(tempDbPath()), db(path), keep(keep) { db.open(); }
	~TempDatabase() {
		// Close first so WAL is removed (CloseMode::WCM_REMOVE_FILE) and handles
		// are released, then delete any remaining files.
		try {
			db.close();
		} catch (...) { /* best effort */ }
		if (!keep) cleanupFiles(path);
	}
};

// ---- graph builders ----

struct GraphHandle {
	size_t nodes;
	size_t edges;
};

// Clique-chain: nodeCount nodes split into groups of size K (10 each), fully
// connected within a group, single bridge edge between consecutive groups.
// Highly clusterable — verifies Leiden recovers the clique structure.
GraphHandle buildCliqueChain(graph::storage::DataManager &dm, size_t nodeCount) {
	constexpr size_t K = 10;
	int64_t nLabel = dm.getOrCreateTokenId("N");
	int64_t etype = dm.getOrCreateTokenId("E");
	std::vector<int64_t> ids;
	ids.reserve(nodeCount);
	for (size_t i = 0; i < nodeCount; ++i) {
		int64_t id = dm.getIdAllocator(graph::EntityType::Node)->allocate();
		graph::Node node(id, nLabel);
		dm.addNode(node);
		ids.push_back(id);
	}
	size_t edges = 0;
	int64_t prevLast = -1;
	for (size_t g = 0; g < nodeCount; g += K) {
		size_t end = std::min(g + K, nodeCount);
		for (size_t i = g; i < end; ++i)
			for (size_t j = i + 1; j < end; ++j) {
				int64_t eid = dm.getIdAllocator(graph::EntityType::Edge)->allocate();
				graph::Edge e(eid, ids[i], ids[j], etype);
				dm.addEdge(e);
				++edges;
			}
		if (prevLast >= 0 && g < nodeCount) {
			int64_t eid = dm.getIdAllocator(graph::EntityType::Edge)->allocate();
			graph::Edge e(eid, ids[prevLast], ids[g], etype);
			dm.addEdge(e);
			++edges;
		}
		if (end > g) prevLast = static_cast<int64_t>(ids[end - 1]);
	}
	return {nodeCount, edges};
}

// Erdős–Rényi random graph with target average degree 6 (p = 6/N). Weak
// community structure — exercises the algorithm's cost when little merging
// occurs. Edges are undirected (added once per pair).
GraphHandle buildRandomGraph(graph::storage::DataManager &dm, size_t nodeCount) {
	std::mt19937 rng(42); // fixed seed for reproducibility
	int64_t nLabel = dm.getOrCreateTokenId("N");
	int64_t etype = dm.getOrCreateTokenId("E");
	std::vector<int64_t> ids;
	ids.reserve(nodeCount);
	for (size_t i = 0; i < nodeCount; ++i) {
		int64_t id = dm.getIdAllocator(graph::EntityType::Node)->allocate();
		graph::Node node(id, nLabel);
		dm.addNode(node);
		ids.push_back(id);
	}
	const double avgDeg = 6.0;
	const double p = (nodeCount > 1) ? avgDeg / static_cast<double>(nodeCount - 1) : 0.0;
	// Sample per candidate pair; for large N this is O(N^2) draws which is too
	// slow at 100k. Use the binomial shortcut: pick expected edges via a
	// geometric draw over pair offsets.
	size_t edges = 0;
	if (nodeCount >= 2) {
		const double totalPairs = static_cast<double>(nodeCount) * (nodeCount - 1) / 2.0;
		(void) p; // documented above; draws below use geom(p)
		std::geometric_distribution<size_t> geom(p);
		size_t pairIndex = geom(rng);
		while (pairIndex < static_cast<size_t>(totalPairs)) {
			// Map linear pairIndex back to (i,j) with i<j.
			// pairIndex = i*(N-1) - i*(i+1)/2 + (j-i-1)
			size_t lo = 0, hi = nodeCount - 1;
			while (lo < hi) {
				size_t mid = (lo + hi) / 2;
				size_t startOfMid = mid * (2 * (nodeCount - 1) - mid + 1) / 2;
				if (startOfMid <= pairIndex) lo = mid + 1;
				else hi = mid;
			}
			size_t i = (lo > 0) ? lo - 1 : 0;
			size_t startOfI = i * (2 * (nodeCount - 1) - i + 1) / 2;
			size_t j = i + 1 + (pairIndex - startOfI);
			if (j < nodeCount) {
				int64_t eid = dm.getIdAllocator(graph::EntityType::Edge)->allocate();
				graph::Edge e(eid, ids[i], ids[j], etype);
				dm.addEdge(e);
				++edges;
			}
			pairIndex += 1 + geom(rng);
		}
	}
	return {nodeCount, edges};
}

} // namespace

int main(int argc, char **argv) {
	Args args;
	try {
		args = parseArgs(argc, argv);
	} catch (const std::exception &e) {
		std::cerr << "error: " << e.what() << "\n";
		printHelp();
		return 2;
	}
	if (args.help) {
		printHelp();
		return 0;
	}

	if (args.cleanupOnly) {
		size_t removed = sweepLeftovers();
		std::cout << "cleanup removed " << removed << " leftover file(s) from "
				  << fs::temp_directory_path().string() << "\n";
		return 0;
	}

	size_t nodeCount = nodeCountForScale(args.scale);

	// Pre-run safety sweep of stale files (cheap, idempotent).
	(void) sweepLeftovers();

	std::cout << "leiden-bench scale=" << args.scale
			  << " graph=" << args.graphType
			  << " nodes=" << nodeCount
			  << " threads=" << args.threadCount << "\n";

	TempDatabase tdb(args.keep);
	std::shared_ptr<graph::storage::DataManager> dm = tdb.db.getStorage()->getDataManager();

	// ---- Phase 1: build synthetic graph ----
	double t0 = nowSec();
	GraphHandle gh{0, 0};
	if (args.graphType == "clique-chain") gh = buildCliqueChain(*dm, nodeCount);
	else if (args.graphType == "random") gh = buildRandomGraph(*dm, nodeCount);
	else {
		std::cerr << "error: unknown graph type (clique-chain|random)\n";
		return 2;
	}
	// flush + reopen so the topology is visible to all subsystems.
	tdb.db.getStorage()->flush();
	tdb.db.close();
	tdb.db.open();
	dm = tdb.db.getStorage()->getDataManager();
	double buildSec = nowSec() - t0;
	std::cout << "result phase=build seconds=" << buildSec
			  << " nodes=" << gh.nodes << " edges=" << gh.edges << "\n";

	// ---- Phase 2: GraphProjection + CSR build ----
	double t1 = nowSec();
	auto proj = graph::query::algorithm::GraphProjection::build(dm);
	std::unique_ptr<graph::concurrent::ThreadPool> pool;
	graph::concurrent::ThreadPool *poolPtr = nullptr;
	if (args.threadCount != 1) {
		pool = std::make_unique<graph::concurrent::ThreadPool>(args.threadCount);
		poolPtr = pool.get();
	}
	auto csr = graph::query::algorithm::CsrProjection::build(proj, poolPtr);
	double csrSec = nowSec() - t1;
	std::cout << "result phase=csr seconds=" << csrSec
			  << " csr_nodes=" << csr->nodeCount()
			  << " csr_edges=" << csr->edgeCount() << "\n";

	size_t rssBeforeLeiden = peakRssKb();

	// ---- Phase 3: Leiden run ----
	double t2 = nowSec();
	graph::query::algorithm::LeidenOptions opts;
	auto res = graph::query::algorithm::LeidenEngine::run(*csr, opts, poolPtr);
	double leidenSec = nowSec() - t2;

	// ---- Phase 4: modularity + community count ----
	std::vector<int64_t> commOf(csr->nodeCount());
	std::unordered_map<int64_t, int64_t> toCsr;
	for (size_t i = 0; i < csr->nodeCount(); ++i) toCsr[csr->nodeIdAt(i)] = static_cast<int64_t>(i);
	for (const auto &nc : res) commOf[static_cast<size_t>(toCsr[nc.nodeId])] = nc.communityId;
	double q = graph::query::algorithm::LeidenEngine::modularity(*csr, commOf);
	std::set<int64_t> commSet;
	for (const auto &nc : res) commSet.insert(nc.communityId);

	size_t rssPeak = peakRssKb();
	long rssDeltaKb = (rssPeak > rssBeforeLeiden) ? static_cast<long>(rssPeak - rssBeforeLeiden) : 0;

	std::cout << "result phase=leiden seconds=" << leidenSec
			  << " communities=" << commSet.size()
			  << " modularity=" << q << "\n";
	std::cout << "result peak_rss_kb=" << rssPeak
			  << " leiden_delta_rss_kb=" << rssDeltaKb
			  << " (delta=0 means algorithm fit under existing peak)\n";
	std::cout << "result total_seconds=" << (buildSec + csrSec + leidenSec) << "\n";

	return 0;
}