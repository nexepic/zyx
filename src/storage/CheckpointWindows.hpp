// Private backing preserves the exact bytes admitted at open even if the named
// file is changed in place. The input is never reopened by a cursor.
#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "graph/utils/Sha256.hpp"

namespace graph::storage::checkpoint {
	using Record = std::array<unsigned char, 40>;
	class Windows {
		static constexpr size_t width = 256;
		using Digest = std::array<std::byte, 32>;
		size_t count = 0;
		mutable std::mutex lock;
		mutable std::vector<Record> window;
		mutable size_t cached = SIZE_MAX;
		std::vector<Digest> hashes;
		std::unique_ptr<std::FILE, decltype(&std::fclose)> file{nullptr, std::fclose};
		static void check(bool ok) {
			if (!ok)
				throw std::runtime_error("Cannot read or preserve cp002 checkpoint window");
		}

	public:
		static size_t bytes(size_t n) {
			return std::min(n, width) * sizeof(Record) + (n <= width ? 0 : ((n + width - 1) / width) * sizeof(Digest));
		}
		void load(std::istream &input, size_t n, utils::Sha256 &whole) {
			count = n;
			window.resize(std::min(width, count));
			if (count > width) {
				file.reset(std::tmpfile());
				check(bool(file));
				check(std::setvbuf(file.get(), nullptr, _IONBF, 0) == 0);
				hashes.reserve((count + width - 1) / width);
			}
			for (size_t i = 0; i < count; i += width) {
				auto part = std::span(window).first(std::min(width, count - i));
				auto bytes = std::as_bytes(part);
				check(bool(input.read(reinterpret_cast<char *>(part.data()), bytes.size())));
				whole.update(bytes);
				if (file) {
					hashes.push_back(utils::Sha256::digest(bytes));
					check(std::fwrite(part.data(), sizeof(Record), part.size(), file.get()) == part.size());
				}
				cached = i / width;
			}
			if (file)
				check(std::fflush(file.get()) == 0);
		}
		Record operator[](size_t i) const {
			if (i >= count)
				throw std::runtime_error("Invalid cp002 record offset");
			if (!file)
				return window[i];
			std::lock_guard guard(lock);
			const auto page = i / width;
			if (cached != page) {
				cached = SIZE_MAX;
				auto part = std::span(window).first(std::min(width, count - page * width));
				check(std::fseek(file.get(), static_cast<long>(page * width * sizeof(Record)), SEEK_SET) == 0);
				check(std::fread(part.data(), sizeof(Record), part.size(), file.get()) == part.size());
				check(utils::Sha256::digest(std::as_bytes(part)) == hashes[page]);
				cached = page;
			}
			return window[i % width];
		}
	};
} // namespace graph::storage::checkpoint
