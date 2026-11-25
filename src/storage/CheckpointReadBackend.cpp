// Immutable cp002 storage adapter. Query semantics belong to the common logical executor.
#include "CheckpointReadBackend.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <tuple>
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/utils/Sha256.hpp"
#include "query/execution/ProcessResourceDomains.hpp"

namespace graph::storage {
	namespace {
		using Record = std::array<unsigned char, 40>;

		uint64_t word(const unsigned char *p) {
			uint64_t v = 0;
			for (unsigned i = 0; i < 8; ++i)
				v |= uint64_t(p[i]) << (8 * i);
			return v;
		}

		void require(bool ok, const char *reason) {
			if (!ok)
				throw std::runtime_error(reason);
		}

		PropertyValue cell(unsigned tag, uint64_t bits) {
			switch (tag) {
				case 1:
					return {};
				case 2:
					return std::bit_cast<int64_t>(bits);
				case 3:
					if (bits < 2)
						return bool(bits);
					break;
				case 4:
					if (std::isfinite(std::bit_cast<double>(bits)))
						return std::bit_cast<double>(bits);
					break;
			}
			throw std::runtime_error("Invalid cp002 property");
		}

		// File offsets never escape this object. Read and authenticate the exact private
		// bytes used by every subsequent cursor, including on mutable native filesystems.
		namespace resource = query::execution::detail;
		struct Image {
			std::shared_ptr<resource::DatabaseResourceBudget> domain;
			std::optional<resource::ResourceLease> reservation;
			LogicalGraphMutationFence fence;
			std::vector<Record> records;
			std::vector<uint32_t> nodes, edges, outgoing, incoming;
			uint64_t field(uint32_t i, size_t off) const { return word(records[i].data() + off); }
			uint64_t nid(uint32_t i) const { return field(i, 12); }
			uint64_t eid(uint32_t i) const { return field(i, 4); }
			auto nodeAt(uint64_t id) const {
				return std::lower_bound(nodes.begin(), nodes.end(), id,
										[&](auto i, auto target) { return nid(i) < target; });
			}
			bool hasNode(uint64_t id) const {
				auto it = nodeAt(id);
				return it != nodes.end() && nid(*it) == id;
			}
			LogicalGraphNode node(uint32_t i) const {
				LogicalGraphNode n{nid(i), {"Item"}, {{"id", std::bit_cast<int64_t>(field(i, 4))}}};
				const auto &r = records[i];
				if (r[1])
					n.properties.push_back({"val", cell(r[1], field(i, 20))});
				if (r[2])
					n.properties.push_back({"grp", cell(r[2], field(i, 28))});
				return n;
			}
			LogicalGraphEdge edge(uint32_t i) const {
				return {eid(i), field(i, 12), field(i, 20), "Link", {{"w", std::bit_cast<int64_t>(field(i, 28))}}};
			}
			Image(const std::string &path, size_t budget) {
				std::ifstream in(path, std::ios::binary | std::ios::ate);
				const auto end = in.tellg();
				require(end >= 88 && end <= 64 * 1024 * 1024, "Invalid cp002 file extent");
				in.seekg(0);
				std::array<unsigned char, 88> h{};
				require(bool(in.read(reinterpret_cast<char *>(h.data()), h.size())) &&
								std::memcmp(h.data(), "ZYXcp002", 8) == 0,
						"Invalid cp002 header");
				const uint64_t length = word(h.data() + 48), size = static_cast<uint64_t>(end);
				require(length <= size - 88 && length % 40 == 0, "Incomplete cp002 payload");
				const uint64_t extent = (88 + length + 4095) & ~uint64_t(4095);
				require(size >= extent, "Incomplete cp002 checkpoint padding");
				require(size == extent, "cp002 main requires its WAL or native recovery");
				const size_t count = length / 40;
				// At most three index entries per record (one edge index plus both directions).
				require(count * (sizeof(Record) + 3 * sizeof(uint32_t)) <= budget,
						"cp002 snapshot exceeds storage cache budget");
				std::string identity("cp002:");
				identity.append(reinterpret_cast<const char *>(h.data() + 8), 16);
				domain = resource::ProcessResourceDomains::defaultOwner()->domain(std::move(identity));
				auto account =
						std::make_shared<resource::ResourceAccount>(domain, resource::ResourceClass::RCK_CACHE, budget);
				reservation.emplace(std::move(account));
				reservation->set(count * (sizeof(Record) + 3 * sizeof(uint32_t)));
				records.resize(count);
				if (length)
					require(bool(in.read(reinterpret_cast<char *>(records.data()), length)), "Short cp002 checkpoint");
				utils::Sha256 hash;
				hash.update(std::as_bytes(std::span(h).first(56)));
				hash.update(std::as_bytes(std::span(records)));
				require(std::memcmp(hash.finalize().data(), h.data() + 56, 32) == 0,
						"cp002 checkpoint checksum mismatch");
				// Only a read-only view of these exact authenticated bytes can expose this fence.
				fence.backend_token = 0x6370303032ULL;
				fence.format_token = 2;
				for (unsigned i = 0; i < 4; ++i)
					fence.graph_state_tokens[i] = word(h.data() + 56 + 8 * i);
				for (uint64_t pos = 88 + length; pos < size; ++pos)
					require(in.get() == 0, "Invalid cp002 padding");
				require(in.peek() == std::char_traits<char>::eof() && !in.bad(), "cp002 extent changed");
				const auto nodeCount =
						std::count_if(records.begin(), records.end(), [](const auto &r) { return r[0] == 1; });
				const auto edgeCount =
						std::count_if(records.begin(), records.end(), [](const auto &r) { return r[0] == 2; });
				nodes.reserve(nodeCount);
				edges.reserve(edgeCount);
				for (uint32_t i = 0; i < count; ++i) {
					const auto &r = records[i];
					require(i == 0 || records[i - 1][0] <= r[0], "Invalid cp002 record order");
					if (r[0] == 1) {
						require(nid(i) > 0 && nid(i) <= word(h.data() + 32) && nid(i) <= INT64_MAX,
								"Invalid cp002 node ID");
						if (!nodes.empty())
							require(std::bit_cast<int64_t>(eid(nodes.back())) < std::bit_cast<int64_t>(eid(i)),
									"Invalid cp002 key order");
						if (r[1])
							(void) cell(r[1], field(i, 20));
						if (r[2])
							(void) cell(r[2], field(i, 28));
						nodes.push_back(i);
					} else if (r[0] == 2) {
						require(eid(i) > 0 && eid(i) <= word(h.data() + 40) && eid(i) <= INT64_MAX &&
										(edges.empty() || eid(edges.back()) < eid(i)),
								"Invalid cp002 edge ID");
						edges.push_back(i);
					} else {
						require(r[0] == 3 && (i == 0 || records[i - 1][0] != 3 ||
											  std::memcmp(records[i - 1].data() + 4, r.data() + 4, 16) < 0),
								"Invalid cp002 receipt order");
					}
				}
				std::sort(nodes.begin(), nodes.end(), [&](auto a, auto b) { return nid(a) < nid(b); });
				for (size_t i = 1; i < nodes.size(); ++i)
					require(nid(nodes[i - 1]) != nid(nodes[i]), "Duplicate cp002 node ID");
				for (auto i: edges)
					require(hasNode(field(i, 12)) && hasNode(field(i, 20)), "Invalid cp002 edge endpoint");
				auto buildDirection = [&](auto &index, size_t off) {
					index = edges;
					std::sort(index.begin(), index.end(), [&](auto a, auto b) {
						return std::pair(field(a, off), eid(a)) < std::pair(field(b, off), eid(b));
					});
				};
				buildDirection(outgoing, 12);
				buildDirection(incoming, 20);
			}
		};
		class Snapshot final : public LogicalGraphReadSnapshot {
			std::shared_ptr<const Image> img;
			auto adjacent(uint64_t id, LogicalGraphReadDirection dir) const {
				require(img->hasNode(id), "Unknown cp002 node");
				const bool out = dir == LogicalGraphReadDirection::LGRD_OUTGOING;
				require(out || dir == LogicalGraphReadDirection::LGRD_INCOMING, "Invalid adjacency direction");
				const auto &v = out ? img->outgoing : img->incoming;
				const auto offset = out ? 12 : 20;
				return std::ranges::equal_range(v, id, {}, [&](auto i) { return img->field(i, offset); });
			}

			template<class Range, class Visitor>
			static void visitComplete(const Range &range, size_t limit, Visitor &&visitor) {
				for (auto i: range) {
					require(limit > 0, "cp002 scan exceeded its record bound before completion");
					--limit;
					if (!visitor(i))
						return;
				}
			}

		public:
			explicit Snapshot(std::shared_ptr<const Image> data) : img(std::move(data)) {}
			std::optional<LogicalGraphMutationFence> mutationFence() const override { return img->fence; }
			bool containsNode(uint64_t id) const override { return img->hasNode(id); }
			bool supportsNodeExistenceProbes() const override { return true; }
			LogicalGraphNode readNode(uint64_t id) const override {
				auto it = img->nodeAt(id);
				require(it != img->nodes.end() && img->nid(*it) == id, "Unknown cp002 node");
				return img->node(*it);
			}
			LogicalGraphEdge readEdge(uint64_t id) const override {
				auto it = std::ranges::lower_bound(img->edges, id, {}, [&](auto i) { return img->eid(i); });
				require(it != img->edges.end() && img->eid(*it) == id, "Unknown cp002 edge");
				return img->edge(*it);
			}
			bool supportsPropertyReadAdmission() const override { return true; }
			using Columns = std::vector<std::vector<PropertyValue>>;
			Columns columns(bool edge, std::span<const uint64_t> ids, std::span<const std::string_view> keys,
							const std::function<void(size_t)> &admit) const {
				require(bool(admit), "Missing cp002 admission callback");
				Columns result(keys.size());
				for (auto &col: result)
					col.reserve(ids.size());
				uint64_t previous = 0;
				for (auto id: ids) {
					require(id > previous, "Unsorted cp002 property owners");
					previous = id;
					const auto &owners = edge ? img->edges : img->nodes;
					const auto it = std::ranges::lower_bound(
							owners, id, {}, [&](uint32_t i) { return edge ? img->eid(i) : img->nid(i); });
					require(it != owners.end() && (edge ? img->eid(*it) : img->nid(*it)) == id,
							"Unknown cp002 property owner");
					admit(sizeof(Record));
					const auto &r = img->records[*it];
					for (size_t col = 0; col < keys.size(); ++col) {
						auto key = keys[col];
						PropertyValue value;
						if (edge && key == "w")
							value = std::bit_cast<int64_t>(img->field(*it, 28));
						if (!edge && key == "id")
							value = std::bit_cast<int64_t>(img->field(*it, 4));
						if (!edge && key == "val" && r[1])
							value = cell(r[1], img->field(*it, 20));
						if (!edge && key == "grp" && r[2])
							value = cell(r[2], img->field(*it, 28));
						result[col].push_back(std::move(value));
					}
				}
				return result;
			}
			Columns
			readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
													   std::span<const std::string_view> keys,
													   const std::function<void(size_t)> &admit) const override {
				return columns(false, ids, keys, admit);
			}
			Columns
			readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
													   std::span<const std::string_view> keys,
													   const std::function<void(size_t)> &admit) const override {
				return columns(true, ids, keys, admit);
			}
			bool supportsShapedPropertyReadAdmission() const override { return true; }
			std::vector<LogicalGraphNode>
			readNodesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &s,
											   const std::function<void(size_t)> &admit) const override {
				admit(detail::checkedLogicalGraphCandidateBytesMul(ids.size(), sizeof(Record)));
				return readNodesShapedSorted(ids, s);
			}
			std::vector<LogicalGraphEdge>
			readEdgesShapedSortedWithAdmission(std::span<const uint64_t> ids, const EntityReadShape &s,
											   const std::function<void(size_t)> &admit) const override {
				admit(detail::checkedLogicalGraphCandidateBytesMul(ids.size(), sizeof(Record)));
				return readEdgesShapedSorted(ids, s);
			}
			void visitNodes(size_t max, const std::function<bool(const LogicalGraphNode &)> &f) const override {
				require(max > 0 && bool(f), "Invalid Node visit bound");
				visitComplete(img->nodes, max, [&](auto i) { return f(img->node(i)); });
			}
			void visitEdges(size_t max, const std::function<bool(const LogicalGraphEdge &)> &f) const override {
				require(max > 0 && bool(f), "Invalid Edge visit bound");
				visitComplete(img->edges, max, [&](auto i) { return f(img->edge(i)); });
			}
			LogicalGraphNodePage scanNodesAfter(uint64_t after, size_t max, size_t pages) const override {
				require(max && pages, "Invalid node scan bound");
				auto it = std::ranges::upper_bound(img->nodes, after, {}, [&](auto i) { return img->nid(i); });
				LogicalGraphNodePage page;
				// One charged directory window is at most 4 KiB of uint32 offsets.
				const size_t slotsLeft = 1024U - static_cast<size_t>(it - img->nodes.begin()) % 1024U;
				page.metadata_pages_visited = it == img->nodes.end() ? 0U : 1U;
				while (it != img->nodes.end() && page.nodes.size() < std::min({max, size_t(128), slotsLeft}))
					page.nodes.push_back(img->node(*it++));
				page.may_have_more = it != img->nodes.end();
				return page;
			}
			LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t id, LogicalGraphReadDirection dir,
														std::optional<LogicalGraphAdjacencyContinuation> cont,
														size_t max) const override {
				require(max > 0, "Invalid adjacency page bound");
				auto range = adjacent(id, dir);
				auto it = range.begin();
				if (cont) {
					it = std::ranges::lower_bound(range, cont->next_edge_id, {}, [&](auto i) { return img->eid(i); });
					require(it != range.begin() && it != range.end() && img->eid(*it) == cont->next_edge_id &&
									img->eid(*(it - 1)) == cont->previous_edge_id,
							"Invalid adjacency continuation");
				}
				LogicalGraphAdjacencyPage page;
				while (it != range.end() && page.edges.size() < std::min(max, size_t(128)))
					page.edges.push_back(img->edge(*it++));
				if (it != range.end())
					page.continuation = {{page.edges.back().edge_id, img->eid(*it)}};
				return page;
			}
			void visitAdjacentEdges(uint64_t id, LogicalGraphReadDirection dir, size_t max,
									const std::function<bool(const LogicalGraphEdge &)> &f) const override {
				require(max > 0 && bool(f), "Invalid Adjacency visit bound");
				visitComplete(adjacent(id, dir), max, [&](auto i) { return f(img->edge(i)); });
			}
		};
		class ReadTxn final : public LogicalGraphTransaction {
			Snapshot snapshot;
			LogicalGraphTransactionState status = LogicalGraphTransactionState::LGTTS_ACTIVE;
			void active() const { require(status == LogicalGraphTransactionState::LGTTS_ACTIVE, "Transaction ended"); }

		public:
			explicit ReadTxn(std::shared_ptr<const Image> img) : snapshot(std::move(img)) {}
			LogicalGraphTransactionState state() const override { return status; }
			bool isReadOnly() const override { return true; }
			LogicalGraphReadSnapshot &readSnapshot() override {
				active();
				return snapshot;
			}
			LogicalGraphMutationBatchResult previewMutationBatch(const LogicalGraphMutationBatch &) const override {
				throw std::logic_error("Read-only checkpoint");
			}
			LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch) override {
				throw std::logic_error("Read-only checkpoint");
			}
			LogicalGraphMutationBatchResult commit() override {
				active();
				status = LogicalGraphTransactionState::LGTTS_COMMITTED;
				return {};
			}
			void rollback() override {
				active();
				status = LogicalGraphTransactionState::LGTTS_ROLLED_BACK;
			}
		};
		class Backend final : public StorageBackend,
							  public LogicalGraphReadBackend,
							  public LogicalGraphTransactionalBackend {
			std::string path;
			size_t budget;
			std::shared_ptr<const Image> image;

		public:
			Backend(std::string file, size_t bytes) : path(std::move(file)), budget(bytes) {}
			void open() override {
				if (!image)
					image = std::make_shared<Image>(path, budget);
			}
			void close() override {
				require(image.use_count() <= 1, "Checkpoint has active snapshots");
				image.reset();
			}
			void flush() override {}
			bool isOpen() const override { return bool(image); }
			bool isReadOnly() const override { return true; }
			StorageFormatKind formatKind() const override { return StorageFormatKind::SFM_CHECKPOINT; }
			std::shared_ptr<resource::DatabaseResourceBudget> queryResourceDomain() const override {
				require(bool(image), "Checkpoint is closed");
				return image->domain;
			}
			StorageBackendCapabilities capabilities() const override { return {true, true, false, true, false}; }
			std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				require(bool(image), "Checkpoint is closed");
				return std::make_unique<Snapshot>(image);
			}
			std::unique_ptr<LogicalGraphTransaction> beginLogicalGraphTransaction(bool ro) override {
				require(ro, "Checkpoint permits read-only transactions");
				require(bool(image), "Checkpoint is closed");
				return std::make_unique<ReadTxn>(image);
			}
		};
	} // namespace
	std::shared_ptr<StorageBackend> openCheckpointBackend(const std::string &path, size_t bytes) {
		return std::make_shared<Backend>(path, bytes);
	}
} // namespace graph::storage
