#include <gtest/gtest.h>
#include <bit>
#include "graph/core/Node.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

using namespace graph;
using namespace graph::storage;
namespace {
std::vector<std::byte> encode(const PropertyValue &value) {
    std::vector<std::byte> result; CanonicalPropertyValueCodec::append(result, value); return result;
}
}
TEST(RelocatableBorrowedPropertyTest, BorrowsPrimitiveBitsAndExactTextWithoutOwningValues) {
    const std::vector<PropertyValue> values{PropertyValue(), PropertyValue(false), PropertyValue(true),
        PropertyValue(int64_t(-42)), PropertyValue(-0.0),
        PropertyValue(std::bit_cast<double>(uint64_t{0x7ff8000000001234})),
        PropertyValue(std::string("a\0b", 3)), PropertyValue(std::string(16384, 'x'))};
    for (const auto &value : values) {
        const auto bytes = encode(value);
        const auto borrowed = CanonicalPropertyValueCodec::tryBorrowPrimitive(bytes);
        ASSERT_TRUE(borrowed); EXPECT_EQ(borrowed->type, value.getType());
        const auto &v = value.getVariant();
        if (const auto *text = std::get_if<std::string>(&v)) {
            EXPECT_EQ(borrowed->text, *text);
            EXPECT_GE(borrowed->text.data(), reinterpret_cast<const char *>(bytes.data()));
            EXPECT_LE(borrowed->text.data() + borrowed->text.size(), reinterpret_cast<const char *>(bytes.data() + bytes.size()));
        } else if (const auto *number = std::get_if<int64_t>(&v)) EXPECT_EQ(borrowed->bits, std::bit_cast<uint64_t>(*number));
        else if (const auto *number = std::get_if<double>(&v)) EXPECT_EQ(borrowed->bits, std::bit_cast<uint64_t>(*number));
        else if (const auto *boolean = std::get_if<bool>(&v)) EXPECT_EQ(borrowed->bits, *boolean ? 1U : 0U);
    }
    EXPECT_FALSE(CanonicalPropertyValueCodec::tryBorrowPrimitive(encode(PropertyValue(std::vector<PropertyValue>{PropertyValue(7)}))));
}
TEST(RelocatableBorrowedPropertyTest, RejectsMalformedPrimitivesInsteadOfBypassingTheCodec) {
    auto boolean = encode(PropertyValue(true)); boolean.back() = std::byte{2};
    EXPECT_THROW((void)CanonicalPropertyValueCodec::tryBorrowPrimitive(boolean), FormatException);
    auto flags = encode(PropertyValue(7)); flags[1] = std::byte{1};
    EXPECT_THROW((void)CanonicalPropertyValueCodec::tryBorrowPrimitive(flags), FormatException);
    auto text = encode(PropertyValue("abc")); LittleEndianCodec::writeU32(text, 6U, 2U);
    EXPECT_THROW((void)CanonicalPropertyValueCodec::tryBorrowPrimitive(text), FormatException);
    text = encode(PropertyValue("abc")); LittleEndianCodec::writeU32(text, 6U, 4U);
    EXPECT_THROW((void)CanonicalPropertyValueCodec::tryBorrowPrimitive(text), FormatException);
    auto truncated = encode(PropertyValue(7)); truncated.pop_back();
    EXPECT_THROW((void)CanonicalPropertyValueCodec::tryBorrowPrimitive(truncated), FormatException);
}
TEST(RelocatableBorrowedPropertyTest, ValidatesOwnerAndWholeDirectoryBeforePublishingSelectedBorrow) {
    PropertyCollectionRoot root{97U, Node::typeId, {{1U, PropertyValue(7)}, {3U, PropertyValue("abc")}}};
    const auto encoded = EntityPropertyCollectionRootCodec::encode(root);
    const std::array<uint32_t, 3> tokens{1U, 2U, 3U}; size_t calls = 0;
    const EntityPropertyCollectionRootCodec::EncodedSelectionSink sink = [&](size_t ordinal, auto bytes) {
        ++calls;
        if (ordinal == 1U) { EXPECT_FALSE(bytes); return; }
        ASSERT_TRUE(bytes); const auto value = CanonicalPropertyValueCodec::tryBorrowPrimitive(*bytes);
        ASSERT_TRUE(value); if (ordinal == 2U) EXPECT_EQ(value->text, "abc");
    };
    EntityPropertyCollectionRootCodec::visitSelectedEncoded(encoded, 97U, Node::typeId, tokens, sink);
    EXPECT_EQ(calls, 3U); calls = 0;
    EXPECT_THROW(EntityPropertyCollectionRootCodec::visitSelectedEncoded(encoded, 98U, Node::typeId, tokens, sink), FormatException);
    const std::array<uint32_t, 2> duplicates{1U, 1U};
    EXPECT_THROW(EntityPropertyCollectionRootCodec::visitSelectedEncoded(encoded, 97U, Node::typeId, duplicates, sink), FormatException);
    auto corrupt = encoded;
    const auto secondOffset = LittleEndianCodec::readU32(corrupt, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + 4U);
    LittleEndianCodec::writeU32(corrupt, RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES + secondOffset, 1U);
    const std::array<uint32_t, 1> first{1U};
    EXPECT_THROW(EntityPropertyCollectionRootCodec::visitSelectedEncoded(corrupt, 97U, Node::typeId, first, sink), FormatException);
    EXPECT_EQ(calls, 0U);
    SelectedPropertyReadAdmission deny = [](size_t) { throw std::runtime_error("denied"); };
    EXPECT_THROW(EntityPropertyCollectionRootCodec::visitSelectedEncoded(encoded, 97U, Node::typeId, first, sink, &deny), std::runtime_error);
    EXPECT_EQ(calls, 0U);
    struct Cancel final : CancellationCheckpoint { void check() const override { throw std::runtime_error("cancelled"); } } cancel;
    EXPECT_THROW(EntityPropertyCollectionRootCodec::visitSelectedEncoded(encoded, 97U, Node::typeId, first, sink, nullptr, &cancel), std::runtime_error);
    EXPECT_EQ(calls, 0U);
}
