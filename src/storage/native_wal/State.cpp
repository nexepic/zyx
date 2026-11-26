#include "UniqueDelta.hpp"
#include "graph/core/GuardedPropertyCopy.hpp"
#include "graph/core/PropertyDecodeAllocationScope.hpp"
namespace graph::storage::nativewal {
	namespace {
		bool container(const PropertyValue &v) {
			return v.getType() == PropertyType::LIST || v.getType() == PropertyType::MAP;
		}
		bool copiesHeap(const PropertyValue &v) {
			return container(v) || v.getType() == PropertyType::BYTES || v.getType() == PropertyType::STRING;
		}
		void checkKind(const PropertyValue &v) {
			const auto t = v.getType();
			require(container(v) || t == PropertyType::NULL_TYPE || t == PropertyType::BOOLEAN ||
							t == PropertyType::INTEGER || t == PropertyType::DOUBLE || t == PropertyType::STRING ||
							t == PropertyType::BYTES,
					"Unsupported native WAL property type");
		}
	}
	template<class Buffer> void BasicEncoder<Buffer>::value(const PropertyValue &v) {
		auto size = CanonicalPropertyValueCodec::encodedSize(v);
		checkKind(v);
		num(size);
		room(size);
		if constexpr (std::is_same_v<Buffer, std::pmr::vector<std::byte>>) {
			// Both the output and any canonical map ordering workspace use the row account.
			CanonicalPropertyValueCodec::append(bytes, v);
		} else if (container(v)) {
			// The canonical encoder orders maps with its destination allocator.
			// Charge both that workspace and the temporary encoded value.
			require(bool(budget), "Missing native container encoding budget");
			VersionMemoryResource memory(budget);
			std::pmr::vector<std::byte> wire(&memory);
			wire.reserve(size);
			CanonicalPropertyValueCodec::append(wire, v);
			raw(wire);
		} else {
			CanonicalPropertyValueCodec::append(bytes, v);
		}
	}
	namespace {
		void labels(const std::vector<std::string> &names) {
			require(names.size() <= maxFields, "Too many native WAL labels");
			std::set<std::string_view> seen;
			for (auto &n: names)
				require(!n.empty() && n.size() <= maxName && seen.insert(n).second,
						"Invalid or duplicate native WAL label");
		}
		void encodeProperties(auto &w, const std::vector<LogicalGraphPropertyEntry> &props) {
			require(props.size() <= maxFields, "Too many native WAL properties");
			w.num(props.size());
			std::set<std::string_view> seen;
			for (auto &p: props) {
				require(!p.key.empty() && seen.insert(p.key).second, "Invalid or duplicate native WAL property");
				w.text(p.key);
				w.value(p.value);
			}
		}
		auto decodeProperties(auto &r, VersionCharge &scratch) {
			std::vector<LogicalGraphPropertyEntry> out;
			auto n = r.count(maxFields);
			if constexpr (requires { r.admit(size_t{}); })
				r.admit(n * sizeof(LogicalGraphPropertyEntry));
			out.reserve(n);
			while (n--) {
				auto key = r.text();
				require(!key.empty() && std::ranges::none_of(out, [&](const auto &p) { return p.key == key; }), "Duplicate native WAL property");
				auto bytes = r.take(r.count(SIZE_MAX));
				size_t offset = 0;
				PropertyValue value;
				if (auto primitive = CanonicalPropertyValueCodec::tryBorrowPrimitive(bytes)) {
					if constexpr (requires { r.admit(size_t{}); })
						if (primitive->type == PropertyType::STRING)
							r.admit(primitive->text.size() + 1);
					value = CanonicalPropertyValueCodec::decodeNext(bytes, offset);
				} else {
					auto *query = property_decode::AllocationScope::current();
					value = CanonicalPropertyValueCodec::decodeNextWithAdmission(bytes, offset,
						[&](const auto &shape) {
							scratch.add(shape.maximum_heap_request_bytes);
							if (query) query->admit(shape.maximum_heap_request_bytes);
						}, [&] { if (query) query->checkpoint(); });
					checkKind(value);
				}
				require(offset == bytes.size(), "Invalid native WAL property extent");
				out.push_back({std::move(key), std::move(value)});
			}
			return out;
		}
		auto properties(const std::vector<LogicalGraphWriteProperty> &props) {
			require(props.size() <= maxFields, "Too many native WAL properties");
			std::vector<LogicalGraphPropertyEntry> out;
			out.reserve(props.size());
			for (auto &p: props) {
				out.push_back({p.key, container(p.value) ? property_copy::copyPropertyValueGuarded(p.value, [] {}) : p.value});
			}
			return out;
		}
		uint64_t resolve(const LogicalGraphMutationEntityReference &r, bool edge,
						 const LogicalGraphMutationBatchResult &ids) {
			using E = LogicalGraphMutationEntityKind;
			using K = LogicalGraphMutationReferenceKind;
			require(r.entity_kind == (edge ? E::LGMK_EDGE : E::LGMK_NODE), "Invalid native WAL entity kind");
			if (r.reference_kind == K::LGRK_PERSISTED) {
				require(r.value, "Invalid native WAL id");
				return r.value;
			}
			require(r.reference_kind == (edge ? K::LGRK_CREATED_EDGE : K::LGRK_CREATED_NODE),
					"Invalid native WAL creation reference");
			return (edge ? ids.created_edge_ids : ids.created_node_ids).at(r.value);
		}
		template<class T>
		std::pair<size_t, size_t> cost(const T &row) {
			// Includes the ID and presence tag as stored in a base or delta.
			size_t wire = 16, heap = row.properties.capacity() * sizeof(LogicalGraphPropertyEntry);
			auto text = [&](const std::string &name) {
				wire += 8 + name.size();
				heap += name.capacity() + 1;
			};
			if constexpr (std::is_same_v<T, LogicalGraphNode>) {
				wire += 8;
				heap += row.labels.capacity() * sizeof(std::string);
				for (auto &l: row.labels)
					text(l);
			} else {
				wire += 16;
				text(row.type);
			}
			wire += 8;
			std::set<std::string_view> seen;
			for (auto &p: row.properties) {
				require(!p.key.empty() && seen.insert(p.key).second, "Invalid or duplicate native WAL property");
				text(p.key);
				wire += 8 + CanonicalPropertyValueCodec::encodedSize(p.value);
				if (auto str = std::get_if<std::string>(&p.value.getVariant()))
					heap += str->capacity() + 1;
				else if (container(p.value) || p.value.getType() == PropertyType::BYTES)
					heap += CanonicalPropertyValueCodec::guardedCopyHeapUpper(p.value);
			}
			return {wire, heap};
		}
	} // namespace
	std::pair<size_t, size_t> rowCost(const LogicalGraphNode &n) { return cost(n); }
	std::pair<size_t, size_t> rowCost(const LogicalGraphEdge &e) { return cost(e); }
	LogicalGraphMutationBatchResult preview(const State &s, const LogicalGraphMutationBatch &b) {
		require(!b.creation_identity_plan, "External identity plans are unsupported");
		LogicalGraphMutationBatchResult r;
		auto ids = [](auto &v, uint64_t high, size_t n) {
			require(n <= maxRows && n <= uint64_t(INT64_MAX) - high, "Native WAL id limit exceeded");
			for (size_t i = 0; i < n; ++i)
				v.push_back(++high);
		};
		ids(r.created_node_ids, s.nodeHigh, b.node_creates.size());
		ids(r.created_edge_ids, s.edgeHigh, b.edge_creates.size());
		return r;
	}
	LogicalGraphMutationBatchResult apply(State &s, Changes &changed, const LogicalGraphMutationBatch &b) {
		auto ids = preview(s, b);
		// Bound input copying even when a single batch contains many large
		// values. The final encoded state and the replay delta have separate limits.
		VersionCharge valueCopies(s.budget, 0);
		size_t remaining = s.budget->capacity();
		auto charge = [&](size_t bytes) {
			require(bytes <= remaining, "Native statement payload limit exceeded");
			remaining -= bytes;
		};
		auto fields = [&](const auto &props) {
			require(props.size() <= maxFields, "Too many native fields");
			for (auto &p: props) {
				charge(p.key.size());
				auto size = CanonicalPropertyValueCodec::encodedSize(p.value);
				require(p.key.size() <= maxName, "Native WAL value limit exceeded");
				checkKind(p.value);
				charge(size);
				if (copiesHeap(p.value))
					valueCopies.add(CanonicalPropertyValueCodec::guardedCopyHeapUpper(p.value));
				charge(16);
			}
		};
		for (auto &n: b.node_creates) {
			for (auto &l: n.labels)
				charge(l.size() + 8);
			fields(n.properties);
		}
		for (auto &e: b.edge_creates) {
			charge(e.type.size());
			fields(e.properties);
		}
		for (auto &n: b.node_label_replacements) {
			for (auto &l: n.labels)
				charge(l.size() + 8);
		}
		for (auto &p: b.property_replacements)
			fields(p.properties);

		for (auto &n: b.node_creates) {
			labels(n.labels);
			auto id = ++s.nodeHigh;
			s.nodes.insert_or_assign(id, LogicalGraphNode{id, n.labels, properties(n.properties)});
			changed.nodes.insert(id);
		}
		for (auto &e: b.edge_creates) {
			require(e.type.size() <= maxName, "Native WAL relationship type too long");
			auto id = ++s.edgeHigh;
			s.edges.insert_or_assign(id,
									 LogicalGraphEdge{id, resolve(e.source, false, ids), resolve(e.target, false, ids),
													  e.type, properties(e.properties)});
			changed.edges.insert(id);
		}
		for (auto &p: b.property_replacements) {
			bool edge = p.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE;
			auto id = resolve(p.entity, edge, ids);
			auto replace = [&](auto &rows) {
				auto prior = rows.find(id);
				require(prior != rows.end(), "Unknown native WAL property owner");
				// Replace the fields without copying the superseded property graph.
				const auto &old = prior->second;
				using Row = std::decay_t<decltype(old)>;
				Row row;
				if constexpr (std::is_same_v<Row, LogicalGraphNode>)
					row = {old.node_id, old.labels, properties(p.properties)};
				else
					row = {old.edge_id, old.source_node_id, old.target_node_id, old.type, properties(p.properties)};
				rows.insert_or_assign(id, std::move(row));
			};
			if (edge)
				replace(s.edges);
			else
				replace(s.nodes);
			(edge ? changed.edges : changed.nodes).insert(id);
		}
		for (auto &n: b.node_label_replacements) {
			labels(n.labels);
			auto id = resolve(n.node, false, ids);
			auto prior = s.nodes.find(id);
			require(prior != s.nodes.end(), "Unknown native WAL label owner");
			const auto &old = prior->second;
			VersionCharge copying(s.budget, 0);
			for (const auto &p : old.properties)
				if (copiesHeap(p.value))
					copying.add(CanonicalPropertyValueCodec::guardedCopyHeapUpper(p.value));
			auto row = old;
			row.labels = n.labels;
			s.nodes.insert_or_assign(id, std::move(row));
			changed.nodes.insert(id);
		}
		for (auto &e: b.edge_deletes) {
			require(s.edges.erase(e.edge_id), "Unknown native WAL relationship");
			changed.edges.insert(e.edge_id);
		}
		for (auto &n: b.node_deletes) {
			require(s.nodes.erase(n.node_id), "Unknown native WAL node");
			changed.nodes.insert(n.node_id);
			size_t count = 0;
			for (auto it = s.edges.begin(); it != s.edges.end();) {
				auto &e = it->second;
				if (e.source_node_id != n.node_id && e.target_node_id != n.node_id) {
					++it;
					continue;
				}
				require(n.detach && ++count <= n.maximum_detach_edges,
						"Native WAL node deletion has incident relationships");
				changed.edges.insert(it->first);
				auto erased = it->first;
				s.edges.erase(erased);
				it = s.edges.upper_bound(erased);
			}
		}
		for (auto id: ids.created_edge_ids) {
			auto it = s.edges.find(id);
			if (it != s.edges.end())
				require(s.nodes.contains(it->second.source_node_id) && s.nodes.contains(it->second.target_node_id),
						"Native WAL dangling relationship");
		}
		return ids;
	}
	namespace {
		void encodeRow(auto &w, const LogicalGraphNode &n) {
			labels(n.labels);
			w.num(n.labels.size());
			for (auto &l: n.labels)
				w.text(l);
			encodeProperties(w, n.properties);
		}
		void encodeRow(auto &w, const LogicalGraphEdge &e) {
			w.num(e.source_node_id);
			w.num(e.target_node_id);
			w.text(e.type);
			encodeProperties(w, e.properties);
		}
	} // namespace
	// The first writable constraint slice uses the same canonical tuple codec as
	// the product validator. Admission includes the ordered set and decoded row.
	void validateUnique(const State &s, const std::function<void()> &check, const std::function<void(size_t)> &admit) {
		if (!s.unique)
			return;
		require(bool(s.declaration), "Missing native UNIQUE scope");
		validateScope(s, {s.unique->name, s.declaration->label, s.declaration->key, s.unique->revision}, check, admit);
	}
	void validateCatalog(const State &s) {
		if (s.catalog)
			for (const auto &entry: s.catalog->entries)
				validateScope(s, entry);
	}
	void validateScope(const State &s, const ScopedUnique &scope, const std::function<void()> &check,
					   const std::function<void(size_t)> &admit) {
		VersionMemoryResource memory(s.budget);
		std::pmr::set<std::pmr::vector<std::byte>> keys(&memory);
		s.nodes.visitAfter(0, [&](auto row) {
			if (check)
				check();
			if (admit)
				admit(row.encodedBytes());
			const auto &n = row->second;
			if (std::ranges::find(n.labels, scope.label) == n.labels.end())
				return true;
			const auto field = std::ranges::find(n.properties, scope.key, &LogicalGraphPropertyEntry::key);
			if (field == n.properties.end() || field->value.getType() == PropertyType::NULL_TYPE)
				return true;
			// A single-property UNIQUE key is its complete canonical encoding.
			// Only the key and its set entry survive the current row.
			std::pmr::vector<std::byte> key(&memory);
			key.reserve(CanonicalPropertyValueCodec::encodedSize(field->value));
			CanonicalPropertyValueCodec::append(key, field->value);
			if (!keys.insert(std::move(key)).second)
				throw UniqueViolation("Constraint violation: '" + scope.name + "' has a duplicate key");
			return true;
		});
	}
	// The preceding snapshot is already valid. Any new violation therefore
	// has at least one surviving changed owner. Keep only those owners' final
	// canonical keys and compare against the final graph (including WAL rows).
	void validateAffectedScope(const State &s, const State *prior, const ScopedUnique &scope,
							   const ChangedNodeIds &ids) {
		VersionMemoryResource memory(s.budget);
		// The canonical wire format tags INTEGER separately and encodes
		// int64 bits injectively. All other types retain their complete wire key.
		using Key = std::variant<int64_t, std::pmr::vector<std::byte>>;
		std::pmr::map<Key, uint64_t> claims(&memory);
		auto encode = [&](const LogicalGraphNode &n) -> std::optional<Key> {
			if (std::ranges::find(n.labels, scope.label) == n.labels.end())
				return {};
			const auto field = std::ranges::find(n.properties, scope.key, &LogicalGraphPropertyEntry::key);
			if (field == n.properties.end() || field->value.getType() == PropertyType::NULL_TYPE)
				return {};
			if (const auto *v = std::get_if<int64_t>(&field->value.getVariant()))
				return *v;
			std::pmr::vector<std::byte> wire(&memory);
			CanonicalPropertyValueCodec::append(wire, field->value);
			return Key{std::move(wire)};
		};
		auto duplicate = [&] {
			throw UniqueViolation("Constraint violation: '" + scope.name + "' has a duplicate key");
		};
		ids([&](uint64_t id) {
			auto now = s.nodes.find(id);
			if (now == s.nodes.end())
				return;
			auto key = encode(now->second);
			if (!key)
				return;
			if (prior) {
				const auto old = prior->nodes.find(id);
				if (old != prior->nodes.end() && key == encode(old->second))
					return;
			}
			if (!claims.emplace(std::move(*key), id).second)
				duplicate();
		});
		if (claims.empty())
			return;
		auto inspect = [&](const LogicalGraphNode &row) {
			auto key = encode(row);
			if (!key)
				return;
			auto hit = claims.find(*key);
			if (hit != claims.end() && hit->second != row.node_id)
				duplicate();
		};
		size_t inspected = 0;
		size_t projected = 0;
		const bool sameScope =
				s.column && s.declaration && s.declaration->label == scope.label && s.declaration->key == scope.key;
		if (sameScope && s.column->complete) {
			// The sealed complete base contains only exact integers in this
			// scope. An absent base key still requires the full resident overlay.
			for (const auto &[key, claimant]: claims) {
				const auto *integer = std::get_if<int64_t>(&key);
				if (!integer)
					continue;
				const int64_t value = *integer;
				const auto &postings = s.column->postings;
				for (auto i = std::ranges::lower_bound(postings, Posting{value, 0});
					 i != postings.end() && i->first == value; ++i) {
					if (i->second == claimant)
						continue;
					auto owner = s.nodes.find(i->second);
					if (owner != s.nodes.end() && owner.onDisk()) {
						++inspected;
						inspect(owner->second);
					}
				}
			}
			s.nodes.visitResident([&](const auto &entry) {
				++inspected;
				inspect(entry.second);
				return true;
			});
		} else {
			// The id-ordered column authenticates only its listed base rows.
			// Walk it alongside the ordered final graph. A missing id, another
			// scope or a resident replacement requires the authoritative value.
			size_t pos = 0;
			s.nodes.visitAfter(0, [&](auto row) {
				if (sameScope && row.onDisk()) {
					const auto &column = s.column->column;
					while (pos < column.size() && column[pos].first < row.id())
						++pos;
					if (pos < column.size() && column[pos].first == row.id()) {
						++projected;
						auto hit = claims.find(Key{column[pos++].second});
						if (hit == claims.end() || hit->second == row.id())
							return true;
					}
				}
				++inspected;
				inspect(row->second);
				return true;
			});
		}
		debug::PerfTrace::addValue("native_wal.unique.projected_base_rows", projected);
		debug::PerfTrace::addValue("native_wal.unique.affected_keys", claims.size());
		debug::PerfTrace::addValue("native_wal.unique.affected_probe_rows", inspected);
	}
	size_t deltaBytes(const State &s, const Changes &changed) {
		// Two identity high-water marks and two changed-ID counts. Row
		// metadata includes its ID and presence tag, also used by the delta.
		size_t size = 32;
		auto measure = [&](const auto &ids, const auto &rows) {
			for (auto id : ids) {
				auto it = rows.find(id);
				const size_t bytes = it == rows.end() ? 16 : it.encodedBytes();
				require(bytes - 16 <= s.budget->capacity(), "Native WAL row exceeds version budget");
				require(bytes <= SIZE_MAX - size, "Native WAL transaction size overflow");
				size += bytes;
			}
		};
		measure(changed.nodes, s.nodes);
		measure(changed.edges, s.edges);
		return size;
	}
	namespace {
		template<class Number, class Row>
		void visitDelta(const State &s, const Changes &changed, Number number, Row row) {
			number(s.nodeHigh);
			number(s.edgeHigh);
			auto append = [&](const auto &ids, const auto &rows) {
				number(ids.size());
				for (auto id: ids) {
					number(id);
					auto it = rows.find(id);
					number(it != rows.end());
					if (it != rows.end())
						row(it);
				}
			};
			append(changed.nodes, s.nodes);
			append(changed.edges, s.edges);
		}
	}
	void encodeDelta(Encoder &out, const State &s, const Changes &changed) {
		visitDelta(s, changed, [&](uint64_t n) { out.num(n); },
				   [&](auto it) { encodeRow(out, it->second); });
	}
	void writeDelta(const State &s, const Changes &changed, const std::function<void(Span)> &emit) {
		auto number = [&](uint64_t n) {
			std::array<std::byte, 8> bytes;
			for (unsigned i = 0; i < bytes.size(); ++i)
				bytes[i] = std::byte(n >> (8 * i));
			emit(bytes);
		};
		visitDelta(s, changed, number, [&](auto it) {
			const size_t size = it.encodedBytes() - 16;
			VersionMemoryResource memory(s.budget);
			RowEncoder row(size, s.budget, &memory);
			row.bytes.reserve(size);
			encodeRow(row, it->second);
			require(row.bytes.size() == size, "Native WAL row size mismatch");
			emit(row.bytes);
		});
	}
	void encodeBaseRow(RowEncoder &w, const LogicalGraphNode &row) { encodeRow(w, row); }
	void encodeBaseRow(RowEncoder &w, const LogicalGraphEdge &row) { encodeRow(w, row); }
	namespace {
		LogicalGraphNode readNode(auto &r, uint64_t id, VersionCharge &scratch) {
			LogicalGraphNode n{id, {}};
			auto count = r.count(maxFields);
			if constexpr (requires { r.admit(size_t{}); })
				r.admit(count * sizeof(std::string));
			n.labels.reserve(count);
			while (count--)
				n.labels.push_back(r.text());
			labels(n.labels);
			n.properties = decodeProperties(r, scratch);
			return n;
		}
		LogicalGraphEdge readEdge(auto &r, uint64_t id, VersionCharge &scratch) {
			auto a = r.num(), b = r.num();
			auto type = r.text();
			return {id, a, b, std::move(type), decodeProperties(r, scratch)};
		}
	} // namespace
	namespace {
		struct RowDecoder : Decoder {
			VersionCharge &owned;
			void admit(size_t n) { owned.add(n); }
			std::string text() {
				auto bytes = take(count(maxName));
				admit(bytes.size() + 1);
				return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
			}
		};
	}
	LogicalGraphNode baseNode(uint64_t id, Span bytes, VersionCharge &scratch) {
		RowDecoder r{{bytes}, scratch};
		auto row = readNode(r, id, scratch);
		require(r.bytes.empty(), "Trailing native node");
		return row;
	}
	LogicalGraphEdge baseEdge(uint64_t id, Span bytes, VersionCharge &scratch) {
		RowDecoder r{{bytes}, scratch};
		auto row = readEdge(r, id, scratch);
		require(r.bytes.empty(), "Trailing native edge");
		return row;
	}
	namespace {
		class DeltaReader {
			const std::function<void(std::span<std::byte>)> &source;
			size_t left, rowEnd = 0, rowLimit;
			VersionMemoryResource memory;
			std::pmr::vector<std::byte> field;
			std::array<std::byte, 64> small{};
			VersionCharge *rowAllocation = nullptr;

		public:
			DeltaReader(size_t extent, const std::function<void(std::span<std::byte>)> &read,
						const std::shared_ptr<VersionBudget> &budget)
				: source(read), left(extent), rowLimit(budget->capacity()), memory(budget), field(&memory) {}
			size_t remaining() const { return left; }
			void beginRow(VersionCharge &scratch) {
				rowEnd = left > rowLimit ? left - rowLimit : 0;
				rowAllocation = &scratch;
			}
			void endRow() {
				rowEnd = 0;
				rowAllocation = nullptr;
			}
			void admit(size_t bytes) { rowAllocation->add(bytes); }
			Span take(size_t size) {
				require(size <= left - rowEnd, "Truncated native WAL transaction or oversized row");
				std::span<std::byte> dest;
				if (size <= small.size())
					dest = std::span(small).first(size);
				else {
					if (size > field.capacity())
						std::pmr::vector<std::byte>(&memory).swap(field);
					field.resize(size);
					dest = field;
				}
				source(dest);
				left -= size;
				return dest;
			}
			uint64_t num() { return Decoder{take(8)}.num(); }
			size_t count(size_t limit) {
				auto n = num();
				require(n <= limit, "Native WAL count or length exceeds limit");
				return size_t(n);
			}
			std::string text() {
				const auto n = count(maxName);
				admit(n + 1);
				auto bytes = take(n);
				return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
			}
		};
	}
	void applyDelta(State &s, size_t extent, const std::function<void(std::span<std::byte>)> &read,
					UniqueDelta &unique) {
		DeltaReader r(extent, read, s.budget);
		auto nh = r.num(), eh = r.num();
		require(nh >= s.nodeHigh && eh >= s.edgeHigh && nh <= INT64_MAX && eh <= INT64_MAX,
				"Invalid native WAL identity high-water mark");
		bool removedNode = false;
		VersionMemoryResource idMemory(s.budget);
		std::pmr::vector<uint64_t> changedIds(&idMemory);
		bool haveIds = true;
		std::vector<uint64_t> changedColumnIds;
		size_t remainingChanges = 2 * maxRows; // Same ceiling as the writer across nodes and edges.
		auto rows = [&](auto &map, uint64_t old, uint64_t high, auto decode) {
			auto count = r.count(std::min(remainingChanges, r.remaining() / 16));
			remainingChanges -= count;
			constexpr bool nodes = std::is_same_v<typename std::decay_t<decltype(map)>::mapped_type, LogicalGraphNode>;
			if constexpr (nodes) {
				if (s.unique || s.catalog) {
					try {
						changedIds.reserve(count);
					} catch (const std::length_error &) {
						haveIds = false;
					} catch (const std::bad_alloc &) {
						haveIds = false;
					}
				}
				unique.reserve(count);
			}
			uint64_t previous = 0;
			while (count--) {
				auto id = r.num();
				require(id > previous && id <= high && (id > old || map.contains(id)),
						"Invalid or resurrected native WAL id");
				previous = id;
				UniqueDelta::Key before;
				if constexpr (nodes) {
					if (haveIds && (s.unique || s.catalog))
						changedIds.push_back(id);
					before = unique.key(s, id);
				}
				if constexpr (std::is_same_v<typename std::decay_t<decltype(map)>::mapped_type, LogicalGraphNode>)
					if (s.declaration && !s.columnOverflow && changedColumnIds.size() <= ColumnChanges::limit)
						changedColumnIds.push_back(id);
				if (r.count(1)) {
					VersionCharge scratch(s.budget, 0);
					r.beginRow(scratch);
					auto row = decode(id, scratch);
					r.endRow();
					map.insert_or_assign(id, std::move(row), &scratch);
				} else {
					bool existed = map.erase(id);
					if constexpr (std::is_same_v<typename std::decay_t<decltype(map)>::mapped_type, LogicalGraphNode>)
						removedNode |= existed;
				}
				if constexpr (nodes)
					unique.observe(id, before, unique.key(s, id));
			}
		};
		rows(s.nodes, s.nodeHigh, nh, [&](uint64_t id, VersionCharge &scratch) { return readNode(r, id, scratch); });
		rows(s.edges, s.edgeHigh, eh, [&](uint64_t id, VersionCharge &scratch) {
			auto edge = readEdge(r, id, scratch);
			require(s.nodes.contains(edge.source_node_id) && s.nodes.contains(edge.target_node_id),
					"Native WAL dangling relationship");
			return edge;
		});
		require(r.remaining() == 0, "Trailing native WAL delta");
		s.recordColumnChanges(changedColumnIds);
		s.nodeHigh = nh;
		s.edgeHigh = eh;
		if (removedNode)
			for (auto &[id, e]: s.edges)
				require(s.nodes.contains(e.source_node_id) && s.nodes.contains(e.target_node_id),
						"Native WAL dangling relationship");
		if (s.unique || s.catalog) {
			if (haveIds) {
				unique.validate(s, nullptr, [&](const auto &visit) {
					for (auto id: changedIds)
						visit(id);
				});
			} else {
				// An absent optional change list carries no coverage proof.
				// Preserve the original full audit for this WAL transaction.
				validateUnique(s);
				validateCatalog(s);
			}
		}
	}
} // namespace graph::storage::nativewal
