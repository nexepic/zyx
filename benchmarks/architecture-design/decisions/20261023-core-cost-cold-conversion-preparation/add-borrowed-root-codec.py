from pathlib import Path
p=Path('include/graph/storage/relocatable/property/PropertyCollectionRoot.hpp');s=p.read_text();idx=s.index('\n\t// A root records');s=s[:idx]+'''
    // A synchronous encoding view over authenticated, immutable source values.
    // The source values and supplied output account must outlive encoding.
    struct BorrowedPropertyCollectionRootEntry {
        uint32_t key_token = 0;
        const PropertyValue *value = nullptr;
    };
''' +s[idx:];key='\t\t[[nodiscard]] static std::vector<std::byte> encode(const PropertyCollectionRoot &root);';s=s.replace(key,key+'''
        [[nodiscard]] static size_t encodedSizeBorrowed(uint64_t owner, uint32_t ownerType,
            std::span<const BorrowedPropertyCollectionRootEntry> entries, const std::function<void()> &checkpoint = {});
        [[nodiscard]] static std::pmr::vector<std::byte> encodeBorrowed(uint64_t owner, uint32_t ownerType,
            std::span<const BorrowedPropertyCollectionRootEntry> entries, std::pmr::memory_resource &memory,
            const std::function<void()> &checkpoint = {});''',1);key='\t\t[[nodiscard]] static std::vector<std::byte> encode(std::span<const PropertyCollectionRootEntry> entries);';s=s.replace(key,key+'''
        [[nodiscard]] static std::pmr::vector<std::byte> encodeBorrowed(
            std::span<const BorrowedPropertyCollectionRootEntry> entries, std::pmr::memory_resource &memory,
            const std::function<void()> &checkpoint = {});''',1);p.write_text(s)
p=Path('src/storage/relocatable/property/PropertyCollectionRoot.cpp');s=p.read_text();idx=s.index('\n\t[[nodiscard]] std::vector<std::optional<PropertyCollectionRootEntry>>\n\tdecodeSelectedEntriesValidated')
s=s[:idx]+'''
    size_t EntityPropertyCollectionRootCodec::encodedSizeBorrowed(uint64_t owner, uint32_t ownerType,
        std::span<const BorrowedPropertyCollectionRootEntry> entries, const std::function<void()> &checkpoint) {
        if (checkpoint) checkpoint();
        if (!owner || !isOwnerType(ownerType))
            throwRootError(FormatError::FERR_INVALID_HEADER, "borrowed Property root has an invalid owner");
        if (entries.size() > (std::numeric_limits<uint32_t>::max)() / sizeof(uint32_t))
            throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "borrowed Property directory exceeds its wire size");
        size_t payload = entries.size() * sizeof(uint32_t); uint32_t previous = 0;
        for (const auto &entry : entries) {
            if (checkpoint) checkpoint();
            if (!entry.key_token || entry.key_token <= previous || !entry.value)
                throwRootError(FormatError::FERR_INVALID_HEADER, "borrowed Property entries are null or unordered");
            const auto bytes = CanonicalPropertyValueCodec::encodedSize(*entry.value);
            if (payload > (std::numeric_limits<uint32_t>::max)() - sizeof(uint32_t) ||
                bytes > (std::numeric_limits<uint32_t>::max)() - payload - sizeof(uint32_t))
                throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "borrowed Property payload exceeds its wire size");
            payload += sizeof(uint32_t) + bytes; previous = entry.key_token;
        }
        if (payload > (std::numeric_limits<size_t>::max)() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES)
            throwRootError(FormatError::FERR_ARITHMETIC_OVERFLOW, "borrowed Property output size overflowed");
        return RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + payload;
    }
    std::pmr::vector<std::byte> EntityPropertyCollectionRootCodec::encodeBorrowed(uint64_t owner, uint32_t ownerType,
        std::span<const BorrowedPropertyCollectionRootEntry> entries, std::pmr::memory_resource &memory,
        const std::function<void()> &checkpoint) {
        const auto total = encodedSizeBorrowed(owner, ownerType, entries, checkpoint);
        std::pmr::vector<std::byte> result(&memory); result.reserve(total);
        result.resize(RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + entries.size() * sizeof(uint32_t));
        LittleEndianCodec::writeU32(result, 0, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_MAGIC);
        LittleEndianCodec::writeU16(result, 4, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_VERSION);
        LittleEndianCodec::writeU16(result, 6, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES);
        LittleEndianCodec::writeU64(result, ENTITY_OWNER_ID_OFFSET, owner);
        LittleEndianCodec::writeU32(result, ENTITY_OWNER_TYPE_OFFSET, ownerType);
        LittleEndianCodec::writeU32(result, ENTITY_ENTRY_COUNT_OFFSET, uint32_t(entries.size()));
        LittleEndianCodec::writeU32(result, ENTITY_PAYLOAD_BYTES_OFFSET, uint32_t(total - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES));
        for (size_t i = 0; i < entries.size(); ++i) {
            if (checkpoint) checkpoint();
            const auto offset = result.size() - RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES;
            LittleEndianCodec::writeU32(result, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + i * sizeof(uint32_t), uint32_t(offset));
            const auto token = result.size(); result.resize(token + sizeof(uint32_t));
            LittleEndianCodec::writeU32(result, token, entries[i].key_token);
            CanonicalPropertyValueCodec::append(result, *entries[i].value);
        }
        if (result.size() != total)
            throwRootError(FormatError::FERR_INVALID_STATE, "borrowed Property input changed during encoding");
        if (checkpoint) checkpoint();
        return result;
    }
''' +s[idx:]
a=s.index('    std::pmr::vector<std::byte> EntityPropertyLargeValuePayloadCodec::encode(\n');b=s.index('    std::pmr::vector<std::byte> EntityPropertyLargeValuePayloadCodec::encodeSingleField',a)
body=s[a:b];body=body.replace('    std::pmr::vector<std::byte> EntityPropertyLargeValuePayloadCodec::encode(\n        std::span<const PropertyCollectionRootEntry> entries,', '    template<class Entry>\n    std::pmr::vector<std::byte> encodeLargeValueEntriesPmr(\n        std::span<const Entry> entries,',1).replace('CanonicalPropertyValueCodec::encodedSize(entry.value)','CanonicalPropertyValueCodec::encodedSize(borrowedEncodingValue(entry))').replace('CanonicalPropertyValueCodec::append(result, entry.value)','CanonicalPropertyValueCodec::append(result, borrowedEncodingValue(entry))')
helpers='''    namespace {
    const PropertyValue &borrowedEncodingValue(const PropertyCollectionRootEntry &entry) { return entry.value; }
    const PropertyValue &borrowedEncodingValue(const BorrowedPropertyCollectionRootEntry &entry) {
        if (!entry.value) throwRootError(FormatError::FERR_INVALID_HEADER, "borrowed LargeValue entry has no value");
        return *entry.value;
    }
'''
wrappers='''    }
    std::pmr::vector<std::byte> EntityPropertyLargeValuePayloadCodec::encode(
        std::span<const PropertyCollectionRootEntry> entries, std::pmr::memory_resource &memory,
        const std::function<void()> &checkpoint) {
        return encodeLargeValueEntriesPmr(entries, memory, checkpoint);
    }
    std::pmr::vector<std::byte> EntityPropertyLargeValuePayloadCodec::encodeBorrowed(
        std::span<const BorrowedPropertyCollectionRootEntry> entries, std::pmr::memory_resource &memory,
        const std::function<void()> &checkpoint) {
        return encodeLargeValueEntriesPmr(entries, memory, checkpoint);
    }

'''
s=s[:a]+helpers+body+wrappers+s[b:];p.write_text(s)
