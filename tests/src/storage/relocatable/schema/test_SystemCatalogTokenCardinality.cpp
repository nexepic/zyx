#include "storage/relocatable/schema/SystemCatalogTestFixture.hpp"
#include "graph/debug/PerfTrace.hpp"

TEST_F(SystemCatalogTest, TokenCardinalityRequiresCompletenessAndPreservesSnapshots) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 const auto label = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
 const auto type = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
 auto old = metadata_->openSnapshot();
 EXPECT_FALSE(catalog.readTokenCardinality(old, label, 7U).has_value());
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.setTokenCardinality(label, 7U, 3U); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 { auto partial = metadata_->openSnapshot();
   EXPECT_FALSE(catalog.readTokenCardinality(partial, label, 7U).has_value()); }
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.markTokenCardinalityComplete(label); session.setTokenCardinality(type, 0U, 2U);
   session.markTokenCardinalityComplete(type); session.finish(); ASSERT_TRUE(tx.commit().committed());
   EXPECT_THROW(session.setTokenCardinality(label, 7U, 4U), std::logic_error); }
 EXPECT_FALSE(catalog.readTokenCardinality(old, label, 7U).has_value()); old.release();
 reopenNativeStores(); BTreePageStore reopenedIndexes(*slotted_pages_, *metadata_);
 SystemCatalog reopenedCatalog(reopenedIndexes, *metadata_); auto snapshot = metadata_->openSnapshot();
 EXPECT_EQ(reopenedCatalog.readTokenCardinality(snapshot, label, 7U), std::optional<uint64_t>(3U));
 EXPECT_EQ(reopenedCatalog.readTokenCardinality(snapshot, label, 8U), std::optional<uint64_t>(0U));
 EXPECT_EQ(reopenedCatalog.readTokenCardinality(snapshot, type, 0U), std::optional<uint64_t>(2U));
 EXPECT_NE(SystemCatalogTokenCardinalityCodec::countKey(label, 7U), SystemCatalogTokenCardinalityCodec::countKey(type, 7U));
 EXPECT_THROW(SystemCatalogTokenCardinalityCodec::countKey(label, 0U), FormatException);
 EXPECT_THROW(SystemCatalogTokenCardinalityCodec::completeKey(static_cast<SystemCatalogSchemaTokenKind>(99U)), FormatException);
 snapshot.release();
}

TEST_F(SystemCatalogTest, TokenCardinalityRejectsInvalidCompletenessAndInactiveSessions) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 const auto kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   auto moved = std::move(session);
   EXPECT_THROW(session.markTokenCardinalityComplete(kind), std::logic_error);
   EXPECT_THROW(session.setTokenCardinality(kind, 7U, 1U), std::logic_error);
   EXPECT_THROW(moved.setTokenCardinality(kind, 7U, std::numeric_limits<uint64_t>::max()), FormatException);
   moved.markTokenCardinalityComplete(kind); moved.finish(); ASSERT_TRUE(tx.commit().committed()); }
 { auto tx = planner.beginTransaction(); tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
   SystemCatalogTokenCardinalityCodec::completeKey(kind), 2U); ASSERT_TRUE(tx.commit().committed()); }
 auto snapshot = metadata_->openSnapshot();
 EXPECT_THROW(catalog.readTokenCardinality(snapshot, kind, 7U), FormatException);
 const std::vector<uint64_t> one{7U};
 EXPECT_THROW(static_cast<void>(catalog.readTokenCardinalities(snapshot, kind, one)), FormatException); snapshot.release();
}

TEST_F(SystemCatalogTest, CombinedPlanningEstimateRequiresCompleteCountsAndTotalsInItsSnapshot) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
 auto old = metadata_->openSnapshot();
 EXPECT_FALSE(catalog.readTokenCardinalityEstimate(old, kind, 0U));
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.setTokenCardinality(kind, 0U, 3U); session.markTokenCardinalityComplete(kind);
   session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::nodeCountKey());
   tx.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::edgeCountKey());
   ASSERT_TRUE(tx.commit().committed()); }
 { auto incomplete = metadata_->openSnapshot();
   EXPECT_FALSE(catalog.readTokenCardinalityEstimate(incomplete, kind, 0U)); }
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.setGraphCardinalityStatistics({2U, 3U}); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 { auto snapshot = metadata_->openSnapshot();
   const auto estimate = catalog.readTokenCardinalityEstimate(snapshot, kind, 0U); ASSERT_TRUE(estimate);
   EXPECT_EQ(estimate->first.node_count, 2U); EXPECT_EQ(estimate->first.edge_count, 3U);
   EXPECT_EQ(estimate->second, 3U);
   EXPECT_EQ(catalog.readTokenCardinalityEstimate(snapshot, kind, 9U)->second, 0U); }
 EXPECT_FALSE(catalog.readTokenCardinalityEstimate(old, kind, 0U)); old.release();
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID, SystemCatalogGraphCardinalityCodec::edgeCountKey());
   ASSERT_TRUE(tx.commit().committed()); }
 { auto snapshot = metadata_->openSnapshot();
   EXPECT_THROW(catalog.readTokenCardinalityEstimate(snapshot, kind, 0U), FormatException); }
}

TEST_F(SystemCatalogTest, BatchedGraphCountsStillRejectMissingAndInvalidBootstrap) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.setGraphCardinalityStatistics({2U, 3U}); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 auto valid = metadata_->openSnapshot();
 EXPECT_EQ(catalog.readGraphCardinalityStatistics(valid),
           std::optional<NativeGraphCardinalityStatistics>({2U, 3U}));
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
                            SystemCatalogTokenKeyCodec::bootstrapKey(), 2U);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto invalid = metadata_->openSnapshot();
   EXPECT_THROW(catalog.readGraphCardinalityStatistics(invalid), FormatException);
   EXPECT_THROW(catalog.readTokenCardinalityEstimate(invalid, SystemCatalogSchemaTokenKind::SCSTK_LABEL, 1U), FormatException);
   EXPECT_THROW(catalog.readTokenCardinality(invalid, SystemCatalogSchemaTokenKind::SCSTK_LABEL, 1U), FormatException);
   const std::vector<uint64_t> one{1U};
   EXPECT_THROW(static_cast<void>(catalog.readTokenCardinalities(invalid, SystemCatalogSchemaTokenKind::SCSTK_LABEL, one)), FormatException); }
 EXPECT_EQ(catalog.readGraphCardinalityStatistics(valid),
           std::optional<NativeGraphCardinalityStatistics>({2U, 3U}));
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
                           SystemCatalogTokenKeyCodec::bootstrapKey());
   ASSERT_TRUE(tx.commit().committed()); }
 { auto missing = metadata_->openSnapshot();
   EXPECT_THROW(catalog.readGraphCardinalityStatistics(missing), FormatException);
   EXPECT_THROW(catalog.readTokenCardinalityEstimate(missing, SystemCatalogSchemaTokenKind::SCSTK_LABEL, 1U), FormatException);
   EXPECT_THROW(catalog.readTokenCardinality(missing, SystemCatalogSchemaTokenKind::SCSTK_LABEL, 1U), FormatException);
   const std::vector<uint64_t> one{1U};
   EXPECT_THROW(static_cast<void>(catalog.readTokenCardinalities(missing, SystemCatalogSchemaTokenKind::SCSTK_LABEL, one)), FormatException); }
 valid.release();
}

TEST_F(SystemCatalogTest, BatchedTokenCountsDecodeOnlyWhenStatisticsAreComplete) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 const auto kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.markTokenCardinalityComplete(kind); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
                            SystemCatalogTokenCardinalityCodec::countKey(kind, 7U), 0U);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto invalid = metadata_->openSnapshot();
   EXPECT_THROW(catalog.readTokenCardinality(invalid, kind, 7U), FormatException);
   const std::vector<uint64_t> one{7U};
   EXPECT_THROW(static_cast<void>(catalog.readTokenCardinalities(invalid, kind, one)), FormatException); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexErase(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
                           SystemCatalogTokenCardinalityCodec::completeKey(kind));
   ASSERT_TRUE(tx.commit().committed()); }
 { auto partial = metadata_->openSnapshot();
   EXPECT_EQ(catalog.readTokenCardinality(partial, kind, 7U), std::nullopt);
   const std::vector<uint64_t> one{7U};
   EXPECT_EQ(catalog.readTokenCardinalities(partial, kind, one), (std::vector<std::optional<uint64_t>>{std::nullopt})); }
}

TEST_F(SystemCatalogTest, TokenCountBatchRequiresSortedInputsAndRetainsCompletenessAndOldSnapshot) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 const auto kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
 const std::vector<uint64_t> tokens{7U, 8U, 9U};
 auto before = metadata_->openSnapshot();
 EXPECT_EQ(catalog.readTokenCardinalities(before, kind, tokens), (std::vector<std::optional<uint64_t>>(3U)));
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.setTokenCardinality(kind, 7U, 3U); session.setTokenCardinality(kind, 9U, 1U);
   session.markTokenCardinalityComplete(kind);
   session.setTokenCardinality(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, 0U, 2U);
   session.markTokenCardinalityComplete(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 auto complete = metadata_->openSnapshot();
 const std::vector<uint64_t> types{0U, 9U};
 EXPECT_EQ(catalog.readTokenCardinalities(complete, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, types),
           (std::vector<std::optional<uint64_t>>{2U, 0U}));
 EXPECT_EQ(catalog.readTokenCardinalities(complete, kind, tokens), (std::vector<std::optional<uint64_t>>{3U, 0U, 1U}));
 EXPECT_EQ(catalog.readTokenCardinalities(before, kind, tokens), (std::vector<std::optional<uint64_t>>(3U)));
 const std::vector<uint64_t> duplicate{7U, 7U}, descending{9U, 7U}, zero{0U};
 EXPECT_THROW(static_cast<void>(catalog.readTokenCardinalities(complete, kind, duplicate)), FormatException);
 EXPECT_THROW(static_cast<void>(catalog.readTokenCardinalities(complete, kind, descending)), FormatException);
 EXPECT_THROW(static_cast<void>(catalog.readTokenCardinalities(complete, kind, zero)), FormatException);
 EXPECT_TRUE(catalog.readTokenCardinalities(complete, kind, {}).empty());
 before.release(); complete.release();
}

TEST_F(SystemCatalogTest, SchemaNameBatchRetainsInputOrderKindBoundariesAndOldSnapshot) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 const auto kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
 const std::vector<std::string> names{"A", "Missing", "Z", "\xE4\xB8\xAD\xE6\x96\x87"};
 auto before = metadata_->openSnapshot();
 EXPECT_EQ(catalog.findSchemaTokens(before, kind, names), (std::vector<std::optional<uint64_t>>(4U)));
 uint64_t a, z, unicode, type;
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   type = session.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, "A");
   a = session.ensureSchemaToken(kind, "A"); z = session.ensureSchemaToken(kind, "Z");
   unicode = session.ensureSchemaToken(kind, "\xE4\xB8\xAD\xE6\x96\x87"); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 auto current = metadata_->openSnapshot();
 EXPECT_EQ(catalog.findSchemaTokens(current, kind, names), (std::vector<std::optional<uint64_t>>{a, std::nullopt, z, unicode}));
 EXPECT_EQ(catalog.findSchemaTokens(current, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, names),
           (std::vector<std::optional<uint64_t>>{type, std::nullopt, std::nullopt, std::nullopt}));
 EXPECT_EQ(catalog.findSchemaTokens(before, kind, names), (std::vector<std::optional<uint64_t>>(4U)));
 const std::vector<std::string> duplicate{"A", "A"}, descending{"Z", "A"}, empty{""};
 EXPECT_THROW(static_cast<void>(catalog.findSchemaTokens(current, kind, duplicate)), FormatException);
 EXPECT_THROW(static_cast<void>(catalog.findSchemaTokens(current, kind, descending)), FormatException);
 EXPECT_THROW(static_cast<void>(catalog.findSchemaTokens(current, kind, empty)), FormatException);
 EXPECT_TRUE(catalog.findSchemaTokens(current, kind, {}).empty());
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
                            SystemCatalogTokenKeyCodec::schemaNameKey(kind, "A"), 0U);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto zero = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.findSchemaTokens(zero, kind, names)), FormatException); }
 { auto tx = planner.beginTransaction();
   tx.stageScalarIndexUpsert(indexes, RSS_SYSTEM_CATALOG_INDEX_ID,
                            SystemCatalogTokenKeyCodec::bootstrapKey(), 2U);
   ASSERT_TRUE(tx.commit().committed()); }
 { auto badBootstrap = metadata_->openSnapshot();
   EXPECT_THROW(static_cast<void>(catalog.findSchemaTokens(badBootstrap, kind, names)), FormatException); }
 EXPECT_EQ(catalog.findSchemaTokens(current, kind, names), (std::vector<std::optional<uint64_t>>{a, std::nullopt, z, unicode}));
 before.release(); current.release();
}

TEST_F(SystemCatalogTest, CombinedPlanningEstimateSharesOneLeafAcrossAuthenticatedStatistics) {
 BTreePageStore indexes(*slotted_pages_, *metadata_); SystemCatalog catalog(indexes, *metadata_);
 NativeGraphWritePlanner planner(*metadata_, *entities_, *labels_, *properties_, indexes, systemCatalogFixturePlannerOptions());
 const auto kind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
 auto old = metadata_->openSnapshot();
 { auto tx = planner.beginTransaction(); auto session = catalog.beginWrite(tx);
   session.setGraphCardinalityStatistics({7U, 3U}); session.setTokenCardinality(kind, 1U, 7U);
   session.markTokenCardinalityComplete(kind); session.finish(); ASSERT_TRUE(tx.commit().committed()); }
 auto current = metadata_->openSnapshot();
 struct ResetTrace { ~ResetTrace() { graph::debug::PerfTrace::setValueCollectionEnabled(false); graph::debug::PerfTrace::reset(); } } reset;
 graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setValueCollectionEnabled(true);
 const auto estimate = catalog.readTokenCardinalityEstimate(current, kind, 1U);
 const auto trace = graph::debug::PerfTrace::snapshotAndReset();
 ASSERT_TRUE(estimate);
 EXPECT_EQ(estimate->first.node_count, 7U); EXPECT_EQ(estimate->first.edge_count, 3U);
 EXPECT_EQ(estimate->second, 7U);
 ASSERT_TRUE(trace.contains("native_btree.read.packed_page_decodes"));
 // One exact probe currently decodes its root once during authentication
 // and once when extracting entries; five separate probes decoded ten times.
 EXPECT_EQ(trace.at("native_btree.read.packed_page_decodes").totalValue, 2);
 EXPECT_FALSE(catalog.readTokenCardinalityEstimate(old, kind, 1U));
 EXPECT_EQ(catalog.readTokenCardinalityEstimate(current, kind, 2U)->second, 0U);
}
