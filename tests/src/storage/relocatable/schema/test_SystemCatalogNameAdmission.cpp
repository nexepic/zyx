/**
 * @file test_SystemCatalogNameAdmission.cpp
 * @brief Source decode admission for catalog names and forward keys.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "storage/relocatable/schema/SystemCatalogTestFixture.hpp"
#include "graph/core/PropertyDecodeAllocationScope.hpp"

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
