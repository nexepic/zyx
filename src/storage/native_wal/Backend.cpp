#include "graph/storage/LogicalGraphBulkPublication.hpp"
#include "Backend.hpp"
#include "Journal.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"
#include "graph/storage/NativeWalDatabase.hpp"
#include "graph/storage/SnapshotNodeIndex.hpp"
#include "query/execution/ProcessResourceDomains.hpp"
namespace graph::storage::nativewal {
	namespace resource = query::execution::detail;
	class Snapshot final : public LogicalGraphReadSnapshot, public SnapshotNodeIndexSource {
	public:
		std::shared_ptr<const State> data;
		explicit Snapshot(std::shared_ptr<const State> value) : data(std::move(value)) {}
		std::optional<LogicalGraphMutationFence> mutationFence() const override {
			LogicalGraphMutationFence f;
			f.backend_token = 0x6e673031;
			f.format_token = data->unique ? 5 : data->declaration ? 4 : 3;
			std::memcpy(f.graph_state_tokens.data(), data->identity.data(), 16);
			f.graph_state_tokens[2] = data->sequence;
			return f;
		}
		std::optional<SnapshotNodeIndex> singleNodePropertyIndex() const override {
			if (!data->declaration)
				return std::nullopt;
			return SnapshotNodeIndex{1, 1, data->declaration->label, data->declaration->key};
		}
		std::optional<uint64_t> nativeSnapshotGeneration() const override {
			if (!data->declaration || data->sequence == UINT64_MAX)
				return std::nullopt;
			return data->sequence + 1;
		}
		bool supportsNativeExactNodeCandidates() const override {
			return nativeSnapshotGeneration().has_value() && data->column && data->column->complete &&
				   (!data->columnOverflow || data->nodes.residentCount() <= data->nodes.size() / 2);
		}
		bool supportsNativeRangeNodeCandidates() const override { return supportsNativeExactNodeCandidates(); }
		std::optional<bool>
		visitNodeCandidatesByPropertyRange(const std::vector<std::string> &labels, const LogicalGraphPropertyRange &r,
										   size_t nodes, size_t fields, size_t bytes,
										   const IndexCandidateBatchVisitor &visitor) const override {
			if (!supportsNativeExactNodeCandidates() || !fields || labels.size() != 1 ||
				labels.front() != data->declaration->label || r.key != data->declaration->key)
				return std::nullopt;
			const auto *lo = std::get_if<int64_t>(&r.min_value.getVariant());
			const auto *hi = std::get_if<int64_t>(&r.max_value.getVariant());
			if ((!lo && !hi) || (lo ? !exactColumnInteger(*lo) : r.min_value.getType() != PropertyType::NULL_TYPE) ||
				(hi ? !exactColumnInteger(*hi) : r.max_value.getType() != PropertyType::NULL_TYPE))
				return std::nullopt;
			const auto &base = data->column->postings;
			auto bound = [&](int64_t v, bool after) {
				return after ? std::ranges::upper_bound(base, Posting{v, UINT64_MAX})
							 : std::ranges::lower_bound(base, Posting{v, 0});
			};
			auto first = lo ? bound(*lo, !r.min_inclusive) : base.begin();
			auto last = hi ? bound(*hi, r.max_inclusive) : base.end();
			auto cap = std::min({size_t(64), nodes, bytes / sizeof(EntityRef)});
			if (last < first || last - first > static_cast<ptrdiff_t>(cap))
				return std::nullopt;
			IndexCandidateBatch batch;
			batch.snapshot_generation = *nativeSnapshotGeneration();
			const auto &changes = data->changedColumn;
			const bool useResident = data->columnOverflow;
			if (useResident && data->nodes.residentCount() > nodes)
				return std::nullopt;
			for (auto it = first; it < last; ++it) {
				if (useResident) {
					// A base posting may now refer to a deleted or replaced row.
					auto current = data->nodes.find(it->second);
					if (current == data->nodes.end() || !current.onDisk())
						continue;
				} else if (changes && std::ranges::binary_search(changes->ids, it->second)) {
					continue;
				}
				batch.candidates.push_back(EntityRef{it->second});
			}
			auto inspect = [&](const LogicalGraphNode &row) {
				if (std::ranges::find(row.labels, labels.front()) == row.labels.end())
					return true;
				if (useResident) {
					if (row.properties.size() > fields)
						return false;
					fields -= row.properties.size();
				}
				for (const auto &p: row.properties) {
					if (p.key != r.key || p.value.getType() == PropertyType::NULL_TYPE)
						continue;
					auto value = std::get_if<int64_t>(&p.value.getVariant());
					if (!value || !exactColumnInteger(*value))
						return false;
					if ((!lo || *value > *lo || (*value == *lo && r.min_inclusive)) &&
						(!hi || *value < *hi || (*value == *hi && r.max_inclusive))) {
						if (batch.candidates.size() >= cap)
							return false;
						batch.candidates.push_back(EntityRef{row.node_id});
					}
				}
				return true;
			};
			if (useResident) {
				if (!data->nodes.visitResident([&](const auto &entry) { return inspect(entry.second); }))
					return std::nullopt;
				debug::PerfTrace::addValue("native_wal.index.resident_rows", data->nodes.residentCount());
			} else if (changes) {
				for (auto id: changes->ids) {
					auto it = data->nodes.find(id);
					if (it != data->nodes.end() && !inspect(it->second))
						return std::nullopt;
				}
			}
			std::ranges::sort(batch.candidates, {}, &EntityRef::entity_id);
			debug::PerfTrace::addValue("native_wal.index.probes", 1);
			debug::PerfTrace::addValue("native_wal.index.candidates", batch.candidates.size());
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

		bool supportsNodeExistenceProbes() const override { return true; }
		bool containsNode(uint64_t id) const override { return data->nodes.contains(id); }
		LogicalGraphNode readNode(uint64_t id) const override { return data->nodes.at(id); }
		LogicalGraphEdge readEdge(uint64_t id) const override { return data->edges.at(id); }
		template<class Map, class Visitor>
		static void visit(const Map &rows, size_t limit, const Visitor &fn) {
			rows.visitAfter(0, [&](auto it) {
				require(limit-- > 0, "Native graph visit limit exceeded");
				return fn(it->second);
			});
		}
		void visitNodes(size_t max, const std::function<bool(const LogicalGraphNode &)> &f) const override {
			visit(data->nodes, max, f);
		}
		void visitEdges(size_t max, const std::function<bool(const LogicalGraphEdge &)> &f) const override {
			visit(data->edges, max, f);
		}
		LogicalGraphNodePage scanNodesAfter(uint64_t after, size_t max, size_t pages) const override {
			require(max && pages, "Invalid native graph scan size");
			LogicalGraphNodePage page;
			data->nodes.visitAfter(after, [&](auto it) {
				if (page.nodes.size() == std::min(max, size_t(64))) {
					page.may_have_more = true;
					return false;
				}
				page.nodes.push_back(it->second);
				return true;
			});
			page.metadata_pages_visited = 1;
			return page;
		}
		static bool matches(const LogicalGraphEdge &e, uint64_t id, LogicalGraphReadDirection dir) {
			return (dir == LogicalGraphReadDirection::LGRD_OUTGOING ? e.source_node_id : e.target_node_id) == id;
		}
		template<class Page, class Append>
		Page adjacencyPage(uint64_t id, LogicalGraphReadDirection dir,
						   std::optional<LogicalGraphAdjacencyContinuation> cursor,
						   size_t max, Append append) const {
			require(max && data->nodes.contains(id), "Invalid native adjacency scan");
			uint64_t after = 0;
			if (cursor) {
				after = cursor->previous_edge_id;
				auto previous = data->edges.find(after);
				require(previous != data->edges.end() && matches(previous.edgeHeader(), id, dir),
						"Invalid native adjacency cursor");
			}
			Page page;
			data->adjacentAfter(id, dir == LogicalGraphReadDirection::LGRD_OUTGOING, after, [&](auto it) {
				if (cursor) {
					require(cursor->next_edge_id == it.id(), "Invalid native adjacency continuation");
					cursor.reset();
				}
				if (page.edges.size() == std::min(max, size_t(64))) {
					page.continuation = {{page.edges.back().edge_id, it.id()}};
					return false;
				}
				append(page, it);
				return true;
			});
			require(!cursor, "Missing native adjacency continuation");
			return page;
		}
		LogicalGraphAdjacencyPage scanAdjacentEdges(uint64_t id, LogicalGraphReadDirection dir,
				std::optional<LogicalGraphAdjacencyContinuation> cursor, size_t max) const override {
			return adjacencyPage<LogicalGraphAdjacencyPage>(id, dir, cursor, max,
				[](auto &p, auto it) { p.edges.push_back(it->second); });
		}
		template<class Iterator>
		static LogicalGraphEdgeTopology topology(const Iterator &it) {
			const auto &e = it.edgeHeader().edge_id ? it.edgeHeader() : it->second;
			return {e.edge_id, e.source_node_id, e.target_node_id, e.type};
		}
		LogicalGraphAdjacencyTopologyPage scanAdjacentEdgeTopologies(uint64_t id, LogicalGraphReadDirection dir,
				std::optional<LogicalGraphAdjacencyContinuation> cursor, size_t max) const override {
			return adjacencyPage<LogicalGraphAdjacencyTopologyPage>(id, dir, cursor, max,
				[](auto &p, auto it) { p.edges.push_back(topology(it)); });
		}
		bool supportsNativeAdjacentEdgeTopologyPages() const override { return true; }
		template<class Visitor>
		void visitAdjacent(uint64_t id, LogicalGraphReadDirection dir, size_t max, Visitor visit) const {
			require(data->nodes.contains(id), "Missing native adjacency owner");
			data->adjacentAfter(id, dir == LogicalGraphReadDirection::LGRD_OUTGOING, 0, [&](auto it) {
				require(max-- > 0, "Native adjacency limit exceeded");
				return visit(it);
			});
		}
		void visitAdjacentEdges(uint64_t id, LogicalGraphReadDirection dir, size_t max,
								const std::function<bool(const LogicalGraphEdge &)> &f) const override {
			visitAdjacent(id, dir, max, [&](auto it) { return f(it->second); });
		}
		void visitAdjacentEdgeTopologies(uint64_t id, LogicalGraphReadDirection dir, size_t max,
								const std::function<bool(const LogicalGraphEdgeTopology &)> &f) const override {
			if (!f)
				throw std::invalid_argument("logical graph adjacency topology scan requires a visitor");
			visitAdjacent(id, dir, max, [&](auto it) { return f(topology(it)); });
		}
		using Columns = std::vector<std::vector<PropertyValue>>;
		using Admission = std::function<void(size_t)>;
		template<class Map>
		static Columns columns(const Map &rows, std::span<const uint64_t> ids, std::span<const std::string_view> keys,
							   const Admission &admit) {
			Columns out(keys.size());
			for (auto &col: out)
				col.resize(ids.size());
			uint64_t prev = 0;
			for (size_t i = 0; i < ids.size(); ++i) {
				require(prev < ids[i], "Invalid native graph owner order");
				prev = ids[i];
				auto found = rows.checked(prev);
				for (auto &p: found->second.properties)
					for (size_t k = 0; k < keys.size(); ++k)
						if (keys[k] == p.key) {
							admit(CanonicalPropertyValueCodec::encodedSize(p.value) + p.key.size() + 8);
							out[k][i] = p.value;
						}
			}
			return out;
		}
		bool supportsNativeEdgeRefScans() const override { return true; }
		void visitEdgeRefs(size_t max, const std::function<bool(const EntityRef &)> &visitor) const override {
			visit(data->edges, max,
				  [&](const auto &e) { return visitor({e.edge_id, true, e.source_node_id, e.target_node_id, 0}); });
		}
		bool supportsShapedPropertyReadAdmission() const override { return true; }
		template<class Map>
		static auto shaped(const Map &rows, std::span<const uint64_t> ids, const EntityReadShape &shape,
						   const Admission &admit) {
			validateEntityReadShape(shape);
			using Row = typename Map::mapped_type;
			std::vector<Row> result;
			result.reserve(ids.size());
			uint64_t previous = 0;
			for (auto id: ids) {
				require(id > previous, "Invalid native entity order");
				previous = id;
				auto found = rows.checked(id);
				const auto &source = found->second;
				Row row{};
				if constexpr (std::is_same_v<Row, LogicalGraphNode>) {
					row.node_id = id;
					if (shape.include_labels) {
						for (auto &l: source.labels)
							admit(l.size() + 8);
						row.labels = source.labels;
					}
				} else {
					row.edge_id = id;
					row.source_node_id = source.source_node_id;
					row.target_node_id = source.target_node_id;
					if (shape.include_type) {
						admit(source.type.size() + 8);
						row.type = source.type;
					}
				}
				if (shape.all_properties || !shape.property_tokens.empty()) {
					for (auto &p: source.properties)
						admit(p.key.size() + 8 + CanonicalPropertyValueCodec::encodedSize(p.value));
					row.properties = source.properties;
				}
				result.emplace_back(std::move(row));
			}
			return result;
		}
		std::vector<LogicalGraphNode> readNodesShapedSortedWithAdmission(std::span<const uint64_t> ids,
																		 const EntityReadShape &shape,
																		 const Admission &a) const override {
			return shaped(data->nodes, ids, shape, a);
		}
		std::vector<LogicalGraphEdge> readEdgesShapedSortedWithAdmission(std::span<const uint64_t> ids,
																		 const EntityReadShape &shape,
																		 const Admission &a) const override {
			return shaped(data->edges, ids, shape, a);
		}
		bool supportsPropertyReadAdmission() const override { return true; }
		Columns readNodePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
														   std::span<const std::string_view> keys,
														   const Admission &a) const override {
			if (keys.size() == 1 && data->column && !data->columnOverflow && keys.front() == data->declaration->key) {
				Columns result(1);
				auto &out = result.front();
				out.reserve(ids.size());
				uint64_t previous = 0;
				for (auto id: ids) {
					require(id > previous, "Invalid native graph owner order");
					previous = id;
					const auto &lane = data->column->column;
					auto it = std::ranges::lower_bound(lane, id, {}, &std::pair<uint64_t, int64_t>::first);
					if (it != lane.end() && it->first == id &&
						(!data->changedColumn || !std::ranges::binary_search(data->changedColumn->ids, id))) {
						PropertyValue v(it->second);
						a(CanonicalPropertyValueCodec::encodedSize(v) + keys.front().size() + 8);
						out.push_back(std::move(v));
					} else {
						auto one = columns(data->nodes, std::span(&id, 1), keys, a);
						out.push_back(std::move(one.front().front()));
					}
				}
				return result;
			}
			return columns(data->nodes, ids, keys, a);
		}
		Columns readEdgePropertyColumnsSortedWithAdmission(std::span<const uint64_t> ids,
														   std::span<const std::string_view> keys,
														   const Admission &a) const override {
			return columns(data->edges, ids, keys, a);
		}
	};
	using Status = LogicalGraphTransactionState;
	class Tx final : public LogicalGraphTransaction {
		std::shared_ptr<Journal> owner;
		Snapshot pinned;
		std::shared_ptr<State> working;
		Changes changes;
		bool ro;
		bool standalone;
		Status status = Status::LGTTS_ACTIVE;
		LogicalGraphMutationBatchResult created;
		void release() {
			owner.reset();
			working.reset();
			pinned.data.reset();
			changes = {};
		}
		void active() const { require(status == Status::LGTTS_ACTIVE, "Native transaction ended"); }

	public:
		Tx(std::shared_ptr<Journal> j, bool readOnly, bool bulk = false) :
			owner(std::move(j)), pinned(owner->snapshot()), ro(readOnly), standalone(bulk) {}
		Status state() const override { return status; }
		bool isReadOnly() const override { return ro; }
		LogicalGraphReadSnapshot &readSnapshot() override {
			active();
			return pinned;
		}
		LogicalGraphMutationBatchResult previewMutationBatch(const LogicalGraphMutationBatch &b) const override {
			active();
			require(!ro, "Read-only native transaction");
			return preview(working ? *working : *pinned.data, b);
		}
		LogicalGraphMutationBatchResult stageMutationBatch(LogicalGraphMutationBatch b) override {
			active();
			require(!ro, "Read-only native transaction");
			auto next = std::make_shared<State>(working ? *working : *pinned.data);
			auto changed = changes;
			auto ids = apply(*next, changed, b);
			require(changed.nodes.size() + changed.edges.size() <= maxRows * 2,
					"Native transaction change limit exceeded");

			auto result = created;
			require(result.created_node_ids.size() + result.created_edge_ids.size() + ids.created_node_ids.size() +
									ids.created_edge_ids.size() <=
							maxRows * 2,
					"Native transaction creation limit exceeded");
			result.created_node_ids.insert(result.created_node_ids.end(), ids.created_node_ids.begin(),
										   ids.created_node_ids.end());
			result.created_edge_ids.insert(result.created_edge_ids.end(), ids.created_edge_ids.begin(),
										   ids.created_edge_ids.end());
			working = std::move(next);
			changes = std::move(changed);
			created = std::move(result);
			return ids;
		}
		LogicalGraphMutationBatchResult commit() override {
			active();
			try {
				if (working)
					owner->commit(pinned.data, working, changes, standalone);
				else if (standalone)
					owner->checkpoint();
			} catch (const UniqueViolation &) {
				status = Status::LGTTS_ROLLED_BACK;
				release();
				throw;
			} catch (const LogicalGraphMutationConflict &) {
				status = Status::LGTTS_CONFLICTED;
				release();
				throw;
			} catch (...) {
				status = Status::LGTTS_INDETERMINATE;
				release();
				throw;
			}
			status = Status::LGTTS_COMMITTED;
			release();
			return std::move(created);
		}
		void rollback() override {
			active();
			status = Status::LGTTS_ROLLED_BACK;
			working.reset();
			release();
		}
	};
	class Backend : public StorageBackend, public NativeWalIntegrityBackend, public LogicalGraphReadBackend, public LogicalGraphTransactionalBackend, public LogicalGraphBulkPublication {
		std::string path;
		size_t memory;
		bool ro;
		mutable std::mutex lifecycle;
		std::shared_ptr<Journal> journal;
		std::shared_ptr<resource::DatabaseResourceBudget> resources;

	protected:
		Journal &schemaJournal() const {
			require(bool(journal), "Native graph is closed");
			return *journal;
		}

	public:
		Backend(std::string p, bool readOnly, size_t bytes) : path(std::move(p)), memory(bytes), ro(readOnly) {}
		void open() override {
			std::lock_guard lock(lifecycle);
			if (!journal) {
				auto opened = std::make_shared<Journal>(path, ro, memory);
				const auto &id = opened->snapshot()->identity;
				resources = resource::ProcessResourceDomains::defaultOwner()->domain(
						"native-wal:" + std::string(reinterpret_cast<const char *>(id.data()), id.size()));
				journal = std::move(opened);
			}
		}
		void flush() override {
			if (journal)
				journal->checkpoint();
		}
		void close() override {
			std::lock_guard lock(lifecycle);
			if (!journal)
				return;
			require(journal.use_count() == 1, "Native graph has active transactions");
			auto closing = std::move(journal);
			resources.reset();
			closing->checkpoint();
		}
		NativeWalIntegrityReport verifyIntegrity() const override {
			std::lock_guard lock(lifecycle);
			if (!journal)
				throw std::logic_error("native WAL backend is not open");
			// Prepay maintenance outside the publication gate. During replay,
			// allocations draw on the local grant and the shared version quota.
			const auto kind = resource::ResourceClass::RCK_MAINTENANCE;
			const auto limit = std::min(memory, resources->classMaximum(kind));
			if (!limit) throw resource::ResourceAdmissionDenied();
			resource::ResourceAccount account(resources, kind, limit);
			resource::ResourceLease lease(account);
			const auto grant = std::min(journal->snapshot()->budget->remaining(), account.available());
			if (!grant || !lease.trySet(grant)) throw resource::ResourceAdmissionDenied();
			try {
				return journal->verifyIntegrity(grant);
			} catch (const std::length_error &) {
				// The replay verifier uses the native version quota. Expose
				// admission failure with the public maintenance error type.
				throw resource::ResourceAdmissionDenied();
			}
		}
		bool isOpen() const override { return bool(journal); }
		bool isReadOnly() const override { return ro; }
		StorageFormatKind formatKind() const override { return StorageFormatKind::SFM_NATIVE_WAL; }
		std::shared_ptr<resource::DatabaseResourceBudget> queryResourceDomain() const override {
			require(bool(journal), "Native graph is closed");
			// An existing transaction still owns its earlier immutable snapshot
			// if a different writer has since failed. Budget identity is per open.
			return resources;
		}
		StorageBackendCapabilities capabilities() const override { return {true, true, false, true, !ro}; }
		std::unique_ptr<LogicalGraphReadSnapshot> openLogicalGraphReadSnapshot() const override {
			require(bool(journal), "Native graph is closed");
			return std::make_unique<Snapshot>(journal->snapshot());
		}
		std::unique_ptr<LogicalGraphTransaction> beginStandaloneTransaction() override {
			require(journal && !ro, "Native bulk write transaction unavailable");
			return std::make_unique<Tx>(journal, false, true);
		}
		std::unique_ptr<LogicalGraphTransaction> beginLogicalGraphTransaction(bool readOnly) override {
			require(journal && (!ro || readOnly), "Native graph write transaction unavailable");
			return std::make_unique<Tx>(journal, readOnly);
		}
	};
	// Schema DDL is a separate auto-commit operation. Read-only opens use Backend.
	class UniqueBackend final : public Backend, public LogicalGraphSchemaBackend {
		static LogicalGraphConstraintDefinition definition(uint64_t revision, const std::string &name,
														   const std::string &label, const std::string &key) {
			LogicalGraphConstraintDefinition d;
			d.constraint_id = revision;
			d.name = name;
			d.label = label;
			d.property_keys = std::vector<std::string>{key};
			return d;
		}
		[[noreturn]] static void unsupported() { throw std::runtime_error("Native schema operation is unsupported"); }

	public:
		using Backend::Backend;
		LogicalGraphConstraintDefinition
		createLogicalConstraint(const LogicalGraphConstraintCreateRequest &r) override {
			return createLogicalConstraintWithCheckpoint(r, {}, {});
		}
		LogicalGraphConstraintDefinition
		createLogicalConstraintWithCheckpoint(const LogicalGraphConstraintCreateRequest &r,
											  const std::function<void()> &check,
											  const std::function<void(size_t)> &admit) override {
			require(r.entity_kind == LogicalGraphConstraintEntityKind::LGCEK_NODE &&
							r.kind == LogicalGraphConstraintKind::LGCK_UNIQUE && r.property_keys.size() == 1 &&
							r.property_type == LogicalGraphConstraintPropertyType::LGCPT_NONE,
					"Native UNIQUE supports one declared node property");
			LogicalGraphConstraintDefinition result;
			result.name = r.name;
			result.label = r.label;
			result.property_keys = r.property_keys;
			result.constraint_id = schemaJournal().createUnique(r.name, r.label, r.property_keys[0], check, admit);
			return result;
		}
		std::vector<LogicalGraphConstraintDefinition> listLogicalConstraints(size_t maximum) const override {
			auto s = schemaJournal().snapshot();
			size_t count = size_t(bool(s->unique)) + (s->catalog ? s->catalog->entries.size() : 0);
			require(count <= maximum, "Native constraint list exceeds budget");
			std::vector<LogicalGraphConstraintDefinition> result;
			result.reserve(count);
			if (s->unique)
				result.push_back(
						definition(s->unique->revision, s->unique->name, s->declaration->label, s->declaration->key));
			if (s->catalog)
				for (const auto &d: s->catalog->entries)
					result.push_back(definition(d.revision, d.name, d.label, d.key));
			std::ranges::sort(result, {}, &LogicalGraphConstraintDefinition::constraint_id);
			return result;
		}
		bool dropLogicalConstraintByName(std::string_view name) override { return schemaJournal().dropUnique(name); }
		LogicalGraphIndexDefinition createLogicalNodeIndex(const LogicalGraphNodeIndexCreateRequest &) override {
			unsupported();
		}
		std::vector<LogicalGraphIndexDefinition>
		createLogicalNodeIndexes(std::span<const LogicalGraphNodeIndexCreateRequest>) override {
			unsupported();
		}
		LogicalGraphIndexDefinition
		createLogicalNodeVectorIndex(const LogicalGraphNodeVectorIndexCreateRequest &) override {
			unsupported();
		}
		bool dropLogicalIndexByName(std::string_view) override { unsupported(); }
		bool dropLogicalNodeIndexBySchema(std::string_view, std::string_view) override { unsupported(); }
		std::vector<LogicalGraphIndexDefinition> listLogicalIndexes(size_t) const override { unsupported(); }
	};
} // namespace graph::storage::nativewal
namespace graph::storage {
	std::shared_ptr<StorageBackend> openNativeWalBackend(const std::string &path, bool readOnly, size_t memory) {
		if (!readOnly)
			return std::make_shared<nativewal::UniqueBackend>(path, false, memory);
		return std::make_shared<nativewal::Backend>(path, true, memory);
	}
	std::string createNativeWalDatabase(const std::string &directory) { return nativewal::Journal::create(directory); }
	std::string createIndexedNativeWalDatabase(const std::string &directory, const std::string &label,
											   const std::string &key) {
		nativewal::require(!label.empty() && label.size() <= 256 && !key.empty() && key.size() <= 256,
						   "Invalid native index declaration");
		return nativewal::Journal::create(
				directory, std::make_shared<nativewal::ColumnDeclaration>(nativewal::ColumnDeclaration{label, key}));
	}
} // namespace graph::storage
