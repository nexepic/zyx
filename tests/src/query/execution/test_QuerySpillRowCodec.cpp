#include <bit>
#include <cmath>
#include <limits>
#include <gtest/gtest.h>
#include "query/execution/QuerySpillRowCodec.hpp"
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::detail;
TEST(QuerySpillRowCodecTest, PreservesCanonicalTypesNestedValuesReferencesKeysAndOwningLifetime) {
	QuerySpillRowCodec::Record record;record.ordinal=91;record.row.values.emplace("z",PropertyValue(PropertyValue::MapType{{"x",PropertyValue(std::vector<PropertyValue>{PropertyValue(),PropertyValue(7),PropertyValue(std::string("text"))})}}));record.row.values.emplace("n",storage::EntityRef{7,false});record.row.values.emplace("e",storage::EntityRef{8,true,7,9,2});record.keys={PropertyValue(7),PropertyValue(),PropertyValue(-0.0)};
	const auto size=QuerySpillRowCodec::encodedSize(record);auto bytes=QuerySpillRowCodec::encode(record,size);ASSERT_EQ(bytes.size(),size);
	const auto restored=QuerySpillRowCodec::decode(bytes,size);EXPECT_EQ(restored.row,record.row);EXPECT_EQ(restored.ordinal,91U);EXPECT_EQ(restored.keys,record.keys);
	EXPECT_TRUE(std::signbit(std::get<double>(restored.keys[2].getVariant())));
	bytes.clear();EXPECT_EQ(std::get<storage::EntityRef>(restored.row.values.at("e")).entity_id,8U);
}
TEST(QuerySpillRowCodecTest, PreservesNaNBitsWithoutUsingEqualityAsAnOracle) {
	QuerySpillRowCodec::Record record;const auto bits=UINT64_C(0x7FF800000000005A);record.keys={PropertyValue(std::bit_cast<double>(bits))};const auto bytes=QuerySpillRowCodec::encode(record,1024);const auto restored=QuerySpillRowCodec::decode(bytes,1024);EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(restored.keys[0].getVariant())),bits);
}
TEST(QuerySpillRowCodecTest, RejectsShortFramesOversizedCountsTrailingBytesAndUnsupportedOwners) {
	QuerySpillRowCodec::Record record;record.row.values.emplace("x",PropertyValue(7));const auto bytes=QuerySpillRowCodec::encode(record,1024);
	for(size_t n=0;n<bytes.size();++n){SCOPED_TRACE(n);EXPECT_THROW(QuerySpillRowCodec::decode(std::span(bytes).first(n),1024),std::exception);}
	auto count=bytes;for(size_t i=16;i<20;++i)count[i]=std::byte{0xFF};EXPECT_THROW(QuerySpillRowCodec::decode(count,1024),std::invalid_argument);
	auto trailing=bytes;trailing.push_back(std::byte{0});EXPECT_THROW(QuerySpillRowCodec::decode(trailing,1024),std::invalid_argument);
	EXPECT_THROW(QuerySpillRowCodec::encode(record,bytes.size()-1),std::runtime_error);
	record.row.values.emplace("node",storage::LogicalGraphNode{});EXPECT_THROW(QuerySpillRowCodec::encode(record,1024),std::runtime_error);
}
