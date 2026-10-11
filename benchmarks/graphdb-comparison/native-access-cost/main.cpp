#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/debug/PerfTrace.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace graph;
using namespace graph::storage;
using Clock = std::chrono::steady_clock;

// Compare identical predicates through the exhaustive and candidate contracts.
// Each timed read owns a fresh snapshot; loading and initialization are separate.
int main(int argc, char **argv) {
 if (argc == 5 && std::string(argv[2]) == "verify") {
  RelocatableStorageBackendOptions options; options.data_path = argv[1];
  options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
  RelocatableStorageBackend backend(options); backend.open();
  const size_t expectedNodes = std::stoull(argv[3]), expectedEdges = std::stoull(argv[4]);
  size_t nodes = 0, edges = 0;
  {
   auto snapshot = backend.openLogicalGraphReadSnapshot();
   snapshot->visitNodeRefsWithLabelMatches(std::max<size_t>(1, expectedNodes), {},
       [&](const EntityRef &, bool) { ++nodes; return true; });
   snapshot->visitEdgeRefs(std::max<size_t>(1, expectedEdges),
       [&](const EntityRef &) { ++edges; return true; });
  }
  backend.close();
  if (nodes != expectedNodes || edges != expectedEdges) throw std::runtime_error("loaded primary graph count mismatch");
  std::cout << "{\"nodes\":" << nodes << ",\"edges\":" << edges << "}\n";
  return 0;
 }
 if (argc != 4 && argc != 6 && argc != 7) throw std::invalid_argument("usage: native-access-cost DB_PATH RECORD_COUNT clustered|scattered [edges PERCENT]");
 const bool edges = argc >= 6;
 const bool candidateBudget = argc == 7 && std::string(argv[6]) == "candidate-budget";
 if (edges && std::string(argv[4]) != "edges") throw std::invalid_argument("unsupported entity kind");
 const std::filesystem::path path(argv[1]);
 const size_t count = std::stoull(argv[2]);
 const bool scattered = std::string(argv[3]) == "scattered";
 if (count == 0 || (std::string(argv[3]) != "clustered" && !scattered))
  throw std::invalid_argument("positive node count and a supported distribution are required");
 if (std::filesystem::exists(path)) throw std::invalid_argument("benchmark requires a new private file");
 struct Cleanup {
  std::filesystem::path path;
  ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
 } cleanup{path};
 RelocatableStorageBackendOptions options;
 options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 const auto start = Clock::now();
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 const std::vector<size_t> percentages = edges ? std::vector<size_t>{std::stoull(argv[5])} :
  std::vector<size_t>{1, 10, 25, 50, 75, 100};
 if (percentages.front() == 0 || percentages.front() > 100) throw std::invalid_argument("invalid percentage");
 std::vector<size_t> expected(percentages.size(), 0);
 std::vector<LogicalGraphNodeCreateRequest> nodes;
 nodes.reserve(count);
 for (size_t i = 0; i < (edges ? 2U : count); ++i) {
  LogicalGraphNodeCreateRequest node;
  const size_t rank = scattered ? (i * 97U) % count : i;
  for (size_t p = 0; p < percentages.size(); ++p) {
   if (!edges && rank < count * percentages[p] / 100U) {
    node.labels.push_back("L" + std::to_string(percentages[p])); ++expected[p];
   }
  }
  nodes.push_back(std::move(node));
 }
 const auto ids = backend->createLogicalNodes(nodes);
 if (ids.size() != (edges ? 2U : count)) throw std::runtime_error("load count mismatch");
 if (edges) {
  std::vector<LogicalGraphEdgeCreateRequest> requests; requests.reserve(count);
  for (size_t i = 0; i < count; ++i) {
   const size_t rank = scattered ? (i * 97U) % count : i;
   const bool match = rank < count * percentages.front() / 100U;
   requests.push_back({ids[0], ids[1], match ? "Selected" : "Other", {}});
   expected[0] += match ? 1U : 0U;
  }
  if (backend->createLogicalEdges(requests).size() != count) throw std::runtime_error("edge load mismatch");
 }
 std::cout << "{\"event\":\"load\",\"nodes\":" << count
  << ",\"distribution\":\"" << argv[3] << "\",\"milliseconds\":"
  << std::chrono::duration<double, std::milli>(Clock::now() - start).count()
  << ",\"file_bytes\":" << std::filesystem::file_size(path) << "}\n";
 for (size_t p = 0; p < percentages.size(); ++p) {
  const std::vector<std::string> labels{"L" + std::to_string(percentages[p])};
  for (int round = -2; round <= 10; ++round) {
   for (int position = 0; position < 2; ++position) {
    const bool candidate = ((round + 2 + position) % 2) != 0;
    const bool diagnostic = round == 10;
    if (diagnostic) { debug::PerfTrace::reset(); debug::PerfTrace::setValueCollectionEnabled(true); }
    size_t matched = 0, visited = 0;
    const auto before = Clock::now();
    {
     auto snapshot = backend->openLogicalGraphReadSnapshot();
     const auto visitor = [&](const EntityRef &, bool match) {
      ++visited; matched += match ? 1U : 0U; return true;
     };
     if (edges) {
      const auto token = snapshot->findRelationshipTypeToken("Selected");
      if (!token) throw std::runtime_error("missing selected type");
      const auto edgeVisitor = [&](const EntityRef &ref) { return visitor(ref, ref.type_token == *token); };
      if (candidate) snapshot->visitEdgeRefsForTypePredicate(candidateBudget ? expected[p] : count, token, edgeVisitor);
      else snapshot->visitEdgeRefs(count, edgeVisitor);
     } else {
      if (candidate) snapshot->visitNodeRefsForLabelPredicate(count, labels, visitor);
      else snapshot->visitNodeRefsWithLabelMatches(count, labels, visitor);
     }
    }
    const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
    if (matched != expected[p]) throw std::runtime_error("predicate count mismatch");
    if (diagnostic) {
     const auto counters = debug::PerfTrace::snapshotAndReset();
     debug::PerfTrace::setValueCollectionEnabled(false);
     for (const auto &[key, entry] : counters) {
      std::cout << "{\"event\":\"counter\",\"nodes\":" << count
       << ",\"distribution\":\"" << argv[3] << "\",\"percent\":" << percentages[p]
       << ",\"path\":\"" << (candidate ? "candidate" : "scan")
       << "\",\"key\":\"" << key << "\",\"value\":" << entry.totalValue << "}\n";
     }
    }
    if (round >= 0 && !diagnostic) std::cout << "{\"event\":\"sample\",\"nodes\":" << count
     << ",\"distribution\":\"" << argv[3] << "\",\"percent\":" << percentages[p]
     << ",\"path\":\"" << (candidate ? "candidate" : "scan")
     << "\",\"round\":" << round << ",\"matches\":" << matched
     << ",\"visited\":" << visited << ",\"milliseconds\":" << elapsed << "}\n";
   }
  }
 }
 backend->close();
}
