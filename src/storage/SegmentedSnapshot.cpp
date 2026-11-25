#include "graph/storage/SegmentedSnapshot.hpp"
#include <bit>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include "CheckpointReadBackend.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/SnapshotNodeIndex.hpp"
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
		enum class NameSpace : size_t { NS_PROPERTY, NS_LABEL, NS_TYPE };
		void token(Bytes &out, uint32_t value) {
			for (unsigned i = 0; i < 4; ++i)
				out.push_back(std::byte(value >> (8 * i)));
		}
		uint32_t token(Cursor &c) {
			auto bytes = c.take(4);
			uint32_t result = 0;
			for (unsigned i = 0; i < 4; ++i)
				result |= uint32_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
			return result;
		}
		struct Dictionary {
			// Separate snapshot-local domains mirror the catalog's property names,
			// label schema tokens and relationship type tokens.
			std::array<std::vector<std::string>, 3> names;
			std::vector<uint8_t> lanes;
			bool separate(uint32_t key, bool edges) const { return lanes.at(key) & (1U << unsigned(edges)); }
			std::string_view lookup(NameSpace ns, uint32_t key) const {
				const auto &table = names[size_t(ns)];
				check(key < table.size(), "Invalid snapshot name token");
				return table[key];
			}
			uint32_t find(NameSpace ns, std::string_view name) const {
				const auto &table = names[size_t(ns)];
				auto it = std::lower_bound(table.begin(), table.end(), name);
				check(it != table.end() && *it == name, "Snapshot name changed during export");
				return static_cast<uint32_t>(it - table.begin());
			}
		};
		std::string_view name(Cursor &c, const Dictionary *dict, NameSpace ns) {
			return dict ? dict->lookup(ns, token(c)) : c.skipText();
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
		auto properties(Cursor &c, const Dictionary *dict) {
			std::vector<LogicalGraphPropertyEntry> p;
			auto count = c.num();
			check(count <= c.b.size() / (dict ? 9 : 14), "Invalid cp003 property count");
			for (size_t i = 0; i < count; i++) {
				auto key = std::string(name(c, dict, NameSpace::NS_PROPERTY));
				size_t off = 0;
				auto value = CanonicalPropertyValueCodec::decodeNext(c.b, off);
				c.take(off);
				p.push_back({std::move(key), std::move(value)});
			}
			return p;
		}
		LogicalGraphNode nodeIdentity(uint64_t id, Cursor c, const Dictionary *dict) {
			LogicalGraphNode n{id, {}};
			auto count = c.num();
			check(count <= c.b.size() / (dict ? 4 : 8), "Invalid cp003 labels");
			while (count--)
				n.labels.emplace_back(name(c, dict, NameSpace::NS_LABEL));

			check(c.b.empty(), "Trailing cp003 node bytes");
			return n;
		}
		LogicalGraphEdge edgeIdentity(uint64_t id, Cursor c, const Dictionary *dict) {
			auto a = c.num(), b = c.num();
			auto type = std::string(name(c, dict, NameSpace::NS_TYPE));
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
		void finishSnapshot(std::ostream &out, Builder &w, const Bytes &directory) {
			Bytes footer;
			put(footer, w.offset);
			put(footer, directory.size());
			Hash hash;
			hash.update(footer);
			hash.update(directory);
			append(footer, hash.finalize());
			write(out, directory);
			write(out, footer);
			auto end = out.tellp();
			out.seekp(8);
			write(out, Span(footer).last(32));
			out.seekp(end);
			out.flush();
			check(bool(out), "cp003 export flush failed");
		}

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
		finishSnapshot(out, w, w.dir);
	}
	namespace {
		using Posting = std::pair<int64_t, uint64_t>;
		constexpr size_t maxIndexNodes = 131072, maxPostings = 32768;
		bool safeInteger(int64_t v) {
			constexpr int64_t exact = (int64_t{1} << 53) - 1;
			return v >= -exact && v <= exact;
		}
		struct IndexedField {
			uint64_t declaration, revision;
			uint32_t label, key;
			uint64_t off, bytes;
			std::array<std::byte, 32> digest;
		};
	} // namespace
	static void writeTokenized(const LogicalGraphReadSnapshot &source, std::ostream &out, bool columns,
							   std::optional<SnapshotNodeIndex> index) {
		check(out.tellp() == 0, "Snapshot export requires a new empty stream");
		Dictionary dict;
		{
			struct Stats {
				std::array<uint64_t, 2> bytes{}, count{};
			};
			std::array<std::map<std::string, Stats, std::less<>>, 3> distinct;
			size_t used = 0;
			auto collect = [&](NameSpace ns, const std::string &key) -> Stats & {
				auto &set = distinct[size_t(ns)];
				if (auto it = set.find(key); it != set.end())
					return it->second;
				check(!key.empty() && key.size() <= maxBlock && used + key.size() + 192 <= maxDirectory,
					  "Snapshot vocabulary capacity exceeded");
				used += key.size() + 192;
				return set.try_emplace(key).first->second;
			};
			source.visitNodes(SIZE_MAX, [&](const LogicalGraphNode &n) {
				for (const auto &label: n.labels)
					collect(NameSpace::NS_LABEL, label);
				for (const auto &p: n.properties) {
					auto &stats = collect(NameSpace::NS_PROPERTY, p.key);
					stats.bytes[0] += CanonicalPropertyValueCodec::encodedSize(p.value);
					++stats.count[0];
				}
				return true;
			});
			source.visitEdges(SIZE_MAX, [&](const LogicalGraphEdge &e) {
				collect(NameSpace::NS_TYPE, e.type);
				for (const auto &p: e.properties) {
					auto &stats = collect(NameSpace::NS_PROPERTY, p.key);
					stats.bytes[1] += CanonicalPropertyValueCodec::encodedSize(p.value);
					++stats.count[1];
				}
				return true;
			});
			for (size_t i = 0; i < distinct.size(); ++i)
				for (const auto &[word, stats]: distinct[i]) {
					dict.names[i].push_back(word);
					if (i != size_t(NameSpace::NS_PROPERTY))
						continue;
					uint8_t mask = 0;
					// Each separate lane adds an owner ID per value and at least
					// one directory entry. Keep small or rare values with the owner.
					for (size_t e = 0; e < 2; ++e)
						if (columns && stats.bytes[e] / 4 > stats.count[e] * sizeof(uint64_t) + entrySize)
							mask |= 1U << e;
					dict.lanes.push_back(mask);
				}
		}
		out.write("ZYXcp004", 8);
		write(out, std::array<std::byte, 32>{});
		Builder w{out, {}, {}};
		struct Lane {
			Bytes data;
			uint64_t first = 0, last = 0, touched = 0;
		};
		// Projected reads may pin an owner window and a field window together.
		// Share the regular window target between them on split layouts.
		const bool hasFields = std::ranges::any_of(dict.lanes, [](auto mask) { return mask != 0; });

		uint64_t clock = 0;
		std::map<uint64_t, Lane> lanes;
		auto flush = [&](auto it) {
			w.kind = it->first;
			w.first = it->second.first;
			w.last = it->second.last;
			w.payload = std::move(it->second.data);
			w.flush();
			lanes.erase(it);
		};
		auto emit = [&](uint64_t kind, uint64_t id, Span data) {
			size_t extra = kind < 4 ? 16 : 8;
			const size_t laneTarget = hasFields && kind >= 2 ? targetBytes / 2 : targetBytes;
			check(id && data.size() <= maxBlock - extra, "Snapshot row exceeds segment limit");
			auto it = lanes.find(kind);
			if (it != lanes.end() && it->second.data.size() + data.size() + extra > laneTarget) {
				flush(it);
				it = lanes.end();
			}
			if (it == lanes.end()) {
				if (lanes.size() == 16)
					flush(std::min_element(lanes.begin(), lanes.end(),
										   [](auto &a, auto &b) { return a.second.touched < b.second.touched; }));
				it = lanes.try_emplace(kind).first;
				it->second.first = id;
			}
			auto &lane = it->second;
			check(id > lane.last, "Unordered snapshot owner");
			lane.last = id;
			lane.touched = ++clock;
			put(lane.data, id);
			if (kind < 4)
				put(lane.data, data.size());
			append(lane.data, data);
		};
		auto attributes = [&](bool edges, uint64_t id, const auto &properties) {
			Bytes owner;
			check(properties.size() <= (maxBlock - 24) / 4, "Snapshot has too many keys");
			put(owner, properties.size());
			for (const auto &p: properties) {
				auto key = dict.find(NameSpace::NS_PROPERTY, p.key);
				token(owner, key);
				auto size = CanonicalPropertyValueCodec::encodedSize(p.value);
				check(size <= maxBlock - 16 && owner.size() <= maxBlock - 16, "Snapshot property exceeds limit");
				if (dict.separate(key, edges)) {
					Bytes value;
					CanonicalPropertyValueCodec::append(value, p.value);
					emit(4 + 2 * uint64_t(key) + unsigned(edges), id, value);
				} else {
					check(size <= maxBlock - 16 - owner.size(), "Snapshot property row exceeds limit");
					CanonicalPropertyValueCodec::append(owner, p.value);
				}
			}
			emit(2 + unsigned(edges), id, owner);
		};
		std::vector<Posting> postings;
		size_t nodeCount = 0;
		uint64_t last = 0;
		source.visitNodes(SIZE_MAX, [&](const LogicalGraphNode &n) {
			if (index && ++nodeCount > maxIndexNodes)
				index.reset();
			if (index && std::ranges::find(n.labels, index->label) != n.labels.end()) {
				for (const auto &p: n.properties) {
					if (p.key != index->property || p.value.getType() == PropertyType::NULL_TYPE)
						continue;
					const auto *integer = std::get_if<int64_t>(&p.value.getVariant());
					if (!integer || !safeInteger(*integer) || postings.size() == maxPostings) {
						index.reset();
						break;
					}
					postings.emplace_back(*integer, n.node_id);
				}
			}
			check(n.node_id > last && n.labels.size() <= (maxBlock - 24) / 4, "Invalid snapshot node");
			last = n.node_id;
			Bytes b;
			put(b, n.labels.size());
			for (const auto &l: n.labels)
				token(b, dict.find(NameSpace::NS_LABEL, l));
			emit(0, last, b);
			attributes(false, last, n.properties);
			return true;
		});
		last = 0;
		source.visitEdges(SIZE_MAX, [&](const LogicalGraphEdge &e) {
			check(e.edge_id > last && e.source_node_id && e.target_node_id, "Invalid snapshot edge");
			last = e.edge_id;
			Bytes b;
			put(b, e.source_node_id);
			put(b, e.target_node_id);
			token(b, dict.find(NameSpace::NS_TYPE, e.type));
			emit(1, last, b);
			attributes(true, last, e.properties);
			return true;
		});
		while (!lanes.empty())
			flush(lanes.begin());
		Bytes directory;
		put(directory, index && !postings.empty() ? 2 : unsigned(columns));
		for (const auto &table: dict.names) {
			put(directory, table.size());
			for (const auto &word: table) {
				put(directory, word.size());
				append(directory, std::as_bytes(std::span(word)));
			}
		}
		if (columns)
			append(directory, std::as_bytes(std::span(dict.lanes)));
		if (index && !postings.empty()) {
			std::ranges::sort(postings);
			Bytes raw;
			raw.reserve(postings.size() * 16);
			for (auto [value, id]: postings) {
				put(raw, std::bit_cast<uint64_t>(value));
				put(raw, id);
			}
			put(directory, index->id);
			put(directory, index->generation);
			token(directory, dict.find(NameSpace::NS_LABEL, index->label));
			token(directory, dict.find(NameSpace::NS_PROPERTY, index->property));
			put(directory, w.offset);
			put(directory, raw.size());
			append(directory, Hash::digest(raw));
			// Bind catalog provenance and ordered postings to this graph directory.
			Hash binding;
			binding.update(directory);
			binding.update(w.dir);
			append(directory, binding.finalize());
			write(out, raw);
			w.offset += raw.size();
		}
		check(directory.size() <= maxDirectory && w.dir.size() <= maxDirectory - directory.size(),
			  "Snapshot directory too large");
		append(directory, w.dir);
		finishSnapshot(out, w, directory);
	}

	void exportTokenizedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out, bool columns) {
		writeTokenized(source, out, columns, std::nullopt);
	}
	void exportIndexedTokenizedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out) {
		auto *catalog = dynamic_cast<const SnapshotNodeIndexSource *>(&source);
		writeTokenized(source, out, true, catalog ? catalog->singleNodePropertyIndex() : std::nullopt);
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
			mutable std::array<std::shared_ptr<const Window>, 8> fields;
			std::optional<Dictionary> dictionary;
			bool columns = false;
			std::optional<IndexedField> descriptor;
			std::optional<resource::ResourceLease> postingLease;
			std::vector<Posting> postings;
			const Dictionary *dict() const { return dictionary ? &*dictionary : nullptr; }
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
				bool v4 = !std::memcmp(h.data(), "ZYXcp004", 8);
				check(v4 || !std::memcmp(h.data(), "ZYXcp003", 8), "Invalid cp003 signature");
				auto f = read(uint64_t(end) - footerSize, footerSize);
				Cursor c{f};
				auto off = c.num(), size = c.num();
				check(off >= headerSize && off <= uint64_t(end) - footerSize &&
							  size == uint64_t(end) - footerSize - off && size <= maxDirectory &&
							  (v4 || size % entrySize == 0),
					  "Invalid cp003 directory extent");
				check(std::ranges::equal(Span(h).subspan(8), c.b), "Mismatched cp003 directory identity");
				domain = resource::ProcessResourceDomains::defaultOwner()->domain(
						std::string(v4 ? "cp004:" : "cp003:") +
						std::string(reinterpret_cast<const char *>(c.b.data()), 32));
				account =
						std::make_shared<resource::ResourceAccount>(domain, resource::ResourceClass::RCK_CACHE, budget);
				lease.emplace(account);
				size_t entries = size / entrySize;
				lease->set(size + (v4 ? 0 : entries * sizeof(Block)));
				auto d = read(off, size);
				Hash hash;
				hash.update(Span(f).first(16));
				hash.update(d);
				auto digest = hash.finalize();
				check(std::ranges::equal(digest, c.b), "Invalid cp003 directory digest");
				fence.backend_token = 0x6370303033ULL;
				fence.format_token = v4 ? 4 : 3;
				Cursor dc{digest};
				for (size_t i = 0; i < 4; ++i)
					fence.graph_state_tokens[i] = dc.num();
				Cursor r{d};
				size_t dictionaryBytes = 0;
				if (v4) {
					auto mode = r.num();
					check(mode <= 2, "Invalid snapshot layout");
					columns = mode != 0;
					dictionary.emplace();
					for (auto &table: dictionary->names) {
						auto count = r.num();
						check(count <= r.b.size() / 8, "Invalid snapshot vocabulary");
						dictionaryBytes += count * sizeof(std::string);
						lease->set(size + dictionaryBytes);
						table.reserve(count);
						for (size_t i = 0; i < count; ++i) {
							auto word = r.skipText();
							check(!word.empty() && (table.empty() || table.back() < word),
								  "Unordered snapshot vocabulary");
							dictionaryBytes += word.size() + 1;
							lease->set(size + dictionaryBytes);
							table.emplace_back(word);
						}
					}
					if (columns) {
						const auto count = dictionary->names[0].size();
						dictionaryBytes += count;
						lease->set(size + dictionaryBytes);
						auto flags = r.take(count);
						dictionary->lanes.reserve(count);
						for (auto raw: flags) {
							auto mask = std::to_integer<uint8_t>(raw);
							check(mask <= 3, "Invalid snapshot field layout");
							dictionary->lanes.push_back(mask);
						}
					}
					if (mode == 2) {
						IndexedField idx{r.num(), r.num(), token(r), token(r), r.num(), r.num(), {}};
						check(idx.declaration && idx.revision && idx.bytes && idx.bytes % 16 == 0 &&
									  idx.bytes / 16 <= maxPostings && idx.off >= headerSize && idx.off <= off &&
									  idx.bytes == off - idx.off,
							  "Invalid snapshot index extent");
						(void) dictionary->lookup(NameSpace::NS_LABEL, idx.label);
						(void) dictionary->lookup(NameSpace::NS_PROPERTY, idx.key);
						std::ranges::copy(r.take(32), idx.digest.begin());
						Hash binding;
						binding.update(Span(d).first(d.size() - r.b.size()));
						auto expectedBinding = r.take(32);
						binding.update(r.b);
						check(std::ranges::equal(binding.finalize(), expectedBinding), "Stale snapshot index binding");
						descriptor = idx;
					}
					check(r.b.size() % entrySize == 0, "Invalid snapshot directory length");
					entries = r.b.size() / entrySize;
					lease->set(size + entries * sizeof(Block) + dictionaryBytes);
				}
				blocks.reserve(entries);
				uint64_t next = headerSize;
				while (!r.b.empty()) {
					Block b{r.num(), r.num(), r.num(), r.num(), r.num(), {}};
					std::ranges::copy(r.take(32), b.digest.begin());
					check((b.kind <= 3 ||
						   (columns && (b.kind - 4) / 2 < dictionary->names[0].size() &&
							dictionary->separate(static_cast<uint32_t>((b.kind - 4) / 2), b.kind % 2))) &&
								  b.first && b.last >= b.first && b.size >= (b.kind < 4 ? 16U : 9U) &&
								  b.size <= maxBlock && b.off == next && b.size <= off - next,
						  "Invalid cp003 segment");
					if (!v4 && !blocks.empty()) {
						auto &prev = blocks.back();
						check(b.kind > prev.kind || (b.kind == prev.kind && b.first > prev.last),
							  "Unordered cp003 directory");
					}
					blocks.push_back(b);
					next += b.size;
				}
				check(next == (descriptor ? descriptor->off : off), "Unaccounted snapshot payload");
				if (v4) {
					std::ranges::sort(blocks, {}, [](const Block &b) { return std::pair{b.kind, b.first}; });
					for (size_t i = 1; i < blocks.size(); ++i)
						check(blocks[i - 1].kind != blocks[i].kind || blocks[i - 1].last < blocks[i].first,
							  "Overlapping snapshot segments");
				}
				Bytes().swap(d);
				lease->set(entries * sizeof(Block) + dictionaryBytes);
			}
			static Span record(Cursor &c, uint64_t kind) {
				if (kind < 4) {
					auto n = c.num();
					check(n <= c.b.size(), "Invalid snapshot row length");
					return c.take(n);
				}
				size_t n = 0;
				(void) CanonicalPropertyValueCodec::inspectNext(c.b, n);
				return c.take(n);
			}
			std::shared_ptr<const Window> window(const Block &b) const {
				std::lock_guard lock(cacheMutex);
				auto *slot = &windows[b.kind < 4 ? b.kind : 0];
				if (b.kind >= 4) {
					auto hit = std::find_if(fields.begin(), fields.end(),
											[&](const auto &w) { return w && w->offset == b.off; });
					if (hit != fields.end()) {
						auto keep = *hit;
						std::rotate(fields.begin(), hit, hit + 1);
						return keep;
					}
					std::rotate(fields.rbegin(), fields.rbegin() + 1, fields.rend());
					slot = &fields.front();
				}
				auto &current = *slot;
				if (current && current->offset == b.off)
					return current;
				current.reset();
				if (dictionary && account->available() < b.size + (b.size / 8) * sizeof(uint32_t)) {
					for (auto &saved: fields)
						if (saved.use_count() == 1)
							saved.reset();
					for (auto &saved: windows)
						if (saved.use_count() == 1)
							saved.reset();
				}
				auto w = std::make_shared<Window>(account, b);
				w->bytes = read(b.off, b.size);
				check(Hash::digest(w->bytes) == b.digest, "Invalid cp003 segment digest");
				// The segment is the unit of authentication and capacity admission.
				// Validate the complete row framing before using a directory bound.
				Cursor framing{w->bytes};
				size_t count = 0;
				uint64_t last = 0;
				while (!framing.b.empty()) {
					auto id = framing.num();
					check((count || id == b.first) && id > last && id <= b.last, "Invalid cp003 row framing");
					record(framing, b.kind);
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
					record(framing, b.kind);
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
						auto id = c.num();
						if (!visitor(id, Cursor{record(c, b.kind)}))
							return false;
					}
				}
				return true;
			}
		};
		class Snapshot final : public LogicalGraphReadSnapshot, public SnapshotNodeIndexSource {
			std::shared_ptr<const Image> img;
			using Admission = std::function<void(size_t)>;
			PropertyValue field(bool edges, uint64_t id, uint32_t key, std::string_view spelling,
								const Admission &admit) const {
				std::optional<PropertyValue> value;
				img->visit(4 + uint64_t(key) * 2 + unsigned(edges), id, [&](auto owner, Cursor c) {
					check(owner == id, "Missing snapshot property cell");
					if (admit)
						admit(c.b.size() + spelling.size() + 8);
					size_t end = 0;
					value = CanonicalPropertyValueCodec::decodeNext(c.b, end);
					check(end == c.b.size(), "Invalid snapshot property cell");
					return false;
				});
				check(value.has_value(), "Missing snapshot property cell");
				return std::move(*value);
			}
			std::vector<LogicalGraphPropertyEntry> readProperties(bool edges, uint64_t id,
																  const Admission &admit = {}) const {
				std::vector<LogicalGraphPropertyEntry> result;
				bool found = false;
				img->visit(2 + unsigned(edges), id, [&](auto key, Cursor c) {
					if (key == id) {
						if (admit)
							admit(c.b.size());
						if (!img->columns) {
							if (img->dict() && admit) {
								auto t = c;
								auto n = t.num();
								while (n--) {
									admit(name(t, img->dict(), NameSpace::NS_PROPERTY).size() + 8);
									size_t end = 0;
									(void) CanonicalPropertyValueCodec::inspectNext(t.b, end);
									t.take(end);
								}
							}
							result = properties(c, img->dict());
						} else {
							auto count = c.num();
							check(count <= c.b.size() / 4, "Invalid snapshot owner keys");
							for (size_t i = 0; i < count; ++i) {
								auto key = token(c);
								auto spelling = img->dict()->lookup(NameSpace::NS_PROPERTY, key);
								PropertyValue value;
								if (img->dict()->separate(key, edges))
									value = field(edges, id, key, spelling, admit);
								else {
									if (admit)
										admit(spelling.size() + 8);
									size_t end = 0;
									value = CanonicalPropertyValueCodec::decodeNext(c.b, end);
									c.take(end);
								}
								result.push_back({std::string(spelling), std::move(value)});
							}
						}
						check(c.b.empty(), "Trailing cp003 property bytes");
						found = true;
					}
					return false;
				});
				check(found, "Missing cp003 property owner");
				return result;
			}
			LogicalGraphNode node(uint64_t id, Cursor c) const {
				auto n = nodeIdentity(id, c, img->dict());
				n.properties = readProperties(false, id);
				return n;
			}
			LogicalGraphEdge edge(uint64_t id, Cursor c) const {
				auto e = edgeIdentity(id, c, img->dict());
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
			static void validateIndex(const std::shared_ptr<Image> &image) {
				if (!image->descriptor)
					return;
				const auto &d = *image->descriptor;
				resource::ResourceLease lease(image->account);
				if (image->account->available() < d.bytes * 2 + maxBlock * 4 || !lease.trySet(d.bytes * 2)) {
					image->descriptor.reset();
					return;
				}
				std::vector<Posting> entries;
				entries.reserve(d.bytes / 16);
				{
					auto raw = image->read(d.off, d.bytes);
					check(Hash::digest(raw) == d.digest, "Invalid snapshot index digest");
					Cursor c{raw};
					while (!c.b.empty()) {
						Posting p{std::bit_cast<int64_t>(c.num()), c.num()};
						check(safeInteger(p.first) && p.second && (entries.empty() || entries.back() < p),
							  "Invalid snapshot posting");
						entries.push_back(p);
					}
				}
				Snapshot audit(image);
				std::array keys{image->dict()->lookup(NameSpace::NS_PROPERTY, d.key)};
				size_t nodes = 0, values = 0;
				image->visit(0, 0, [&](uint64_t id, Cursor c) {
					check(++nodes <= maxIndexNodes, "Invalid indexed snapshot node count");
					auto n = c.num();
					bool match = false;
					while (n--) {
						auto label = token(c);
						(void) image->dict()->lookup(NameSpace::NS_LABEL, label);
						match |= label == d.label;
					}
					check(c.b.empty(), "Invalid indexed node labels");
					if (!match)
						return true;
					bool seen = false;
					audit.selected(
							false, std::array{id}, keys, [](size_t) {},
							[&](size_t, size_t, Span bytes) {
								auto v = CanonicalPropertyValueCodec::tryBorrowPrimitive(bytes);
								check(!seen && v.has_value(), "Invalid indexed snapshot field");
								seen = true;
								if (v->type == PropertyType::NULL_TYPE)
									return;
								check(v->type == PropertyType::INTEGER &&
											  std::binary_search(entries.begin(), entries.end(),
																 Posting{std::bit_cast<int64_t>(v->bits), id}),
									  "Incomplete snapshot index");
								++values;
							});
					return true;
				});
				check(values == entries.size(), "Surplus snapshot index postings");
				lease.set(entries.capacity() * sizeof(Posting));
				image->postings = std::move(entries);
				image->postingLease.emplace(std::move(lease));
				debug::PerfTrace::addValue("snapshot.index.audit_nodes", nodes);
				debug::PerfTrace::addValue("snapshot.index.audit_values", values);
			}
			std::optional<SnapshotNodeIndex> singleNodePropertyIndex() const override {
				if (!img->descriptor)
					return std::nullopt;
				const auto &d = *img->descriptor;
				return SnapshotNodeIndex{d.declaration, d.revision,
										 std::string(img->dict()->lookup(NameSpace::NS_LABEL, d.label)),
										 std::string(img->dict()->lookup(NameSpace::NS_PROPERTY, d.key))};
			}
			std::optional<uint64_t> nativeSnapshotGeneration() const override {
				if (!img->descriptor)
					return std::nullopt;
				return img->fence.graph_state_tokens.front() ? img->fence.graph_state_tokens.front() : 1;
			}
			bool supportsNativeExactNodeCandidates() const override { return img->descriptor.has_value(); }
			bool supportsNativeRangeNodeCandidates() const override { return img->descriptor.has_value(); }
			std::optional<bool>
			visitNodeCandidatesByPropertyRange(const std::vector<std::string> &labels,
											   const LogicalGraphPropertyRange &r, size_t nodes, size_t properties,
											   size_t bytes, const IndexCandidateBatchVisitor &visitor) const override {
				if (!img->descriptor || labels.size() != 1 || !properties)
					return std::nullopt;
				const auto &d = *img->descriptor;
				if (labels.front() != img->dict()->lookup(NameSpace::NS_LABEL, d.label) ||
					r.key != img->dict()->lookup(NameSpace::NS_PROPERTY, d.key))
					return std::nullopt;
				const auto *lo = std::get_if<int64_t>(&r.min_value.getVariant());
				const auto *hi = std::get_if<int64_t>(&r.max_value.getVariant());
				if ((!lo && !hi) || (lo ? !safeInteger(*lo) : r.min_value.getType() != PropertyType::NULL_TYPE) ||
					(hi ? !safeInteger(*hi) : r.max_value.getType() != PropertyType::NULL_TYPE))
					return std::nullopt;
				const auto &list = img->postings;
				auto bound = [&](int64_t v, bool after) {
					return after ? std::ranges::upper_bound(list, Posting{v, UINT64_MAX})
								 : std::ranges::lower_bound(list, Posting{v, 0});
				};
				auto first = lo ? bound(*lo, !r.min_inclusive) : list.begin();
				auto end = hi ? bound(*hi, r.max_inclusive) : list.end();
				auto count = static_cast<size_t>(std::max<ptrdiff_t>(0, end - first));
				if (count > 64 || count > nodes || count > bytes / sizeof(EntityRef))
					return std::nullopt;
				IndexCandidateBatch batch;
				batch.snapshot_generation = *nativeSnapshotGeneration();
				batch.candidates.reserve(count);
				for (size_t i = 0; i < count; ++i)
					batch.candidates.push_back(EntityRef{first[i].second});
				std::ranges::sort(batch.candidates, {}, &EntityRef::entity_id);
				debug::PerfTrace::addValue("snapshot.index.candidates", count);
				return visitor(batch);
			}
			std::optional<bool>
			visitNodeCandidatesByExactProperties(const std::vector<std::string> &labels,
												 const std::vector<std::pair<std::string, PropertyValue>> &properties,
												 size_t nodes, size_t fields, size_t bytes,
												 const IndexCandidateBatchVisitor &visitor) const override {
				if (properties.size() != 1 || properties[0].second.getType() != PropertyType::INTEGER)
					return std::nullopt;
				return visitNodeCandidatesByPropertyRange(
						labels, {properties[0].first, properties[0].second, properties[0].second, true, true}, nodes,
						fields, bytes, visitor);
			}

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
						n = nodeIdentity(k, c, img->dict());
					return false;
				});
				check(n.node_id, "Unknown cp003 node");
				if (!s.include_labels)
					n.labels.clear();
				if (s.all_properties || !s.property_tokens.empty())
					n.properties = readProperties(false, id);
				return n;
			}
			bool supportsNativeEdgeRefScans() const override { return true; }
			void visitEdgeRefs(size_t max, const std::function<bool(const EntityRef &)> &visitor) const override {
				check(bool(visitor), "Invalid cp003 relationship visitor");
				// No compact type tokens are advertised. Typed predicates resolve
				// the stored relationship name through the common topology path.
				visitEdgeTopologies(max, [&](const LogicalGraphEdgeTopology &e) {
					return visitor({e.edge_id, true, e.source_node_id, e.target_node_id, 0});
				});
			}
			LogicalGraphEdge readEdgeShaped(uint64_t id, const EntityReadShape &s) const override {
				validateEntityReadShape(s);
				LogicalGraphEdge e;
				img->visit(1, id, [&](auto k, Cursor c) {
					if (k == id)
						e = edgeIdentity(k, c, img->dict());
					return false;
				});
				check(e.edge_id, "Unknown cp003 edge");
				if (!s.include_type)
					e.type.clear();
				if (s.all_properties || !s.property_tokens.empty())
					e.properties = readProperties(true, id);
				return e;
			}
			void visitEdgeTopologies(size_t maximum,
									 const std::function<bool(const LogicalGraphEdgeTopology &)> &f) const override {
				check(maximum && f, "Invalid cp003 topology visit");
				img->visit(1, 0, [&](auto k, Cursor c) {
					check(maximum--, "cp003 edge bound exceeded");
					auto e = edgeIdentity(k, c, img->dict());
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
				EntityReadShape shape;
				shape.include_type = true;
				shape.all_properties = true;
				return readEdgeShaped(id, shape);
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
						names.emplace_back(name(c, img->dict(), NameSpace::NS_LABEL));
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
			template<class Emit>
			void selected(bool edges, std::span<const uint64_t> ids, std::span<const std::string_view> keys,
						  const Admission &admit, Emit &&emit) const {
				check(bool(admit), "Missing property admission");
				owners(2 + unsigned(edges), ids, [&](size_t row, Cursor c) {
					auto n = c.num();
					while (n--) {
						uint32_t key = 0;
						std::string_view spelling;
						if (img->dict()) {
							key = token(c);
							spelling = img->dict()->lookup(NameSpace::NS_PROPERTY, key);
						} else {
							spelling = c.skipText();
						}
						const auto deliver = [&](Span encoded) {
							for (size_t col = 0; col < keys.size(); ++col)
								if (keys[col] == spelling) {
									admit(encoded.size() + spelling.size() + 8);
									emit(row, col, encoded);
								}
						};
						if (img->columns && img->dict()->separate(key, edges)) {
							if (std::ranges::find(keys, spelling) == keys.end())
								continue;
							bool found = false;
							img->visit(4 + uint64_t(key) * 2 + unsigned(edges), ids[row],
									   [&](auto owner, Cursor value) {
										   check(owner == ids[row], "Missing snapshot property cell");
										   deliver(value.b);
										   found = true;
										   return false;
									   });
							check(found, "Missing snapshot property cell");
						} else {
							size_t end = 0;
							(void) CanonicalPropertyValueCodec::inspectNext(c.b, end);
							deliver(c.take(end));
						}
					}
					check(c.b.empty(), "Trailing snapshot property bytes");
				});
			}
			Columns legacyColumns(bool edges, std::span<const uint64_t> ids, std::span<const std::string_view> keys,
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
			Columns columns(bool edges, std::span<const uint64_t> ids, std::span<const std::string_view> keys,
							const Admission &admit) const {
				if (!img->dict())
					return legacyColumns(edges, ids, keys, admit);
				Columns result(keys.size());
				for (auto &col: result)
					col.resize(ids.size());
				selected(edges, ids, keys, admit, [&](size_t row, size_t col, Span value) {
					size_t offset = 0;
					result[col][row] = CanonicalPropertyValueCodec::decodeNext(value, offset);
				});
				return result;
			}
			bool supportsNodePropertyCellBatches() const override { return img->columns; }
			bool hasImmutableReadIdentity() const noexcept override { return img->columns; }
			void readNodePropertyCellsSortedWithAdmission(std::span<const uint64_t> ids,
														  std::span<const std::string_view> keys,
														  const PropertyCellSink &sink,
														  const Admission &admit) const override {
				check(bool(sink), "Missing snapshot property sink");
				selected(false, ids, keys, admit, [&](size_t row, size_t col, Span value) {
					size_t offset = 0;
					sink(row, col, CanonicalPropertyValueCodec::decodeNext(value, offset));
				});
			}
			void readNodePropertyCellsWithViewsSortedWithAdmission(std::span<const uint64_t> ids,
																   std::span<const std::string_view> keys,
																   const PropertyCellSink &values,
																   const PropertyScalarReadSink &scalars,
																   const PropertyTextReadSink &texts,
																   const Admission &admit) const override {
				check(values && scalars && texts, "Missing snapshot property sinks");
				selected(false, ids, keys, admit, [&](size_t row, size_t col, Span value) {
					if (auto p = CanonicalPropertyValueCodec::tryBorrowPrimitive(value)) {
						if (p->type == PropertyType::STRING)
							texts(row, col, p->text);
						else
							scalars(row, col, p->type, p->bits);
					} else {
						size_t offset = 0;
						values(row, col, CanonicalPropertyValueCodec::decodeNext(value, offset));
					}
				});
			}
			bool tryReadNodePropertyColumnBatchesSortedWithAdmission(std::span<const uint64_t> ids,
																	 std::span<const std::string_view> keys,
																	 const PropertyCellSink &values,
																	 const PropertyColumnBatchReadSinks &batches,
																	 const Admission &admit) const override {
				if (!img->columns)
					return false;
				check(batches.scalars && batches.texts, "Missing snapshot property runs");
				// Borrowed text is consumed before the owning window is released.
				readNodePropertyCellsWithViewsSortedWithAdmission(
						ids, keys, values,
						[&](size_t row, size_t col, PropertyType type, uint64_t bits) {
							batches.scalars({&row, 1}, col, type, {&bits, 1});
						},
						[&](size_t row, size_t col, std::string_view text) {
							batches.texts({&row, 1}, col, {&text, 1});
						},
						admit);
				return true;
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
					if (img->dict()) {
						auto t = c;
						auto count = t.num();
						while (count--)
							a(name(t, img->dict(), NameSpace::NS_LABEL).size() + 8);
					}
					auto n = nodeIdentity(ids[i], c, img->dict());
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
					if (img->dict()) {
						auto t = c;
						t.num();
						t.num();
						a(name(t, img->dict(), NameSpace::NS_TYPE).size() + 8);
					}
					auto e = edgeIdentity(ids[i], c, img->dict());
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
				if (!image) {
					auto next = std::make_shared<Image>(path, budget);
					Snapshot::validateIndex(next);
					image = std::move(next);
				}
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
