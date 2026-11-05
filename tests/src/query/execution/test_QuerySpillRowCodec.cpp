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

TEST(QuerySpillRowCodecTest, ReusableEncodingPreservesCanonicalBytesAndReleasesBorrowedNames) {
    struct Requests final : std::pmr::memory_resource {
        size_t allocations = 0U, live = 0U;
        void *do_allocate(size_t n,size_t a) override { auto *p=std::pmr::new_delete_resource()->allocate(n,a);++allocations;live+=n;return p; }
        void do_deallocate(void *p,size_t n,size_t a) override { live-=n;std::pmr::new_delete_resource()->deallocate(p,n,a); }
        bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this==&other; }
    } requests;
    {
        QuerySpillRowCodec::Record record;
        record.row.values.emplace("later", PropertyValue(2));
        record.row.values.emplace("first", PropertyValue(std::string(200U,'a')));
        record.keys={PropertyValue(5)};
        std::vector<std::byte> encoded;
        std::pmr::vector<const QuerySpillRowCodec::Binding *> names(&requests);
        QuerySpillRowCodec::encodeInto(record,4096U,encoded,names);
        const auto *storage=encoded.data(); const auto allocated=requests.allocations;
        for (size_t i=0U;i<1000U;++i) {
            record.ordinal=i;
            QuerySpillRowCodec::encodeInto(record,4096U,encoded,names);
            EXPECT_EQ(encoded,QuerySpillRowCodec::encode(record,4096U));
            EXPECT_EQ(encoded.data(),storage);
            EXPECT_TRUE(names.empty()); // no record/key borrow survives this call
        }
        EXPECT_EQ(requests.allocations,allocated);
        const auto saved=encoded;
        EXPECT_THROW(QuerySpillRowCodec::encodeInto(record,1U,encoded,names),std::runtime_error);
        EXPECT_EQ(encoded,saved); EXPECT_TRUE(names.empty());
    }
    EXPECT_EQ(requests.live,0U);
}

TEST(QuerySpillRowCodecTest, ReusableDecodeOwnsChangingRowsAndDropsStaleBindingsOnSuccess) {
    QuerySpillRowCodec::Record scratch;
    std::pmr::vector<std::string_view> names;
    QuerySpillRowCodec::Record input;
    input.row.values.emplace("a",PropertyValue(std::string(200U,'a')));
    input.row.values.emplace("b",storage::EntityRef{7,false});
    input.keys={PropertyValue(1)};
    auto bytes=QuerySpillRowCodec::encode(input,4096U);
    QuerySpillRowCodec::decodeInto(bytes,4096U,scratch,names);
    EXPECT_EQ(scratch.row,input.row); EXPECT_EQ(scratch.keys,input.keys); EXPECT_TRUE(names.empty());
    for(size_t i=0;i<100U;++i) {
        input.row.values.erase("b");input.row.values.insert_or_assign("a",PropertyValue(static_cast<int64_t>(i)));
        input.row.values.insert_or_assign("z",PropertyValue(std::string(100U,char('a'+i%26U))));
        input.keys={PropertyValue(i%2U==0U)};input.ordinal=i;
        bytes=QuerySpillRowCodec::encode(input,4096U);
        QuerySpillRowCodec::decodeInto(bytes,4096U,scratch,names);
        EXPECT_EQ(scratch.row,input.row); EXPECT_EQ(scratch.keys,input.keys); EXPECT_EQ(scratch.ordinal,input.ordinal);
        EXPECT_FALSE(scratch.row.values.contains("b")); EXPECT_TRUE(names.empty());
    }
    bytes.clear(); EXPECT_EQ(std::get<PropertyValue>(scratch.row.values.at("z")),PropertyValue(std::string(100U,'v')));
    auto malformed=QuerySpillRowCodec::encode(input,4096U);malformed.push_back(std::byte{0});
    EXPECT_THROW(QuerySpillRowCodec::decodeInto(malformed,4096U,scratch,names),std::invalid_argument);
    EXPECT_TRUE(names.empty());
    input.row.values.clear();input.keys.clear();bytes=QuerySpillRowCodec::encode(input,4096U);
    QuerySpillRowCodec::decodeInto(bytes,4096U,scratch,names);
    EXPECT_TRUE(scratch.row.values.empty()); EXPECT_TRUE(scratch.keys.empty()); EXPECT_TRUE(names.empty());
}
