/**
 * @file test_SystemCatalogNameAdmission.cpp
 * @brief Source decode admission for catalog names and forward keys.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "storage/relocatable/schema/SystemCatalogTestFixture.hpp"
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

TEST_F(SystemCatalogTest, PrivateSourceNameLookupAdmitsFreshNamesAndForwardKeysWithoutAnExternalCallback) {
 BTreePageStore indexes(*slotted_pages_, *metadata_);
 SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes,
  systemCatalogFixturePlannerOptions());
 const std::string name(1024U, 'k');
 auto transaction = planner.beginTransaction(); auto session = catalog.beginWrite(transaction);
 const auto property = session.ensurePropertyToken(name);
 const auto label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
 session.finish(); ASSERT_TRUE(transaction.commit().committed());
 auto snapshot = metadata_->openSnapshot();
 const auto generation = snapshot.superblock().generation;
 struct Admission { size_t total = 0U; bool reject = false; } admission;
 const auto admit = [](void *context, size_t bytes) {
  auto &state = *static_cast<Admission *>(context);
  if (state.reject && bytes > 128U) throw std::length_error("private source name denied");
  state.total += bytes;
 };
 {
  graph::property_decode::AllocationScope scope(&admission, admit);
  EXPECT_EQ(catalog.findPropertyName(snapshot, property), name);
  EXPECT_GE(admission.total, 2U * (name.size() + 1U));
  admission.total = 0U;
  const auto resolved = catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
  ASSERT_TRUE(resolved); EXPECT_EQ(resolved->name, name);
  EXPECT_GE(admission.total, name.size() * 2U + 3U);
  admission.reject = true;
  EXPECT_THROW(static_cast<void>(catalog.findPropertyName(snapshot, property)), std::length_error);
  EXPECT_THROW(static_cast<void>(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label)),
   std::length_error);
 }
 EXPECT_EQ(catalog.findPropertyName(snapshot, property), name);
 EXPECT_EQ(metadata_->openSnapshot().superblock().generation, generation);
}

TEST_F(SystemCatalogTest, OwnedSchemaNameRetainsItsAuthorityAndChecksBothDirectionsAcrossSnapshots) {
    BTreePageStore indexes(*slotted_pages_, *metadata_);
    SystemCatalog catalog(indexes, *metadata_);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes,
        systemCatalogFixturePlannerOptions());
    const std::string name(1000U, 'n');
    auto transaction = planner.beginTransaction(); auto session = catalog.beginWrite(transaction);
    const auto label = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
    session.finish(); ASSERT_TRUE(transaction.commit().committed());
    auto before = metadata_->openSnapshot();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(65536U);
    std::weak_ptr<std::pmr::memory_resource> authority = memory;
    std::optional<OwnedSystemCatalogSchemaToken> owned;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        owned = catalog.findOwnedSchemaToken(before, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
    }
    ASSERT_TRUE(owned); EXPECT_EQ(owned->name, name); EXPECT_EQ(owned->token_id, label);
    EXPECT_GE(memory->used(), name.size() + 1U);
    auto corrupt = planner.beginTransaction();
    corrupt.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
        SystemCatalogTokenKeyCodec::schemaNameKey(SystemCatalogSchemaTokenKind::SCSTK_LABEL, name), label + 1U);
    ASSERT_TRUE(corrupt.commit().committed());
    auto current = metadata_->openSnapshot();
    EXPECT_THROW(static_cast<void>(catalog.findOwnedSchemaToken(current,
        SystemCatalogSchemaTokenKind::SCSTK_LABEL, label)), FormatException);
    const auto previous = catalog.findOwnedSchemaToken(before, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
    ASSERT_TRUE(previous); EXPECT_EQ(previous->name, name);
    // The exported name remains usable independently of the snapshot and caller's owner handle.
    before.release(); current.release(); memory.reset();
    ASSERT_FALSE(authority.expired()); EXPECT_EQ(owned->name, name);
    owned.reset(); EXPECT_TRUE(authority.expired());
}

TEST_F(SystemCatalogTest, EscapedWriteSessionRetainsItsCapturedAuthorityAcrossDifferentCallerScopes) {
    BTreePageStore indexes(*slotted_pages_, *metadata_);
    SystemCatalog catalog(indexes, *metadata_);
    NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes,
        systemCatalogFixturePlannerOptions());
    auto transaction = planner.beginTransaction();
    auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(2U * 1024U * 1024U);
    auto other = std::make_shared<graph::query::execution::MaterializedByteBudget>(2U * 1024U * 1024U);
    std::weak_ptr<std::pmr::memory_resource> authority = memory;
    std::optional<SystemCatalogWriteSession> session;
    {
        graph::utils::OwnedAllocationScope scope(memory);
        session.emplace(catalog.beginWrite(transaction));
    }
    const auto initial = memory->used();
    const std::string name(1000U, 'n');
    uint64_t label;
    uint32_t property;
    {
        graph::utils::OwnedAllocationScope foreign(other);
        label = session->ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
        property = session->ensurePropertyToken(name);
        session->setConfiguration("config.string", PropertyValue(std::string(128U, 'c')));
    }
    EXPECT_GT(memory->used(), initial + 2U * name.size());
    EXPECT_EQ(other->used(), 0U);
    const auto retained = memory->used();
    EXPECT_EQ(session->ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, name), label);
    EXPECT_EQ(session->ensurePropertyToken(name), property);
    EXPECT_EQ(memory->used(), retained);
    memory.reset();
    EXPECT_FALSE(authority.expired());
    session.reset(); transaction.rollback();
    EXPECT_TRUE(authority.expired());
    auto snapshot = metadata_->openSnapshot();
    EXPECT_FALSE(catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name));
    EXPECT_FALSE(catalog.findPropertyToken(snapshot, name));
}
