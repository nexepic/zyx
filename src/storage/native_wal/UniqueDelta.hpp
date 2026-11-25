#pragma once
#include <map>
#include <optional>
#include "State.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace graph::storage::nativewal {
	// Writer-only proof for one logical prefix. The latest claimant for a key
	// shadows its base posting. Deletions can leave stale owners: every hit
	// is checked against the final authoritative row before rejecting.
	class UniqueOwners {
		struct Owner {
			VersionCharge charge;
			uint64_t id;
			Owner(const std::shared_ptr<VersionBudget> &budget, uint64_t id) : charge(budget, 128), id(id) {}
		};
		std::map<int64_t, Owner> changes;
		const IndexedColumn *base = nullptr;
		const UniqueDeclaration *declaration = nullptr;
		Identity identity{};
		uint64_t sequence = 0;

	public:
		void discard() {
			changes.clear();
			base = nullptr;
		}
		void bind(const State &s) {
			discard();
			identity = s.identity;
			sequence = s.sequence;
			declaration = s.unique.get();
			if (declaration && s.column && s.column->complete)
				base = s.column.get();
		}
		bool covers(const State &s) const {
			return base && base == s.column.get() && declaration == s.unique.get() && identity == s.identity &&
				   sequence == s.sequence;
		}
		uint64_t lookup(int64_t key) const {
			if (auto it = changes.find(key); it != changes.end())
				return it->second.id;
			auto it = std::ranges::lower_bound(base->postings, Posting{key, 0});
			return it != base->postings.end() && it->first == key ? it->second : 0;
		}
		void advance(const State &s, std::span<const Posting> claims) {
			if (!base)
				return;
			try {
				for (auto [key, id]: claims) {
					auto it = changes.find(key);
					if (it != changes.end())
						it->second.id = id;
					else {
						if (changes.size() >= std::min<size_t>(4096, s.budget->capacity() / 8192)) {
							discard();
							return;
						}
						changes.try_emplace(key, s.budget, id);
					}
				}
			} catch (const std::length_error &) {
				// Only the optional map node is being admitted or allocated.
				discard();
			} catch (const std::bad_alloc &) {
				discard();
			}
			sequence = s.sequence;
		}
	};

	class UniqueDelta {
		UniqueOwners &owners;
		std::shared_ptr<VersionBudget> budget;
		std::optional<VersionCharge> scratch;
		std::vector<Posting> claims;
		bool covered;
		void fallback() {
			covered = false;
			std::vector<Posting>().swap(claims);
			scratch.reset();
			owners.discard();
		}

	public:
		struct Key {
			bool supported = true;
			std::optional<int64_t> value;
		};
		UniqueDelta(const State &prior, UniqueOwners &owners) :
			owners(owners), budget(prior.budget), covered(owners.covers(prior)) {}
		void reserve(size_t count) {
			if (!covered)
				return;
			if (count > 4096 || count > budget->capacity() / (16 * sizeof(Posting))) {
				fallback();
				return;
			}
			try {
				scratch.emplace(budget, count * sizeof(Posting));
				claims.reserve(count);
			} catch (const std::length_error &) {
				fallback();
			} catch (const std::bad_alloc &) {
				fallback();
			}
		}
		Key key(const State &s, uint64_t id) const {
			if (!covered)
				return {};
			auto it = s.nodes.find(id);
			if (it == s.nodes.end())
				return {};
			const auto &row = it->second;
			if (std::ranges::find(row.labels, s.declaration->label) == row.labels.end())
				return {};
			auto p = std::ranges::find(row.properties, s.declaration->key, &LogicalGraphPropertyEntry::key);
			if (p == row.properties.end() || p->value.getType() == PropertyType::NULL_TYPE)
				return {};
			const auto *v = std::get_if<int64_t>(&p->value.getVariant());
			return v && exactColumnInteger(*v) ? Key{true, *v} : Key{false, {}};
		}
		void observe(uint64_t id, Key before, Key after) {
			if (!covered)
				return;
			if (!before.supported || !after.supported) {
				fallback();
				return;
			}
			if (before.value != after.value && after.value)
				claims.emplace_back(*after.value, id);
		}
		void validate(const State &s, const State *prior, const ChangedNodeIds &changed) {
			if (!covered) {
				if (s.unique)
					validateAffectedScope(
							s, prior, {s.unique->name, s.declaration->label, s.declaration->key, s.unique->revision},
							changed);
			} else
				validateIntegers(s);
			if (s.catalog)
				for (const auto &scope: s.catalog->entries)
					validateAffectedScope(s, prior, scope, changed);
		}

	private:
		void validateIntegers(const State &s) {
			debug::PerfTrace::addValue("native_wal.unique.claims", claims.size());
			std::ranges::sort(claims);
			std::optional<int64_t> previous;
			for (auto [value, id]: claims) {
				auto owner = owners.lookup(value);
				if (previous == value || (owner && owner != id && key(s, owner).value == value))
					throw UniqueViolation("Constraint violation: '" + s.unique->name + "' has a duplicate key");
				previous = value;
			}
		}

	public:
		void advance(const State &s) {
			if (covered)
				owners.advance(s, claims);
			else
				owners.discard();
		}
	};
} // namespace graph::storage::nativewal
