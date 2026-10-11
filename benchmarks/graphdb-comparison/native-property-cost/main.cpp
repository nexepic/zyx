#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace graph;
using namespace graph::storage;
using Clock = std::chrono::steady_clock;

// Existing authoritative Property roots only: establish whether page grouping
// or value width dominates before introducing a replacement persistent layout.
int main(int argc, char **argv) {
 if (argc != 5 && argc != 6 && argc != 7 && argc != 8) throw std::invalid_argument("usage: native-property-cost PRIVATE_DB OWNERS FIELDS PAYLOAD_BYTES [PROJECTED_FIELDS] [batch-only] [write-cost]");
 const std::filesystem::path path(argv[1]);
 const size_t count = std::stoull(argv[2]), fields = std::stoull(argv[3]), payload = std::stoull(argv[4]);
 const size_t projected = argc >= 6 ? std::stoull(argv[5]) : 1U;
 const bool batchOnly = argc >= 7;
 const bool writeCost = argc == 8;
 if (writeCost && std::string_view(argv[7]) != "write-cost") throw std::invalid_argument("unknown write selection");
 if (batchOnly && std::string_view(argv[6]) != "batch-only") throw std::invalid_argument("unknown path selection");
 if ((writeCost && count < 14U) || !projected || projected > fields || !count || !fields || count > 100000 || fields > 256 || payload > 65536 || count * fields * std::max<size_t>(8, payload) > 256U * 1024U * 1024U || std::filesystem::exists(path))
  throw std::invalid_argument("positive bounded dimensions and a new private database required");
 struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove(path, e); } } cleanup{path};
 RelocatableStorageBackendOptions options; options.data_path = path; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
 auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
 std::vector<LogicalGraphNodeCreateRequest> nodes(count);
 for (size_t i = 0; i < count; ++i) {
  nodes[i].properties.push_back({"p0", PropertyValue(static_cast<int64_t>(i))});
  for (size_t column = 1; column < fields; ++column) {
   uint64_t state = (i + 1) * 1000003U + column;
   std::string text(payload, ' ');
   for (char &character : text) {
    state ^= state << 13U; state ^= state >> 7U; state ^= state << 17U;
    character = char('!' + state % 90U);
   }
   nodes[i].properties.push_back({"p" + std::to_string(column), PropertyValue(std::move(text))});
  }
 }
 std::vector<std::vector<PropertyValue>> expected(count);
 for (size_t i = 0; i < count; ++i) {
  expected[i].reserve(fields);
  for (size_t column = 0; column < fields; ++column) expected[i].push_back(nodes[i].properties[column].value);
 }
 // Fixture construction is outside timed reads. Bound each ordinary write
 // batch by both owner count and estimated property payload; keep production
 // transaction admission unchanged and use identical publication for A/B.
 const size_t bytesPerOwner = fields * (std::max<size_t>(8U, payload) + 64U);
 const size_t ownersPerWrite = std::max<size_t>(1U, std::min<size_t>(128U, (256U * 1024U) / bytesPerOwner));
 std::vector<uint64_t> ids;
 ids.reserve(count);
 for (size_t offset = 0; offset < count; offset += ownersPerWrite) {
  const size_t end = std::min(count, offset + ownersPerWrite);
  std::vector<LogicalGraphNodeCreateRequest> batch;
  batch.reserve(end - offset);
  for (size_t i = offset; i < end; ++i) batch.push_back(std::move(nodes[i]));
  const auto writeStart = Clock::now();
  const auto created = backend->createLogicalNodes(batch);
  const double writeMs = std::chrono::duration<double, std::milli>(Clock::now() - writeStart).count();
  if (writeCost) std::cout << "{\"event\":\"fixture_write\",\"owners\":" << batch.size() << ",\"milliseconds\":" << writeMs << "}\n";
  if (created.size() != batch.size()) throw std::runtime_error("fixture write count mismatch");
  ids.insert(ids.end(), created.begin(), created.end());
 }
 if (ids.size() != count || !std::ranges::is_sorted(ids)) throw std::runtime_error("unexpected node IDs");
 nodes.clear(); nodes.shrink_to_fit();
 std::cout << "{\"event\":\"fixture\",\"owners\":" << count << ",\"fields\":" << fields
           << ",\"owners_per_write\":" << ownersPerWrite << ",\"projected_fields\":" << projected << ",\"payload_bytes\":" << payload << ",\"file_bytes\":" << std::filesystem::file_size(path) << "}\n";
 std::vector<std::string> names;
 names.reserve(projected + 1U);
 for (size_t column = 0; column < projected; ++column) names.push_back("p" + std::to_string(column));
 names.push_back("missing");
 std::vector<std::string_view> columns(names.begin(), names.end());
 for (int round = -3; round <= 10; ++round) {
  for (int position = 0; position < 3; ++position) {
   const int mode = (round + 3 + position) % 3;
   if (batchOnly && mode == 1) continue;
   const char *name = mode == 0 ? "selected_batch" : mode == 1 ? "selected_point" : "full_batch";
   const bool diagnostic = round == 10;
   if (diagnostic) { debug::PerfTrace::reset(); debug::PerfTrace::setEnabled(true); debug::PerfTrace::setValueCollectionEnabled(true); }
   size_t checked = 0;
   const auto start = Clock::now();
   {
    auto snapshot = backend->openLogicalGraphReadSnapshot();
    for (size_t offset = 0; offset < count; offset += 256) {
     const auto batch = std::span<const uint64_t>(ids).subspan(offset, std::min<size_t>(256, count - offset));
     if (mode == 0) {
      const auto values = snapshot->readNodePropertyColumnsSorted(batch, columns);
      if (values.size() != columns.size()) throw std::runtime_error("column shape mismatch");
      for (const auto &column : values) if (column.size() != batch.size()) throw std::runtime_error("column owner shape mismatch");
      for (size_t i = 0; i < batch.size(); ++i) {
       for (size_t column = 0; column < projected; ++column)
        if (values[column][i] != expected[offset + i][column]) throw std::runtime_error("selected column result mismatch");
       if (values.back()[i] != PropertyValue()) throw std::runtime_error("selected missing result mismatch");
       ++checked;
      }
     } else if (mode == 1) {
      for (size_t i = 0; i < batch.size(); ++i) {
       for (size_t column = 0; column < projected; ++column)
        if (snapshot->readNodeProperty(batch[i], columns[column]) != expected[offset + i][column]) throw std::runtime_error("point result mismatch");
       if (snapshot->readNodeProperty(batch[i], "missing") != PropertyValue()) throw std::runtime_error("point missing result mismatch");
       ++checked;
      }
     } else {
      const auto values = snapshot->readNodesShapedSorted(batch, EntityReadShape{.all_properties = true});
      if (values.size() != batch.size()) throw std::runtime_error("entity shape mismatch");
      for (size_t i = 0; i < batch.size(); ++i) {
       if (values[i].properties.size() != fields) throw std::runtime_error("full field count mismatch");
       if (std::ranges::find(values[i].properties, std::string("missing"), &LogicalGraphPropertyEntry::key) != values[i].properties.end()) throw std::runtime_error("full missing projection mismatch");
       for (size_t column = 0; column < fields; ++column) {
        const auto found = std::ranges::find(values[i].properties, "p" + std::to_string(column), &LogicalGraphPropertyEntry::key);
        if (found == values[i].properties.end() || found->value != expected[offset + i][column]) throw std::runtime_error("full result mismatch");
       }
       ++checked;
      }
     }
    }
   }
   const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
   if (checked != count) throw std::runtime_error("owner count mismatch");
   if (diagnostic) {
    const auto trace = debug::PerfTrace::snapshotAndReset();
    debug::PerfTrace::setEnabled(false); debug::PerfTrace::setValueCollectionEnabled(false);
    for (const auto &[key, value] : trace)
     std::cout << "{\"event\":\"diagnostic\",\"path\":\"" << name << "\",\"key\":\"" << key
               << "\",\"value\":" << value.totalValue << ",\"calls\":" << value.calls << ",\"nanoseconds\":" << value.totalNs << "}\n";
   } else if (round >= 0) {
    std::cout << "{\"event\":\"sample\",\"path\":\"" << name << "\",\"round\":" << round
              << ",\"checked\":" << checked << ",\"milliseconds\":" << elapsed << "}\n";
   }
  }
 }
 if (writeCost) {
  if (count < 14U) throw std::invalid_argument("write-cost requires at least 14 owners");
  for (const char *operation : {"replace_one_field", "delete_owner"}) {
   for (int round = -3; round <= 10; ++round) {
    const size_t ordinal = static_cast<size_t>(round + 3);
    const uint64_t id = ids[ordinal];
    auto oldSnapshot = backend->openLogicalGraphReadSnapshot();
    const std::array one{id};
    const auto before = oldSnapshot->readNodesShapedSorted(one, EntityReadShape{.all_properties = true});
    if (before.size() != 1U || before[0].properties.size() != fields) throw std::runtime_error("write fixture shape mismatch");
    LogicalGraphPropertyReplaceRequest replacement;
    replacement.entity_id = id;
    for (const auto &entry : before[0].properties) replacement.properties.push_back({entry.key, entry.value});
    auto changed = std::ranges::find(replacement.properties, std::string("p0"), &LogicalGraphWriteProperty::key);
    if (changed == replacement.properties.end()) throw std::runtime_error("write fixture p0 missing");
    changed->value = PropertyValue(static_cast<int64_t>(count + ordinal));
    const bool deleting = std::string_view(operation) == "delete_owner";
    const bool diagnostic = round == 10;
    const auto fileBefore = std::filesystem::file_size(path);
    if (diagnostic) { debug::PerfTrace::reset(); debug::PerfTrace::setEnabled(true); debug::PerfTrace::setValueCollectionEnabled(true); }
    const auto start = Clock::now();
    if (deleting) backend->deleteLogicalNodes({LogicalGraphNodeDeleteRequest{.node_id = id}});
    else backend->replaceLogicalNodeProperties({replacement});
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (diagnostic) {
     const auto trace = debug::PerfTrace::snapshotAndReset();
     debug::PerfTrace::setEnabled(false); debug::PerfTrace::setValueCollectionEnabled(false);
     for (const auto &[key, value] : trace) std::cout << "{\"event\":\"write_diagnostic\",\"operation\":\"" << operation << "\",\"key\":\"" << key << "\",\"value\":" << value.totalValue << "}\n";
    }
    // Validate all fields and the old snapshot outside measured writes.
    const auto retained = oldSnapshot->readNodesShapedSorted(one, EntityReadShape{.all_properties = true});
    if (retained.size() != 1U || retained[0].properties != before[0].properties) throw std::runtime_error("write changed old snapshot");
    auto afterSnapshot = backend->openLogicalGraphReadSnapshot();
    if (deleting) {
     bool found = false;
     afterSnapshot->visitNodeRefs(count, [&](const EntityRef &ref) { if (ref.entity_id == id) found = true; return true; });
     if (found) throw std::runtime_error("deleted owner still visible");
    } else {
     const auto after = afterSnapshot->readNodesShapedSorted(one, EntityReadShape{.all_properties = true});
     if (after.size() != 1U || after[0].properties.size() != fields) throw std::runtime_error("updated field count mismatch");
     for (const auto &entry : replacement.properties) {
      const auto actual = std::ranges::find(after[0].properties, entry.key, &LogicalGraphPropertyEntry::key);
      if (actual == after[0].properties.end() || actual->value != entry.value) throw std::runtime_error("updated field value mismatch");
     }
    }
    if (round >= 0 && !diagnostic) std::cout << "{\"event\":\"write_sample\",\"operation\":\"" << operation << "\",\"round\":" << round << ",\"milliseconds\":" << ms << ",\"file_before\":" << fileBefore << ",\"file_after\":" << std::filesystem::file_size(path) << "}\n";
   }
  }
 }
 backend->close();
}
