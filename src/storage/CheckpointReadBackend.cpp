// Immutable cp002 storage adapter. Query semantics belong to the common logical executor.
#include "CheckpointReadBackend.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <boost/iterator/iterator_facade.hpp>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <tuple>
#include "CheckpointSegments.hpp"
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
		// On-disk order is sufficient unless authenticated keys prove otherwise.
		struct Order {
			uint32_t first = 0, count = 0;
			std::vector<uint32_t> permutation;
			struct Cursor : boost::iterator_facade<Cursor, uint32_t, boost::random_access_traversal_tag, uint32_t,
												   std::ptrdiff_t> {
				using iterator_concept = std::random_access_iterator_tag;
				using iterator_category = std::random_access_iterator_tag;
				uint32_t operator[](std::ptrdiff_t n) const { return *(*this + n); }
				const Order *order = nullptr;
				uint32_t pos = 0;
				Cursor() = default;
				Cursor(const Order *o, uint32_t p) : order(o), pos(p) {}
				uint32_t dereference() const {
					return order->permutation.empty() ? order->first + pos : order->permutation[pos];
				}
				bool equal(const Cursor &rhs) const { return order == rhs.order && pos == rhs.pos; }
				void increment() { ++pos; }
				void decrement() { --pos; }
				void advance(std::ptrdiff_t d) { pos = static_cast<uint32_t>(pos + d); }
				std::ptrdiff_t distance_to(const Cursor &rhs) const { return std::ptrdiff_t(rhs.pos) - pos; }
			};
			auto iter(uint32_t i) const { return Cursor(this, i); }
			auto begin() const { return iter(0); }
			auto end() const { return iter(count); }
			template<class Key>
			void sortBy(Key key) {
				permutation.resize(count);
				std::iota(permutation.begin(), permutation.end(), first);
				std::sort(permutation.begin(), permutation.end(), [&](auto a, auto b) { return key(a) < key(b); });
			}
		};
		static_assert(std::random_access_iterator<Order::Cursor>);
		struct Image {
			std::shared_ptr<resource::DatabaseResourceBudget> domain;
			std::optional<resource::ResourceLease> reservation;
			LogicalGraphMutationFence fence;
			checkpoint::Windows privateFile;
			checkpoint::Segments segments;
			bool packed = false;
			Record record(uint32_t i) const { return packed ? segments[i] : privateFile[i]; }
			bool dense = false;
			uint64_t firstId = 0;
			Order nodes, edges, outgoing, incoming;
			uint64_t field(uint32_t i, size_t off) const {
				return packed ? segments.value(i, (off - 4) / 8) : word(privateFile[i].data() + off);
			}
			uint64_t nid(uint32_t i) const { return field(i, 12); }
			uint64_t eid(uint32_t i) const { return field(i, 4); }
			auto nodeAt(uint64_t id) const {
				if (dense) {
					auto offset = id <= firstId ? 0 : std::min<uint64_t>(id - firstId, nodes.count);
					return nodes.iter(static_cast<uint32_t>(offset));
				}
				return std::lower_bound(nodes.begin(), nodes.end(), id,
										[&](auto i, auto target) { return nid(i) < target; });
			}
			bool hasNode(uint64_t id) const {
				auto it = nodeAt(id);
				return it != nodes.end() && nid(*it) == id;
			}
			LogicalGraphNode node(uint32_t i) const {
				const auto tags = packed ? segments.value(i, 4) : word(privateFile[i].data());
				LogicalGraphNode n{nid(i), {"Item"}, {{"id", std::bit_cast<int64_t>(field(i, 4))}}};
				if (auto t = (tags >> 8) & 255)
					n.properties.push_back({"val", cell(t, field(i, 20))});
				if (auto t = (tags >> 16) & 255)
					n.properties.push_back({"grp", cell(t, field(i, 28))});
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
				const size_t working = checkpoint::Segments::workingBytes(count);
				const size_t directory = checkpoint::Segments::directoryBytes(count);
				// With a small input the fixed window is already the cheapest private form.
				// Admit the worst case up front on native, so a tight cap can use disk instead.
				packed = count > 256 && working + directory + count * sizeof(Record) <= budget;
#ifdef __EMSCRIPTEN__
				// A temporary file here would consume unaccounted MEMFS memory.
				packed = count > 256;
#endif
				size_t admitted = packed ? working + directory : checkpoint::Windows::bytes(count);
				require(admitted <= budget, "cp002 snapshot exceeds storage cache budget");
				std::string identity("cp002:");
				identity.append(reinterpret_cast<const char *>(h.data() + 8), 16);
				domain = resource::ProcessResourceDomains::defaultOwner()->domain(std::move(identity));
				auto account =
						std::make_shared<resource::ResourceAccount>(domain, resource::ResourceClass::RCK_CACHE, budget);
				reservation.emplace(std::move(account));
				reservation->set(admitted);
				utils::Sha256 hash;
				hash.update(std::as_bytes(std::span(h).first(56)));
				if (packed) {
					segments.load(in, count, hash, [&](size_t bytes) {
						require(bytes <= budget - admitted, "cp002 snapshot exceeds storage cache budget");
						admitted += bytes;
						reservation->set(admitted);
					});
					admitted -= working;
					reservation->set(admitted);
				} else
					privateFile.load(in, count, hash);
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
				bool nodeOrder = true, outOrder = true, inOrder = true;
				dense = true;
				Record last{};
				for (uint32_t i = 0; i < count; ++i) {
					const Record r = record(i);
					require(last[0] <= r[0], "Invalid cp002 record order");
					if (r[0] == 1) {
						const auto id = word(r.data() + 12);
						require(id && id <= INT64_MAX && id <= word(h.data() + 32), "Invalid cp002 node ID");
						if (!nodes.count)
							firstId = id;
						else {
							require(std::bit_cast<int64_t>(word(last.data() + 4)) <
											std::bit_cast<int64_t>(word(r.data() + 4)),
									"Invalid cp002 key order");
							nodeOrder &= word(last.data() + 12) < id;
						}
						dense &= id == firstId + nodes.count;
						if (r[1])
							(void) cell(r[1], word(r.data() + 20));
						if (r[2])
							(void) cell(r[2], word(r.data() + 28));
						++nodes.count;
					} else if (r[0] == 2) {
						const auto id = word(r.data() + 4);
						require(id && id <= INT64_MAX && id <= word(h.data() + 40), "Invalid cp002 edge ID");
						if (edges.count) {
							require(word(last.data() + 4) < id, "Invalid cp002 edge ID");
							outOrder &= word(last.data() + 12) <= word(r.data() + 12);
							inOrder &= word(last.data() + 20) <= word(r.data() + 20);
						} else
							edges.first = i;
						++edges.count;
					} else {
						require(r[0] == 3 && (last[0] != 3 || std::memcmp(last.data() + 4, r.data() + 4, 16) < 0),
								"Invalid cp002 receipt order");
					}
					last = r;
				}
				auto admitOrder = [&](const Order &order) {
					admitted += order.count * sizeof(uint32_t);
					require(admitted <= budget, "cp002 snapshot exceeds storage cache budget");
					reservation->set(admitted);
				};
				if (!nodeOrder) {
					admitOrder(nodes);
					nodes.sortBy([&](auto i) { return nid(i); });
					for (uint32_t i = 1; i < nodes.count; ++i)
						require(nid(nodes.permutation[i - 1]) != nid(nodes.permutation[i]), "Duplicate cp002 node ID");
				}
				for (auto i: edges) {
					auto r = record(i);
					require(hasNode(word(r.data() + 12)) && hasNode(word(r.data() + 20)),
							"Invalid cp002 edge endpoint");
				}
				auto direction = [&](Order &order, bool sorted, size_t off) {
					order.first = edges.first;
					order.count = edges.count;
					if (!sorted) {
						admitOrder(order);
						order.sortBy([&](auto i) {
							auto r = record(i);
							return std::pair(word(r.data() + off), word(r.data() + 4));
						});
					}
				};
				direction(outgoing, outOrder, 12);
				direction(incoming, inOrder, 20);
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
			bool supportsNativeNodeRefScans() const override { return true; }
			void visitNodeRefs(size_t max, const std::function<bool(const EntityRef &)> &visitor) const override {
				require(max && bool(visitor), "Invalid Node reference visit bound");
				visitComplete(img->nodes, max, [&](auto i) { return visitor({img->nid(i), false, 0, 0, 0}); });
			}
			void
			visitNodeRefsWithLabelMatches(size_t max, const std::vector<std::string> &labels,
										  const std::function<bool(const EntityRef &, bool)> &visitor) const override {
				require(bool(visitor), "Invalid Node label visitor");
				// cp002 has exactly one label for every authenticated Node record.
				const bool matches = std::ranges::all_of(labels, [](const auto &s) { return s == "Item"; });
				visitNodeRefs(max, [&](const EntityRef &ref) { return visitor(ref, matches); });
			}
			LogicalGraphNode readNodeShaped(uint64_t id, const EntityReadShape &shape) const override {
				validateEntityReadShape(shape);
				if (shape.all_properties || !shape.property_tokens.empty())
					return LogicalGraphReadSnapshot::readNodeShaped(id, shape);
				require(img->hasNode(id), "Unknown cp002 node");
				LogicalGraphNode n{id, {}, {}};
				if (shape.include_labels)
					n.labels.emplace_back("Item");
				return n;
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
					const auto it = edge ? std::ranges::lower_bound(owners, id, {}, [&](auto i) { return img->eid(i); })
										 : img->nodeAt(id);
					require(it != owners.end() && (edge ? img->eid(*it) : img->nid(*it)) == id,
							"Unknown cp002 property owner");
					admit(sizeof(Record));
					const auto tags = img->packed ? img->segments.value(*it, 4) : word(img->privateFile[*it].data());
					for (size_t col = 0; col < keys.size(); ++col) {
						auto key = keys[col];
						PropertyValue value;
						if (edge && key == "w")
							value = std::bit_cast<int64_t>(img->field(*it, 28));
						if (!edge && key == "id")
							value = std::bit_cast<int64_t>(img->field(*it, 4));
						if (!edge && key == "val" && (tags & 0xff00))
							value = cell((tags >> 8) & 255, img->field(*it, 20));
						if (!edge && key == "grp" && (tags & 0xff0000))
							value = cell((tags >> 16) & 255, img->field(*it, 28));
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
				auto it = img->nodeAt(after);
				if (it != img->nodes.end() && img->nid(*it) == after)
					++it;
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
