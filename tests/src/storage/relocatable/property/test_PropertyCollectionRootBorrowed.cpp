#include <gtest/gtest.h>
#include <algorithm>
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
using namespace graph::storage;
using graph::PropertyValue;
using graph::query::execution::MaterializedByteBudget;
namespace {
PropertyCollectionRoot source() {
    return {97, graph::Node::typeId, {{1, PropertyValue(int64_t(-44))}, {3, PropertyValue()},
        {5, PropertyValue(std::string(8193, 'x'))}, {7, PropertyValue(std::vector<PropertyValue>{
            PropertyValue(true), PropertyValue(PropertyValue::MapType{{"b", PropertyValue(2.5)}, {"a", PropertyValue("text")}})})}}};
}
std::vector<BorrowedPropertyCollectionRootEntry> borrow(const PropertyCollectionRoot &root) {
    std::vector<BorrowedPropertyCollectionRootEntry> result;
    for (const auto &entry : root.entries) result.push_back({entry.key_token, &entry.value});
    return result;
}
}
TEST(RelocatableBorrowedPropertyRootTest, CanonicalNodeAndEdgeBytesMatchExistingCodecWithoutCloningSourceValues) {
    auto root = source(); const auto entries = borrow(root); auto memory = std::make_shared<MaterializedByteBudget>(65536);
    for (const auto type : {graph::Node::typeId, graph::Edge::typeId}) {
        root.owner_entity_type = type;
        {
            const auto expected = EntityPropertyCollectionRootCodec::encode(root);
            const auto bytes = EntityPropertyCollectionRootCodec::encodeBorrowed(root.owner_entity_id, type, entries, *memory);
            EXPECT_TRUE(std::ranges::equal(bytes, expected));
            EXPECT_EQ(bytes.size(), EntityPropertyCollectionRootCodec::encodedSizeBorrowed(root.owner_entity_id, type, entries));
            EXPECT_EQ(EntityPropertyCollectionRootCodec::decode(bytes), root);
            EXPECT_GE(memory->used(), bytes.capacity());
        }
        EXPECT_EQ(memory->used(), 0U);
    }
    EXPECT_EQ(root.entries, source().entries);
}
TEST(RelocatableBorrowedPropertyRootTest, OwnerNeutralLargeBytesMatchExistingCanonicalPayloadForNestedValues) {
    const auto root = source(); const auto entries = borrow(root); auto memory = std::make_shared<MaterializedByteBudget>(65536);
    {
        const auto expected = EntityPropertyLargeValuePayloadCodec::encode(root.entries);
        const auto bytes = EntityPropertyLargeValuePayloadCodec::encodeBorrowed(entries, *memory);
        EXPECT_TRUE(std::ranges::equal(bytes, expected)); EXPECT_EQ(EntityPropertyLargeValuePayloadCodec::decode(bytes), root.entries);
    }
    EXPECT_EQ(memory->used(), 0U);
}
TEST(RelocatableBorrowedPropertyRootTest, EmptyCanonicalRootsAndInvalidOwnersPointersOrOrderRetainExistingContract) {
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    {
        const PropertyCollectionRoot empty{1, graph::Node::typeId, {}};
        const auto bytes = EntityPropertyCollectionRootCodec::encodeBorrowed(1, graph::Node::typeId, {}, *memory);
        EXPECT_TRUE(std::ranges::equal(bytes, EntityPropertyCollectionRootCodec::encode(empty)));
    }
    const auto root = source(); auto entries = borrow(root);
    EXPECT_THROW(EntityPropertyCollectionRootCodec::encodeBorrowed(0, graph::Node::typeId, entries, *memory), FormatException);
    EXPECT_THROW(EntityPropertyCollectionRootCodec::encodeBorrowed(1, UINT32_MAX, entries, *memory), FormatException);
    EXPECT_THROW(EntityPropertyCollectionRootCodec::encode({1, UINT32_MAX, {}}), FormatException);
    entries[1].value = nullptr;
    EXPECT_THROW(EntityPropertyCollectionRootCodec::encodeBorrowed(1, graph::Node::typeId, entries, *memory), FormatException);
    EXPECT_THROW(EntityPropertyLargeValuePayloadCodec::encodeBorrowed(entries, *memory), FormatException);
    entries = borrow(root); entries[1].key_token = entries[0].key_token;
    EXPECT_THROW(EntityPropertyCollectionRootCodec::encodeBorrowed(1, graph::Node::typeId, entries, *memory), FormatException);
    EXPECT_THROW(EntityPropertyLargeValuePayloadCodec::encodeBorrowed(entries, *memory), FormatException);
    EXPECT_EQ(memory->used(), 0U);
}
TEST(RelocatableBorrowedPropertyRootTest, QuotaAndCancellationAfterAllocationReleaseBothOutputAndCanonicalWorkspace) {
    const auto root = source(); const auto entries = borrow(root); auto small = std::make_shared<MaterializedByteBudget>(64);
    EXPECT_THROW(EntityPropertyCollectionRootCodec::encodeBorrowed(1, graph::Node::typeId, entries, *small), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_THROW(EntityPropertyLargeValuePayloadCodec::encodeBorrowed(entries, *small), graph::query::execution::MaterializedByteLimitExceeded);
    EXPECT_EQ(small->used(), 0U);
    auto memory = std::make_shared<MaterializedByteBudget>(65536);
    const auto cancel = [&] { if (memory->used()) throw std::runtime_error("cancel after output allocation"); };
    EXPECT_THROW(EntityPropertyCollectionRootCodec::encodeBorrowed(1, graph::Node::typeId, entries, *memory, cancel), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U);
    EXPECT_THROW(EntityPropertyLargeValuePayloadCodec::encodeBorrowed(entries, *memory, cancel), std::runtime_error);
    EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(root, source());
}
