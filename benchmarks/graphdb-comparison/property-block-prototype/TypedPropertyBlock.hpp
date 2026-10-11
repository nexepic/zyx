#pragma once
// Experimental authoritative local property layout. Not linked into production.
#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace zyx::benchmark {
using graph::PropertyValue;
using namespace graph::storage;

class TypedPropertyBlock final {
public:
 enum class ColumnKind : uint32_t { COL_CANONICAL = 1, COL_INTEGER = 2, COL_DOUBLE = 3, COL_BOOLEAN = 4 };
 struct Image {
  std::vector<std::byte> metadata;
  std::vector<std::shared_ptr<const std::vector<std::byte>>> columns;
 };
 class VerifiedImage final {
 public:
  [[nodiscard]] const Image &image() const { return *image_; }
 private:
  friend class TypedPropertyBlock;
  VerifiedImage(Image image, uint64_t block, uint32_t type) : image_(std::make_shared<const Image>(std::move(image))), block_(block), type_(type) {}
  std::shared_ptr<const Image> image_;
  uint64_t block_;
  uint32_t type_;
 };
 static constexpr size_t MAX_ROWS = 64, HEADER = 32, DESCRIPTOR = 40;
 static constexpr uint32_t MAGIC = 0x4250545aU;

 // Explicit empty-block version retains block/type identity after last-owner deletion.
 // Version1 remains nonempty; version2 is reserved strictly for a zero-row/zero-column image.
 static Image encodeEmpty(uint64_t blockId,uint32_t type) {
  require(blockId!=0,"invalid empty property block identity");Image image;image.metadata.resize(HEADER);
  put32(image.metadata,0,MAGIC);put16(image.metadata,4,2);put32(image.metadata,12,type);put64(image.metadata,16,blockId);
  put32(image.metadata,24,crc(image.metadata));return image;
 }
 static Image encode(uint64_t blockId, std::span<const PropertyCollectionRoot> owners) {
  require(blockId != 0 && !owners.empty() && owners.size() <= MAX_ROWS, "invalid property block identity/rows");
  std::map<uint32_t, std::vector<std::optional<PropertyValue>>> columns;
  uint64_t previousOwner = 0;
  for (size_t row = 0; row < owners.size(); ++row) {
   const auto &owner = owners[row];
   require(owner.owner_entity_id > previousOwner && owner.owner_entity_type == owners[0].owner_entity_type,
           "invalid property block owner mapping");
   previousOwner = owner.owner_entity_id;
   uint32_t previousToken = 0;
   for (const auto &entry : owner.entries) {
    require(entry.key_token > previousToken, "invalid property block token order");
    previousToken = entry.key_token;
    require(columns.contains(entry.key_token) || columns.size()<4096,"property block column bound exceeded");
    auto [it, inserted] = columns.try_emplace(entry.key_token, owners.size());
    it->second[row] = entry.value;
   }
  }
  require(columns.size() <= 4096, "property block column bound exceeded");
  Image image;
  image.metadata.resize(HEADER + owners.size() * 8 + columns.size() * DESCRIPTOR);
  put32(image.metadata, 0, MAGIC); put16(image.metadata, 4, 1); put16(image.metadata, 6, owners.size());
  put32(image.metadata, 8, columns.size()); put32(image.metadata, 12, owners[0].owner_entity_type);
  put64(image.metadata, 16, blockId);
  for (size_t row = 0; row < owners.size(); ++row) put64(image.metadata, HEADER + row * 8, owners[row].owner_entity_id);
  size_t column = 0;
  for (const auto &[token, values] : columns) {
   uint64_t presence = 0, nulls = 0;
   std::optional<ColumnKind> kind;
   bool mixed = false;
   for (size_t row = 0; row < values.size(); ++row) {
    if (!values[row]) continue;
    presence |= uint64_t{1} << row;
    if (std::holds_alternative<std::monostate>(values[row]->getVariant())) { nulls |= uint64_t{1} << row; continue; }
    const auto next = classify(*values[row]);
    if (kind && *kind != next) mixed = true;
    else if (!kind) kind = next;
   }
   const auto storageKind = mixed ? ColumnKind::COL_CANONICAL : kind.value_or(ColumnKind::COL_CANONICAL);
   std::vector<std::byte> payload;
   if (storageKind == ColumnKind::COL_CANONICAL) {
    payload.resize((values.size() + 1) * 4);
    for (size_t row = 0; row < values.size(); ++row) {
     require(payload.size() <= UINT32_MAX, "property block payload too large");
     put32(payload, row * 4, payload.size());
     if (values[row] && !(nulls & (uint64_t{1} << row))) CanonicalPropertyValueCodec::append(payload, *values[row]);
    }
    require(payload.size() <= UINT32_MAX, "property block payload too large");
    put32(payload, values.size() * 4, payload.size());
   } else {
    const size_t stride = storageKind == ColumnKind::COL_BOOLEAN ? 1 : 8;
    payload.resize(values.size() * stride);
    for (size_t row = 0; row < values.size(); ++row) {
     if (!values[row] || (nulls & (uint64_t{1} << row))) continue;
     const auto &value = values[row]->getVariant();
     if (storageKind == ColumnKind::COL_BOOLEAN) payload[row] = std::byte(std::get<bool>(value));
     else put64(payload, row * 8, storageKind == ColumnKind::COL_INTEGER ?
                std::bit_cast<uint64_t>(std::get<int64_t>(value)) : std::bit_cast<uint64_t>(std::get<double>(value)));
    }
   }
   const size_t offset = HEADER + owners.size() * 8 + column * DESCRIPTOR;
   put32(image.metadata, offset, token); put32(image.metadata, offset + 4, static_cast<uint32_t>(storageKind));
   put64(image.metadata, offset + 8, presence); put64(image.metadata, offset + 16, nulls);
   put64(image.metadata, offset + 24, payload.size()); put32(image.metadata, offset + 32, crc(payload));
   image.columns.push_back(std::make_shared<const std::vector<std::byte>>(std::move(payload))); ++column;
  }
  put32(image.metadata, 24, crc(image.metadata));
  return image;
 }

 // Selected columns authenticate the complete owner map and selected payloads.
 // Untouched payloads are not read; this prototype is not a new integrity protocol.
 static std::vector<PropertyCollectionRoot> decode(const Image &image, uint64_t expectedBlock,
                                                   uint32_t expectedType, std::span<const uint32_t> tokens) {
  const auto &m = image.metadata;
  require(m.size() >= HEADER && get32(m, 0) == MAGIC && (get16(m, 4) == 1 || get16(m, 4) == 2) && get32(m, 28) == 0,
          "invalid property block header");
  require(expectedBlock != 0 && get64(m, 16) == expectedBlock && get32(m, 12) == expectedType,
          "property block owner mismatch");
  const size_t rows = get16(m, 6), count = get32(m, 8);
  require(((get16(m,4)==1 && rows>0) || (get16(m,4)==2 && rows==0 && count==0)) && rows <= MAX_ROWS && count <= 4096 && image.columns.size() == count &&
          m.size() == HEADER + rows * 8 + count * DESCRIPTOR, "invalid property block sizes");
  auto authenticated = m; const auto checksum = get32(m, 24); put32(authenticated, 24, 0);
  require(crc(authenticated) == checksum, "property block metadata checksum mismatch");
  uint32_t previousToken = 0;
  for (auto token : tokens) { require(token > previousToken, "selected tokens must be sorted/unique"); previousToken = token; }
  std::vector<PropertyCollectionRoot> result(rows);
  uint64_t previousOwner = 0;
  for (size_t row = 0; row < rows; ++row) {
   const auto owner = get64(m, HEADER + row * 8);
   require(owner > previousOwner, "invalid property block owner order"); previousOwner = owner;
   result[row].owner_entity_id = owner; result[row].owner_entity_type = expectedType;
  }
  const uint64_t validMask = rows == MAX_ROWS ? UINT64_MAX : (uint64_t{1} << rows) - 1;
  previousToken = 0;
  for (size_t column = 0; column < count; ++column) {
   const size_t d = HEADER + rows * 8 + column * DESCRIPTOR;
   const auto token = get32(m, d), rawKind = get32(m, d + 4);
   const auto presence = get64(m, d + 8), nulls = get64(m, d + 16);
   require(token > previousToken && rawKind >= 1 && rawKind <= 4 && get32(m, d + 36) == 0 &&
           (presence & ~validMask) == 0 && (nulls & ~presence) == 0 && presence != 0,
           "invalid property block descriptor"); previousToken = token;
   if (!std::binary_search(tokens.begin(), tokens.end(), token)) {
    if (image.columns[column]) require(get64(m, d + 24) == image.columns[column]->size(), "property column size mismatch");
    continue; // lazy image may omit unrequested payloads; metadata still authenticated
   }
   require(image.columns[column] != nullptr, "selected property column missing");
   const auto &bytes = *image.columns[column];
   require(get64(m, d + 24) == bytes.size(), "property column size mismatch");
   require(crc(bytes) == get32(m, d + 32), "property column checksum mismatch");
   const auto kind = static_cast<ColumnKind>(rawKind);
   if (kind == ColumnKind::COL_CANONICAL) {
    require(bytes.size() >= (rows + 1) * 4 && get32(bytes, 0) == (rows + 1) * 4 &&
            get32(bytes, rows * 4) == bytes.size(), "invalid canonical column directory");
   } else require(bytes.size() == rows * (kind == ColumnKind::COL_BOOLEAN ? 1 : 8), "invalid typed column length");
   for (size_t row = 0; row < rows; ++row) {
    const bool present = presence & (uint64_t{1} << row), isNull = nulls & (uint64_t{1} << row);
    PropertyValue value;
    if (kind == ColumnKind::COL_CANONICAL) {
     const size_t begin = get32(bytes, row * 4), end = get32(bytes, (row + 1) * 4);
     require(begin <= end && end <= bytes.size(), "invalid canonical column offsets");
     if (!present || isNull) require(begin == end, "absent/null column owns payload");
     else {
      auto range = std::span(bytes).subspan(begin, end - begin); size_t consumed = 0;
      value = CanonicalPropertyValueCodec::decodeNext(range, consumed);
      require(consumed == range.size() && !std::holds_alternative<std::monostate>(value.getVariant()), "invalid canonical column cell");
     }
    } else {
     if (!present || isNull) {
      const size_t stride = kind == ColumnKind::COL_BOOLEAN ? 1 : 8;
      for (size_t i = 0; i < stride; ++i) require(bytes[row * stride + i] == std::byte{}, "absent/null typed payload is nonzero");
     } else if (kind == ColumnKind::COL_INTEGER) value = PropertyValue(std::bit_cast<int64_t>(get64(bytes, row * 8)));
     else if (kind == ColumnKind::COL_DOUBLE) value = PropertyValue(std::bit_cast<double>(get64(bytes, row * 8)));
     else { require(bytes[row] == std::byte{0} || bytes[row] == std::byte{1}, "invalid boolean cell"); value = PropertyValue(bytes[row] == std::byte{1}); }
    }
    if (present) result[row].entries.push_back({token, std::move(value)});
   }
  }
  return result;
 }

 static std::vector<uint32_t> tokens(const Image &image) {
  // Validate all metadata, then expose only token IDs; decode(...,{}) reads no payload.
  require(image.metadata.size() >= HEADER, "truncated property block");
  static_cast<void>(decode(image, get64(image.metadata, 16), get32(image.metadata, 12), {}));
  std::vector<uint32_t> result;
  const size_t rows = get16(image.metadata, 6), count = get32(image.metadata, 8);
  for (size_t i = 0; i < count; ++i) result.push_back(get32(image.metadata, HEADER + rows * 8 + i * DESCRIPTOR));
  return result;
 }
 // Freeze raw input into privately owned const buffers before granting proof.
 // shared_ptr<const T> alone does not exclude a caller's mutable shared_ptr<T> alias.
 static VerifiedImage verify(const Image &image, uint64_t blockId, uint32_t type) {
  static_cast<void>(tokens(image)); // reject malformed metadata/references before payload copies
  Image frozen;
  frozen.metadata = image.metadata;
  for (const auto &column : image.columns) {
   require(column != nullptr, "property column missing");
   frozen.columns.push_back(std::make_shared<const std::vector<std::byte>>(*column));
  }
  const auto keys = tokens(frozen);
  static_cast<void>(decode(frozen, blockId, type, keys));
  return VerifiedImage(std::move(frozen), blockId, type);
 }
 static VerifiedImage replaceVerified(const VerifiedImage &before, uint64_t blockId, uint32_t type, uint64_t owner,
                                      const std::vector<PropertyCollectionRootEntry> &entries) {
  require(before.block_ == blockId && before.type_ == type, "verified property identity mismatch");
  return VerifiedImage(replaceImpl(*before.image_, blockId, type, owner, entries, &before), blockId, type);
 }
 static Image replace(const Image &image, uint64_t blockId, uint32_t type, uint64_t owner,
                      const std::vector<PropertyCollectionRootEntry> &entries) {
  return replaceImpl(image, blockId, type, owner, entries, nullptr);
 }
 static size_t byteSize(const Image &image) {
  size_t bytes = image.metadata.size(); for (const auto &column : image.columns) { require(column != nullptr, "property column missing"); bytes += column->size(); } return bytes;
 }
 static size_t rewrittenBytes(const Image &before, const Image &after) {
  size_t bytes = before.metadata == after.metadata ? 0 : after.metadata.size();
  for (const auto &column : after.columns) {
   require(column != nullptr, "property column missing");
   if (std::find(before.columns.begin(), before.columns.end(), column) == before.columns.end()) bytes += column->size();
  }
  return bytes;
 }
private:
 static std::vector<PropertyCollectionRootEntry> verifiedCell(const VerifiedImage &proof, size_t column, size_t row) {
  const auto &image = *proof.image_;
  const auto &m = image.metadata;
  const size_t rows = get16(m, 6), descriptor = HEADER + rows * 8 + column * DESCRIPTOR;
  const uint64_t bit = uint64_t{1} << row;
  if (!(get64(m, descriptor + 8) & bit)) return {};
  const uint32_t token = get32(m, descriptor);
  if (get64(m, descriptor + 16) & bit) return {{token, PropertyValue()}};
  const auto &bytes = *image.columns[column];
  const auto kind = static_cast<ColumnKind>(get32(m, descriptor + 4));
  PropertyValue value;
  if (kind == ColumnKind::COL_CANONICAL) {
   const size_t begin = get32(bytes, row * 4), end = get32(bytes, (row + 1) * 4);
   size_t consumed = 0; value = CanonicalPropertyValueCodec::decodeNext(std::span(bytes).subspan(begin, end - begin), consumed);
  } else if (kind == ColumnKind::COL_INTEGER) value = PropertyValue(std::bit_cast<int64_t>(get64(bytes, row * 8)));
  else if (kind == ColumnKind::COL_DOUBLE) value = PropertyValue(std::bit_cast<double>(get64(bytes, row * 8)));
  else value = PropertyValue(bytes[row] == std::byte{1});
  return {{token, std::move(value)}};
 }
 static Image replaceImpl(const Image &image, uint64_t blockId, uint32_t type, uint64_t owner,
                          const std::vector<PropertyCollectionRootEntry> &entries, const VerifiedImage *proof) {
  require(!proof || (proof->image_.get() == &image && proof->block_ == blockId && proof->type_ == type), "unbound property proof");
  const auto keys = tokens(image);
  auto mapping = decode(image, blockId, type, {});
  auto ownerIt = std::lower_bound(mapping.begin(), mapping.end(), owner, [](const auto &root, uint64_t id) { return root.owner_entity_id < id; });
  require(ownerIt != mapping.end() && ownerIt->owner_entity_id == owner, "property replacement owner absent");
  const size_t target = ownerIt - mapping.begin();
  require(entries.size() <= 4096, "replacement column bound exceeded");
  uint32_t previous = 0;
  std::set<uint32_t> unionKeys(keys.begin(), keys.end());
  for (const auto &entry : entries) {
   require(entry.key_token > previous, "invalid replacement token order");
   previous = entry.key_token; unionKeys.insert(entry.key_token);
  }
  Image result;
  const size_t prefix = HEADER + mapping.size() * 8;
  result.metadata.assign(image.metadata.begin(), image.metadata.begin() + prefix);
  for (uint32_t token : unionKeys) {
   auto oldKey = std::lower_bound(keys.begin(), keys.end(), token);
   const bool existed = oldKey != keys.end() && *oldKey == token;
   const size_t oldColumn = oldKey - keys.begin();
   const auto newEntry = std::lower_bound(entries.begin(), entries.end(), token,
                                        [](const auto &entry, uint32_t key) { return entry.key_token < key; });
   std::vector<PropertyCollectionRootEntry> changed;
   if (newEntry != entries.end() && newEntry->key_token == token) changed.push_back(*newEntry);
   std::vector<PropertyCollectionRoot> columnRows = mapping;
   if (existed) {
    const std::array<uint32_t, 1> selected{token};
    std::vector<PropertyCollectionRootEntry> previousCell;
    if (proof) previousCell = verifiedCell(*proof, oldColumn, target);
    else { columnRows = decode(image, blockId, type, selected); previousCell = columnRows[target].entries; }
    if (sameEntries(previousCell, changed)) {
     const size_t descriptor = prefix + oldColumn * DESCRIPTOR;
     result.metadata.insert(result.metadata.end(), image.metadata.begin() + descriptor, image.metadata.begin() + descriptor + DESCRIPTOR);
     result.columns.push_back(image.columns[oldColumn]);
     continue;
    }
    if (proof) columnRows = decode(image, blockId, type, selected);
   }
   columnRows[target].entries = std::move(changed);
   const auto encoded = encode(blockId, columnRows);
   if (encoded.columns.empty()) continue; // last presence removed: remove column
   require(encoded.columns.size() == 1, "replacement created unexpected columns");
   result.metadata.insert(result.metadata.end(), encoded.metadata.begin() + prefix, encoded.metadata.end());
   result.columns.push_back(encoded.columns.front());
  }
  require(result.columns.size() <= 4096, "replacement column bound exceeded");
  put32(result.metadata, 8, result.columns.size());
  put32(result.metadata, 24, 0); put32(result.metadata, 24, crc(result.metadata));
  return result; // unchanged columns are shared immutable payloads; failures leave source intact
 }
 static void require(bool valid, const char *message) { if (!valid) throw std::runtime_error(message); }
 static bool sameEntries(std::span<const PropertyCollectionRootEntry> before, std::span<const PropertyCollectionRootEntry> after) {
  if (before.size() != after.size()) return false;
  for (size_t i = 0; i < before.size(); ++i) {
   if (before[i].key_token != after[i].key_token) return false;
   std::vector<std::byte> oldWire, newWire;
   CanonicalPropertyValueCodec::append(oldWire, before[i].value);
   CanonicalPropertyValueCodec::append(newWire, after[i].value);
   if (oldWire != newWire) return false; // distinguish signed zero; identical NaN bits are a no-op
  }
  return true;
 }
 static ColumnKind classify(const PropertyValue &value) {
  const auto &v = value.getVariant();
  if (std::holds_alternative<int64_t>(v)) return ColumnKind::COL_INTEGER;
  if (std::holds_alternative<double>(v)) return ColumnKind::COL_DOUBLE;
  if (std::holds_alternative<bool>(v)) return ColumnKind::COL_BOOLEAN;
  return ColumnKind::COL_CANONICAL;
 }
 static uint32_t crc(std::span<const std::byte> bytes) { return graph::utils::calculateCrc(bytes.data(), bytes.size()); }
 static void put16(std::span<std::byte> b, size_t p, uint16_t v) { LittleEndianCodec::writeU16(b, p, v); }
 static void put32(std::span<std::byte> b, size_t p, uint32_t v) { LittleEndianCodec::writeU32(b, p, v); }
 static void put64(std::span<std::byte> b, size_t p, uint64_t v) { LittleEndianCodec::writeU64(b, p, v); }
 static uint16_t get16(std::span<const std::byte> b, size_t p) { return LittleEndianCodec::readU16(b, p); }
 static uint32_t get32(std::span<const std::byte> b, size_t p) { return LittleEndianCodec::readU32(b, p); }
 static uint64_t get64(std::span<const std::byte> b, size_t p) { return LittleEndianCodec::readU64(b, p); }
};
}
