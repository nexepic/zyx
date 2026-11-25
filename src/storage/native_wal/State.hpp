#pragma once
#include <cstring>
#include <map>
#include <set>
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/Sha256.hpp"
namespace graph::storage::nativewal {
	using Bytes = std::vector<std::byte>;
	using Span = std::span<const std::byte>;
	using Digest = std::array<std::byte, 32>;
	using Identity = std::array<std::byte, 16>;
	constexpr size_t maxRows = 4096, maxFields = 32, maxValue = 16384, maxState = 2 << 20, maxWal = 16 << 20;
	inline void require(bool condition, const char *message) {
		if (!condition)
			throw std::runtime_error(message);
	}
	struct State {
		Identity identity{};
		uint64_t sequence = 0, nodeHigh = 0, edgeHigh = 0;
		std::map<uint64_t, LogicalGraphNode> nodes;
		std::map<uint64_t, LogicalGraphEdge> edges;
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
	Bytes encodeBase(const State &);
	void applyDelta(State &, Span);
} // namespace graph::storage::nativewal
