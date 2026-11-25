#pragma once
#include <cstring>
#include <set>
#include "IndexedColumn.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/Sha256.hpp"
namespace graph::storage::nativewal {
	using Bytes = std::vector<std::byte>;
	using Span = std::span<const std::byte>;
	using Digest = std::array<std::byte, 32>;
	using Identity = std::array<std::byte, 16>;
	constexpr size_t maxRows = (2U << 20) / 24, maxFields = 32, maxValue = 16384, maxState = 2 << 20, maxWal = 16 << 20;
	inline void require(bool condition, const char *message) {
		if (!condition)
			throw std::runtime_error(message);
	}
	struct State {
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
			budget(std::make_shared<VersionBudget>(memory)), nodes(budget), edges(budget) {}
		size_t encodedSize() const { return 32 + nodes.encodedBytes() + edges.encodedBytes(); }
	};
	struct Changes {
		std::set<uint64_t> nodes, edges;
	};
	struct Encoder {
		Bytes bytes;
		size_t limit;
		explicit Encoder(size_t maximum) : limit(maximum) {}
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
			require(s.size() <= maxValue, "Native WAL string limit exceeded");
			num(s.size());
			raw(std::as_bytes(std::span(s)));
		}
		void magic(std::string_view s) { raw(std::as_bytes(std::span(s))); }
		void value(const PropertyValue &v);
	};
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
			auto s = take(count(maxValue));
			return {reinterpret_cast<const char *>(s.data()), s.size()};
		}
		void magic(std::string_view s) {
			require(std::ranges::equal(take(s.size()), std::as_bytes(std::span(s))), "Incorrect native WAL format");
		}
	};
	LogicalGraphMutationBatchResult preview(const State &, const LogicalGraphMutationBatch &);
	LogicalGraphMutationBatchResult apply(State &, Changes &, const LogicalGraphMutationBatch &);
	Bytes encodeDelta(const State &, const Changes &);
	void encodeBaseRow(Encoder &, const LogicalGraphNode &);
	void encodeBaseRow(Encoder &, const LogicalGraphEdge &);
	LogicalGraphNode baseNode(uint64_t, Span);
	LogicalGraphEdge baseEdge(uint64_t, Span);
	void applyDelta(State &, Span);
} // namespace graph::storage::nativewal
