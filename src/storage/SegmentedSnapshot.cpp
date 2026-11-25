#include "graph/storage/SegmentedSnapshot.hpp"
#include <cstring>
#include <fstream>
#include <mutex>
#include "CheckpointReadBackend.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/Sha256.hpp"
#include "query/execution/ProcessResourceDomains.hpp"

namespace graph::storage {
	namespace {
		using Bytes = std::vector<std::byte>;
		using Span = std::span<const std::byte>;
		using Hash = utils::Sha256;
		namespace resource = query::execution::detail;
		constexpr size_t headerSize = 40, footerSize = 48, entrySize = 72;
		constexpr size_t targetBytes = 64 * 1024, maxBlock = 1024 * 1024, maxDirectory = 4 * 1024 * 1024;
		void check(bool ok, const char *msg) {
			if (!ok)
				throw std::runtime_error(msg);
		}
		void put(Bytes &b, uint64_t n) {
			for (unsigned i = 0; i < 8; ++i)
				b.push_back(std::byte(n >> (8 * i)));
		}
		void append(Bytes &b, Span s) { b.insert(b.end(), s.begin(), s.end()); }
		struct Cursor {
			Span b;
			Span take(size_t n) {
				check(n <= b.size(), "Truncated cp003 field");
				auto r = b.first(n);
				b = b.subspan(n);
				return r;
			}
			uint64_t num() {
				auto s = take(8);
				uint64_t n = 0;
				for (unsigned i = 0; i < 8; ++i)
					n |= uint64_t(std::to_integer<unsigned>(s[i])) << (8 * i);
				return n;
			}
			std::string_view skipText() {
				auto n = num();
				check(n <= b.size(), "Invalid cp003 string length");
				auto s = take(n);
				return {reinterpret_cast<const char *>(s.data()), s.size()};
			}
			std::string text() { return std::string(skipText()); }
		};
		void text(Bytes &b, std::string_view s) {
			check(b.size() + 8 <= maxBlock && s.size() <= maxBlock - b.size() - 8, "cp003 record too large");
			put(b, s.size());
			append(b, std::as_bytes(std::span(s)));
		}
		void write(std::ostream &out, Span s) {
			out.write(reinterpret_cast<const char *>(s.data()), s.size());
			check(bool(out), "cp003 export write failed");
		}
		void props(Bytes &b, const std::vector<LogicalGraphPropertyEntry> &p) {
			put(b, p.size());
			for (const auto &e: p) {
				text(b, e.key);
				check(CanonicalPropertyValueCodec::encodedSize(e.value) <= maxBlock - b.size(),
					  "cp003 property too large");
				CanonicalPropertyValueCodec::append(b, e.value);
			}
		}
		auto properties(Cursor &c) {
			std::vector<LogicalGraphPropertyEntry> p;
			auto count = c.num();
			check(count <= c.b.size() / 14, "Invalid cp003 property count");
			for (size_t i = 0; i < count; i++) {
				auto key = c.text();
				size_t off = 0;
				auto value = CanonicalPropertyValueCodec::decodeNext(c.b, off);
				c.take(off);
				p.push_back({std::move(key), std::move(value)});
			}
			return p;
		}
		LogicalGraphNode nodeIdentity(uint64_t id, Cursor c) {
			LogicalGraphNode n{id, {}};
			auto count = c.num();
			check(count <= c.b.size() / 8, "Invalid cp003 labels");
			while (count--)
				n.labels.push_back(c.text());

			check(c.b.empty(), "Trailing cp003 node bytes");
			return n;
		}
		LogicalGraphEdge edgeIdentity(uint64_t id, Cursor c) {
			auto a = c.num(), b = c.num();
			auto type = c.text();
			check(a && b, "Invalid cp003 endpoints");
			LogicalGraphEdge e{id, a, b, std::move(type), {}};
			check(c.b.empty(), "Trailing cp003 edge bytes");
			return e;
		}
		struct Builder {
			std::ostream &out;
			Bytes dir, payload;
			uint64_t offset = headerSize, kind = 0, first = 0, last = 0;
			void flush() {
				if (payload.empty())
					return;
				check(dir.size() <= maxDirectory - entrySize, "cp003 directory capacity exceeded");
				for (auto v: {kind, first, last, offset, uint64_t(payload.size())})
					put(dir, v);
				append(dir, Hash::digest(payload));
				write(out, payload);
				offset += payload.size();
				payload.clear();
			}
			void row(uint64_t id, const Bytes &b) {
				check(id > last && b.size() <= maxBlock - 16, "Invalid cp003 row order or size");
				if (!payload.empty() && payload.size() + 16 + b.size() > targetBytes)
					flush();
				if (payload.empty())
					first = id;
				last = id;
				put(payload, id);
				put(payload, b.size());
				append(payload, b);
			}
		};
	} // namespace
	void exportSegmentedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out) {
		check(out.tellp() == 0, "cp003 export needs a new empty stream");
		out.write("ZYXcp003", 8);
		write(out, std::array<std::byte, 32>{});
		Builder w{out, {}, {}};
		source.visitNodes(std::numeric_limits<size_t>::max(), [&](const LogicalGraphNode &n) {
			Bytes b;
			put(b, n.labels.size());
			for (auto &l: n.labels)
				text(b, l);

			w.row(n.node_id, b);
			return true;
		});
		w.flush();
		w.kind = 1;
		w.last = 0;
		source.visitEdges(std::numeric_limits<size_t>::max(), [&](const LogicalGraphEdge &e) {
			Bytes b;
			put(b, e.source_node_id);
			put(b, e.target_node_id);
			text(b, e.type);

			w.row(e.edge_id, b);
			return true;
		});
		w.flush();
		// Both passes use the caller's single pinned graph snapshot.
		w.kind = 2;
		w.last = 0;
		source.visitNodes(std::numeric_limits<size_t>::max(), [&](const LogicalGraphNode &n) {
			Bytes b;
			props(b, n.properties);
			w.row(n.node_id, b);
			return true;
		});
		w.flush();
		w.kind = 3;
		w.last = 0;
		source.visitEdges(std::numeric_limits<size_t>::max(), [&](const LogicalGraphEdge &e) {
			Bytes b;
			props(b, e.properties);
			w.row(e.edge_id, b);
			return true;
		});
		w.flush();
		Bytes footer;
		put(footer, w.offset);
		put(footer, w.dir.size());
		Hash hash;
		hash.update(footer);
		hash.update(w.dir);
		append(footer, hash.finalize());
		write(out, w.dir);
		write(out, footer);
		auto end = out.tellp();
		out.seekp(8);
		write(out, Span(footer).last(32));
		out.seekp(end);
		out.flush();
		check(bool(out), "cp003 export flush failed");
	}
	namespace {
		struct Block {
			uint64_t kind, first, last, off, size;
			std::array<std::byte, 32> digest;
		};
		struct Image {
			struct Window {
				resource::ResourceLease lease;
				Bytes bytes;
				std::vector<uint32_t> offsets;
				uint64_t offset;
				Window(const std::shared_ptr<resource::ResourceAccount> &account, const Block &block) :
					lease(account), offset(block.off) {
					lease.set(block.size);
				}
			};
			mutable std::mutex cacheMutex;
			mutable std::array<std::shared_ptr<const Window>, 4> windows;
			mutable std::mutex mutex;
			mutable std::ifstream in;
			std::vector<Block> blocks;
			LogicalGraphMutationFence fence;
			std::shared_ptr<resource::DatabaseResourceBudget> domain;
			std::optional<resource::ResourceLease> lease;
			std::shared_ptr<resource::ResourceAccount> account;
			Bytes read(uint64_t off, size_t n) const {
				Bytes b(n);
				std::lock_guard lock(mutex);
				in.clear();
				in.seekg(off);
				check(bool(in.read(reinterpret_cast<char *>(b.data()), n)), "Incomplete cp003 read");
				return b;
			}
			Image(const std::string &path, size_t budget) {
				in.rdbuf()->pubsetbuf(nullptr, 0);
				in.open(path, std::ios::binary | std::ios::ate);
				auto end = in.tellg();
				check(end >= std::streamoff(headerSize + footerSize), "Truncated cp003 file");
				auto h = read(0, headerSize);
				check(!std::memcmp(h.data(), "ZYXcp003", 8), "Invalid cp003 signature");
				auto f = read(uint64_t(end) - footerSize, footerSize);
				Cursor c{f};
				auto off = c.num(), size = c.num();
				check(off >= headerSize && off <= uint64_t(end) - footerSize &&
							  size == uint64_t(end) - footerSize - off && size <= maxDirectory && size % entrySize == 0,
					  "Invalid cp003 directory extent");
				check(std::ranges::equal(Span(h).subspan(8), c.b), "Mismatched cp003 directory identity");
				domain = resource::ProcessResourceDomains::defaultOwner()->domain(
						"cp003:" + std::string(reinterpret_cast<const char *>(c.b.data()), 32));
				account =
						std::make_shared<resource::ResourceAccount>(domain, resource::ResourceClass::RCK_CACHE, budget);
				lease.emplace(account);
				const size_t entries = size / entrySize;
				lease->set(size + entries * sizeof(Block));
				auto d = read(off, size);
				Hash hash;
				hash.update(Span(f).first(16));
				hash.update(d);
				auto digest = hash.finalize();
				check(std::ranges::equal(digest, c.b), "Invalid cp003 directory digest");
				fence.backend_token = 0x6370303033ULL;
				fence.format_token = 3;
				Cursor dc{digest};
				for (size_t i = 0; i < 4; ++i)
					fence.graph_state_tokens[i] = dc.num();
				Cursor r{d};
				blocks.reserve(entries);
				uint64_t next = headerSize;
				while (!r.b.empty()) {
					Block b{r.num(), r.num(), r.num(), r.num(), r.num(), {}};
					std::ranges::copy(r.take(32), b.digest.begin());
					check(b.kind <= 3 && b.first && b.last >= b.first && b.size >= 16 && b.size <= maxBlock &&
								  b.off == next && b.size <= off - next,
						  "Invalid cp003 segment");
					if (!blocks.empty()) {
						auto &prev = blocks.back();
						check(b.kind > prev.kind || (b.kind == prev.kind && b.first > prev.last),
							  "Unordered cp003 directory");
					}
					blocks.push_back(b);
					next += b.size;
				}
				check(next == off, "Unaccounted cp003 payload");
				Bytes().swap(d);
				lease->set(entries * sizeof(Block));
			}
			std::shared_ptr<const Window> window(const Block &b) const {
				std::lock_guard lock(cacheMutex);
				auto &current = windows[b.kind];
				if (current && current->offset == b.off)
					return current;
				current.reset();
				auto w = std::make_shared<Window>(account, b);
				w->bytes = read(b.off, b.size);
				check(Hash::digest(w->bytes) == b.digest, "Invalid cp003 segment digest");
				// The segment is the unit of authentication and capacity admission.
				// Validate the complete row framing before using a directory bound.
				Cursor framing{w->bytes};
				size_t count = 0;
				uint64_t last = 0;
				while (!framing.b.empty()) {
					auto id = framing.num(), length = framing.num();
					check((count || id == b.first) && id > last && id <= b.last && length <= framing.b.size(),
						  "Invalid cp003 row framing");
					framing.take(length);
					last = id;
					++count;
				}
				check(last == b.last, "Invalid cp003 segment end");
				w->lease.set(b.size + count * sizeof(uint32_t));
				w->offsets.reserve(count);
				framing = {w->bytes};
				while (!framing.b.empty()) {
					w->offsets.push_back(static_cast<uint32_t>(w->bytes.size() - framing.b.size()));
					framing.num();
					auto length = framing.num();
					framing.take(length);
				}

				current = w;
				return w;
			}
			template<class F>
			bool visit(uint64_t kind, uint64_t start, F &&visitor) const {
				auto it = std::lower_bound(blocks.begin(), blocks.end(), std::pair{kind, start},
										   [](const Block &b, const auto &k) { return std::pair{b.kind, b.last} < k; });
				for (; it != blocks.end() && it->kind == kind; ++it) {
					const auto &b = *it;
					auto hold = window(b);
					auto cursor = [&](uint32_t pos) { return Cursor{Span(hold->bytes).subspan(pos)}; };
					auto row = hold->offsets.begin();
					if (start > b.first)
						row = std::lower_bound(row, hold->offsets.end(), start,
											   [&](uint32_t off, uint64_t key) { return cursor(off).num() < key; });
					for (; row != hold->offsets.end(); ++row) {
						auto c = cursor(*row);
						auto id = c.num(), length = c.num();
						if (!visitor(id, Cursor{c.take(length)}))
							return false;
					}
				}
				return true;
			}
		};
		class Snapshot final : public LogicalGraphReadSnapshot {
			std::shared_ptr<const Image> img;
			using Admission = std::function<void(size_t)>;
			std::vector<LogicalGraphPropertyEntry> readProperties(bool edges, uint64_t id,
																  const Admission &admit = {}) const {
				std::vector<LogicalGraphPropertyEntry> result;
				bool found = false;
				img->visit(2 + unsigned(edges), id, [&](auto key, Cursor c) {
					if (key == id) {
						if (admit)
							admit(c.b.size());
						result = properties(c);
						check(c.b.empty(), "Trailing cp003 property bytes");
						found = true;
					}
					return false;
				});
				check(found, "Missing cp003 property owner");
				return result;
			}
			LogicalGraphNode node(uint64_t id, Cursor c) const {
				auto n = nodeIdentity(id, c);
				n.properties = readProperties(false, id);
				return n;
			}
			LogicalGraphEdge edge(uint64_t id, Cursor c) const {
				auto e = edgeIdentity(id, c);
				e.properties = readProperties(true, id);
				return e;
			}

			template<class F>
			void adjacency(uint64_t id, LogicalGraphReadDirection dir, F &&visit, uint64_t start = 0) const {
				check(dir == LogicalGraphReadDirection::LGRD_OUTGOING ||
							  dir == LogicalGraphReadDirection::LGRD_INCOMING,
					  "Invalid cp003 direction");
				check(containsNode(id), "Unknown cp003 node");
				img->visit(1, start, [&](uint64_t eid, Cursor c) {
					auto t = c;
					auto a = t.num(), b = t.num();
					return (dir == LogicalGraphReadDirection::LGRD_OUTGOING ? a : b) != id || visit(eid, c);
				});
			}

		public:
			explicit Snapshot(std::shared_ptr<const Image> image) : img(std::move(image)) {}
			std::optional<LogicalGraphMutationFence> mutationFence() const override { return img->fence; }
			bool containsNode(uint64_t id) const override {
				bool hit = false;
				img->visit(0, id, [&](auto k, Cursor) {
					hit = k == id;
					return false;
				});
				return hit;
			}
			LogicalGraphNode readNodeShaped(uint64_t id, const EntityReadShape &s) const override {
				validateEntityReadShape(s);
				LogicalGraphNode n;
				img->visit(0, id, [&](auto k, Cursor c) {
					if (k == id)
						n = nodeIdentity(k, c);
					return false;
				});
				check(n.node_id, "Unknown cp003 node");
				if (!s.include_labels)
					n.labels.clear();
				if (s.all_properties || !s.property_tokens.empty())
					n.properties = readProperties(false, id);
				return n;
			}
			void visitEdgeTopologies(size_t maximum,
									 const std::function<bool(const LogicalGraphEdgeTopology &)> &f) const override {
				check(maximum && f, "Invalid cp003 topology visit");
				img->visit(1, 0, [&](auto k, Cursor c) {
					check(maximum--, "cp003 edge bound exceeded");
					auto e = edgeIdentity(k, c);
					return f({k, e.source_node_id, e.target_node_id, e.type});
				});
			}
			bool supportsNodeExistenceProbes() const override { return true; }
			LogicalGraphNode readNode(uint64_t id) const override {
				LogicalGraphNode n;
				img->visit(0, id, [&](auto k, Cursor c) {
					if (k == id)
						n = node(k, c);
					return false;
				});
				check(n.node_id, "Unknown cp003 node");
				return n;
			}
			LogicalGraphEdge readEdge(uint64_t id) const override {
				LogicalGraphEdge e;
				img->visit(1, id, [&](auto k, Cursor c) {
					if (k == id)
						e = edge(k, c);
					return false;
				});
				check(e.edge_id, "Unknown cp003 edge");
				return e;
			}
			void visitNodes(size_t max, const std::function<bool(const LogicalGraphNode &)> &f) const override {
				check(max && f, "Invalid cp003 node visit");
				img->visit(0, 0, [&](auto k, Cursor c) {
					check(max--, "cp003 node limit exceeded");
					return f(node(k, c));
				});
			}
			void visitEdges(size_t max, const std::function<bool(const LogicalGraphEdge &)> &f) const override {
				check(max && f, "Invalid cp003 edge visit");
				img->visit(1, 0, [&](auto k, Cursor c) {
					check(max--, "cp003 edge limit exceeded");
					return f(edge(k, c));
				});
			}
			bool supportsNativeNodeRefScans() const override { return true; }
			void visitNodeRefs(size_t max, const std::function<bool(const EntityRef &)> &f) const override {
				check(max && f, "Invalid cp003 ref visit");
				img->visit(0, 0, [&](auto k, Cursor) {
					check(max--, "cp003 node limit exceeded");
					return f({k, false, 0, 0, 0});
				});
			}
			void visitNodeRefsWithLabelMatches(size_t max, const std::vector<std::string> &labels,
											   const std::function<bool(const EntityRef &, bool)> &f) const override {
				check(max && f, "Invalid cp003 label visit");
				img->visit(0, 0, [&](auto k, Cursor c) {
					check(max--, "cp003 node limit exceeded");
					auto count = c.num();
					std::vector<std::string> names;
					while (count--)
						names.push_back(c.text());
					return f({k, false, 0, 0, 0}, std::ranges::all_of(labels, [&](auto &l) {
								 return std::ranges::find(names, l) != names.end();
							 }));
				});
			}

			using Columns = std::vector<std::vector<PropertyValue>>;
			template<class F>
			void owners(uint64_t kind, std::span<const uint64_t> ids, F &&f) const {
				if (ids.empty())
					return;
				uint64_t prev = 0;
				for (auto id: ids) {
					check(id > prev, "Invalid cp003 owner order");
					prev = id;
				}
				size_t next = 0;
				// Restart the directory search for gaps; consecutive owners in a segment
				// share its authenticated bytes.
				for (auto id: ids) {
					bool found = false;
					img->visit(kind, id, [&](auto k, Cursor c) {
						if (k == id) {
							f(next, c);
							found = true;
						}
						return false;
					});
					check(found, "Unknown cp003 owner");
					++next;
				}
			}
			Columns columns(bool edges, std::span<const uint64_t> ids, std::span<const std::string_view> keys,
							const Admission &admit) const {
				check(bool(admit), "Missing cp003 admission");
				Columns result(keys.size());
				for (auto &col: result)
					col.resize(ids.size());
				owners(2 + unsigned(edges), ids, [&](size_t i, Cursor c) {
					auto n = c.num();
					while (n--) {
						auto name = c.skipText();
						auto it = std::ranges::find(keys, name);
						size_t end = 0;
						auto shape = CanonicalPropertyValueCodec::inspectNext(c.b, end);
						if (it != keys.end()) {
							admit(shape.encoded_bytes + name.size() + 8);
							size_t off = 0;
							auto value = CanonicalPropertyValueCodec::decodeNext(c.b, off);
							for (size_t col = 0; col < keys.size(); ++col)
								if (keys[col] == name)
									result[col][i] = value;
						}
						c.take(end);
					}
					check(c.b.empty(), "Trailing cp003 Property bytes");
				});
				return result;
			}
			bool supportsPropertyReadAdmission() const override { return true; }
			Columns readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
															   std::span<const std::string_view> keys,
															   const Admission &a) const override {
				return columns(false, ids, keys, a);
			}
			Columns readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
															   std::span<const std::string_view> keys,
															   const Admission &a) const override {
				return columns(true, ids, keys, a);
			}
			bool supportsShapedPropertyReadAdmission() const override { return true; }
			std::vector<LogicalGraphNode> readNodesShapedSortedWithAdmission(std::span<const uint64_t> ids,
																			 const EntityReadShape &shape,
																			 const Admission &a) const override {
				validateEntityReadShape(shape);
				std::vector<LogicalGraphNode> out;
				out.reserve(ids.size());
				owners(false, ids, [&](size_t i, Cursor c) {
					a(c.b.size());
					auto n = nodeIdentity(ids[i], c);
					if (!shape.include_labels)
						n.labels.clear();
					if (shape.all_properties || !shape.property_tokens.empty())
						n.properties = readProperties(false, ids[i], a);
					out.push_back(std::move(n));
				});
				return out;
			}
			std::vector<LogicalGraphEdge> readEdgesShapedSortedWithAdmission(std::span<const uint64_t> ids,
																			 const EntityReadShape &shape,
																			 const Admission &a) const override {
				validateEntityReadShape(shape);
				std::vector<LogicalGraphEdge> out;
				out.reserve(ids.size());
				owners(true, ids, [&](size_t i, Cursor c) {
					a(c.b.size());
					auto e = edgeIdentity(ids[i], c);
					if (!shape.include_type)
						e.type.clear();
					if (shape.all_properties || !shape.property_tokens.empty())
						e.properties = readProperties(true, ids[i], a);
					out.push_back(std::move(e));
				});
				return out;
			}
			LogicalGraphNodePage scanNodesAfter(uint64_t after, size_t max, size_t pages) const override {
				check(max && pages, "Invalid cp003 node page");
				LogicalGraphNodePage p;
				if (after == UINT64_MAX)
					return p;
				auto it = std::lower_bound(img->blocks.begin(), img->blocks.end(), after + 1,
										   [](const Block &b, uint64_t k) { return b.kind == 0 && b.last < k; });
				if (it == img->blocks.end() || it->kind)
					return p;
				p.metadata_pages_visited = 1;
				p.may_have_more = std::next(it) != img->blocks.end() && !std::next(it)->kind;
				img->visit(0, after + 1, [&](auto k, Cursor c) {
					p.nodes.push_back(node(k, c));
					if (k == it->last)
						return false;
					if (p.nodes.size() >= std::min(max, size_t(64))) {
						p.may_have_more = true;
						return false;
					}
					return true;
				});
				return p;
			}
			LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t id, LogicalGraphReadDirection dir,
														std::optional<LogicalGraphAdjacencyContinuation> cont,
														size_t max) const override {
				check(max, "Invalid cp003 adjacency page");
				LogicalGraphAdjacencyPage p;
				auto skip = cont.has_value();
				auto resume = skip;
				adjacency(
						id, dir,
						[&](auto k, Cursor c) {
							if (skip) {
								check(k == cont->previous_edge_id, "Invalid cp003 continuation");
								skip = false;
								return true;
							}
							if (resume) {
								check(k == cont->next_edge_id, "Invalid cp003 continuation");
								resume = false;
							}
							if (p.edges.size() == std::min(max, size_t(64))) {
								p.continuation = {{p.edges.back().edge_id, k}};
								return false;
							}
							p.edges.push_back(edge(k, c));
							return true;
						},
						cont ? cont->previous_edge_id : 0);
				check(!resume, "Invalid cp003 continuation");
				return p;
			}
			void visitAdjacentEdges(uint64_t id, LogicalGraphReadDirection dir, size_t max,
									const std::function<bool(const LogicalGraphEdge &)> &f) const override {
				check(max && f, "Invalid cp003 adjacency");
				adjacency(id, dir, [&](auto k, Cursor c) {
					check(max--, "cp003 adjacency limit exceeded");
					return f(edge(k, c));
				});
			}
		};
		class ReadTxn final : public LogicalGraphTransaction {
			Snapshot snapshot;
			LogicalGraphTransactionState status = LogicalGraphTransactionState::LGTTS_ACTIVE;
			void active() const { check(status == LogicalGraphTransactionState::LGTTS_ACTIVE, "Transaction ended"); }

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
				check(image.use_count() <= 1, "Checkpoint has active snapshots");
				image.reset();
			}
			void flush() override {}
			bool isOpen() const override { return bool(image); }
			bool isReadOnly() const override { return true; }
			StorageFormatKind formatKind() const override { return StorageFormatKind::SFM_CHECKPOINT; }
			std::shared_ptr<resource::DatabaseResourceBudget> queryResourceDomain() const override {
				check(bool(image), "Checkpoint is closed");
				return image->domain;
			}
			StorageBackendCapabilities capabilities() const override { return {true, true, false, true, false}; }
			std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
				check(bool(image), "Checkpoint is closed");
				return std::make_unique<Snapshot>(image);
			}
			std::unique_ptr<LogicalGraphTransaction> beginLogicalGraphTransaction(bool ro) override {
				check(ro, "Checkpoint permits read-only transactions");
				check(bool(image), "Checkpoint is closed");
				return std::make_unique<ReadTxn>(image);
			}
		};
	} // namespace
	std::shared_ptr<StorageBackend> openSegmentedBackend(const std::string &path, size_t bytes) {
		return std::make_shared<Backend>(path, bytes);
	}
} // namespace graph::storage
