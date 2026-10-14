#pragma once

#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "graph/storage/LogicalGraphRead.hpp"
#include "query/execution/BoundSlotRegistry.hpp"

namespace graph::query::execution::detail {

struct BoundPropertyRequest {
	BoundSlotId source;
	std::string_view key;
};
struct BoundPropertyId {
	uint32_t index;
	uint64_t program;
	bool operator==(const BoundPropertyId &) const = default;
};
struct BoundPropertyView {
	BoundSlotId source;
	std::string_view key;
	// No token means name-based access, never proof of logical NULL/absence.
	std::optional<uint32_t> token;
};

class BoundPropertyBindingConflict final : public std::runtime_error {
public:
	BoundPropertyBindingConflict() : std::runtime_error("property binding source identity changed; rebind against the execution snapshot") {}
};

// Immutable query-local binding table. The budget outlives this nonmovable
// owner; the sealed layout is borrowed only during construction. Views borrow
// its owned keys, not AST strings,
// rows or pages. Callers revalidate against the actual snapshot before use.
// This table does not enable physical-plan caching or specialize value types.
class BoundPropertyBindings final {
	struct Binding {
		BoundSlotId source;
		std::pmr::string key;
		std::optional<uint32_t> token;
		Binding(BoundSlotId slot, std::string_view name, std::optional<uint32_t> resolved,
			std::pmr::memory_resource *memory) : source(slot), key(name, memory), token(resolved) {}
	};
public:
	BoundPropertyBindings(const BoundSlotRegistry &slots, storage::LogicalGraphReadSnapshot &snapshot,
		std::span<const BoundPropertyRequest> requests, size_t maximumProperties, size_t maximumKeyBytes,
		MaterializedByteBudget &budget, std::pmr::memory_resource *upstream = std::pmr::get_default_resource(),
		const std::function<void()> &checkpoint = {})
		: identity_(readIdentity(snapshot)), token_support_(snapshot.supportsPropertyTokenResolution()),
		  program_(nextIdentity()), memory_(budget, upstream), bindings_(&memory_), requests_(&memory_) {
		if (!slots.sealed()) throw std::invalid_argument("property binding requires a sealed slot layout");
		if (!upstream || maximumProperties == 0U || maximumKeyBytes == 0U ||
			requests.size() > maximumProperties || requests.size() > (std::numeric_limits<uint32_t>::max)())
			throw std::invalid_argument("property binding requires valid descriptor and key limits");
		const auto guard = [&] { if (checkpoint) checkpoint(); };
		// Validate all references and input work before allocation/token resolution.
		size_t keyBytes = 0U;
		for (const auto &request : requests) {
			guard();
			(void) slots.name(request.source);
			if (request.key.empty()) throw std::invalid_argument("property binding key must not be empty");
			if (request.key.size() > maximumKeyBytes - keyBytes)
				throw std::length_error("property binding exceeded its key work bound");
			keyBytes += request.key.size();
		}
		guard();
		bindings_.reserve(requests.size()); // Key views below stay stable, including SSO.
		requests_.reserve(requests.size());
		{
			std::pmr::map<std::pair<uint32_t, std::string_view>, uint32_t> unique(&memory_);
			for (const auto &request : requests) {
				guard();
				const auto key = std::pair{request.source.index, request.key};
				if (const auto found = unique.find(key); found != unique.end()) {
					requests_.push_back(found->second);
					continue;
				}
				const auto token = token_support_ ? snapshot.findPropertyToken(request.key) : std::nullopt;
				if (token && *token == 0U) throw std::logic_error("property binding resolver returned an invalid zero token");
				guard();
				const auto index = static_cast<uint32_t>(bindings_.size());
				bindings_.emplace_back(request.source, request.key, token, &memory_);
				unique.emplace(std::pair{request.source.index, std::string_view(bindings_.back().key)}, index);
				requests_.push_back(index);
			}
		} // Temporary lookup capacity refunds before the immutable table is published.
		guard();
		validateSnapshot(snapshot);
	}
	BoundPropertyBindings(const BoundPropertyBindings &) = delete;
	BoundPropertyBindings &operator=(const BoundPropertyBindings &) = delete;
	BoundPropertyBindings(BoundPropertyBindings &&) = delete;
	BoundPropertyBindings &operator=(BoundPropertyBindings &&) = delete;

	[[nodiscard]] size_t size() const noexcept { return bindings_.size(); }
	[[nodiscard]] size_t requestCount() const noexcept { return requests_.size(); }
	[[nodiscard]] BoundPropertyId descriptor(size_t requestIndex) const {
		return {requests_.at(requestIndex), program_};
	}
	void validateSnapshot(const storage::LogicalGraphReadSnapshot &snapshot) const {
		if (readIdentity(snapshot) != identity_ || snapshot.supportsPropertyTokenResolution() != token_support_)
			throw BoundPropertyBindingConflict();
	}
	[[nodiscard]] BoundPropertyView view(BoundPropertyId descriptor,
		const storage::LogicalGraphReadSnapshot &snapshot) const {
		if (descriptor.program != program_ || descriptor.index >= bindings_.size())
			throw std::invalid_argument("property descriptor belongs to another binding program");
		validateSnapshot(snapshot);
		const auto &binding = bindings_[descriptor.index];
		return {binding.source, binding.key, binding.token};
	}
private:
	static storage::LogicalGraphSchemaIdentity readIdentity(const storage::LogicalGraphReadSnapshot &snapshot) {
		const auto identity = snapshot.bindingSchemaIdentity();
		if (!identity || !identity->isValid())
			throw std::logic_error("property binding requires an authenticated source identity");
		return *identity;
	}
	static uint64_t nextIdentity() {
		static std::atomic<uint64_t> next{1U};
		uint64_t value = next.load(std::memory_order_relaxed);
		for (;;) {
			if (value == (std::numeric_limits<uint64_t>::max)())
				throw std::overflow_error("property binding program identity exhausted");
			if (next.compare_exchange_weak(value, value + 1U, std::memory_order_relaxed)) return value;
		}
	}
	const storage::LogicalGraphSchemaIdentity identity_;
	const bool token_support_;
	const uint64_t program_;
	BudgetMemoryResource memory_;
	std::pmr::vector<Binding> bindings_;
	std::pmr::vector<uint32_t> requests_;
};

} // namespace graph::query::execution::detail
