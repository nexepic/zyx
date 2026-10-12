#pragma once

#include <algorithm>
#include <array>
#include "graph/utils/Sha256.hpp"
#include <limits>
#include <vector>

#include "PropertyBlockOwnerSlots.hpp"
#include "SparseFixedWidthPropertyColumn.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/entity/PageBackedGraphReadAdapter.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"

namespace zyx::benchmark {

// Candidate-only actual-field enumeration. No durable codec or production route.
class PropertyBlockFieldMembership final {
public:
	struct SnapshotIdentity {
		uint64_t database_id;
		uint64_t generation;
		bool operator==(const SnapshotIdentity &) const = default;
	};

	// Read all source fields from one actual snapshot instead of trusting a
	// caller-assembled owner collection. Admission follows the native read API.
	static PropertyBlockFieldMembership buildFromNativeSnapshot(const PropertyBlockOwnerSlots &slots,
		const graph::storage::PageBackedGraphReadAdapter &graph, const graph::storage::MetadataSnapshotPin &snapshot,
		size_t maximumFields, const graph::storage::SelectedPropertyReadAdmission &admission,
		const std::function<void()> &checkpoint = {}) {
		if (checkpoint) checkpoint();
		if (maximumFields == 0U || !admission) throw std::invalid_argument("Native field membership requires admission and a bound");
		if (slots.ownerType() != graph::Node::typeId && slots.ownerType() != graph::Edge::typeId)
			throw std::invalid_argument("Native field membership requires Node or Edge owners");
		class CheckpointAdapter final : public graph::storage::CancellationCheckpoint {
		public:
			explicit CheckpointAdapter(const std::function<void()> &callback) : callback_(callback) {}
			void check() const override { if (callback_) callback_(); }
		private:
			const std::function<void()> &callback_;
		} cancellation(checkpoint);
		graph::storage::NativeReadSession session;
		const graph::storage::SelectedPropertyReadAdmission controlledAdmission = [&](size_t bytes) {
			if (checkpoint) checkpoint();
			admission(bytes);
			if (checkpoint) checkpoint();
		};
		const auto identity = snapshotIdentity(snapshot);
		PropertyBlockFieldMembership result(identity, slots.blockId(), slots.ownerType());
		const auto checkedAdd = [](size_t left, size_t right) {
			if (right > (std::numeric_limits<size_t>::max)() - left)
				throw std::overflow_error("Native field membership byte count overflowed");
			return left + right;
		};
		size_t sourceTokenBytes = 0U, sourceTransientBytes = 0U, ownerBytes = 0U;
		const auto account = [&] {
			controlledAdmission(checkedAdd(result.metadataCapacityBytes(), checkedAdd(ownerBytes, checkedAdd(sourceTokenBytes, sourceTransientBytes))));
		};
		controlledAdmission(checkedAdd(sizeof(result), slots.activeOwnerCount() * sizeof(Member)));
		result.members_.reserve(slots.activeOwnerCount());
		account();
		const graph::storage::SelectedPropertyReadAdmission admitTransient = [&](size_t bytes) {
			sourceTransientBytes = bytes;
			account();
		};
		ownerBytes = slots.activeOwnerCount() * sizeof(uint64_t);
		account();
		auto owners = slots.activeOwnersSorted();
		ownerBytes = owners.capacity() * sizeof(uint64_t);
		account();
		for (const auto owner : owners) {
			if (checkpoint) checkpoint();
			bool requestedCapacity = true;
			const graph::storage::SelectedPropertyReadAdmission admitTokens = [&](size_t bytes) {
				if (checkpoint) checkpoint();
				if (requestedCapacity && bytes / sizeof(uint32_t) > maximumFields - result.tokens_.size())
					throw std::runtime_error("Native field membership exceeded its entry bound");
				requestedCapacity = false;
				sourceTokenBytes = bytes;
				account();
			};
			const graph::storage::EntityPropertyKey key{
				slots.ownerType() == graph::Node::typeId
					? graph::storage::EntityPropertyOwnerKind::EPOK_NODE
					: graph::storage::EntityPropertyOwnerKind::EPOK_EDGE, owner};
			auto tokens = graph.readEntityPropertyTokens(snapshot, key, admitTokens, admitTransient, &session, checkpoint ? &cancellation : nullptr);
			if (checkpoint) checkpoint();
			if (tokens.size() > maximumFields - result.tokens_.size())
				throw std::runtime_error("Native field membership exceeded its entry bound");
			const size_t required = result.tokens_.size() + tokens.size();
			if (required > result.tokens_.capacity()) {
				const size_t oldCapacity = result.tokens_.capacity();
				const size_t growth = (std::min)(oldCapacity, maximumFields - oldCapacity);
				const size_t requested = (std::max)(required, oldCapacity + growth);
				if (requested > (std::numeric_limits<size_t>::max)() / sizeof(uint32_t))
					throw std::overflow_error("Native field membership token capacity overflowed");
				const size_t sourceBytes = checkedAdd(ownerBytes, checkedAdd(sourceTokenBytes, sourceTransientBytes));
				// During reserve both the previous array and its replacement may live.
				controlledAdmission(checkedAdd(checkedAdd(result.metadataCapacityBytes(), sourceBytes), requested * sizeof(uint32_t)));
				result.tokens_.reserve(requested);
				// Actual allocator capacity may exceed the request. Report its overlap
				// with the former buffer before accepting the resulting directory.
				controlledAdmission(checkedAdd(checkedAdd(result.metadataCapacityBytes(), sourceBytes), oldCapacity * sizeof(uint32_t)));
				account();
			}
			result.members_.push_back({slots.route(owner), result.tokens_.size(), tokens.size()});
			result.tokens_.insert(result.tokens_.end(), tokens.begin(), tokens.end());
			std::vector<uint32_t>().swap(tokens);
			sourceTokenBytes = 0U;
			account();
		}
		std::vector<uint64_t>().swap(owners);
		ownerBytes = 0U;
		account();
		if (checkpoint) checkpoint();
		std::ranges::sort(result.members_, {}, [](const auto &member) { return member.route.slot; });
		return result;
	}

	// The caller must obtain this version from the authoritative descriptor in
	// the supplied snapshot. A birth generation alone is not a freshness proof.
	static PropertyBlockFieldMembership decodeFromDescriptor(std::span<const std::byte> bytes,
		const PropertyBlockOwnerSlots &slots, const graph::storage::MetadataSnapshotPin &snapshot,
		uint64_t directoryGeneration, size_t maximumFields, size_t maximumBytes) {
		return decode(bytes, slots, descriptorIdentity(snapshot, directoryGeneration), maximumFields, maximumBytes);
	}

	std::span<const uint32_t> fieldsFromDescriptor(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockOwnerSlots::Route &route, const graph::storage::MetadataSnapshotPin &snapshot,
		uint64_t directoryGeneration) const {
		return fields(slots, route, descriptorIdentity(snapshot, directoryGeneration));
	}

	static PropertyBlockFieldMembership buildSnapshotBound(const PropertyBlockOwnerSlots &slots,
		std::span<const graph::storage::PropertyCollectionRoot> owners, size_t maximumFields,
		const graph::storage::MetadataSnapshotPin &snapshot) {
		return build(slots, owners, maximumFields, snapshotIdentity(snapshot));
	}

	std::span<const uint32_t> fieldsSnapshotBound(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockOwnerSlots::Route &route, const graph::storage::MetadataSnapshotPin &snapshot) const {
		return fields(slots, route, snapshotIdentity(snapshot));
	}

	static PropertyBlockFieldMembership build(const PropertyBlockOwnerSlots &slots,
		std::span<const graph::storage::PropertyCollectionRoot> owners, size_t maximumFields, SnapshotIdentity identity) {
		if (identity.database_id == 0U || identity.generation == 0U)
			throw std::invalid_argument("Field membership requires a valid snapshot identity");
		if (maximumFields == 0U) throw std::invalid_argument("Field membership requires a positive bound");
		slots.validateOwners(owners);
		size_t count = 0U;
		// Admit all metadata work before allocating any field vectors.
		for (const auto &owner : owners) {
			if (owner.entries.size() > maximumFields - count)
				throw std::runtime_error("Field membership exceeded its entry bound");
			count += owner.entries.size();
			uint32_t previous = 0U;
			for (const auto &entry : owner.entries) {
				if (entry.key_token <= previous) throw std::invalid_argument("Field membership requires ordered unique tokens");
				previous = entry.key_token;
			}
		}
		PropertyBlockFieldMembership result(identity, slots.blockId(), slots.ownerType());
		result.members_.reserve(owners.size());
		result.tokens_.reserve(count);
		for (const auto &owner : owners) {
			const auto route = slots.route(owner.owner_entity_id);
			result.members_.push_back({route, result.tokens_.size(), owner.entries.size()});
			for (const auto &entry : owner.entries) result.tokens_.push_back(entry.key_token);
		}
		std::ranges::sort(result.members_, {}, [](const auto &member) { return member.route.slot; });
		return result;
	}

	std::span<const uint32_t> fields(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockOwnerSlots::Route &route, SnapshotIdentity identity) const {
		if (identity != identity_)
			throw std::runtime_error("Field membership does not match the database snapshot");
		static_cast<void>(slots.resolve(route));
		const auto member = std::lower_bound(members_.begin(), members_.end(), route.slot,
			[](const auto &candidate, uint32_t slot) { return candidate.route.slot < slot; });
		if (member == members_.end() || member->route != route)
			throw std::runtime_error("Field membership does not match the owner slot generation");
		return std::span<const uint32_t>(tokens_).subspan(member->begin, member->count);
	}

	// Candidate metadata replacement; publication must pair this directory with
	// its changed column in one descriptor generation. Values are not decoded.
	PropertyBlockFieldMembership withFieldPresence(const PropertyBlockOwnerSlots &slots,
		const PropertyBlockOwnerSlots::Route &route, uint32_t token, bool present,
		SnapshotIdentity current, SnapshotIdentity next, size_t maximumFields) const {
		if (token == 0U || maximumFields == 0U || next.database_id != current.database_id ||
			next.generation <= current.generation)
			throw std::invalid_argument("Field presence update requires a token, bound and advancing snapshot identity");
		if (slots.blockId() != block_id_ || slots.ownerType() != owner_type_ || slots.activeOwnerCount() != members_.size())
			throw std::runtime_error("Field presence update owner set mismatch");
		const auto original = fields(slots, route, current);
		const bool existed = std::binary_search(original.begin(), original.end(), token);
		size_t count = tokens_.size();
		if (present && !existed) {
			if (count >= maximumFields) throw std::length_error("Field presence update exceeded its entry bound");
			++count;
		} else if (!present && existed) --count;
		if (count > maximumFields) throw std::length_error("Field presence update exceeded its entry bound");
		for (const auto &member : members_) static_cast<void>(fields(slots, member.route, current));
		PropertyBlockFieldMembership result(next, block_id_, owner_type_);
		result.members_.reserve(members_.size());
		result.tokens_.reserve(count);
		for (const auto &member : members_) {
			const auto keys = fields(slots, member.route, current);
			const size_t begin = result.tokens_.size();
			if (member.route != route || existed == present) {
				result.tokens_.insert(result.tokens_.end(), keys.begin(), keys.end());
			} else {
				const auto position = std::lower_bound(keys.begin(), keys.end(), token);
				result.tokens_.insert(result.tokens_.end(), keys.begin(), position);
				if (present) {
					result.tokens_.push_back(token);
					result.tokens_.insert(result.tokens_.end(), position, keys.end());
				} else {
					result.tokens_.insert(result.tokens_.end(), position + 1, keys.end());
				}
			}
			result.members_.push_back({member.route, begin, result.tokens_.size() - begin});
		}
		return result;
	}

	void validateOwnerRoutes(const PropertyBlockOwnerSlots &slots, SnapshotIdentity identity) const {
		if (identity != identity_ || slots.blockId() != block_id_ || slots.ownerType() != owner_type_ ||
			slots.activeOwnerCount() != members_.size())
			throw std::runtime_error("Field directory owner identity mismatch");
		for (const auto &member : members_) static_cast<void>(fields(slots, member.route, identity));
	}

	// Compact candidate. Owner IDs and slot generations are supplied by
	// the enclosing authenticated owner-slot table, not duplicated per member.
	std::vector<std::byte> encodeSlotCounts(const PropertyBlockOwnerSlots &slots, SnapshotIdentity identity,
		size_t maximumBytes) const {
		validateOwnerRoutes(slots, identity);
		constexpr size_t header = 80U, countsBytes = PropertyBlockOwnerSlots::CAPACITY * sizeof(uint32_t);
		if (maximumBytes < header + countsBytes || tokens_.size() > UINT32_MAX ||
			tokens_.size() > (maximumBytes - header - countsBytes) / sizeof(uint32_t))
			throw std::length_error("Compact field directory byte bound exceeded");
		std::vector<std::byte> bytes(header + countsBytes + tokens_.size() * sizeof(uint32_t));
		LittleEndianCodec::writeU32(bytes, 0U, 0x43464D5AU);
		LittleEndianCodec::writeU16(bytes, 4U, 2U);
		LittleEndianCodec::writeU16(bytes, 6U, PropertyBlockOwnerSlots::CAPACITY);
		LittleEndianCodec::writeU64(bytes, 8U, identity.database_id);
		LittleEndianCodec::writeU64(bytes, 16U, identity.generation);
		LittleEndianCodec::writeU64(bytes, 24U, block_id_);
		LittleEndianCodec::writeU32(bytes, 32U, owner_type_);
		LittleEndianCodec::writeU32(bytes, 36U, static_cast<uint32_t>(tokens_.size()));
		const auto ownerHash = graph::utils::Sha256::digest(slots.encode());
		std::copy(ownerHash.begin(), ownerHash.end(), bytes.begin() + 48U);
		size_t offset = header + countsBytes;
		for (const auto &member : members_) {
			const auto keys = fields(slots, member.route, identity);
			LittleEndianCodec::writeU32(bytes, header + member.route.slot * sizeof(uint32_t), static_cast<uint32_t>(keys.size()));
			for (const auto token : keys) {
				LittleEndianCodec::writeU32(bytes, offset, token);
				offset += sizeof(uint32_t);
			}
		}
		LittleEndianCodec::writeU32(bytes, 40U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		return bytes;
	}

	static PropertyBlockFieldMembership decodeSlotCounts(std::span<const std::byte> bytes,
		const PropertyBlockOwnerSlots &slots, SnapshotIdentity identity, size_t maximumFields, size_t maximumBytes) {
		constexpr size_t header = 80U, base = header + PropertyBlockOwnerSlots::CAPACITY * sizeof(uint32_t);
		if (bytes.size() < base || bytes.size() > maximumBytes || maximumFields == 0U ||
			identity.database_id == 0U || identity.generation == 0U ||
			LittleEndianCodec::readU32(bytes, 0U) != 0x43464D5AU || LittleEndianCodec::readU16(bytes, 4U) != 2U ||
			LittleEndianCodec::readU16(bytes, 6U) != PropertyBlockOwnerSlots::CAPACITY ||
			LittleEndianCodec::readU64(bytes, 8U) != identity.database_id || LittleEndianCodec::readU64(bytes, 16U) != identity.generation ||
			LittleEndianCodec::readU64(bytes, 24U) != slots.blockId() || LittleEndianCodec::readU32(bytes, 32U) != slots.ownerType() ||
			LittleEndianCodec::readU32(bytes, 44U) != 0U)
			throw std::runtime_error("Invalid compact field directory header");
		const size_t tokens = LittleEndianCodec::readU32(bytes, 36U);
		if (tokens > maximumFields || (bytes.size() - base) % 4U != 0U || tokens != (bytes.size() - base) / 4U)
			throw std::runtime_error("Invalid compact field directory length");
		const auto ownerHash = graph::utils::Sha256::digest(slots.encode());
		if (!std::equal(ownerHash.begin(), ownerHash.end(), bytes.begin() + 48U))
			throw std::runtime_error("Compact field directory owner table mismatch");
		auto checked = std::vector<std::byte>(bytes.begin(), bytes.end());
		LittleEndianCodec::writeU32(checked, 40U, 0U);
		if (graph::utils::calculateCrc(checked.data(), checked.size()) != LittleEndianCodec::readU32(bytes, 40U))
			throw std::runtime_error("Invalid compact field directory checksum");
		std::array<std::optional<PropertyBlockOwnerSlots::Route>, PropertyBlockOwnerSlots::CAPACITY> routes{};
		for (const auto owner : slots.activeOwnersSorted()) {
			const auto route = slots.route(owner); routes[route.slot] = route;
		}
		size_t used = 0U;
		for (size_t slot = 0U; slot < routes.size(); ++slot) {
			const size_t count = LittleEndianCodec::readU32(bytes, header + slot * 4U);
			if (count > tokens - used || (!routes[slot] && count != 0U))
				throw std::runtime_error("Invalid compact field directory slot count");
			uint32_t previous = 0U;
			for (size_t index = 0U; index < count; ++index) {
				const auto token = LittleEndianCodec::readU32(bytes, base + (used + index) * 4U);
				if (token <= previous) throw std::runtime_error("Invalid compact field directory token order");
				previous = token;
			}
			used += count;
		}
		if (used != tokens) throw std::runtime_error("Compact field directory omitted tokens");
		PropertyBlockFieldMembership result(identity, slots.blockId(), slots.ownerType());
		result.members_.reserve(slots.activeOwnerCount()); result.tokens_.reserve(tokens);
		used = 0U;
		for (size_t slot = 0U; slot < routes.size(); ++slot) {
			const size_t count = LittleEndianCodec::readU32(bytes, header + slot * 4U);
			if (routes[slot]) result.members_.push_back({*routes[slot], used, count});
			used += count;
		}
		for (size_t index = 0U; index < tokens; ++index)
			result.tokens_.push_back(LittleEndianCodec::readU32(bytes, base + index * 4U));
		return result;
	}

	std::vector<std::byte> encodeAdaptive(const PropertyBlockOwnerSlots &slots, SnapshotIdentity identity,
		size_t maximumBytes) const {
		validateOwnerRoutes(slots, identity);
		// At nine owners the two fixed costs tie; keep the older representation.
		return members_.size() <= 9U ? encode(maximumBytes) : encodeSlotCounts(slots, identity, maximumBytes);
	}

	static PropertyBlockFieldMembership decodeAdaptive(std::span<const std::byte> bytes,
		const PropertyBlockOwnerSlots &slots, SnapshotIdentity identity, size_t maximumFields, size_t maximumBytes) {
		if (bytes.size() < sizeof(uint32_t)) throw std::runtime_error("Truncated field directory encoding tag");
		const auto magic = LittleEndianCodec::readU32(bytes, 0U);
		if (magic == WIRE_MAGIC) return decode(bytes, slots, identity, maximumFields, maximumBytes);
		if (magic == 0x43464D5AU) return decodeSlotCounts(bytes, slots, identity, maximumFields, maximumBytes);
		throw std::runtime_error("Unknown field directory encoding tag");
	}

	// Check the complete metadata relation without decoding another payload copy.
	// A verified image owns authenticated immutable column buffers.
	void validateBlockMembership(const PropertyBlockOwnerSlots &slots,
		const TypedPropertyBlock::VerifiedImage &proof, SnapshotIdentity identity) const {
		if (identity != identity_) throw std::runtime_error("Field membership does not match the database snapshot");
		if (slots.blockId() != block_id_ || slots.ownerType() != owner_type_)
			throw std::runtime_error("Field membership does not match the property block identity");
		const auto &image = proof.image();
		const auto owners = TypedPropertyBlock::decode(image, slots.blockId(), slots.ownerType(), {});
		slots.validateOwners(owners);
		const auto &metadata = image.metadata;
		const size_t count = graph::storage::LittleEndianCodec::readU32(metadata, 8U);
		for (size_t row = 0U; row < owners.size(); ++row) {
			const auto expected = fields(slots, slots.route(owners[row].owner_entity_id), identity);
			size_t index = 0U;
			for (size_t column = 0U; column < count; ++column) {
				const size_t offset = TypedPropertyBlock::HEADER + owners.size() * 8U + column * TypedPropertyBlock::DESCRIPTOR;
				const auto presence = graph::storage::LittleEndianCodec::readU64(metadata, offset + 8U);
				if ((presence & (uint64_t{1U} << row)) == 0U) continue;
				const auto token = graph::storage::LittleEndianCodec::readU32(metadata, offset);
				if (index == expected.size() || expected[index++] != token)
					throw std::runtime_error("Property block presence disagrees with field membership");
			}
			if (index != expected.size()) throw std::runtime_error("Property block omitted a field membership entry");
		}
	}

	// Sparse columns use physical owner slots, unlike the sorted rows of the
	// earlier typed block. Empty columns must be retired from the group directory.
	void validateSparseColumn(const PropertyBlockOwnerSlots &slots, uint32_t token,
		std::span<const std::byte> bytes, SnapshotIdentity identity) const {
		if (identity != identity_ || slots.blockId() != block_id_ || slots.ownerType() != owner_type_)
			throw std::runtime_error("Sparse column membership identity mismatch");
		if (token == 0U || slots.activeOwnerCount() != members_.size())
			throw std::runtime_error("Sparse column membership owner set mismatch");
		SparseFixedWidthPropertyColumn::validate(bytes);
		if (graph::storage::LittleEndianCodec::readU32(bytes, 8U) != PropertyBlockOwnerSlots::CAPACITY)
			throw std::runtime_error("Sparse column must address the complete owner slot range");
		uint64_t expected = 0U;
		for (const auto &member : members_) {
			const auto keys = fields(slots, member.route, identity);
			if (std::binary_search(keys.begin(), keys.end(), token)) expected |= uint64_t{1U} << member.route.slot;
		}
		if (expected == 0U || graph::storage::LittleEndianCodec::readU64(bytes, 16U) != expected)
			throw std::runtime_error("Sparse column presence disagrees with field membership");
	}

	struct SparseColumnView {
		uint32_t token;
		std::span<const std::byte> bytes;
	};

	// Complete group coverage, including owners with no actual fields. Views
	// are borrowed synchronously and do not grant a persistent validation proof.
	void validateSparseColumns(const PropertyBlockOwnerSlots &slots,
		std::span<const SparseColumnView> columns, SnapshotIdentity identity) const {
		if (identity != identity_ || slots.blockId() != block_id_ || slots.ownerType() != owner_type_ ||
			slots.activeOwnerCount() != members_.size())
			throw std::runtime_error("Sparse group membership identity mismatch");
		if (columns.size() > tokens_.size()) throw std::runtime_error("Sparse group column bound exceeded");
		for (const auto &member : members_) static_cast<void>(fields(slots, member.route, identity));
		uint32_t previous = 0U;
		for (const auto &column : columns) {
			if (column.token <= previous) throw std::runtime_error("Sparse group columns must have ordered unique tokens");
			previous = column.token;
			validateSparseColumn(slots, column.token, column.bytes, identity);
		}
		for (const auto token : tokens_) {
			const auto found = std::lower_bound(columns.begin(), columns.end(), token,
				[](const auto &column, uint32_t candidate) { return column.token < candidate; });
			if (found == columns.end() || found->token != token)
				throw std::runtime_error("Sparse group omitted a field column");
		}
	}

	static constexpr size_t WIRE_HEADER = 48U, WIRE_MEMBER = 32U;
	static constexpr uint32_t WIRE_MAGIC = 0x44464D5AU;

	std::vector<std::byte> encode(size_t maximumBytes) const {
		if (tokens_.size() > UINT32_MAX) throw std::runtime_error("Field directory token count exceeds wire bounds");
		const size_t base = WIRE_HEADER + members_.size() * WIRE_MEMBER;
		if (base > maximumBytes || tokens_.size() > (maximumBytes - base) / 4U)
			throw std::runtime_error("Field directory encoded byte bound exceeded");
		std::vector<std::byte> bytes(base + tokens_.size() * 4U);
		LittleEndianCodec::writeU32(bytes, 0U, WIRE_MAGIC);
		LittleEndianCodec::writeU16(bytes, 4U, 1U);
		LittleEndianCodec::writeU16(bytes, 6U, PropertyBlockOwnerSlots::CAPACITY);
		LittleEndianCodec::writeU64(bytes, 8U, identity_.database_id);
		LittleEndianCodec::writeU64(bytes, 16U, identity_.generation);
		LittleEndianCodec::writeU64(bytes, 24U, block_id_);
		LittleEndianCodec::writeU32(bytes, 32U, owner_type_);
		LittleEndianCodec::writeU32(bytes, 36U, static_cast<uint32_t>(members_.size()));
		LittleEndianCodec::writeU32(bytes, 40U, static_cast<uint32_t>(tokens_.size()));
		for (size_t index = 0U; index < members_.size(); ++index) {
			const auto &member = members_[index];
			const size_t offset = WIRE_HEADER + index * WIRE_MEMBER;
			LittleEndianCodec::writeU64(bytes, offset, member.route.owner);
			LittleEndianCodec::writeU64(bytes, offset + 8U, member.route.generation);
			LittleEndianCodec::writeU32(bytes, offset + 16U, member.route.slot);
			LittleEndianCodec::writeU32(bytes, offset + 20U, static_cast<uint32_t>(member.begin));
			LittleEndianCodec::writeU32(bytes, offset + 24U, static_cast<uint32_t>(member.count));
		}
		for (size_t index = 0U; index < tokens_.size(); ++index) LittleEndianCodec::writeU32(bytes, base + index * 4U, tokens_[index]);
		LittleEndianCodec::writeU32(bytes, 44U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		return bytes;
	}

	static PropertyBlockFieldMembership decode(std::span<const std::byte> bytes,
		const PropertyBlockOwnerSlots &slots, SnapshotIdentity identity, size_t maximumFields, size_t maximumBytes) {
		if (maximumFields == 0U || bytes.size() < WIRE_HEADER || bytes.size() > maximumBytes)
			throw std::runtime_error("Invalid field directory byte or field bound");
		if (LittleEndianCodec::readU32(bytes, 0U) != WIRE_MAGIC || LittleEndianCodec::readU16(bytes, 4U) != 1U ||
			LittleEndianCodec::readU16(bytes, 6U) != PropertyBlockOwnerSlots::CAPACITY ||
			identity.database_id == 0U || identity.generation == 0U ||
			LittleEndianCodec::readU64(bytes, 8U) != identity.database_id || LittleEndianCodec::readU64(bytes, 16U) != identity.generation ||
			LittleEndianCodec::readU64(bytes, 24U) != slots.blockId() || LittleEndianCodec::readU32(bytes, 32U) != slots.ownerType())
			throw std::runtime_error("Invalid field directory identity");
		const size_t members = LittleEndianCodec::readU32(bytes, 36U), tokens = LittleEndianCodec::readU32(bytes, 40U);
		if (members > PropertyBlockOwnerSlots::CAPACITY || members != slots.activeOwnerCount() || tokens > maximumFields)
			throw std::runtime_error("Invalid field directory counts");
		const size_t base = WIRE_HEADER + members * WIRE_MEMBER;
		if (bytes.size() < base || tokens != (bytes.size() - base) / 4U || (bytes.size() - base) % 4U != 0U)
			throw std::runtime_error("Invalid field directory length");
		auto checked = std::vector<std::byte>(bytes.begin(), bytes.end());
		LittleEndianCodec::writeU32(checked, 44U, 0U);
		if (graph::utils::calculateCrc(checked.data(), checked.size()) != LittleEndianCodec::readU32(bytes, 44U))
			throw std::runtime_error("Invalid field directory checksum");
		PropertyBlockFieldMembership result(identity, slots.blockId(), slots.ownerType());
		result.members_.reserve(members); result.tokens_.reserve(tokens);
		for (size_t index = 0U; index < tokens; ++index) result.tokens_.push_back(LittleEndianCodec::readU32(bytes, base + index * 4U));
		std::vector<std::pair<size_t, size_t>> ranges;
		for (size_t index = 0U; index < members; ++index) {
			const size_t offset = WIRE_HEADER + index * WIRE_MEMBER;
			PropertyBlockOwnerSlots::Route route{slots.blockId(), slots.ownerType(), LittleEndianCodec::readU32(bytes, offset + 16U),
				LittleEndianCodec::readU64(bytes, offset + 8U), LittleEndianCodec::readU64(bytes, offset)};
			static_cast<void>(slots.resolve(route));
			if ((index != 0U && result.members_.back().route.slot >= route.slot) || LittleEndianCodec::readU32(bytes, offset + 28U) != 0U)
				throw std::runtime_error("Invalid field directory member order");
			const size_t begin = LittleEndianCodec::readU32(bytes, offset + 20U), count = LittleEndianCodec::readU32(bytes, offset + 24U);
			if (begin > tokens || count > tokens - begin) throw std::runtime_error("Invalid field directory slice");
			uint32_t previous = 0U;
			for (size_t token = begin; token < begin + count; ++token) {
				if (result.tokens_[token] <= previous) throw std::runtime_error("Invalid field directory token order");
				previous = result.tokens_[token];
			}
			result.members_.push_back({route, begin, count});
			if (count != 0U) ranges.emplace_back(begin, begin + count);
		}
		std::ranges::sort(ranges);
		size_t end = 0U;
		for (const auto &range : ranges) {
			if (range.first != end) throw std::runtime_error("Field directory slices overlap or omit tokens");
			end = range.second;
		}
		if (end != tokens) throw std::runtime_error("Field directory omitted token storage");
		return result;
	}

	// In-memory candidate footprint, not encoded bytes or allocator/RSS usage.
	size_t metadataCapacityBytes() const {
		return sizeof(*this) + members_.capacity() * sizeof(Member) + tokens_.capacity() * sizeof(uint32_t);
	}

private:
	static SnapshotIdentity descriptorIdentity(const graph::storage::MetadataSnapshotPin &snapshot, uint64_t generation) {
		const auto identity = snapshotIdentity(snapshot);
		if (generation == 0U || generation > identity.generation)
			throw std::runtime_error("Field directory generation is outside the selected snapshot");
		return {identity.database_id, generation};
	}
	static SnapshotIdentity snapshotIdentity(const graph::storage::MetadataSnapshotPin &snapshot) {
		const auto &superblock = snapshot.superblock();
		return {superblock.database_id.value(), superblock.generation.value()};
	}
	explicit PropertyBlockFieldMembership(SnapshotIdentity identity, uint64_t block, uint32_t type)
		: identity_(identity), block_id_(block), owner_type_(type) {}
	SnapshotIdentity identity_;
	uint64_t block_id_;
	uint32_t owner_type_;
	struct Member {
		PropertyBlockOwnerSlots::Route route;
		size_t begin;
		size_t count;
	};
	std::vector<Member> members_;
	std::vector<uint32_t> tokens_;
};

} // namespace zyx::benchmark
