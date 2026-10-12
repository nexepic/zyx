#pragma once

#include "SparseFixedWidthPropertyColumn.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"

namespace zyx::benchmark {

// Laboratory representation only; authoritative routing remains external.
class AdaptivePropertyColumn final {
public:
	enum class Layout { APC_FIXED, APC_CANONICAL };
	static constexpr uint32_t WIRE_MAGIC = 0x4341505AU;
	static constexpr size_t WIRE_HEADER = 32U;

	[[nodiscard]] std::vector<std::byte> encodeImage(size_t maximumBytes) const {
		const size_t payload = layout_ == Layout::APC_FIXED ? fixed_.size() :
		    bytes_ - sizeof(uint64_t) + canonical_.size() * sizeof(uint32_t);
		if (maximumBytes < WIRE_HEADER || payload > maximumBytes - WIRE_HEADER || payload > UINT32_MAX)
			throw std::length_error("Adaptive image byte bound exceeded");
		std::vector<std::byte> image(WIRE_HEADER + payload);
		LittleEndianCodec::writeU32(image, 0U, WIRE_MAGIC);
		LittleEndianCodec::writeU16(image, 4U, 1U);
		LittleEndianCodec::writeU16(image, 6U, layout_ == Layout::APC_FIXED ? 1U : 2U);
		LittleEndianCodec::writeU32(image, 8U, static_cast<uint32_t>(rows_));
		LittleEndianCodec::writeU32(image, 12U, static_cast<uint32_t>(payload));
		LittleEndianCodec::writeU64(image, 16U, presence_);
		size_t offset = WIRE_HEADER;
		if (layout_ == Layout::APC_FIXED) std::copy(fixed_.begin(), fixed_.end(), image.begin() + offset);
		else for (const auto &root : canonical_) {
			LittleEndianCodec::writeU32(image, offset, static_cast<uint32_t>(root->size()));
			offset += sizeof(uint32_t);
			std::copy(root->begin(), root->end(), image.begin() + offset);
			offset += root->size();
		}
		LittleEndianCodec::writeU32(image, 24U, graph::utils::calculateCrc(image.data(), image.size()));
		return image;
	}

	static AdaptivePropertyColumn decodeImage(std::span<const std::byte> image, size_t maximumBytes,
	                                         const CancellationCheckpoint *checkpoint = nullptr) {
		if (checkpoint) checkpoint->check();
		if (image.size() > maximumBytes || image.size() < WIRE_HEADER ||
		    LittleEndianCodec::readU32(image, 0U) != WIRE_MAGIC || LittleEndianCodec::readU16(image, 4U) != 1U ||
		    LittleEndianCodec::readU32(image, 28U) != 0U ||
		    LittleEndianCodec::readU32(image, 12U) != image.size() - WIRE_HEADER)
			throw std::runtime_error("Invalid adaptive image header or bounds");
		const auto rows = LittleEndianCodec::readU32(image, 8U);
		const auto layout = LittleEndianCodec::readU16(image, 6U);
		const auto presence = LittleEndianCodec::readU64(image, 16U);
		if (rows > 64U || (layout != 1U && layout != 2U) ||
		    (rows < 64U && (presence >> rows) != 0U) || (layout == 1U && presence != 0U))
			throw std::runtime_error("Invalid adaptive image shape");
		// Substitute the checksum field without copying the complete image.
		const std::array<std::byte, 4U> zero{};
		uint32_t crc = graph::utils::calculateCrc(image.data(), 24U);
		crc = graph::utils::updateCrc(crc, zero.data(), zero.size());
		for (size_t offset = 28U; offset < image.size();) {
			if (checkpoint) checkpoint->check();
			const size_t length = std::min<size_t>(65536U, image.size() - offset);
			crc = graph::utils::updateCrc(crc, image.data() + offset, length);
			offset += length;
		}
		if (crc != LittleEndianCodec::readU32(image, 24U))
			throw std::runtime_error("Adaptive image checksum mismatch");
		std::vector<std::optional<PropertyValue>> values(rows);
		AdaptivePropertyColumn result;
		result.rows_ = rows;
		result.layout_ = layout == 1U ? Layout::APC_FIXED : Layout::APC_CANONICAL;
		result.presence_ = presence;
		if (layout == 1U) {
			const auto column = image.subspan(WIRE_HEADER);
			values = SparseFixedWidthPropertyColumn::readAll(column, checkpoint);
			if (LittleEndianCodec::readU32(column, 8U) != rows)
				throw std::runtime_error("Adaptive image row mismatch");
			result.fixed_.assign(column.begin(), column.end());
			result.bytes_ = column.size();
		} else {
			result.bytes_ = sizeof(uint64_t);
			result.canonical_.reserve(static_cast<size_t>(std::popcount(presence)));
			size_t offset = WIRE_HEADER;
			for (size_t row = 0U; row < rows; ++row) {
				if (checkpoint) checkpoint->check();
				if ((presence & (uint64_t{1U} << row)) == 0U) continue;
				if (image.size() - offset < sizeof(uint32_t)) throw std::runtime_error("Truncated adaptive root length");
				const auto length = LittleEndianCodec::readU32(image, offset);
				offset += sizeof(uint32_t);
				if (length > image.size() - offset) throw std::runtime_error("Truncated adaptive root");
				const auto root = EntityPropertyCollectionRootCodec::decode(image.subspan(offset, length));
				if (root.owner_entity_id != row + 1U || root.owner_entity_type != 1U || root.entries.size() != 1U || root.entries.front().key_token != 1U)
					throw std::runtime_error("Adaptive root identity mismatch");
				values[row] = root.entries.front().value;
				result.canonical_.push_back(std::make_shared<const std::vector<std::byte>>(
				    image.begin() + offset, image.begin() + offset + length));
				result.bytes_ += length;
				offset += length;
			}
			if (offset != image.size()) throw std::runtime_error("Trailing adaptive image bytes");
		}
		if (checkpoint) checkpoint->check();
		if (SparseFixedWidthPropertyColumn::canEncode(values) != (layout == 1U))
			throw std::runtime_error("Noncanonical adaptive layout selection");
		for (const auto &value : values) ++result.typeCounts_[typeIndex(value)];
		if (checkpoint) checkpoint->check();
		return result;
	}
	static AdaptivePropertyColumn encode(std::span<const std::optional<PropertyValue>> values,
	                                    size_t maximumBytes) {
		if (values.size() > 64U || maximumBytes == 0U)
			throw std::invalid_argument("Adaptive column requires bounded rows and bytes");
		AdaptivePropertyColumn result;
		result.rows_ = values.size();
		for (const auto &value : values) ++result.typeCounts_[typeIndex(value)];
		if (SparseFixedWidthPropertyColumn::canEncode(values)) {
			result.layout_ = Layout::APC_FIXED;
			result.fixed_ = SparseFixedWidthPropertyColumn::encode(values);
			result.bytes_ = result.fixed_.size();
		} else {
			result.layout_ = Layout::APC_CANONICAL;
			// Account for the presence mask; only present rows own canonical roots.
			result.bytes_ = sizeof(uint64_t);
			if (result.bytes_ > maximumBytes)
				throw std::length_error("Adaptive column encoded byte bound exceeded");
			for (size_t row = 0U; row < values.size(); ++row)
				if (values[row]) result.presence_ |= uint64_t{1U} << row;
			result.canonical_.reserve(static_cast<size_t>(std::popcount(result.presence_)));
			for (size_t row = 0U; row < values.size(); ++row) {
				if (!values[row]) continue;
				PropertyCollectionRoot root{row + 1U, 1U, {}};
				root.entries.push_back({1U, *values[row]});
				auto encoded = EntityPropertyCollectionRootCodec::encode(root);
				if (encoded.size() > maximumBytes - result.bytes_)
					throw std::length_error("Adaptive column encoded byte bound exceeded");
				result.bytes_ += encoded.size();
				result.canonical_.push_back(std::make_shared<const std::vector<std::byte>>(std::move(encoded)));
			}
		}
		if (result.bytes_ > maximumBytes)
			throw std::length_error("Adaptive column encoded byte bound exceeded");
		return result;
	}

	[[nodiscard]] Layout layout() const { return layout_; }
	[[nodiscard]] size_t encodedBytes() const { return bytes_; }
	[[nodiscard]] std::vector<std::optional<PropertyValue>> readAll(const CancellationCheckpoint *checkpoint = nullptr) const {
		if (layout_ == Layout::APC_FIXED) return SparseFixedWidthPropertyColumn::readAll(fixed_, checkpoint);
		if (checkpoint) checkpoint->check();
		std::vector<std::optional<PropertyValue>> values;
		values.reserve(rows_);
		for (size_t row = 0U; row < rows_; ++row) {
			if (checkpoint) checkpoint->check();
			values.push_back(read(row));
		}
		if (checkpoint) checkpoint->check();
		return values;
	}
	[[nodiscard]] std::optional<PropertyValue> read(size_t row) const {
		if (row >= rows_) throw std::out_of_range("Adaptive column row exceeds bounds");
		if (layout_ == Layout::APC_FIXED) return SparseFixedWidthPropertyColumn::read(fixed_, row);
		const uint64_t bit = uint64_t{1U} << row;
		if ((presence_ & bit) == 0U) return std::nullopt;
		const size_t rank = static_cast<size_t>(std::popcount(presence_ & (bit - 1U)));
		const auto root = EntityPropertyCollectionRootCodec::decode(*canonical_.at(rank));
		if (root.owner_entity_id != row + 1U || root.owner_entity_type != 1U ||
		    root.entries.size() != 1U || root.entries.front().key_token != 1U)
			throw std::runtime_error("Adaptive canonical column identity mismatch");
		return root.entries.front().value;
	}

	[[nodiscard]] std::shared_ptr<const std::vector<std::byte>> canonicalRoot(size_t row) const {
		if (row >= rows_) throw std::out_of_range("Adaptive column row exceeds bounds");
		const uint64_t bit = uint64_t{1U} << row;
		if (layout_ != Layout::APC_CANONICAL || (presence_ & bit) == 0U) return {};
		return canonical_.at(static_cast<size_t>(std::popcount(presence_ & (bit - 1U))));
	}

	[[nodiscard]] AdaptivePropertyColumn replace(size_t row, std::optional<PropertyValue> value,
	                                             size_t maximumBytes) const {
		if (row >= rows_) throw std::out_of_range("Adaptive column row exceeds bounds");
		if (maximumBytes == 0U) throw std::invalid_argument("Adaptive column requires bounded rows and bytes");
		const auto previous = read(row);
		if (exactlyEqual(previous, value)) {
			if (bytes_ > maximumBytes) throw std::length_error("Adaptive column encoded byte bound exceeded");
			return *this;
		}
		auto nextCounts = typeCounts_;
		--nextCounts[typeIndex(previous)];
		++nextCounts[typeIndex(value)];
		const size_t primitiveKinds = static_cast<size_t>(nextCounts[1U] != 0U) +
		    static_cast<size_t>(nextCounts[2U] != 0U) + static_cast<size_t>(nextCounts[3U] != 0U);
		const bool fixedEligible = nextCounts[4U] == 0U && primitiveKinds <= 1U;
		if (layout_ == Layout::APC_CANONICAL && !fixedEligible) {
			if (maximumBytes == 0U) throw std::invalid_argument("Adaptive column requires bounded rows and bytes");
			AdaptivePropertyColumn result;
			result.layout_ = Layout::APC_CANONICAL;
			result.rows_ = rows_;
			result.typeCounts_ = nextCounts;
			result.bytes_ = sizeof(uint64_t);
			if (result.bytes_ > maximumBytes)
				throw std::length_error("Adaptive column encoded byte bound exceeded");
			for (size_t index = 0U; index < rows_; ++index) {
				if (index == row ? !value.has_value() : (presence_ & (uint64_t{1U} << index)) == 0U) continue;
				auto root = canonicalRoot(index);
				if (index == row) {
					PropertyCollectionRoot replacement{index + 1U, 1U, {{1U, *value}}};
					root = std::make_shared<const std::vector<std::byte>>(EntityPropertyCollectionRootCodec::encode(replacement));
				}
				if (!root || root->size() > maximumBytes - result.bytes_)
					throw std::length_error("Adaptive column encoded byte bound exceeded");
				result.bytes_ += root->size();
				result.presence_ |= uint64_t{1U} << index;
				result.canonical_.push_back(std::move(root));
			}
			return result;
		}
		std::vector<std::optional<PropertyValue>> values;
		values.reserve(rows_);
		for (size_t index = 0U; index < rows_; ++index) values.push_back(index == row ? value : read(index));
		return encode(values, maximumBytes);
	}

private:
	AdaptivePropertyColumn() = default;
	static bool exactlyEqual(const std::optional<PropertyValue> &left,
	                         const std::optional<PropertyValue> &right) {
		if (!left || !right) return left.has_value() == right.has_value();
		if (left->getVariant().index() != right->getVariant().index()) return false;
		// Canonical bytes preserve floating-point bits and nested value types.
		std::vector<std::byte> leftBytes, rightBytes;
		CanonicalPropertyValueCodec::append(leftBytes, *left);
		CanonicalPropertyValueCodec::append(rightBytes, *right);
		return leftBytes == rightBytes;
	}
	// Missing and NULL do not constrain primitive column eligibility.
	static size_t typeIndex(const std::optional<PropertyValue> &value) {
		if (!value || std::holds_alternative<std::monostate>(value->getVariant())) return 0U;
		if (std::holds_alternative<int64_t>(value->getVariant())) return 1U;
		if (std::holds_alternative<double>(value->getVariant())) return 2U;
		if (std::holds_alternative<bool>(value->getVariant())) return 3U;
		return 4U;
	}
	std::array<size_t, 5U> typeCounts_{};
	Layout layout_ = Layout::APC_FIXED;
	size_t rows_ = 0U;
	size_t bytes_ = 0U;
	uint64_t presence_ = 0U;
	std::vector<std::byte> fixed_;
	std::vector<std::shared_ptr<const std::vector<std::byte>>> canonical_;
};

} // namespace zyx::benchmark
