#pragma once

#include <array>
#include <memory>

#include "PropertyBlockFieldMembership.hpp"

namespace zyx::benchmark {

// Candidate-only immutable group. Durable publication and retirement belong to
// the enclosing descriptor protocol; this object grants no storage authority.
class SparsePropertyGroup final {
public:
	using Identity = PropertyBlockFieldMembership::SnapshotIdentity;
	using ColumnBytes = std::shared_ptr<const std::vector<std::byte>>;

	static SparsePropertyGroup freeze(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockFieldMembership &directory,
		std::span<const PropertyBlockFieldMembership::SparseColumnView> columns, Identity identity) {
		directory.validateSparseColumns(slots, columns, identity);
		SparsePropertyGroup result(directory, identity);
		result.columns_.reserve(columns.size());
		for (const auto &column : columns)
			result.columns_.push_back({column.token,
				std::make_shared<const std::vector<std::byte>>(column.bytes.begin(), column.bytes.end())});
		result.validate(slots);
		return result;
	}

	SparsePropertyGroup replace(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockOwnerSlots::Route &route, uint32_t token,
		const std::optional<graph::PropertyValue> &value, Identity next, size_t maximumFields) const {
		validate(slots);
		auto directory = directory_.withFieldPresence(slots, route, token, value.has_value(), identity_, next, maximumFields);
		SparsePropertyGroup result(std::move(directory), next);
		result.columns_ = columns_;
		auto position = std::lower_bound(result.columns_.begin(), result.columns_.end(), token,
			[](const auto &column, uint32_t key) { return column.token < key; });
		const bool existing = position != result.columns_.end() && position->token == token;
		if (existing || value.has_value()) {
			std::vector<std::byte> empty;
			if (!existing) {
				const std::array<std::optional<graph::PropertyValue>, PropertyBlockOwnerSlots::CAPACITY> values{};
				empty = SparseFixedWidthPropertyColumn::encode(values);
			}
			const auto &source = existing ? *position->bytes : empty;
			auto changed = SparseFixedWidthPropertyColumn::replace(source, route.slot, value);
			if (graph::storage::LittleEndianCodec::readU64(changed, 16U) == 0U) {
				if (existing) result.columns_.erase(position);
			} else {
				Column column{token, std::make_shared<const std::vector<std::byte>>(std::move(changed))};
				if (existing) *position = std::move(column);
				else result.columns_.insert(position, std::move(column));
			}
		}
		result.validate(slots);
		return result;
	}

	ColumnBytes column(uint32_t token) const {
		const auto position = std::lower_bound(columns_.begin(), columns_.end(), token,
			[](const auto &column, uint32_t key) { return column.token < key; });
		return position != columns_.end() && position->token == token ? position->bytes : nullptr;
	}

	std::optional<graph::PropertyValue> read(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockOwnerSlots::Route &route, uint32_t token, Identity expected) const {
		if (expected != identity_ || token == 0U)
			throw std::invalid_argument("Sparse group read requires its snapshot identity and a valid token");
		const auto keys = directory_.fields(slots, route, expected);
		if (!std::binary_search(keys.begin(), keys.end(), token)) return std::nullopt;
		const auto bytes = column(token);
		if (!bytes) throw std::runtime_error("Sparse group omitted a present field column");
		const auto value = SparseFixedWidthPropertyColumn::read(*bytes, route.slot);
		if (!value) throw std::runtime_error("Sparse group omitted a present field value");
		return value;
	}

	graph::storage::PropertyCollectionRoot readOwner(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockOwnerSlots::Route &route, Identity expected, size_t maximumFields) const {
		if (expected != identity_ || maximumFields == 0U)
			throw std::invalid_argument("Sparse group owner read requires its snapshot identity and a positive bound");
		const auto keys = directory_.fields(slots, route, expected);
		if (keys.size() > maximumFields) throw std::length_error("Sparse group owner read exceeded its field bound");
		graph::storage::PropertyCollectionRoot result{route.owner, slots.ownerType(), {}};
		result.entries.reserve(keys.size());
		for (const auto token : keys) result.entries.push_back({token, *read(slots, route, token, expected)});
		return result;
	}

	const PropertyBlockFieldMembership &directory() const { return directory_; }
	Identity identity() const { return identity_; }

private:
	struct Column { uint32_t token; ColumnBytes bytes; };
	SparsePropertyGroup(PropertyBlockFieldMembership directory, Identity identity)
		: directory_(std::move(directory)), identity_(identity) {}
	void validate(const PropertyBlockOwnerSlots &slots) const {
		std::vector<PropertyBlockFieldMembership::SparseColumnView> views;
		views.reserve(columns_.size());
		for (const auto &column : columns_) views.push_back({column.token, *column.bytes});
		directory_.validateSparseColumns(slots, views, identity_);
	}
	PropertyBlockFieldMembership directory_;
	Identity identity_;
	std::vector<Column> columns_;
};

} // namespace zyx::benchmark
