#pragma once

#include <algorithm>
#include <bit>
#include <span>
#include <string_view>

#include "graph/core/PropertyTypes.hpp"
#include "query/execution/BatchIdentity.hpp"
#include "query/execution/FixedWidthColumn.hpp"

namespace graph::query::execution::detail {

struct DynamicValueId {
	uint64_t index, owner, incarnation;
	bool operator==(const DynamicValueId &) const = default;
};
struct DynamicForestId {
	uint64_t first, count, owner, incarnation;
};
struct DynamicValueLimits {
	size_t values, bytes, children, map_entries, depth;
};

// Task-local owner; upstream and local budget outlive it. Descriptors are
// checked on every access. Raw returned views borrow only until any mutation.
// This stores values, not an alternative evaluator or persistent wire format.
// Guards may throw/cancel, but must not mutate this owner or its input.
class DynamicValueArena final {
	struct Value {
		PropertyType type = PropertyType::NULL_TYPE;
		ColumnCellState state = ColumnCellState::CCS_NULL;
		uint64_t bits = 0U, offset = 0U, count = 0U;
	};
	struct Entry { uint64_t key_offset = 0U, key_length = 0U, value = 0U; };
	struct Inventory { size_t values = 0U, bytes = 0U, children = 0U, entries = 0U, durations = 0U, roots = 0U; };
public:
	struct MapEntryView { std::string_view key; DynamicValueId value; };
	DynamicValueArena(DynamicValueLimits limits, MaterializedByteBudget &budget,
		std::pmr::memory_resource *upstream = std::pmr::get_default_resource())
		: limits_(limits), memory_(budget, upstream), values_(&memory_), bytes_(&memory_),
		  children_(&memory_), entries_(&memory_), durations_(&memory_), roots_(&memory_), budget_(&budget) {
		if (!upstream || limits.values == 0U || limits.depth == 0U || limits.depth > 256U)
			throw std::invalid_argument("dynamic arena requires bounded values/depth and an allocator");
	}
	DynamicValueArena(const DynamicValueArena &) = delete;
	DynamicValueArena &operator=(const DynamicValueArena &) = delete;
	DynamicValueArena(DynamicValueArena &&) = delete;
	DynamicValueArena &operator=(DynamicValueArena &&) = delete;
	[[nodiscard]] size_t size() const noexcept { return published_values_; }
	[[nodiscard]] size_t rootCount() const noexcept { return published_roots_; }
	[[nodiscard]] const MaterializedByteBudget *budgetDomain() const noexcept { return budget_; }
	[[nodiscard]] BatchRowId borrowVersion() const { return identity_.row(0U); }
	void validateBorrowVersion(BatchRowId version) const { identity_.validate(version); }
	[[nodiscard]] size_t storageBytes() const noexcept {
		return values_.capacity() * sizeof(Value) + bytes_.capacity() + children_.capacity() * sizeof(uint64_t) +
			entries_.capacity() * sizeof(Entry) + durations_.capacity() * sizeof(TemporalDuration) + roots_.capacity() * sizeof(uint64_t);
	}
	[[nodiscard]] DynamicValueId valueId(size_t index) const {
		if (index >= published_values_) throw std::out_of_range("dynamic descriptor is not published");
		const auto identity = identity_.row(0U);
		return {index, identity.owner, identity.incarnation};
	}
	[[nodiscard]] DynamicValueId rootId(size_t index) const {
		if (index >= published_roots_) throw std::out_of_range("dynamic root is not published");
		return valueId(roots_.at(index));
	}
	[[nodiscard]] DynamicValueId root(DynamicForestId forest, size_t index) const {
		identity_.validate({0U, forest.owner, forest.incarnation});
		if (forest.first > published_roots_ || forest.count > published_roots_ - forest.first || index >= forest.count)
			throw std::out_of_range("dynamic forest descriptor exceeds its roots");
		return rootId(forest.first + index);
	}
	template <typename Guard>
	[[nodiscard]] DynamicValueId append(const PropertyValue &value, Guard &&guard) {
		return root(appendMany(std::span<const PropertyValue>(&value, 1U), guard), 0U);
	}
	template <typename Guard>
	[[nodiscard]] DynamicForestId appendMany(std::span<const PropertyValue> source, Guard &&guard) {
		identity_.advance(); // Partial reserve failures can already invalidate spans.
		guard();
		const Inventory previous{values_.size(), bytes_.size(), children_.size(), entries_.size(), durations_.size(), roots_.size()};
		Inventory requested;
		add(roots_.size(), requested.roots, source.size(), limits_.values);
		for (const auto &value : source) scan(value, 1U, requested, guard);
		guard();
		try {
			guard(); values_.reserve(previous.values + requested.values);
			guard(); bytes_.reserve(previous.bytes + requested.bytes);
			guard(); children_.reserve(previous.children + requested.children);
			guard(); entries_.reserve(previous.entries + requested.entries);
			guard(); durations_.reserve(previous.durations + requested.durations);
			guard(); roots_.reserve(previous.roots + requested.roots);
			for (const auto &value : source) roots_.push_back(appendNode(value, guard));
			guard();
			published_values_ = values_.size(); published_roots_ = roots_.size();
			const auto identity = identity_.row(0U);
			return {previous.roots, source.size(), identity.owner, identity.incarnation};
		} catch (...) {
			values_.resize(previous.values); bytes_.resize(previous.bytes); children_.resize(previous.children);
			entries_.resize(previous.entries); durations_.resize(previous.durations); roots_.resize(previous.roots);
			published_values_ = previous.values; published_roots_ = previous.roots;
			throw; // Retained capacity remains charged; published values stay intact.
		}
	}
	[[nodiscard]] DynamicValueId appendMissing() {
		identity_.advance();
		if (values_.size() == limits_.values) throw std::length_error("dynamic arena value limit exceeded");
		Value missing;
		missing.state = ColumnCellState::CCS_MISSING;
		// Both reserves precede publication; a later root allocation cannot
		// leave a missing value published without its root index.
		values_.reserve(values_.size() + 1U);
		roots_.reserve(roots_.size() + 1U);
		const size_t index = values_.size();
		values_.push_back(missing);
		roots_.push_back(index);
		published_values_ = values_.size(); published_roots_ = roots_.size();
		return valueId(index);
	}
	[[nodiscard]] PropertyType type(DynamicValueId id) const { return value(id).type; }
	[[nodiscard]] ColumnCellState state(DynamicValueId id) const { return value(id).state; }
	[[nodiscard]] PropertyValue scalar(DynamicValueId id) const {
		const auto &item = value(id);
		if (item.state != ColumnCellState::CCS_VALUE) return PropertyValue{};
		switch (item.type) {
			case PropertyType::BOOLEAN: return PropertyValue(item.bits != 0U);
			case PropertyType::INTEGER: return PropertyValue(std::bit_cast<int64_t>(item.bits));
			case PropertyType::DOUBLE: return PropertyValue(std::bit_cast<double>(item.bits));
			case PropertyType::DATE: return PropertyValue(TemporalDate{static_cast<int32_t>(std::bit_cast<int64_t>(item.bits))});
			case PropertyType::DATETIME: return PropertyValue(TemporalDateTime{std::bit_cast<int64_t>(item.bits)});
			case PropertyType::DURATION: return PropertyValue(durations_.at(item.offset));
			default: throw std::logic_error("dynamic arena value is not a fixed scalar");
		}
	}
	[[nodiscard]] std::string_view text(DynamicValueId id) const {
		const auto &item = require(id, PropertyType::STRING);
		return key(item.offset, item.count);
	}
	[[nodiscard]] std::span<const std::byte> binary(DynamicValueId id) const {
		const auto &item = require(id, PropertyType::BYTES);
		if (item.count == 0U) return {};
		return std::span<const std::byte>(bytes_).subspan(item.offset, item.count);
	}
	[[nodiscard]] size_t elementCount(DynamicValueId id) const {
		const auto &item = value(id);
		if (item.type != PropertyType::LIST && item.type != PropertyType::MAP)
			throw std::logic_error("dynamic arena value has no children");
		return item.count;
	}
	[[nodiscard]] DynamicValueId child(DynamicValueId id, size_t index) const {
		const auto &item = require(id, PropertyType::LIST);
		if (index >= item.count) throw std::out_of_range("dynamic list index exceeds its range");
		return valueId(children_.at(item.offset + index));
	}
	[[nodiscard]] MapEntryView mapEntry(DynamicValueId id, size_t index) const {
		const auto &item = require(id, PropertyType::MAP);
		if (index >= item.count) throw std::out_of_range("dynamic map index exceeds its range");
		const auto &entry = entries_.at(item.offset + index);
		return {key(entry.key_offset, entry.key_length), valueId(entry.value)};
	}
	void clear() noexcept {
		identity_.invalidate(); published_values_ = 0U; published_roots_ = 0U; values_.clear(); bytes_.clear(); children_.clear(); entries_.clear(); durations_.clear(); roots_.clear();
	}
	void release() noexcept {
		clear();
		decltype(values_)(&memory_).swap(values_); decltype(bytes_)(&memory_).swap(bytes_);
		decltype(children_)(&memory_).swap(children_); decltype(entries_)(&memory_).swap(entries_);
		decltype(durations_)(&memory_).swap(durations_); decltype(roots_)(&memory_).swap(roots_);
	}
private:
	const Value &value(DynamicValueId id) const {
		identity_.validate({0U, id.owner, id.incarnation});
		if (id.index >= published_values_) throw std::out_of_range("dynamic value descriptor exceeds its owner");
		return values_[static_cast<size_t>(id.index)];
	}
	const Value &require(DynamicValueId id, PropertyType type) const {
		const auto &item = value(id);
		if (item.state != ColumnCellState::CCS_VALUE || item.type != type)
			throw std::logic_error("dynamic arena value has a different type or state");
		return item;
	}
	std::string_view key(uint64_t offset, uint64_t count) const {
		if (offset > bytes_.size() || count > bytes_.size() - offset) throw std::logic_error("dynamic byte range is invalid");
		if (count == 0U) return {};
		return {reinterpret_cast<const char *>(bytes_.data() + offset), static_cast<size_t>(count)};
	}
	static void add(size_t existing, size_t &requested, size_t count, size_t limit) {
		if (existing > limit || requested > limit - existing || count > limit - existing - requested)
			throw std::length_error("dynamic arena exceeded its value/byte/element bound");
		requested += count;
	}
	template <typename Guard>
	void scan(const PropertyValue &source, size_t depth, Inventory &count, Guard &guard) const {
		guard();
		if (depth > limits_.depth) throw std::length_error("dynamic arena exceeded its depth bound");
		add(values_.size(), count.values, 1U, limits_.values);
		const auto &variant = source.getVariant();
		if (const auto *text = std::get_if<std::string>(&variant)) add(bytes_.size(), count.bytes, text->size(), limits_.bytes);
		else if (const auto *bytes = std::get_if<PropertyValue::Bytes>(&variant)) add(bytes_.size(), count.bytes, bytes->size(), limits_.bytes);
		else if (const auto *list = std::get_if<std::vector<PropertyValue>>(&variant)) {
			add(children_.size(), count.children, list->size(), limits_.children);
			for (const auto &child : *list) scan(child, depth + 1U, count, guard);
		} else if (const auto *map = std::get_if<PropertyValue::MapType>(&variant)) {
			add(entries_.size(), count.entries, map->size(), limits_.map_entries);
			for (const auto &[name, child] : *map) {
				guard(); add(bytes_.size(), count.bytes, name.size(), limits_.bytes);
				scan(child, depth + 1U, count, guard);
			}
		} else if (source.getType() == PropertyType::DURATION) add(durations_.size(), count.durations, 1U, limits_.values);
	}
	template <typename Guard>
	uint64_t appendBytes(std::span<const std::byte> source, Guard &guard) {
		const uint64_t offset = bytes_.size();
		for (size_t copied = 0U; copied < source.size();) {
			guard();
			const size_t count = std::min<size_t>(4096U, source.size() - copied);
			bytes_.insert(bytes_.end(), source.begin() + copied, source.begin() + copied + count);
			copied += count;
		}
		return offset;
	}
	template <typename Guard>
	size_t appendNode(const PropertyValue &source, Guard &guard) {
		guard();
		const size_t index = values_.size();
		values_.push_back(Value{});
		Value item;
		item.type = source.getType();
		item.state = item.type == PropertyType::NULL_TYPE ? ColumnCellState::CCS_NULL : ColumnCellState::CCS_VALUE;
		const auto &variant = source.getVariant();
		switch (item.type) {
			case PropertyType::NULL_TYPE: break;
			case PropertyType::BOOLEAN: item.bits = std::get<bool>(variant); break;
			case PropertyType::INTEGER: item.bits = std::bit_cast<uint64_t>(std::get<int64_t>(variant)); break;
			case PropertyType::DOUBLE: item.bits = std::bit_cast<uint64_t>(std::get<double>(variant)); break;
			case PropertyType::DATE: item.bits = std::bit_cast<uint64_t>(static_cast<int64_t>(std::get<TemporalDate>(variant).epochDays)); break;
			case PropertyType::DATETIME: item.bits = std::bit_cast<uint64_t>(std::get<TemporalDateTime>(variant).epochMillis); break;
			case PropertyType::DURATION: item.offset = durations_.size(); durations_.push_back(std::get<TemporalDuration>(variant)); break;
			case PropertyType::STRING: {
				const auto &text = std::get<std::string>(variant);
				item.offset = appendBytes(std::as_bytes(std::span(text.data(), text.size())), guard); item.count = text.size(); break;
			}
			case PropertyType::BYTES: {
				const auto &bytes = std::get<PropertyValue::Bytes>(variant);
				item.offset = appendBytes(std::span<const std::byte>(bytes), guard); item.count = bytes.size(); break;
			}
			case PropertyType::LIST: {
				const auto &list = std::get<std::vector<PropertyValue>>(variant);
				item.offset = children_.size(); item.count = list.size(); children_.resize(children_.size() + list.size());
				for (size_t child = 0U; child < list.size(); ++child) children_[item.offset + child] = appendNode(list[child], guard);
				break;
			}
			case PropertyType::MAP: {
				const auto &map = std::get<PropertyValue::MapType>(variant);
				item.offset = entries_.size(); item.count = map.size(); entries_.resize(entries_.size() + map.size());
				size_t entry = item.offset;
				for (const auto &[name, child] : map) {
					const uint64_t offset = appendBytes(std::as_bytes(std::span(name.data(), name.size())), guard);
					const uint64_t value = appendNode(child, guard);
					entries_[entry++] = {offset, name.size(), value};
				}
				break;
			}
			default: throw std::invalid_argument("unsupported dynamic arena type");
		}
		values_[index] = item;
		return index;
	}
	const DynamicValueLimits limits_;
	size_t published_values_ = 0U, published_roots_ = 0U;
	BatchIdentity identity_;
	BudgetMemoryResource memory_;
	std::pmr::vector<Value> values_;
	std::pmr::vector<std::byte> bytes_;
	std::pmr::vector<uint64_t> children_;
	std::pmr::vector<Entry> entries_;
	std::pmr::vector<TemporalDuration> durations_;
	std::pmr::vector<uint64_t> roots_;
	const MaterializedByteBudget *budget_;
};
} // namespace graph::query::execution::detail
