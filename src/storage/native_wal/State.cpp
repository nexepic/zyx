#include "State.hpp"
#include "graph/storage/relocatable/schema/ConstraintSupportTree.hpp"
namespace graph::storage::nativewal {
	void Encoder::value(const PropertyValue &v) {
		auto type = v.getType();
		require(type == PropertyType::NULL_TYPE || type == PropertyType::BOOLEAN || type == PropertyType::INTEGER ||
						type == PropertyType::DOUBLE || type == PropertyType::STRING,
				"Native WAL supports scalar properties only");
		auto size = CanonicalPropertyValueCodec::encodedSize(v);
		require(size <= maxValue, "Native WAL value limit exceeded");
		num(size);
		room(size);
		CanonicalPropertyValueCodec::append(bytes, v);
	}
	namespace {
		void labels(const std::vector<std::string> &names) {
			require(names.size() <= maxFields, "Too many native WAL labels");
			std::set<std::string_view> seen;
			for (auto &n: names)
				require(!n.empty() && n.size() <= maxValue && seen.insert(n).second,
						"Invalid or duplicate native WAL label");
		}
		void encodeProperties(Encoder &w, const std::vector<LogicalGraphPropertyEntry> &props) {
			require(props.size() <= maxFields, "Too many native WAL properties");
			w.num(props.size());
			std::set<std::string_view> seen;
			for (auto &p: props) {
				require(!p.key.empty() && seen.insert(p.key).second, "Invalid or duplicate native WAL property");
				w.text(p.key);
				w.value(p.value);
			}
		}
		auto decodeProperties(Decoder &r) {
			std::vector<LogicalGraphPropertyEntry> out;
			auto n = r.count(maxFields);
			std::set<std::string> seen;
			while (n--) {
				auto key = r.text();
				require(!key.empty() && seen.insert(key).second, "Duplicate native WAL property");
				auto bytes = r.take(r.count(maxValue));
				require(CanonicalPropertyValueCodec::tryBorrowPrimitive(bytes).has_value(),
						"Unsupported native WAL property");
				size_t offset = 0;
				auto value = CanonicalPropertyValueCodec::decodeNext(bytes, offset);
				require(offset == bytes.size(), "Invalid native WAL property extent");
				out.push_back({std::move(key), std::move(value)});
			}
			return out;
		}
		auto properties(const std::vector<LogicalGraphWriteProperty> &props) {
			require(props.size() <= maxFields, "Too many native WAL properties");
			std::vector<LogicalGraphPropertyEntry> out;
			Encoder check(maxState);
			for (auto &p: props) {
				check.text(p.key);
				check.value(p.value);
				out.push_back({p.key, p.value});
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
		size_t remaining = maxState;
		auto charge = [&](size_t bytes) {
			require(bytes <= remaining, "Native statement payload limit exceeded");
			remaining -= bytes;
		};
		auto fields = [&](const auto &props) {
			require(props.size() <= maxFields, "Too many native fields");
			for (auto &p: props) {
				charge(p.key.size());
				charge(CanonicalPropertyValueCodec::encodedSize(p.value));
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
			require(e.type.size() <= maxValue, "Native WAL relationship type too long");
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
				auto row = rows.at(id);
				row.properties = properties(p.properties);
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
			auto row = s.nodes.at(id);
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
		void encodeRow(Encoder &w, const LogicalGraphNode &n) {
			labels(n.labels);
			w.num(n.labels.size());
			for (auto &l: n.labels)
				w.text(l);
			encodeProperties(w, n.properties);
		}
		void encodeRow(Encoder &w, const LogicalGraphEdge &e) {
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
		VersionCharge retained(s.budget, 0);
		std::set<Bytes> keys;
		size_t charged = 0;
		s.nodes.visitAfter(0, [&](auto row) {
			if (check)
				check();
			if (admit)
				admit(row.encodedBytes());
			const auto &n = row->second;
			if (std::ranges::find(n.labels, s.declaration->label) == n.labels.end())
				return true;
			const auto field = std::ranges::find(n.properties, s.declaration->key, &LogicalGraphPropertyEntry::key);
			if (field == n.properties.end() || field->value.getType() == PropertyType::NULL_TYPE)
				return true;
			VersionCharge scratch(s.budget, 4 * row.encodedBytes() + 4096);
			auto key = ConstraintSupportTree::canonicalKey(std::span(&field->value, 1));
			charged += key.capacity() + sizeof(Bytes) + 64;
			retained.growTo(charged);
			if (!keys.insert(std::move(key)).second)
				throw UniqueViolation("Constraint violation: '" + s.unique->name + "' has a duplicate key");
			return true;
		});
	}
	Bytes encodeDelta(const State &s, const Changes &changed) {
		Encoder w(maxState);
		w.num(s.nodeHigh);
		w.num(s.edgeHigh);
		auto append = [&](const auto &ids, const auto &rows) {
			w.num(ids.size());
			for (auto id: ids) {
				w.num(id);
				auto it = rows.find(id);
				w.num(it != rows.end());
				if (it != rows.end())
					encodeRow(w, it->second);
			}
		};
		append(changed.nodes, s.nodes);
		append(changed.edges, s.edges);
		return std::move(w.bytes);
	}
	void encodeBaseRow(Encoder &w, const LogicalGraphNode &row) { encodeRow(w, row); }
	void encodeBaseRow(Encoder &w, const LogicalGraphEdge &row) { encodeRow(w, row); }
	namespace {
		LogicalGraphNode readNode(Decoder &r, uint64_t id) {
			LogicalGraphNode n{id, {}};
			auto count = r.count(maxFields);
			while (count--)
				n.labels.push_back(r.text());
			labels(n.labels);
			n.properties = decodeProperties(r);
			return n;
		}
		LogicalGraphEdge readEdge(Decoder &r, uint64_t id) {
			auto a = r.num(), b = r.num();
			auto type = r.text();
			return {id, a, b, std::move(type), decodeProperties(r)};
		}
	} // namespace
	LogicalGraphNode baseNode(uint64_t id, Span bytes) {
		Decoder r{bytes};
		auto row = readNode(r, id);
		require(r.bytes.empty(), "Trailing native node");
		return row;
	}
	LogicalGraphEdge baseEdge(uint64_t id, Span bytes) {
		Decoder r{bytes};
		auto row = readEdge(r, id);
		require(r.bytes.empty(), "Trailing native edge");
		return row;
	}
	void applyDelta(State &s, Span bytes) {
		Decoder r{bytes};
		auto nh = r.num(), eh = r.num();
		require(nh >= s.nodeHigh && eh >= s.edgeHigh && nh <= INT64_MAX && eh <= INT64_MAX,
				"Invalid native WAL identity high-water mark");
		bool removedNode = false;
		std::vector<uint64_t> changedColumnIds;
		auto rows = [&](auto &map, uint64_t old, uint64_t high, auto decode) {
			auto count = r.count(2 * maxRows);
			uint64_t previous = 0;
			while (count--) {
				auto id = r.num();
				require(id > previous && id <= high && (id > old || map.contains(id)),
						"Invalid or resurrected native WAL id");
				previous = id;
				if constexpr (std::is_same_v<typename std::decay_t<decltype(map)>::mapped_type, LogicalGraphNode>)
					if (s.declaration && !s.columnOverflow && changedColumnIds.size() <= ColumnChanges::limit)
						changedColumnIds.push_back(id);
				if (r.count(1))
					map.insert_or_assign(id, decode(id));
				else {
					bool existed = map.erase(id);
					if constexpr (std::is_same_v<typename std::decay_t<decltype(map)>::mapped_type, LogicalGraphNode>)
						removedNode |= existed;
				}
			}
		};
		rows(s.nodes, s.nodeHigh, nh, [&](uint64_t id) { return readNode(r, id); });
		rows(s.edges, s.edgeHigh, eh, [&](uint64_t id) {
			auto edge = readEdge(r, id);
			require(s.nodes.contains(edge.source_node_id) && s.nodes.contains(edge.target_node_id),
					"Native WAL dangling relationship");
			return edge;
		});
		require(r.bytes.empty(), "Trailing native WAL delta");
		s.recordColumnChanges(changedColumnIds);
		s.nodeHigh = nh;
		s.edgeHigh = eh;
		if (removedNode)
			for (auto &[id, e]: s.edges)
				require(s.nodes.contains(e.source_node_id) && s.nodes.contains(e.target_node_id),
						"Native WAL dangling relationship");
	}
} // namespace graph::storage::nativewal
