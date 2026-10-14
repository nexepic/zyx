#include <gtest/gtest.h>
#include <map>
#include <numeric>
#include "query/execution/ExternalQuerySort.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"
using namespace graph;
using namespace graph::query::execution;
using namespace graph::query::execution::detail;
namespace {
struct ScratchState {std::map<uint64_t,std::vector<std::byte>> bytes;uint64_t next=1;size_t opened=0,writes=0,finished=0;bool readonly=false,corrupt=false;};
class Scratch final:public storage::LogicalQueryScratchSession {
public:
	Scratch(ScratchState &state,size_t bytes,size_t fragments):state_(state),maximum_(bytes/storage::RSS_SEGMENT_SIZE),fragments_(fragments){++state_.opened;}
	~Scratch(){finish();}
	uint64_t write(std::span<const std::byte> bytes) override {if(state_.bytes.size()>=maximum_ || state_.bytes.size()>=fragments_)throw std::runtime_error("disk exhausted");const auto id=state_.next++;state_.bytes.emplace(id,std::vector<std::byte>(bytes.begin(),bytes.end()));++state_.writes;return id;}
	std::vector<std::byte> read(uint64_t id) override {auto result=state_.bytes.at(id);if(state_.corrupt)for(size_t i=8;i<12;++i)result[i]=std::byte{0xFF};return result;}
	void release(uint64_t id) override {if(state_.bytes.erase(id)!=1)throw std::logic_error("foreign handle");}
	void finish() override {if(!finished_){state_.bytes.clear();++state_.finished;finished_=true;}}
	size_t maximumFragmentBytes() const override{return storage::RSS_SEGMENT_SIZE-storage::RSS_ENVELOPE_BYTES;}
private:ScratchState &state_;size_t maximum_,fragments_;bool finished_=false;
};
class Provider final:public storage::LogicalQueryScratchProvider {public:mutable ScratchState state;std::unique_ptr<storage::LogicalQueryScratchSession> openQueryScratchSession(size_t bytes,size_t fragments) const override {if(state.readonly)return nullptr;return std::make_unique<Scratch>(state,bytes,fragments);}};
size_t size(const QuerySpillRowCodec::Record &record){size_t bytes=sizeof(record)+record.row.values.bucket_count()*sizeof(void *)+record.keys.capacity()*sizeof(PropertyValue);for(const auto &[name,value]:record.row.values){bytes+=sizeof(decltype(record.row.values)::value_type)+32U+name.capacity();if(const auto *primitive=std::get_if<PropertyValue>(&value))bytes+=propertyHeapBufferBytes(*primitive);}for(const auto &key:record.keys)bytes+=propertyHeapBufferBytes(key);return bytes;}
QuerySpillRowCodec::Record row(size_t id){QuerySpillRowCodec::Record record;record.row.values.emplace("id",PropertyValue(static_cast<int64_t>(id)));record.keys={id%17==0?PropertyValue():PropertyValue(static_cast<int64_t>(id%11))};return record;}
}
TEST(ExternalQuerySortTest, MemoryPathPreservesStableNullOrderWithoutOpeningScratch) {
	Provider provider;MaterializedByteBudget budget(4U*1024U*1024U);const std::array<uint8_t,1> ascending{1};
	{ExternalQuerySort sort(provider,budget,100,4U*1024U*1024U,ascending,size,[]{});for(size_t i=0;i<30;++i)sort.append(row(i),MaterializedByteReservation(&budget));
	std::vector<size_t> actual;EXPECT_TRUE(sort.finish([&](auto record,auto charge){actual.push_back(static_cast<size_t>(std::get<int64_t>(std::get<PropertyValue>(record.row.values.at("id")).getVariant())));return true;}));
	std::vector<size_t> expected(30);std::iota(expected.begin(),expected.end(),0);std::stable_sort(expected.begin(),expected.end(),[](auto a,auto b){const auto x=row(a).keys[0],y=row(b).keys[0];return orderByPrecedes(x,y,true);});EXPECT_EQ(actual,expected);EXPECT_EQ(provider.state.opened,0U);}
	EXPECT_EQ(budget.used(),0U);
}
TEST(ExternalQuerySortTest, MultipleRunsMergeCompleteValuesStableTiesAndRefundOwnership) {
	Provider provider;MaterializedByteBudget budget(4U*1024U*1024U);const std::array<uint8_t,1> ascending{1};
	{ExternalQuerySort sort(provider,budget,30000,4U*1024U*1024U,ascending,size,[]{});for(size_t i=0;i<12000;++i)sort.append(row(i),MaterializedByteReservation(&budget));
	std::vector<size_t> actual;EXPECT_TRUE(sort.finish([&](auto record,auto charge){actual.push_back(static_cast<size_t>(std::get<int64_t>(std::get<PropertyValue>(record.row.values.at("id")).getVariant())));return true;}));
	std::vector<size_t> expected(12000);std::iota(expected.begin(),expected.end(),0);std::stable_sort(expected.begin(),expected.end(),[](auto a,auto b){return orderByPrecedes(row(a).keys[0],row(b).keys[0],true);});EXPECT_EQ(actual,expected);EXPECT_TRUE(sort.spilled());EXPECT_GT(provider.state.writes,2U);EXPECT_TRUE(provider.state.bytes.empty());}
	EXPECT_EQ(budget.used(),0U);
}
TEST(ExternalQuerySortTest, CorruptCountsAndPhysicalReadonlyFailWithoutPartialSuccessfulResults) {
	for(bool readonly:{false,true}){Provider provider;provider.state.readonly=readonly;provider.state.corrupt=!readonly;MaterializedByteBudget budget(4U*1024U*1024U);const std::array<uint8_t,1> ascending{1};
		EXPECT_THROW(([&]{ExternalQuerySort sort(provider,budget,30000,4U*1024U*1024U,ascending,size,[]{});for(size_t i=0;i<12000;++i)sort.append(row(i),MaterializedByteReservation(&budget));(void)sort.finish([](auto record,auto charge){return true;});}()),std::exception);
		EXPECT_EQ(budget.used(),0U);EXPECT_TRUE(provider.state.bytes.empty());
	}
}
