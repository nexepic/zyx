/**
 * @file test_PropertyValueCodec_Inspection.cpp
 * @brief Allocation-free validation and pre-decode admission contracts.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include <gtest/gtest.h>
#include <optional>
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/core/Node.hpp"

using namespace graph;
using namespace graph::storage;
namespace {
PropertyValue inspectionValue() {
    PropertyValue::MapType map;
    map.emplace("a", PropertyValue(std::vector<PropertyValue>{PropertyValue("abc"),
        PropertyValue(PropertyValue::Bytes(4U, std::byte{0x5a}))}));
    map.emplace("z", PropertyValue(7)); return PropertyValue(std::move(map));
}
}

TEST(RelocatablePropertyValueInspectionTest, CountsNestedShapeAndConsumesOnlyOneOuterValue) {
    std::vector<std::byte> encoded{std::byte{0xff}};
    const auto value = inspectionValue(); CanonicalPropertyValueCodec::append(encoded, value);
    const auto end = encoded.size(); CanonicalPropertyValueCodec::append(encoded, PropertyValue(9));
    size_t offset = 1U, checkpoints = 0U;
    const auto shape = CanonicalPropertyValueCodec::inspectNext(encoded, offset, [&] { ++checkpoints; });
    EXPECT_EQ(offset, end); EXPECT_EQ(shape.encoded_bytes, end - 1U);
    EXPECT_EQ(shape.value_count, 5U); EXPECT_EQ(shape.list_elements, 2U); EXPECT_EQ(shape.map_entries, 2U);
    EXPECT_EQ(shape.string_bytes, 3U); EXPECT_EQ(shape.map_key_bytes, 2U); EXPECT_EQ(shape.binary_bytes, 4U);
    EXPECT_EQ(shape.maximum_depth, 2U); EXPECT_GE(checkpoints, 2U);
    offset = 1U; bool admitted = false;
    const auto decoded = CanonicalPropertyValueCodec::decodeNextWithAdmission(encoded, offset, [&](const auto &plan) {
        admitted = true; EXPECT_EQ(plan.encoded_bytes, shape.encoded_bytes);
    });
    EXPECT_TRUE(admitted); EXPECT_EQ(decoded, value); EXPECT_EQ(offset, end);
}

TEST(RelocatablePropertyValueInspectionTest, RejectionAndCancellationKeepTheOriginalCursorAndDoNotCallLaterAdmission) {
    std::vector<std::byte> encoded;
    CanonicalPropertyValueCodec::append(encoded, PropertyValue(std::vector<PropertyValue>(1024U, PropertyValue(7))));
    size_t offset = 0U, checkpoints = 0U; bool admitted = false;
    EXPECT_THROW(static_cast<void>(CanonicalPropertyValueCodec::decodeNextWithAdmission(encoded, offset,
        [&](const auto &) { admitted = true; }, [&] {
            if (++checkpoints == 2U) throw std::runtime_error("cancel inspection");
        })), std::runtime_error);
    EXPECT_FALSE(admitted); EXPECT_EQ(offset, 0U);
    EXPECT_THROW(static_cast<void>(CanonicalPropertyValueCodec::decodeNextWithAdmission(encoded, offset,
        [](const auto &) { throw std::runtime_error("deny decode"); })), std::runtime_error);
    EXPECT_EQ(offset, 0U);
    EXPECT_THROW(static_cast<void>(CanonicalPropertyValueCodec::decodeNextWithAdmission(encoded, offset, {})), std::invalid_argument);
    EXPECT_EQ(offset, 0U);
}

TEST(RelocatablePropertyValueInspectionTest, ValidationMatchesDecoderForEverySingleBitWireMutation) {
    std::vector<std::byte> encoded; CanonicalPropertyValueCodec::append(encoded, inspectionValue());
    for (size_t index = 0U; index < encoded.size(); ++index) {
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            auto changed = encoded; changed[index] ^= std::byte{static_cast<unsigned char>(1U << bit)};
            size_t inspected = 0U, decoded = 0U;
            std::optional<FormatError> inspectionError, decodeError;
            try { static_cast<void>(CanonicalPropertyValueCodec::inspectNext(changed, inspected)); }
            catch (const FormatException &error) { inspectionError = error.error(); }
            try { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(changed, decoded)); }
            catch (const FormatException &error) { decodeError = error.error(); }
            EXPECT_EQ(inspectionError, decodeError) << index << ":" << bit;
            EXPECT_EQ(inspected, decoded) << index << ":" << bit;
        }
    }
}

TEST(RelocatablePropertyValueInspectionTest, ScopedPrivateDecodeRejectsBeforeCopyAndRetainsCreditThroughItsSourceLifetime) {
    const PropertyValue expected(std::vector<PropertyValue>(1024U, PropertyValue(std::string(512U, 's'))));
    std::vector<std::byte> encoded; CanonicalPropertyValueCodec::append(encoded, expected);
    auto small = std::make_shared<query::execution::MaterializedByteBudget>(1024U);
    size_t offset = 0U;
    auto *previous = property_decode::AllocationScope::current();
    {
        detail::PropertyDecodeWorkspace workspace(small);
        EXPECT_THROW(static_cast<void>(CanonicalPropertyValueCodec::decodeNext(encoded, offset)),
            query::execution::MaterializedByteLimitExceeded);
        EXPECT_EQ(offset, 0U); EXPECT_EQ(small->used(), 0U);
    }
    EXPECT_EQ(property_decode::AllocationScope::current(), previous);
    auto enough = std::make_shared<query::execution::MaterializedByteBudget>(1048576U);
    {
        detail::PropertyDecodeWorkspace workspace(enough);
        const auto decoded = CanonicalPropertyValueCodec::decodeNext(encoded, offset);
        EXPECT_EQ(decoded, expected); EXPECT_EQ(offset, encoded.size());
        EXPECT_GE(enough->used(), 524288U);
    }
    EXPECT_EQ(enough->used(), 0U);
    EXPECT_EQ(property_decode::AllocationScope::current(), previous);
}

TEST(RelocatablePropertyValueInspectionTest, NestedDecodeScopesReplaceAdmissionAndInheritCancellationCheckpoints) {
    struct Counts { size_t admissions = 0U, checkpoints = 0U; } outer, inner;
    std::vector<std::byte> encoded; CanonicalPropertyValueCodec::append(encoded, PropertyValue("text"));
    const auto admit = [](void *context, size_t) { ++static_cast<Counts *>(context)->admissions; };
    const auto checkpoint = [](void *context) { ++static_cast<Counts *>(context)->checkpoints; };
    auto *previous = property_decode::AllocationScope::current();
    {
        property_decode::AllocationScope parent(&outer, admit, checkpoint);
        {
            property_decode::AllocationScope child(&inner, admit);
            size_t offset = 0U;
            EXPECT_EQ(CanonicalPropertyValueCodec::decodeNext(encoded, offset), PropertyValue("text"));
            EXPECT_EQ(inner.admissions, 1U); EXPECT_EQ(outer.admissions, 0U);
            EXPECT_GT(outer.checkpoints, 0U);
        }
        EXPECT_EQ(property_decode::AllocationScope::current(), &parent);
        size_t offset = 0U;
        EXPECT_EQ(CanonicalPropertyValueCodec::decodeNext(encoded, offset), PropertyValue("text"));
        EXPECT_EQ(outer.admissions, 1U);
    }
    EXPECT_EQ(property_decode::AllocationScope::current(), previous);
}

TEST(RelocatablePropertyValueInspectionTest, ImplicitDecodeQuotaCannotHideLaterCorruptCollectionTokens) {
    PropertyCollectionRoot root{1001U, Node::typeId,
        {{3U, PropertyValue(std::string(8192U, 'p'))}, {5U, PropertyValue(7)}}};
    auto encoded = EntityPropertyCollectionRootCodec::encode(root);
    constexpr size_t header = RSS_ENTITY_PROPERTY_COLLECTION_ROOT_HEADER_BYTES;
    const auto second = LittleEndianCodec::readU32(encoded, header + sizeof(uint32_t));
    LittleEndianCodec::writeU32(encoded, header + second, 0U);
    auto budget = std::make_shared<query::execution::MaterializedByteBudget>(1024U);
    {
        detail::PropertyDecodeWorkspace workspace(budget);
        try {
            static_cast<void>(EntityPropertyCollectionRootCodec::decode(encoded));
            FAIL() << "later invalid token was accepted";
        } catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER);
        }
    }
    EXPECT_EQ(budget->used(), 0U);
}

TEST(RelocatablePropertyValueInspectionTest, ScalarCollectionSlotsAreAdmittedBeforeTheirBackingAllocation) {
    PropertyCollectionRoot root{1001U, Node::typeId, {}};
    for (uint32_t token = 1U; token <= 1024U; ++token) root.entries.push_back({token, PropertyValue(7)});
    const auto encoded = EntityPropertyCollectionRootCodec::encode(root);
    auto small = std::make_shared<query::execution::MaterializedByteBudget>(128U);
    {
        detail::PropertyDecodeWorkspace workspace(small);
        EXPECT_THROW(static_cast<void>(EntityPropertyCollectionRootCodec::decode(encoded)),
            query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(small->used(), 0U);
    auto enough = std::make_shared<query::execution::MaterializedByteBudget>(196608U);
    {
        detail::PropertyDecodeWorkspace workspace(enough);
        const auto decoded = EntityPropertyCollectionRootCodec::decode(encoded);
        EXPECT_EQ(decoded, root);
        EXPECT_GE(enough->used(), 1024U * sizeof(PropertyCollectionRootEntry));
    }
    EXPECT_EQ(enough->used(), 0U);
}

TEST(RelocatablePropertyValueInspectionTest, ExternalCollectionDirectoryAndSlotsShareThePrivateDecodeBudget) {
    std::vector<PropertyCollectionRootEntry> entries;
    for (uint32_t token = 1U; token <= 1024U; ++token) entries.push_back({token, PropertyValue(7)});
    const auto encoded = EntityPropertyLargeValuePayloadCodec::encode(entries);
    auto small = std::make_shared<query::execution::MaterializedByteBudget>(128U);
    {
        detail::PropertyDecodeWorkspace workspace(small);
        EXPECT_THROW(static_cast<void>(EntityPropertyLargeValuePayloadCodec::decode(encoded)),
            query::execution::MaterializedByteLimitExceeded);
    }
    EXPECT_EQ(small->used(), 0U);
    auto enough = std::make_shared<query::execution::MaterializedByteBudget>(262144U);
    {
        detail::PropertyDecodeWorkspace workspace(enough);
        const auto decoded = EntityPropertyLargeValuePayloadCodec::decode(encoded);
        EXPECT_EQ(decoded, entries);
        EXPECT_GE(enough->used(), entries.size() *
            (sizeof(PropertyCollectionRootEntry) + sizeof(EntityPropertyLargeValueEntryLocation)));
    }
    EXPECT_EQ(enough->used(), 0U);
    auto corrupt = encoded;
    LittleEndianCodec::writeU32(corrupt, RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES + 8U, 0U);
    {
        detail::PropertyDecodeWorkspace workspace(small);
        try {
            static_cast<void>(EntityPropertyLargeValuePayloadCodec::decode(corrupt));
            FAIL() << "later corrupt directory token was accepted";
        } catch (const FormatException &error) {
            EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER);
        }
    }
    EXPECT_EQ(small->used(), 0U);
}

TEST(RelocatablePropertyValueInspectionTest, ProtectedMaintenanceGrantCoversDecodedPayloadsAndRefundsOnDenial) {
    using namespace query::execution::detail;
    auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(2097152U));
    auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(2097152U));
    auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_MAINTENANCE, 2097152U);
    const PropertyValue expected(std::string(65536U, 'm'));
    std::vector<std::byte> encoded; CanonicalPropertyValueCodec::append(encoded, expected);
    {
        auto memory = std::make_shared<PrepaidMemoryResource>(account, 1024U);
        {
            detail::PropertyDecodeWorkspace workspace(memory);
            size_t offset = 0U;
            EXPECT_THROW(static_cast<void>(CanonicalPropertyValueCodec::decodeNext(encoded, offset)), ResourceAdmissionDenied);
            EXPECT_EQ(offset, 0U); EXPECT_EQ(memory->used(), 0U);
        }
        EXPECT_EQ(account->used(), 1024U);
    }
    {
        auto memory = std::make_shared<PrepaidMemoryResource>(account, 1048576U);
        {
            detail::PropertyDecodeWorkspace workspace(memory);
            size_t offset = 0U;
            const auto decoded = CanonicalPropertyValueCodec::decodeNext(encoded, offset);
            EXPECT_EQ(decoded, expected); EXPECT_GE(memory->used(), 65537U);
        }
        EXPECT_EQ(memory->used(), 0U); EXPECT_EQ(account->used(), 1048576U);
    }
    EXPECT_EQ(account->used(), 0U);
}

TEST(RelocatablePropertyValueInspectionTest, GuardedCopyUpperUsesFreshLayoutRatherThanSourceLoadFactor) {
    for (const auto factor : {0.01F, 1000.0F}) {
        PropertyValue::MapType source;
        for (size_t index = 0U; index < 257U; ++index)
            source.emplace(std::to_string(index), PropertyValue(std::string(22U, 'v')));
        source.max_load_factor(factor);
        const PropertyValue value(std::move(source));
        std::vector<std::byte> encoded; CanonicalPropertyValueCodec::append(encoded, value);
        size_t offset = 0U;
        const auto shape = CanonicalPropertyValueCodec::inspectNext(encoded, offset);
        EXPECT_EQ(CanonicalPropertyValueCodec::guardedCopyHeapUpper(value), shape.maximum_heap_request_bytes);
    }
}
