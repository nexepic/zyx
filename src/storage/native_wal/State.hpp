#pragma once
#include <cstring>
#include <set>
#include "IndexedColumn.hpp"
#include "AdjacencyOrder.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/Sha256.hpp"
namespace graph::storage::nativewal {
	using Bytes = std::vector<std::byte>;
	using Span = std::span<const std::byte>;
	using Digest = std::array<std::byte, 32>;
	using Identity = std::array<std::byte, 16>;
	constexpr size_t maxRows = (2U << 20) / 24, maxFields = 32, maxName = 16384, maxState = 2 << 20, maxWal = 16 << 20;
	inline void require(bool condition, const char *message) {
		if (!condition)
			throw std::runtime_error(message);
	}
	struct UniqueViolation : std::runtime_error {
		using std::runtime_error::runtime_error;
	};
	struct UniqueDeclaration {
		std::string name;
		uint64_t revision;
		bool ownsProjection = false;
	};
	// Additional scopes share the transaction root and use authoritative row
	// validation when the single projected column belongs to another scope.
	struct ScopedUnique {
		std::string name, label, key;
		uint64_t revision;
	};
	struct UniqueCatalog {
		static constexpr size_t maximum = 65536;
		VersionCharge charge;
		std::vector<ScopedUnique> entries;
		size_t retained;
		UniqueCatalog(const std::shared_ptr<VersionBudget> &budget, size_t count) :
			charge(budget, sizeof(UniqueCatalog) + count * sizeof(ScopedUnique)),
			retained(sizeof(UniqueCatalog) + count * sizeof(ScopedUnique)) {
			entries.reserve(count);
		}
		void append(const ScopedUnique &d) {
			retained += d.name.size() + d.label.size() + d.key.size() + 3;
			charge.growTo(retained);
			entries.push_back(d);
		}
	};
	struct State {
		std::shared_ptr<const AdjacencyOrder> adjacency;
		std::shared_ptr<const AdjacencyChanges> adjacencyChanges;
		bool adjacencyComplete = true;
		void discardAdjacency() {
			adjacencyComplete = false;
			adjacency.reset();
			adjacencyChanges.reset();
		}
		void buildAdjacency();
		void recordNewEndpoints(std::span<const uint64_t> ids);
		void boundDeadPostings() {
			if (!adjacencyComplete) return;
			size_t retained = adjacency ? adjacency->outgoing.size() : 0;
			if (adjacencyChanges)
				for (const auto &run : adjacencyChanges->levels)
					if (run) retained += run->outgoing.size();
			if (retained > edges.size() && retained - edges.size() > std::max(size_t(128), edges.size())) {
				discardAdjacency();
				adjacencyComplete = !edges.size();
			}
		}
		template<class Visitor>
		void adjacentAfter(uint64_t owner, bool outgoing, uint64_t after, Visitor visit) const {
			auto matches = [&](const LogicalGraphEdge &e) {
				return (outgoing ? e.source_node_id : e.target_node_id) == owner;
			};
			if (!adjacencyComplete) {
				size_t inspected = 0;
				edges.visitAfter(after, [&](auto it) {
					++inspected;
					return !matches(it.edgeHeader()) || visit(it);
				});
				debug::PerfTrace::addValue("native_wal.adjacency.scan_headers", inspected);
				return;
			}
			AdjacentCursor cursor(owner, outgoing, after);
			if (adjacency) cursor.add(*adjacency);
			if (adjacencyChanges)
				for (const auto &run : adjacencyChanges->levels)
					if (run) cursor.add(*run);
			size_t inspected = 0;
			while (auto id = cursor.next()) {
				++inspected;
				auto it = edges.find(id);
				if (it != edges.end() && matches(it.edgeHeader()) && !visit(it)) break;
			}
			debug::PerfTrace::addValue("native_wal.adjacency.posting_ids", inspected);
		}
		std::shared_ptr<const UniqueCatalog> catalog;
		std::shared_ptr<const UniqueDeclaration> unique;
		Identity identity{};
		std::shared_ptr<const ColumnDeclaration> declaration;
		std::shared_ptr<const IndexedColumn> column;
		std::shared_ptr<const ColumnChanges> changedColumn;
		bool columnOverflow = false;
		template<class Range>
		void recordColumnChanges(const Range &ids) {
			if (!declaration || ids.empty() || columnOverflow)
				return;
			if (ids.size() <= ColumnChanges::limit) {
				auto next = std::make_shared<ColumnChanges>(budget);
				if (changedColumn)
					next->ids = changedColumn->ids;
				for (auto id: ids) {
					auto p = std::ranges::lower_bound(next->ids, id);
					if (p != next->ids.end() && *p == id)
						continue;
					if (next->ids.size() == ColumnChanges::limit) {
						columnOverflow = true;
						break;
					}
					next->ids.insert(p, id);
				}
				if (!columnOverflow) {
					changedColumn = std::move(next);
					return;
				}
			}
			columnOverflow = true;
			changedColumn.reset();
		}
		uint64_t sequence = 0, nodeHigh = 0, edgeHigh = 0;
		std::shared_ptr<VersionBudget> budget;
		VersionedRows<LogicalGraphNode> nodes;
		VersionedRows<LogicalGraphEdge> edges;
		explicit State(size_t memory = 64U << 20) :
			State(std::make_shared<VersionBudget>(memory)) {}
		explicit State(std::shared_ptr<VersionBudget> sharedBudget) :
			budget(std::move(sharedBudget)), nodes(budget), edges(budget) {}
		size_t encodedSize() const { return 32 + nodes.encodedBytes() + edges.encodedBytes(); }
	};
	void validateScope(const State &, const ScopedUnique &, const std::function<void()> & = {},
					   const std::function<void(size_t)> & = {});
	using ChangedNodeIds = std::function<void(const std::function<void(uint64_t)> &)>;
	void validateAffectedScope(const State &, const State *prior, const ScopedUnique &, const ChangedNodeIds &);
	void validateCatalog(const State &);
	void validateUnique(const State &, const std::function<void()> &check = {},
						const std::function<void(size_t)> &admit = {});
	struct Changes {
		std::set<uint64_t> nodes, edges;
	};
	template<class Buffer> struct BasicEncoder {
		Buffer bytes;
		size_t limit;
		std::shared_ptr<VersionBudget> budget;
		explicit BasicEncoder(size_t maximum, std::shared_ptr<VersionBudget> b = {},
							  typename Buffer::allocator_type allocator = {}) :
			bytes(allocator), limit(maximum), budget(std::move(b)) {}
		void room(size_t n) const { require(n <= limit - bytes.size(), "Native WAL encoding budget exceeded"); }
		void raw(Span s) {
			room(s.size());
			bytes.insert(bytes.end(), s.begin(), s.end());
		}
		void num(uint64_t n) {
			room(8);
			for (unsigned i = 0; i < 8; ++i)
				bytes.push_back(std::byte(n >> (8 * i)));
		}
		void text(std::string_view s) {
			require(s.size() <= maxName, "Native WAL string limit exceeded");
			num(s.size());
			raw(std::as_bytes(std::span(s)));
		}
		void magic(std::string_view s) { raw(std::as_bytes(std::span(s))); }
		void value(const PropertyValue &v);
	};
	using Encoder = BasicEncoder<Bytes>;
	using RowEncoder = BasicEncoder<std::pmr::vector<std::byte>>;
	struct Decoder {
		Span bytes;
		Span take(size_t n) {
			require(n <= bytes.size(), "Truncated native WAL field");
			auto s = bytes.first(n);
			bytes = bytes.subspan(n);
			return s;
		}
		uint64_t num() {
			auto s = take(8);
			uint64_t n = 0;
			for (unsigned i = 0; i < 8; ++i)
				n |= uint64_t(std::to_integer<unsigned>(s[i])) << (8 * i);
			return n;
		}
		size_t count(size_t cap) {
			auto n = num();
			require(n <= cap, "Native WAL count or length exceeds limit");
			return static_cast<size_t>(n);
		}
		std::string text() {
			auto s = take(count(maxName));
			return {reinterpret_cast<const char *>(s.data()), s.size()};
		}
		void magic(std::string_view s) {
			require(std::ranges::equal(take(s.size()), std::as_bytes(std::span(s))), "Incorrect native WAL format");
		}
	};
	LogicalGraphMutationBatchResult preview(const State &, const LogicalGraphMutationBatch &);
	LogicalGraphMutationBatchResult apply(State &, Changes &, const LogicalGraphMutationBatch &);
	size_t deltaBytes(const State &, const Changes &);
	void encodeDelta(Encoder &, const State &, const Changes &);
	void writeDelta(const State &, const Changes &, const std::function<void(Span)> &);
	void encodeBaseRow(RowEncoder &, const LogicalGraphNode &);
	void encodeBaseRow(RowEncoder &, const LogicalGraphEdge &);
	LogicalGraphNode baseNode(uint64_t, Span, VersionCharge &);
	LogicalGraphEdge baseEdge(uint64_t, Span, VersionCharge &);
	LogicalGraphNode inspectBaseNode(uint64_t, Span, VersionCharge &, const State &);
	LogicalGraphEdge inspectBaseEdge(uint64_t, Span, VersionCharge &, const State &);
	class UniqueDelta;
	void applyDelta(State &, size_t, const std::function<void(std::span<std::byte>)> &, UniqueDelta &, bool audit = false);
} // namespace graph::storage::nativewal
