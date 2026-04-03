/**
 * @file test_ScanConfigs.cpp
 * @brief Unit tests for scan config helpers.
 **/

#include <gtest/gtest.h>

#include "graph/query/execution/ScanConfigs.hpp"

using namespace graph::query::execution;

TEST(ScanConfigsTest, LabelHelperReturnsFirstOrEmpty) {
	NodeScanConfig cfg;
	EXPECT_EQ(cfg.label(), "");

	cfg.labels = {"Person", "Employee"};
	EXPECT_EQ(cfg.label(), "Person");
}

TEST(ScanConfigsTest, ToStringCoversAllScanTypes) {
	NodeScanConfig fullCfg;
	fullCfg.type = ScanType::FULL_SCAN;
	EXPECT_EQ(fullCfg.toString(), "FullScan");

	NodeScanConfig labelCfg;
	labelCfg.type = ScanType::LABEL_SCAN;
	labelCfg.labels = {"Person", "Employee"};
	EXPECT_EQ(labelCfg.toString(), "LabelScan(Person:Employee)");

	NodeScanConfig propCfg;
	propCfg.type = ScanType::PROPERTY_SCAN;
	propCfg.labels = {"Person"};
	propCfg.indexKey = "name";
	propCfg.indexValue = graph::PropertyValue(std::string("Alice"));
	EXPECT_EQ(propCfg.toString(), "IndexScan(Person, name=Alice)");

	NodeScanConfig rangeCfg;
	rangeCfg.type = ScanType::RANGE_SCAN;
	rangeCfg.labels = {"Person"};
	rangeCfg.indexKey = "age";
	EXPECT_EQ(rangeCfg.toString(), "RangeScan(Person, age range)");

	NodeScanConfig compCfg;
	compCfg.type = ScanType::COMPOSITE_SCAN;
	compCfg.labels = {"Person"};
	compCfg.compositeKeys = {"city", "age"};
	EXPECT_EQ(compCfg.toString(), "CompositeScan(Person, [city,age])");
}
