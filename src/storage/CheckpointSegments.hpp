// A verified cp002 row group exposes five lossless lanes: four payload words
// and the tag/padding bytes. Exact arithmetic progressions need no per-row
// storage. Other lanes use offsets from their minimum, rounded to whole bytes.
#pragma once
#include <array>
#include <bit>
#include <vector>
#include "CheckpointWindows.hpp"

namespace graph::storage::checkpoint {
	class Segments {
		static constexpr size_t width = 256;
		struct Lane {
			uint64_t base = 0, step = 0;
			uint32_t start = 0;
			unsigned char size = 0;
		};
		struct Group {
			std::array<Lane, 5> lanes;
			std::vector<unsigned char> literals;
		};
		std::vector<Group> groups;
		static uint64_t little(const unsigned char *p, size_t n) {
			uint64_t v = 0;
			for (size_t i = 0; i < n; ++i)
				v |= uint64_t(p[i]) << (8 * i);
			return v;
		}
		static uint64_t laneValue(const Record &r, size_t k) {
			if (k < 4)
				return little(r.data() + 4 + 8 * k, 8);
			return little(r.data(), 4) | (little(r.data() + 36, 4) << 32);
		}
		static void encode(std::span<const Record> rows, Group &g, const auto &admit) {
			size_t bytes = 0;
			for (size_t k = 0; k < 5; ++k) {
				auto &lane = g.lanes[k];
				lane.base = laneValue(rows.front(), k);
				lane.step = rows.size() > 1 ? laneValue(rows[1], k) - lane.base : 0;
				auto low = lane.base, high = low;
				bool linear = true;
				for (size_t j = 1; j < rows.size(); ++j) {
					auto v = laneValue(rows[j], k);
					low = std::min(low, v);
					high = std::max(high, v);
					linear &= v == lane.base + j * lane.step;
				}
				if (linear)
					continue;
				lane.base = low;
				lane.step = 0;
				lane.size = (std::bit_width(high - low) + 7) / 8;
				lane.start = static_cast<uint32_t>(bytes);
				bytes += rows.size() * lane.size;
			}
			admit(bytes);
			g.literals.resize(bytes);
			for (size_t k = 0; k < 5; ++k) {
				auto &lane = g.lanes[k];
				if (!lane.size)
					continue;
				for (size_t j = 0; j < rows.size(); ++j) {
					auto v = laneValue(rows[j], k) - lane.base;
					for (size_t b = 0; b < lane.size; ++b)
						g.literals[lane.start + j * lane.size + b] = static_cast<unsigned char>(v >> (8 * b));
				}
			}
		}

	public:
		static size_t workingBytes(size_t n) { return std::min(width, n) * sizeof(Record); }
		static size_t directoryBytes(size_t n) { return ((n + width - 1) / width) * sizeof(Group); }
		void load(std::istream &input, size_t n, utils::Sha256 &hash, const auto &admit) {
			groups.resize((n + width - 1) / width);
			std::vector<Record> window(std::min(width, n));
			for (size_t i = 0; i < n; i += width) {
				auto part = std::span(window).first(std::min(width, n - i));
				auto bytes = std::as_bytes(part);
				if (!input.read(reinterpret_cast<char *>(part.data()), bytes.size()))
					throw std::runtime_error("Short cp002 checkpoint");
				hash.update(bytes);
				encode(part, groups[i / width], admit);
			}
		}
		uint64_t value(size_t i, size_t k) const {
			const auto &g = groups[i / width];
			const auto &lane = g.lanes[k];
			const size_t j = i % width;
			if (!lane.size)
				return lane.base + j * lane.step;
			return lane.base + little(g.literals.data() + lane.start + j * lane.size, lane.size);
		}
		Record operator[](size_t i) const {
			Record r{};
			for (size_t k = 0; k < 5; ++k) {
				uint64_t v = value(i, k);
				for (unsigned b = 0; b < 8; ++b)
					r[k < 4 ? 4 + 8 * k + b : (b < 4 ? b : 32 + b)] = static_cast<unsigned char>(v >> (8 * b));
			}
			return r;
		}
	};
} // namespace graph::storage::checkpoint
