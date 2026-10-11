/**
 * @file GlobalAdjacencyMutationMerger.cpp
 * @brief Rewindable merge of one global adjacency tree and tagged final-state updates.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/adjacency/GlobalAdjacencyMutationMerger.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		struct MergeState {
			OrderedBTreeEntryCursor old;
			OrderedBTreeEntryCursor updates;
			std::optional<BTreePageEntry> current_old;
			std::optional<BTreePageEntry> current_update;
			std::optional<std::vector<std::byte>> previous_old_key;
			std::optional<std::vector<std::byte>> previous_update_key;
			size_t maximum_entries = 0U;
			size_t emitted_entries = 0U;

			void advanceOld() {
				current_old = old();
				if (!current_old.has_value()) {
					return;
				}
				if (current_old->key.empty() || current_old->value_or_child != 0U ||
					(previous_old_key.has_value() && !lessKey(*previous_old_key, current_old->key))) {
					fail(FormatError::FERR_INVALID_STATE,
						 "global adjacency merge received an invalid or unordered key-only tree entry");
				}
				previous_old_key = current_old->key;
			}

			void advanceUpdate() {
				current_update = updates();
				if (!current_update.has_value()) {
					return;
				}
				if (current_update->key.empty() || (current_update->value_or_child != 1U && current_update->value_or_child != 2U) ||
					(previous_update_key.has_value() && !lessKey(*previous_update_key, current_update->key))) {
					fail(FormatError::FERR_INVALID_STATE,
						 "global adjacency merge received an invalid or unordered tagged update entry");
				}
				previous_update_key = current_update->key;
			}

			[[nodiscard]] std::optional<BTreePageEntry> next() {
				for (;;) {
					if (!current_old.has_value() && !current_update.has_value()) {
						return std::nullopt;
					}
					if (!current_update.has_value() ||
						(current_old.has_value() && lessKey(current_old->key, current_update->key))) {
						BTreePageEntry result = std::move(*current_old);
						advanceOld();
						if (emitted_entries++ == maximum_entries) {
							fail(FormatError::FERR_INVALID_STATE,
								 "global adjacency merge exceeded its configured final-entry bound");
						}
						return result;
					}
					const uint64_t tag = current_update->value_or_child;
					const bool updatePrecedesOld = !current_old.has_value() || lessKey(current_update->key, current_old->key);
					if (!updatePrecedesOld) {
						advanceOld();
					}
					BTreePageEntry update = std::move(*current_update);
					advanceUpdate();
					if (tag == 1U) {
						if (emitted_entries++ == maximum_entries) {
							fail(FormatError::FERR_INVALID_STATE,
								 "global adjacency merge exceeded its configured final-entry bound");
						}
						return BTreePageEntry{std::move(update.key), 0U};
					}
				}
			}
		};

	} // namespace

	OrderedBTreeEntrySource GlobalAdjacencyMutationMerger::merge(
			const OrderedBTreeEntrySource &existing, const OrderedBTreeEntrySource &updates, size_t maximumEntries) {
		if (!existing || !updates || maximumEntries == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "global adjacency merge requires rewindable sources and a finite final-entry bound");
		}
		return [existing, updates, maximumEntries] {
			auto state = std::make_shared<MergeState>();
			state->old = existing();
			state->updates = updates();
			if (!state->old || !state->updates) {
				fail(FormatError::FERR_INVALID_STATE,
					 "global adjacency merge source factory returned an empty cursor");
			}
			state->maximum_entries = maximumEntries;
			state->advanceOld();
			state->advanceUpdate();
			return [state] { return state->next(); };
		};
	}

} // namespace graph::storage
