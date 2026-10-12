#pragma once

#include "PackedPropertyColumnPage.hpp"
#include "PropertyBlockFieldMembership.hpp"

namespace zyx::benchmark {

// Codec-cost selection only; production routing and migration remain external.
class PropertyLayoutCandidate final {
public:
	static constexpr size_t CONTAINER_HEADER_BYTES = 48U, CONTAINER_SECTION_BYTES = 8U;
	enum class Layout { PLC_OWNER_ROOTS, PLC_PACKED_COLUMNS };
	struct Image {
		PropertyBlockFieldMembership::SnapshotIdentity identity{};
		Layout layout = Layout::PLC_OWNER_ROOTS;
		size_t bytes = 0U;
		std::vector<std::vector<std::byte>> owners;
		std::vector<std::vector<std::byte>> pages;
		std::vector<std::byte> slots, membership;
	};
	class VerifiedImage final {
	public:
		// Encoded payload only; parsed routes, indices and allocator overhead are separate.
		[[nodiscard]] size_t retainedEncodedBytes() const {
			size_t bytes = image_->slots.size() + image_->membership.size();
			for (const auto &root : image_->owners) bytes += root.size();
			for (const auto &page : image_->pages) bytes += page.size();
			for (const auto &[token, column] : columns_) { static_cast<void>(token); bytes += column.encodedBytes(); }
			return bytes;
		}
		PropertyCollectionRoot readOwner(uint64_t owner, PropertyBlockFieldMembership::SnapshotIdentity identity,
		    const CancellationCheckpoint *checkpoint = nullptr) const {
			if (checkpoint) checkpoint->check();
			if (identity != image_->identity) throw std::runtime_error("Verified candidate snapshot mismatch");
			const auto route = slots_.route(owner);
			if (image_->layout == Layout::PLC_OWNER_ROOTS) {
				auto result = EntityPropertyCollectionRootCodec::decode(image_->owners.at(ownerIndex_.at(owner)));
				if (checkpoint) checkpoint->check();
				return result;
			}
			PropertyCollectionRoot result{owner, slots_.ownerType(), {}};
			for (const auto token : directory_->fields(slots_, route, identity)) {
				if (checkpoint) checkpoint->check();
				const auto value = columns_.at(token).read(route.slot);
				if (!value) throw std::runtime_error("Verified candidate field missing");
				result.entries.push_back({token, *value});
			}
			if (checkpoint) checkpoint->check();
			return result;
		}
	private:
		friend class PropertyLayoutCandidate;
		VerifiedImage(std::shared_ptr<const Image> image, const PropertyBlockOwnerSlots &slots)
		    : image_(std::move(image)), slots_(slots) {}
		std::shared_ptr<const Image> image_;
		PropertyBlockOwnerSlots slots_;
		std::optional<PropertyBlockFieldMembership> directory_;
		std::map<uint64_t, size_t> ownerIndex_;
		std::map<uint32_t, CompactFixedPropertyColumn::VerifiedColumn> columns_;
	};
	static VerifiedImage freeze(Image image, const PropertyBlockOwnerSlots &slots,
	    PropertyBlockFieldMembership::SnapshotIdentity identity, size_t maximumFields, size_t maximumBytes,
	    const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		const auto owners = slots.activeOwnersSorted();
		if (owners.empty()) throw std::invalid_argument("Verified property candidate requires owners");
		static_cast<void>(readOwner(image, slots, owners.front(), identity, maximumFields, maximumBytes, checkpoint));
		// The only mutable alias remains local until all construction succeeds.
		auto storage = std::make_shared<Image>(std::move(image));
		VerifiedImage verified(storage, slots);
		if (verified.image_->layout == Layout::PLC_OWNER_ROOTS) {
			for (size_t index = 0U; index < verified.image_->owners.size(); ++index) {
				if (checkpoint) checkpoint->check();
				const auto root = EntityPropertyCollectionRootCodec::decode(verified.image_->owners[index]);
				verified.ownerIndex_.emplace(root.owner_entity_id, index);
			}
		} else {
			verified.directory_ = PropertyBlockFieldMembership::decodeAdaptive(verified.image_->membership,
			    slots, identity, maximumFields, maximumBytes);
			for (size_t index = 0U; index < verified.image_->pages.size(); ++index)
				for (const auto &column : PackedPropertyColumnPage::decode(verified.image_->pages[index], maximumBytes, checkpoint))
					verified.columns_.emplace(column.token, CompactFixedPropertyColumn::freeze(column.image, checkpoint));
			std::vector<std::vector<std::byte>>{}.swap(storage->pages);
			std::vector<std::byte>{}.swap(storage->slots);
			std::vector<std::byte>{}.swap(storage->membership);
		}
		if (checkpoint) checkpoint->check();
		return verified;
	}
	static PropertyCollectionRoot readOwner(const Image &image, const PropertyBlockOwnerSlots &slots,
	    uint64_t owner, PropertyBlockFieldMembership::SnapshotIdentity identity, size_t maximumFields,
	    size_t maximumBytes, const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (maximumBytes == 0U) throw std::invalid_argument("Property candidate read requires positive byte bound");
		size_t inputBytes = 0U;
		const auto admitInput = [&](size_t bytes) {
			if (bytes > maximumBytes - inputBytes) throw std::length_error("Property candidate total input byte bound exceeded");
			inputBytes += bytes;
		};
		admitInput(image.slots.size()); admitInput(image.membership.size());
		for (const auto &bytes : image.owners) { if (checkpoint) checkpoint->check(); admitInput(bytes.size()); }
		for (const auto &bytes : image.pages) { if (checkpoint) checkpoint->check(); admitInput(bytes.size()); }
		if (image.identity != identity) throw std::runtime_error("Property candidate snapshot identity mismatch");
		const auto route = slots.route(owner);
		if (image.layout == Layout::PLC_OWNER_ROOTS) {
			if (!image.pages.empty() || !image.slots.empty() || !image.membership.empty())
				throw std::runtime_error("Property candidate has competing payload layouts");
			std::vector<PropertyCollectionRoot> roots;
			size_t fieldCount = 0U;
			for (const auto &bytes : image.owners) {
				if (checkpoint) checkpoint->check();
				const SelectedPropertyReadAdmission fieldsAdmission = [&](size_t tokenBytes) {
					if (tokenBytes / sizeof(uint32_t) > maximumFields - fieldCount)
						throw std::length_error("Property candidate total field bound exceeded");
				};
				const auto tokens = EntityPropertyCollectionRootCodec::decodeTokens(bytes, &fieldsAdmission, checkpoint);
				fieldCount += tokens.size();
				roots.push_back(EntityPropertyCollectionRootCodec::decode(bytes));
			}
			static_cast<void>(PropertyBlockFieldMembership::build(slots, roots, maximumFields, identity));
			for (const auto &root : roots) if (root.owner_entity_id == owner) return root;
			throw std::runtime_error("Property candidate owner missing");
		}
		if (image.layout != Layout::PLC_PACKED_COLUMNS || !image.owners.empty() || image.slots != slots.encode())
			throw std::runtime_error("Property candidate layout or owner table mismatch");
		const auto directory = PropertyBlockFieldMembership::decodeAdaptive(image.membership, slots, identity, maximumFields, maximumBytes);
		std::map<uint32_t, std::vector<std::optional<PropertyValue>>> columns;
		for (const auto &page : image.pages) for (const auto &column : PackedPropertyColumnPage::decode(page, maximumBytes, checkpoint)) {
			if (columns.contains(column.token)) throw std::runtime_error("Duplicate property candidate column");
			if (column.kind != PackedPropertyColumnPage::Kind::PCP_COMPACT_FIXED)
				throw std::runtime_error("Unexpected property candidate column kind");
			columns.emplace(column.token, CompactFixedPropertyColumn::readAll(column.image, checkpoint));
		}
		std::set<uint32_t> expectedTokens;
		uint64_t activeSlots = 0U;
		for (const auto activeOwner : slots.activeOwnersSorted()) {
			if (checkpoint) checkpoint->check();
			const auto activeRoute = slots.route(activeOwner);
			activeSlots |= uint64_t{1U} << activeRoute.slot;
			const auto fields = directory.fields(slots, activeRoute, identity);
			for (const auto token : fields) expectedTokens.insert(token);
			for (const auto &[token, values] : columns) {
				if (checkpoint) checkpoint->check();
				if (values.size() != 64U || values[activeRoute.slot].has_value() != std::binary_search(fields.begin(), fields.end(), token))
					throw std::runtime_error("Property candidate membership and presence disagree");
			}
		}
		for (const auto &[token, values] : columns) {
			if (checkpoint) checkpoint->check();
			static_cast<void>(token);
			for (size_t slot = 0U; slot < values.size(); ++slot)
				if ((activeSlots & (uint64_t{1U} << slot)) == 0U && values[slot])
					throw std::runtime_error("Property candidate inactive slot contains a value");
		}
		if (expectedTokens.size() != columns.size()) throw std::runtime_error("Property candidate column coverage mismatch");
		for (const auto token : expectedTokens) if (!columns.contains(token)) throw std::runtime_error("Property candidate column missing");
		PropertyCollectionRoot result{owner, slots.ownerType(), {}};
		for (const auto token : directory.fields(slots, route, identity)) result.entries.push_back({token, *columns.at(token)[route.slot]});
		if (checkpoint) checkpoint->check();
		return result;
	}
	static Image build(const PropertyBlockOwnerSlots &slots, std::span<const PropertyCollectionRoot> roots,
	    PropertyBlockFieldMembership::SnapshotIdentity identity, size_t maximumFields, size_t maximumBytes,
	    size_t pageBytes, size_t maximumPages, size_t additionalCandidateMetadataBytes,
	    const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (maximumBytes == 0U || pageBytes < PackedPropertyColumnPage::HEADER + PackedPropertyColumnPage::ENTRY ||
		    pageBytes > UINT32_MAX || maximumPages == 0U)
			throw std::invalid_argument("Invalid property layout candidate bounds");
		const auto directory = PropertyBlockFieldMembership::build(slots, roots, maximumFields, identity);
		if (checkpoint) checkpoint->check();
		Image fallback;
		fallback.identity = identity;
		for (const auto &root : roots) {
			if (checkpoint) checkpoint->check();
			auto encoded = EntityPropertyCollectionRootCodec::encode(root);
			if (encoded.size() > maximumBytes - fallback.bytes) throw std::length_error("Owner fallback byte bound exceeded");
			fallback.bytes += encoded.size(); fallback.owners.push_back(std::move(encoded));
		}
		Image candidate;
		candidate.identity = identity;
		candidate.layout = Layout::PLC_PACKED_COLUMNS;
		candidate.bytes = additionalCandidateMetadataBytes;
		const auto admit = [&](size_t bytes) {
			if (candidate.bytes > maximumBytes || bytes > maximumBytes - candidate.bytes) return false;
			candidate.bytes += bytes; return true;
		};
		if (!admit(CONTAINER_HEADER_BYTES + 2U * CONTAINER_SECTION_BYTES)) return fallback;
		candidate.slots = slots.encode();
		if (checkpoint) checkpoint->check();
		if (!admit(candidate.slots.size()) || candidate.bytes >= fallback.bytes) return fallback;
		try { candidate.membership = directory.encodeAdaptive(slots, identity, maximumBytes - candidate.bytes); }
		catch (const std::length_error &) { return fallback; }
		if (checkpoint) checkpoint->check();
		if (!admit(candidate.membership.size()) || candidate.bytes >= fallback.bytes) return fallback;
		std::map<uint32_t, std::vector<std::optional<PropertyValue>>> columns;
		for (const auto &root : roots) {
			if (checkpoint) checkpoint->check();
			// A single owner exceeding the column limit cannot fit this candidate.
			if (root.entries.size() > 4096U) return fallback;
			for (const auto &entry : root.entries) {
				if (checkpoint) checkpoint->check();
				if (!columns.contains(entry.key_token) && columns.size() == 4096U) return fallback;
				auto [column, inserted] = columns.try_emplace(entry.key_token, 64U);
				column->second[slots.route(root.owner_entity_id).slot] = entry.value;
			}
		}
		std::vector<std::vector<std::byte>> payloads;
		for (const auto &[token, values] : columns) {
			if (checkpoint) checkpoint->check();
			static_cast<void>(token);
			if (!SparseFixedWidthPropertyColumn::canEncode(values)) return fallback;
			auto payload = CompactFixedPropertyColumn::encode(values);
			if (payload.size() > pageBytes - PackedPropertyColumnPage::HEADER - PackedPropertyColumnPage::ENTRY) return fallback;
			payloads.push_back(std::move(payload));
		}
		std::vector<PackedPropertyColumnPage::Column> views;
		size_t index = 0U;
		for (const auto &[token, values] : columns) {
			static_cast<void>(values);
			views.push_back({token, payloads[index++], PackedPropertyColumnPage::Kind::PCP_COMPACT_FIXED});
		}
		// Admit page count explicitly so callback exceptions are never swallowed.
		size_t requiredPages = views.empty() ? 0U : 1U, used = PackedPropertyColumnPage::HEADER;
		for (const auto &view : views) {
			if (checkpoint) checkpoint->check();
			const size_t bytes = PackedPropertyColumnPage::ENTRY + view.image.size();
			if (bytes > pageBytes - used) { ++requiredPages; used = PackedPropertyColumnPage::HEADER; }
			if (requiredPages > maximumPages) return fallback;
			used += bytes;
		}
		candidate.pages = PackedPropertyColumnPage::pack(views, pageBytes, maximumPages, checkpoint);
		for (const auto &page : candidate.pages)
			if (!admit(CONTAINER_SECTION_BYTES) || !admit(page.size())) return fallback;
		if (candidate.bytes >= fallback.bytes) return fallback;
		if (checkpoint) checkpoint->check();
		return candidate;
	}
};

} // namespace zyx::benchmark
