#pragma once
#include <cmath>
#include <functional>
#include <memory>
#include <memory_resource>
#include <optional>
#include "graph/query/execution/BoundedBatch.hpp"
#include "graph/storage/LogicalQueryScratch.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "query/execution/BudgetMemoryResource.hpp"
#include "query/execution/OrderByComparison.hpp"
#include "query/execution/QuerySpillRowCodec.hpp"
namespace graph::query::execution::detail {
// Bounded two-way external merge. Unsupported rows/NaN ordering keep the
// in-memory path; if that exceeds memory, fail explicitly instead of replaying
// already consumed source rows. Every disk read remains query-owner scoped.
class ExternalQuerySort final {
public:
	using Record=QuerySpillRowCodec::Record;
	using SizeFunction=std::function<size_t(const Record &)>;
	using Visitor=std::function<bool(Record,MaterializedByteReservation)>;
	static constexpr size_t MAX_RECORD_BYTES=4096U;
	static constexpr size_t WORKSPACE_BYTES=6U*storage::RSS_SEGMENT_SIZE+6U*MAX_RECORD_BYTES*64U;
	static constexpr size_t MINIMUM_QUERY_BYTES=4U*1024U*1024U;
	ExternalQuerySort(storage::LogicalQueryScratchProvider &provider,MaterializedByteBudget &budget,
		size_t maximumRows,size_t maximumBytes,std::span<const uint8_t> ascending,SizeFunction size,
		std::function<void()> checkpoint,size_t maximumDiskBytes=128U*1024U*1024U,size_t maximumFragments=1024U,std::function<void()> publicationCheckpoint={},std::shared_ptr<ResourceAccount> account={})
		:provider_(provider),budget_(budget),maximum_rows_(maximumRows),buffer_target_(maximumBytes/8U),
		workspace_(&budget),memory_(budget),ascending_(ascending.begin(),ascending.end(),&memory_),size_(std::move(size)),checkpoint_(std::move(checkpoint)),publication_checkpoint_(publicationCheckpoint?std::move(publicationCheckpoint):checkpoint_),
		maximum_disk_(maximumDiskBytes),maximum_fragments_(maximumFragments),account_(std::move(account)),
		runs_(&memory_),rows_(maximumRows,maximumBytes,size_,&budget,true,sizeof(LogicalPlanReadRow)) {
		if(maximumBytes<MINIMUM_QUERY_BYTES || ascending_.empty() || !checkpoint_)
			throw std::invalid_argument("external Sort requires ordering, guards and bounded workspace");
	}
	void append(Record record,MaterializedByteReservation charge) {
		checkpoint_();if(finished_ || total_==maximum_rows_)throw std::runtime_error("external Sort exceeded its input row bound");
		if(record.keys.size()!=ascending_.size())throw std::invalid_argument("external Sort key width changed");
		record.ordinal=total_++;
		const bool eligible=qualified(record);
		if(!eligible && !runs_.empty())throw std::runtime_error("external Sort encountered an unsupported spill row after sealing runs");
		spillable_=spillable_ && eligible;
		if(spillable_ && !rows_.empty() && rows_.bytes()+size_(record)>buffer_target_)flush();
		if(charge.bytes())rows_.appendCharged(std::move(record),std::move(charge));else rows_.append(std::move(record));
	}
	bool finish(const Visitor &visitor) {
		if(finished_ || !visitor)throw std::logic_error("external Sort finish is invalid");finished_=true;
		if(runs_.empty()) {
			sortRows();std::reverse(rows_.values().begin(),rows_.values().end());
			while(!rows_.empty()){checkpoint_();auto row=rows_.popBackCharged();if(!visitor(std::move(row.value),std::move(row.reservation)))return false;}
			return true;
		}
		if(!rows_.empty())flush();
		for(size_t level=0;runs_.size()>1U;++level){
			if(level==12U)throw std::runtime_error("external Sort exceeded its merge-depth bound");
			std::pmr::vector<Run> next(&memory_);next.reserve((runs_.size()+1U)/2U);
			for(size_t i=0;i<runs_.size();i+=2U){checkpoint_();if(i+1U==runs_.size())next.push_back(std::move(runs_[i]));else next.push_back(merge(runs_[i],runs_[i+1U]));}
			runs_=std::move(next);
		}
		Cursor cursor(*this,runs_.front());
		while(cursor.current){checkpoint_();auto record=std::move(*cursor.current);record.keys.clear();record.keys.shrink_to_fit();
			const auto bytes=size_(record);if(bytes>workspace_.bytes())throw std::runtime_error("external Sort decoded output exceeded workspace");
			auto charge=workspace_.splitOff(bytes);cursor.current.reset();
			if(!visitor(std::move(record),std::move(charge))){scratch_->finish();return false;}
			workspace_.set(WORKSPACE_BYTES);cursor.advance();
		}
		scratch_->finish();return true;
	}
	[[nodiscard]] bool spilled() const {return scratch_!=nullptr;}
private:
	struct Run {
		explicit Run(std::pmr::memory_resource *resource):handles(resource){}
		std::pmr::vector<uint64_t> handles;uint64_t rows=0U;
	};
	static constexpr uint64_t BLOCK_MAGIC=UINT64_C(0x3154525358595A);
	static void put(std::vector<std::byte> &bytes,uint64_t value,size_t width){for(size_t i=0;i<width;++i)bytes.push_back(static_cast<std::byte>((value>>(8*i))&255U));}
	static uint64_t get(std::span<const std::byte> bytes,size_t &offset,size_t width){if(offset>bytes.size() || width>bytes.size()-offset)throw std::invalid_argument("external Sort block is truncated");uint64_t value=0;for(size_t i=0;i<width;++i)value|=uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(8*i);offset+=width;return value;}
	struct Writer {
		ExternalQuerySort &sort;Run run;std::vector<std::byte> block;uint32_t count=0;
		explicit Writer(ExternalQuerySort &owner):sort(owner),run(&owner.memory_){block.reserve(storage::RSS_SEGMENT_SIZE-storage::RSS_ENVELOPE_BYTES);start();}
		void start(){block.clear();put(block,BLOCK_MAGIC,8);put(block,0,4);put(block,0,4);count=0;}
		void flush(){if(!count)return;for(size_t i=0;i<4;++i)block[8+i]=static_cast<std::byte>((count>>(8*i))&255U);
			// Reserve route capacity before publishing private bytes.
			if(run.handles.size()==run.handles.capacity())run.handles.reserve(std::max<size_t>(1,run.handles.size()*2));
			sort.publication_checkpoint_();run.handles.push_back(sort.scratch_->write(block));start();}
		void append(const Record &record){sort.checkpoint_();auto bytes=QuerySpillRowCodec::encode(record,MAX_RECORD_BYTES,&sort.memory_);
			if(block.size()+4U+bytes.size()>sort.scratch_->maximumFragmentBytes())flush();put(block,bytes.size(),4);block.insert(block.end(),bytes.begin(),bytes.end());++count;++run.rows;}
		Run finish(){flush();return std::move(run);}
	};
	struct Cursor {
		ExternalQuerySort &sort;Run &run;size_t handle=0,offset=0;uint32_t remaining=0;std::vector<std::byte> block;std::optional<Record> current;std::optional<Record> prior;uint64_t seen=0U;
		Cursor(ExternalQuerySort &owner,Run &input):sort(owner),run(input){advance();}
		void advance(){sort.checkpoint_();current.reset();
			if(remaining==0){if(!block.empty() && offset!=block.size())throw std::invalid_argument("external Sort block has trailing bytes");
				if(handle==run.handles.size()){if(seen!=run.rows)throw std::invalid_argument("external Sort run cardinality changed");block.clear();return;}
				block=sort.scratch_->read(run.handles[handle++]);offset=0;
				if(get(block,offset,8)!=BLOCK_MAGIC)throw std::invalid_argument("external Sort block magic/version is invalid");
				remaining=static_cast<uint32_t>(get(block,offset,4));if(get(block,offset,4)!=0U || remaining==0 || remaining>(block.size()-offset)/(4U+QuerySpillRowCodec::HEADER_BYTES))throw std::invalid_argument("external Sort block count is invalid");
			}
			const auto bytes=get(block,offset,4);if(bytes>MAX_RECORD_BYTES || bytes>block.size()-offset)throw std::invalid_argument("external Sort record is oversized or truncated");
			current=QuerySpillRowCodec::decode(std::span(block).subspan(offset,static_cast<size_t>(bytes)),MAX_RECORD_BYTES);offset+=bytes;--remaining;
			if(current->keys.size()!=sort.ascending_.size() || current->ordinal>=sort.total_ || !qualified(*current) || sort.size_(*current)>MAX_RECORD_BYTES*64U)
				throw std::invalid_argument("external Sort decoded record failed its layout/ownership bound");
			if(prior && (sort.precedes(*current,*prior) || prior->ordinal==current->ordinal))throw std::invalid_argument("external Sort run is unordered or has duplicate adjacent ordinals");
			prior.reset();prior.emplace();prior->keys=current->keys;prior->ordinal=current->ordinal;++seen;
		}
	};
	static bool hasNaN(const PropertyValue &value){const auto &variant=value.getVariant();if(const auto *number=std::get_if<double>(&variant))return std::isnan(*number);if(const auto *list=std::get_if<std::vector<PropertyValue>>(&variant))return std::ranges::any_of(*list,hasNaN);if(const auto *map=std::get_if<PropertyValue::MapType>(&variant))return std::ranges::any_of(*map,[](const auto &entry){return hasNaN(entry.second);});return false;}
	static bool qualified(const Record &record){if(!record.row.path_cache.empty() || record.row.values.size()>4096U || record.keys.size()>4096U || std::ranges::any_of(record.keys,hasNaN))return false;
		for(const auto &[name,value]:record.row.values)if(!std::holds_alternative<PropertyValue>(value) && !std::holds_alternative<storage::EntityRef>(value))return false;
		return QuerySpillRowCodec::encodedSize(record)<=MAX_RECORD_BYTES;
	}
	bool precedes(const Record &left,const Record &right) const {
		checkpoint_();for(size_t i=0;i<ascending_.size();++i){if(left.keys[i]==right.keys[i])continue;
			if(orderByPrecedes(left.keys[i],right.keys[i],ascending_[i]!=0))return true;
			if(orderByPrecedes(right.keys[i],left.keys[i],ascending_[i]!=0))return false;
			return left.ordinal<right.ordinal;}
		return left.ordinal<right.ordinal;
	}
	void sortRows(){if(rows_.size()<2U)return;
		if(spillable_)std::sort(rows_.values().begin(),rows_.values().end(),[&](const auto &a,const auto &b){return precedes(a,b);});
		else {MaterializedByteReservation temporary(&budget_);temporary.set(rows_.size()*sizeof(Record));std::stable_sort(rows_.values().begin(),rows_.values().end(),[&](const auto &a,const auto &b){checkpoint_();for(size_t i=0;i<ascending_.size();++i)if(a.keys[i]!=b.keys[i])return orderByPrecedes(a.keys[i],b.keys[i],ascending_[i]!=0);return false;});}
	}
	void ensureScratch(){if(scratch_)return;workspace_.set(WORKSPACE_BYTES);publication_checkpoint_();scratch_=provider_.openQueryScratchSession(maximum_disk_,maximum_fragments_,account_);if(!scratch_)throw std::runtime_error("external Sort cannot spill to a physically read-only file");}
	void flush(){if(!spillable_)throw std::runtime_error("external Sort has no qualified spill codec for its input");ensureScratch();sortRows();Writer writer(*this);
		for(const auto &record:rows_.values())writer.append(record);auto run=writer.finish();if(runs_.size()==4096U)throw std::runtime_error("external Sort exceeded its run bound");runs_.push_back(std::move(run));rows_.clear();}
	Run merge(Run &left,Run &right){Cursor a(*this,left),b(*this,right);Writer writer(*this);
		while(a.current || b.current){checkpoint_();if(!b.current || (a.current && precedes(*a.current,*b.current))){writer.append(*a.current);a.advance();}else{writer.append(*b.current);b.advance();}}
		auto result=writer.finish();for(const auto handle:left.handles)scratch_->release(handle);for(const auto handle:right.handles)scratch_->release(handle);return result;}
	storage::LogicalQueryScratchProvider &provider_;MaterializedByteBudget &budget_;size_t maximum_rows_,buffer_target_,total_=0;
	MaterializedByteReservation workspace_;BudgetMemoryResource memory_;std::pmr::vector<uint8_t> ascending_;
	SizeFunction size_;std::function<void()> checkpoint_,publication_checkpoint_;size_t maximum_disk_,maximum_fragments_;std::shared_ptr<ResourceAccount> account_;std::pmr::vector<Run> runs_;BoundedBatch<Record> rows_;
	std::unique_ptr<storage::LogicalQueryScratchSession> scratch_;bool spillable_=true,finished_=false;
};
}
