#include "storage/relocatable/schema/SystemCatalogTestFixture.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"

TEST_F(SystemCatalogTest, IndexEnumerationRejectsDuplicateLogicalIdsAndPreservesOlderDefinitions) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 IndexDefinition definition;
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   definition.name = "first"; definition.kind = IndexDefinitionKind::IDK_PROPERTY;
   definition.property_names = {"age"}; definition = session.createIndexDefinition(definition);
   session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 auto old = metadata_->openSnapshot();
 EXPECT_EQ(catalog.readIndexDefinitions(old, 1U).size(), 1U);
 auto duplicate = definition; duplicate.name = "duplicate";
 std::vector<std::byte> key{std::byte{6U}};
 for (int shift = 56; shift >= 0; shift -= 8)
  key.push_back(static_cast<std::byte>((duplicate.index_id >> shift) & 0xffU));
 const auto encoded = IndexDefinitionCodec::encode(duplicate);
 key.insert(key.end(), encoded.begin(), encoded.end());
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, key, duplicate.index_id);
   // Both definitions retain valid name and semantic directions, leaving the
   // duplicated logical ID as the only broken enumeration invariant.
   std::vector<std::byte> nameKey{std::byte{7U}};
   for (unsigned char ch : duplicate.name) nameKey.push_back(static_cast<std::byte>(ch));
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, nameKey, duplicate.index_id);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto snapshot = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 4U)), FormatException);
   EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 1U)), FormatException);
   EXPECT_THROW(static_cast<void>(catalog.findIndexDefinitionById(snapshot, definition.index_id)), FormatException); }
 EXPECT_EQ(catalog.readIndexDefinitions(old, 1U).size(), 1U); old.release();
}

TEST_F(SystemCatalogTest, IndexEnumerationRejectsLeafValuesDisagreeingWithTheEncodedLogicalId) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 IndexDefinition definition;
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   definition.name = "first"; definition.kind = IndexDefinitionKind::IDK_PROPERTY;
   definition.property_names = {"age"}; definition = session.createIndexDefinition(definition);
   session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 const std::vector<std::byte> prefix{std::byte{6U}};
 std::vector<BTreePageEntry> entries;
 { auto snapshot = metadata_->openSnapshot();
   entries = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, indexes, *metadata_).scan(snapshot, prefix, 1U); }
 ASSERT_EQ(entries.size(), 1U);
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, entries[0].key, definition.index_id + 1U);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto snapshot = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.readIndexDefinitions(snapshot, 4U)), FormatException); }
}
