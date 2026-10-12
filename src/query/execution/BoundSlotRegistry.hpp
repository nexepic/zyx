#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "query/execution/BudgetMemoryResource.hpp"

namespace graph::query::execution::detail {

struct BoundSlotId {
	uint32_t index;
	uint64_t plan;
	bool operator==(const BoundSlotId &) const = default;
};
struct BoundScopeId {
	uint32_t index;
	uint64_t plan;
	bool operator==(const BoundScopeId &) const = default;
};
enum class BindingScopeVisibility { BSV_LEXICAL, BSV_ISOLATED };

// Compilation-time name lookup only. Runtime consumers retain numeric slots.
// The plan's budget must outlive this nonmovable registry. Name views expire on
// mutation; seal() makes the layout immutable for subsequent plan consumers.
class BoundSlotRegistry final {
public:
	BoundSlotRegistry(size_t maximumBindings, size_t maximumScopes, size_t maximumNameBytes,
		MaterializedByteBudget &budget, std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: maximum_bindings_(maximumBindings), maximum_scopes_(maximumScopes),
		maximum_name_bytes_(maximumNameBytes), memory_(budget, upstream), scopes_(&memory_), slots_(&memory_) {
		if (maximumBindings == 0U || maximumScopes == 0U || maximumNameBytes == 0U || upstream == nullptr ||
			maximumBindings > (std::numeric_limits<uint32_t>::max)() || maximumScopes > (std::numeric_limits<uint32_t>::max)())
			throw std::invalid_argument("bound slot registry requires valid positive limits and allocator");
		identity_ = nextIdentity();
		scopes_.emplace_back(std::nullopt, BindingScopeVisibility::BSV_ISOLATED, &memory_);
	}
	BoundSlotRegistry(const BoundSlotRegistry &) = delete;
	BoundSlotRegistry &operator=(const BoundSlotRegistry &) = delete;
	BoundSlotRegistry(BoundSlotRegistry &&) = delete;
	BoundSlotRegistry &operator=(BoundSlotRegistry &&) = delete;
	[[nodiscard]] BoundScopeId root() const noexcept { return {0U, identity_}; }
	[[nodiscard]] size_t slotCount() const noexcept { return slots_.size(); }
	[[nodiscard]] size_t bindingCount() const noexcept { return bindings_; }
	[[nodiscard]] bool sealed() const noexcept { return sealed_; }
	void seal() noexcept { sealed_ = true; }

	[[nodiscard]] BoundScopeId createScope(BoundScopeId parent, BindingScopeVisibility visibility) {
		requireMutable();
		(void) scope(parent);
		if (visibility != BindingScopeVisibility::BSV_LEXICAL && visibility != BindingScopeVisibility::BSV_ISOLATED)
			throw std::invalid_argument("invalid binding scope visibility");
		if (scopes_.size() == maximum_scopes_) throw std::runtime_error("bound scope limit exceeded");
		const auto index = static_cast<uint32_t>(scopes_.size());
		scopes_.emplace_back(parent, visibility, &memory_);
		return {index, identity_};
	}
	[[nodiscard]] BoundSlotId declare(BoundScopeId target, std::string_view name) {
		requireMutable();
		auto &destination = scope(target);
		checkName(destination, name);
		const BoundSlotId slot{static_cast<uint32_t>(slots_.size()), identity_};
		auto [entry, inserted] = destination.names.emplace(std::pmr::string(name, &memory_), slot);
		(void) inserted; // checkName already rejected duplicates; no concurrent mutation.
		try { slots_.emplace_back(name, target, &memory_); }
		catch (...) { destination.names.erase(entry); throw; }
		++bindings_;
		name_bytes_ += name.size();
		return slot;
	}
	[[nodiscard]] std::optional<BoundSlotId> resolve(BoundScopeId target, std::string_view name) const {
		for (;;) {
			const auto &current = scope(target);
			const auto found = current.names.find(name);
			if (found != current.names.end()) return found->second;
			if (current.visibility == BindingScopeVisibility::BSV_ISOLATED || !current.parent.has_value())
				return std::nullopt;
			target = *current.parent;
		}
	}
	[[nodiscard]] BoundSlotId importFromParent(BoundScopeId target, std::string_view localName,
		std::string_view parentName) {
		requireMutable();
		auto &destination = scope(target);
		checkName(destination, localName);
		if (!destination.parent.has_value()) throw std::invalid_argument("root scope has no parent to import");
		const auto source = resolve(*destination.parent, parentName);
		if (!source.has_value()) throw std::invalid_argument("correlated import references an unbound parent variable");
		destination.names.emplace(std::pmr::string(localName, &memory_), *source);
		++bindings_;
		name_bytes_ += localName.size();
		return *source; // Alias preserves the parent's slot, never allocates a new one.
	}
	[[nodiscard]] std::string_view name(BoundSlotId slot) const { return slotInfo(slot).name; }
	[[nodiscard]] BoundScopeId origin(BoundSlotId slot) const { return slotInfo(slot).origin; }

private:
	struct NameHash {
		using is_transparent = void;
		size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
	};
	struct NameEqual {
		using is_transparent = void;
		bool operator()(std::string_view left, std::string_view right) const noexcept { return left == right; }
	};
	struct Scope {
		Scope(std::optional<BoundScopeId> source, BindingScopeVisibility mode, std::pmr::memory_resource *resource)
			: parent(source), visibility(mode), names(0U, NameHash{}, NameEqual{}, resource) {}
		Scope(const Scope &) = delete;
		Scope(Scope &&) = default;
		std::optional<BoundScopeId> parent;
		BindingScopeVisibility visibility;
		std::pmr::unordered_map<std::pmr::string, BoundSlotId, NameHash, NameEqual> names;
	};
	struct SlotInfo {
		SlotInfo(std::string_view text, BoundScopeId source, std::pmr::memory_resource *resource)
			: name(text, resource), origin(source) {}
		SlotInfo(const SlotInfo &) = delete;
		SlotInfo(SlotInfo &&) = default;
		std::pmr::string name;
		BoundScopeId origin;
	};
	static uint64_t nextIdentity() {
		static std::atomic<uint64_t> next{1U};
		auto value = next.load(std::memory_order_relaxed);
		for (;;) {
			if (value == (std::numeric_limits<uint64_t>::max)()) throw std::overflow_error("bound plan identity exhausted");
			if (next.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed)) return value;
		}
	}
	void requireMutable() const {
		if (sealed_) throw std::logic_error("bound slot registry is sealed");
	}
	void checkName(const Scope &target, std::string_view name) const {
		if (name.empty() || target.names.contains(name)) throw std::invalid_argument("empty or duplicate local binding");
		if (bindings_ == maximum_bindings_ || name.size() > maximum_name_bytes_ - name_bytes_)
			throw std::runtime_error("bound variable or name byte limit exceeded");
	}
	[[nodiscard]] const Scope &scope(BoundScopeId id) const {
		if (id.plan != identity_ || id.index >= scopes_.size()) throw std::invalid_argument("foreign or invalid binding scope");
		return scopes_[id.index];
	}
	[[nodiscard]] Scope &scope(BoundScopeId id) {
		return const_cast<Scope &>(std::as_const(*this).scope(id));
	}
	[[nodiscard]] const SlotInfo &slotInfo(BoundSlotId id) const {
		if (id.plan != identity_ || id.index >= slots_.size()) throw std::invalid_argument("foreign or invalid bound slot");
		return slots_[id.index];
	}

	size_t maximum_bindings_;
	size_t maximum_scopes_;
	size_t maximum_name_bytes_;
	size_t bindings_ = 0U;
	size_t name_bytes_ = 0U;
	uint64_t identity_ = 0U;
	bool sealed_ = false;
	BudgetMemoryResource memory_;
	std::pmr::vector<Scope> scopes_;
	std::pmr::vector<SlotInfo> slots_;
};

} // namespace graph::query::execution::detail
