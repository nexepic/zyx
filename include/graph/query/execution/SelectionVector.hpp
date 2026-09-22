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
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace graph::query::execution {

	// A compact, allocation-stable selection bitmap. Positions remain aligned
	// with the input batch while operators reject rows independently.
	class SelectionVector final {
	public:
		explicit SelectionVector(size_t size) : size_(size), words_(checkedWordCount(size), ~uint64_t{0U}), selected_count_(size) {
			if (!words_.empty() && size % BITS_PER_WORD != 0U) {
				words_.back() = (uint64_t{1U} << (size % BITS_PER_WORD)) - 1U;
			}
		}

		[[nodiscard]] size_t size() const noexcept { return size_; }
		[[nodiscard]] size_t storageBytes() const noexcept { return words_.size() * sizeof(uint64_t); }
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
		static constexpr size_t BITS_PER_WORD = std::numeric_limits<uint64_t>::digits;
		[[nodiscard]] static size_t checkedWordCount(size_t size) {
			if (size > std::numeric_limits<size_t>::max() - (BITS_PER_WORD - 1U)) {
				throw std::length_error("selection vector size overflows its bitmap");
			}
			return (size + BITS_PER_WORD - 1U) / BITS_PER_WORD;
		}
		size_t size_ = 0U;
		std::vector<uint64_t> words_;
		size_t selected_count_ = 0U;
	};

} // namespace graph::query::execution
