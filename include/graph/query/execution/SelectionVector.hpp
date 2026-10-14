/**
 * @file SelectionVector.hpp
 * @brief Bounded bitmap used by storage-neutral batch operators.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace graph::query::execution {

	// A compact, allocation-stable selection bitmap. Positions remain aligned
	// with the input batch while operators reject rows independently.
	class SelectionVector final {
	public:
		// The supplied resource outlives this owner and all resource-preserving
		// copies. Query operators supply an allocation-admitting budget resource.
		explicit SelectionVector(size_t size, std::pmr::memory_resource *memory = std::pmr::get_default_resource()) :
			size_(size), words_(checkedWordCount(size), ~uint64_t{0U}, requireMemory(memory)), selected_count_(size) {
			if (!words_.empty() && size % BITS_PER_WORD != 0U) {
				words_.back() = (uint64_t{1U} << (size % BITS_PER_WORD)) - 1U;
			}
		}
		SelectionVector(const SelectionVector &other) : size_(other.size_),
			words_(other.words_, other.words_.get_allocator()), selected_count_(other.selected_count_) {}
		SelectionVector &operator=(const SelectionVector &other) {
			if (this != &other) {
				// Preserve the destination resource and old selection on admission
				// failure. Old/new buffers overlap and are both charged until swap.
				std::pmr::vector<uint64_t> replacement(other.words_, words_.get_allocator());
				words_.swap(replacement);
				size_ = other.size_;
				selected_count_ = other.selected_count_;
			}
			return *this;
		}
		SelectionVector(SelectionVector &&other) noexcept : size_(std::exchange(other.size_, 0U)),
			words_(std::move(other.words_)), selected_count_(std::exchange(other.selected_count_, 0U)) {
			other.words_.clear();
		}
		SelectionVector &operator=(SelectionVector &&other) {
			if (this != &other) {
				if (words_.get_allocator() == other.words_.get_allocator()) words_ = std::move(other.words_);
				else {
					std::pmr::vector<uint64_t> replacement(other.words_, words_.get_allocator());
					words_.swap(replacement);
				}
				size_ = std::exchange(other.size_, 0U);
				selected_count_ = std::exchange(other.selected_count_, 0U);
				other.words_.clear(); // Cross-resource retained capacity stays charged.
			}
			return *this;
		}

		[[nodiscard]] static size_t requiredStorageBytes(size_t size) {
			return checkedWordCount(size) * sizeof(uint64_t);
		}

		[[nodiscard]] size_t size() const noexcept { return size_; }
		[[nodiscard]] size_t storageBytes() const noexcept { return words_.capacity() * sizeof(uint64_t); }
		[[nodiscard]] bool includes(size_t index) const {
			if (index >= size_) {
				throw std::out_of_range("selection vector index is outside the batch");
			}
			return (words_[index / BITS_PER_WORD] & (uint64_t{1U} << (index % BITS_PER_WORD))) != 0U;
		}

		void reject(size_t index) {
			if (index >= size_) {
				throw std::out_of_range("selection vector index is outside the batch");
			}
			uint64_t &word = words_[index / BITS_PER_WORD];
			const uint64_t mask = uint64_t{1U} << (index % BITS_PER_WORD);
			if ((word & mask) != 0U) {
				word &= ~mask;
				--selected_count_;
			}
		}

		void rejectAll() noexcept {
			for (uint64_t &word: words_) {
				word = 0U;
			}
			selected_count_ = 0U;
		}

		[[nodiscard]] size_t selectedCount() const noexcept { return selected_count_; }

		[[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
		[[nodiscard]] bool noneSelected() const noexcept {
			for (const uint64_t word: words_) {
				if (word != 0U) {
					return false;
				}
			}
			return true;
		}
		[[nodiscard]] bool allSelected() const noexcept { return selectedCount() == size_; }

		void select(size_t index) {
			if (index >= size_) {
				throw std::out_of_range("selection vector index is outside the batch");
			}
			uint64_t &word = words_[index / BITS_PER_WORD];
			const uint64_t mask = uint64_t{1U} << (index % BITS_PER_WORD);
			if ((word & mask) == 0U) {
				word |= mask;
				++selected_count_;
			}
		}

		void intersect(const SelectionVector &other) {
			if (size_ != other.size_) {
				throw std::invalid_argument("selection vectors must have the same size");
			}
			// Maintain the count from the words that actually changed. This keeps
			// repeated filter/project intersections linear in the number of words
			// touched, without a second pass over the bitmap.
			for (size_t index = 0U; index < words_.size(); ++index) {
				const uint64_t before = words_[index];
				const uint64_t after = before & other.words_[index];
				words_[index] = after;
				selected_count_ -= static_cast<size_t>(std::popcount(before) - std::popcount(after));
			}
		}

		template <typename Visitor>
		void forEachSelected(Visitor &&visitor) const {
			static_assert(std::is_invocable_v<Visitor &, size_t>);
			static_cast<void>(forEachSelectedUntil([&](const size_t index) {
				visitor(index);
				return true;
			}));
		}

		// Visits selected positions in input order and stops as soon as the visitor
		// returns false. Batch operators use this form at LIMIT/early-termination
		// boundaries so a selection bitmap never forces the remaining rows through
		// user code after the consumer has stopped.
		template <typename Visitor>
		[[nodiscard]] bool forEachSelectedUntil(Visitor &&visitor) const {
			static_assert(std::is_invocable_r_v<bool, Visitor &, size_t>);
			for (size_t wordIndex = 0U; wordIndex < words_.size(); ++wordIndex) {
				uint64_t word = words_[wordIndex];
				while (word != 0U) {
					const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
					if (!visitor(wordIndex * BITS_PER_WORD + bit)) {
						return false;
					}
					word &= word - 1U;
				}
			}
			return true;
		}

	private:
		static std::pmr::memory_resource *requireMemory(std::pmr::memory_resource *memory) {
			if (!memory) throw std::invalid_argument("selection vector requires an allocation resource");
			return memory;
		}
		static constexpr size_t BITS_PER_WORD = std::numeric_limits<uint64_t>::digits;
		[[nodiscard]] static size_t checkedWordCount(size_t size) {
			if (size > std::numeric_limits<size_t>::max() - (BITS_PER_WORD - 1U)) {
				throw std::length_error("selection vector size overflows its bitmap");
			}
			return (size + BITS_PER_WORD - 1U) / BITS_PER_WORD;
		}
		size_t size_ = 0U;
		std::pmr::vector<uint64_t> words_;
		size_t selected_count_ = 0U;
	};

} // namespace graph::query::execution
