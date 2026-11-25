#include "State.hpp"
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
		void validate(const State &s) {
			require(s.nodes.size() + s.edges.size() <= maxRows, "Native WAL entity limit exceeded");
			for (auto &[id, e]: s.edges)
				require(s.nodes.contains(e.source_node_id) && s.nodes.contains(e.target_node_id),
						"Native WAL dangling relationship");
		}
	} // namespace
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
			s.nodes.emplace(id, LogicalGraphNode{id, n.labels, properties(n.properties)});
			changed.nodes.insert(id);
		}
		for (auto &e: b.edge_creates) {
			require(e.type.size() <= maxValue, "Native WAL relationship type too long");
			auto id = ++s.edgeHigh;
			s.edges.emplace(id, LogicalGraphEdge{id, resolve(e.source, false, ids), resolve(e.target, false, ids),
												 e.type, properties(e.properties)});
			changed.edges.insert(id);
		}
		for (auto &p: b.property_replacements) {
			bool edge = p.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE;
			auto id = resolve(p.entity, edge, ids);
			(edge ? s.edges.at(id).properties : s.nodes.at(id).properties) = properties(p.properties);
			(edge ? changed.edges : changed.nodes).insert(id);
		}
		for (auto &n: b.node_label_replacements) {
			labels(n.labels);
			auto id = resolve(n.node, false, ids);
			s.nodes.at(id).labels = n.labels;
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
				it = s.edges.erase(it);
			}
		}
		validate(s);
		return ids;
	}
	Bytes encodeDelta(const State &s, const Changes &c) {
		Encoder w(maxState);
		w.num(s.nodeHigh);
		w.num(s.edgeHigh);
		w.num(c.nodes.size());
		for (auto id: c.nodes) {
			w.num(id);
			auto it = s.nodes.find(id);
			w.num(it != s.nodes.end());
			if (it != s.nodes.end()) {
				auto &n = it->second;
				labels(n.labels);
				w.num(n.labels.size());
				for (auto &l: n.labels)
					w.text(l);
				encodeProperties(w, n.properties);
			}
		}
		w.num(c.edges.size());
		for (auto id: c.edges) {
			w.num(id);
			auto it = s.edges.find(id);
			w.num(it != s.edges.end());
			if (it != s.edges.end()) {
				auto &e = it->second;
				w.num(e.source_node_id);
				w.num(e.target_node_id);
				w.text(e.type);
				encodeProperties(w, e.properties);
			}
		}
		return std::move(w.bytes);
	}
	Bytes encodeBase(const State &s) {
		Changes c;
		for (auto &[id, n]: s.nodes)
			c.nodes.insert(id);
		for (auto &[id, e]: s.edges)
			c.edges.insert(id);
		return encodeDelta(s, c);
	}
	void applyDelta(State &s, Span bytes) {
		Decoder r{bytes};
		auto nh = r.num(), eh = r.num();
		require(nh >= s.nodeHigh && eh >= s.edgeHigh && nh <= INT64_MAX && eh <= INT64_MAX,
				"Invalid native WAL identity high-water mark");
		auto rows = [&](auto &map, uint64_t old, uint64_t high, auto decode) {
			auto count = r.count(2 * maxRows);
			uint64_t previous = 0;
			while (count--) {
				auto id = r.num();
				require(id > previous && id <= high && (id > old || map.contains(id)),
						"Invalid or resurrected native WAL id");
				previous = id;
				if (r.count(1))
					map.insert_or_assign(id, decode(id));
				else
					map.erase(id);
			}
		};
		rows(s.nodes, s.nodeHigh, nh, [&](uint64_t id) {
			LogicalGraphNode n{id, {}};
			auto count = r.count(maxFields);
			while (count--)
				n.labels.push_back(r.text());
			labels(n.labels);
			n.properties = decodeProperties(r);
			return n;
		});
		rows(s.edges, s.edgeHigh, eh, [&](uint64_t id) {
			auto a = r.num(), b = r.num();
			auto type = r.text();
			return LogicalGraphEdge{id, a, b, std::move(type), decodeProperties(r)};
		});
		require(r.bytes.empty(), "Trailing native WAL delta");
		s.nodeHigh = nh;
		s.edgeHigh = eh;
		validate(s);
	}
} // namespace graph::storage::nativewal
