#pragma once
#include "graph/utils/AdmissionPmrContainers.hpp"
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
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include "graph/debug/ResidentMemoryDiagnostics.hpp"
#endif
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
		:provider_(provider),budget_(budget),maximum_rows_(maximumRows),buffer_target_(maximumBytes>WORKSPACE_BYTES ? (maximumBytes-WORKSPACE_BYTES)/2U : 0U),
		workspace_(&budget),memory_(budget),ascending_(ascending.begin(),ascending.end(),&memory_),size_(std::move(size)),checkpoint_(std::move(checkpoint)),publication_checkpoint_(publicationCheckpoint?std::move(publicationCheckpoint):checkpoint_),
		maximum_disk_(maximumDiskBytes),maximum_fragments_(maximumFragments),account_(std::move(account)),
		runs_(&memory_),rows_(maximumRows,maximumBytes,size_,&budget,true,sizeof(LogicalPlanReadRow)) {
        // Protect the complete merge workspace first, then use at most half
        // the remaining allowance for live runs. The other half covers
        // incoming rows and old/new vector buffers during capacity growth.
        // All allocations/reservations still use the same hard budget.
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
        if(spillable_ && !rows_.empty()) {
            const auto recordBytes=size_(record);
            const auto &storage=rows_.values();
            // The source already owns/charges this row when charge is present.
            // Preserve enough headroom for the first spill workspace and an
            // admitted incoming record; do not discover that shortage only
            // after buffering the row. All actual allocations remain charged.
            size_t required=workspace_.bytes()?0U:WORKSPACE_BYTES+MAX_RECORD_BYTES*64U;
            if(!charge.bytes()) {
                if(recordBytes>SIZE_MAX-required) throw std::overflow_error("Sort append size overflowed");
                required+=recordBytes;
            }
            if(storage.size()==storage.capacity()) {
                // Audited standard/Boost vector growth is at most doubling.
                // Count the complete new buffer while the old one is live.
                if(storage.capacity()>SIZE_MAX/(2U*sizeof(Record))) throw std::overflow_error("Sort capacity overflowed");
                const size_t growth=(storage.capacity()?2U*storage.capacity():1U)*sizeof(Record);
                if(growth>SIZE_MAX-required) throw std::overflow_error("Sort workspace size overflowed");
                required+=growth;
            }
            if(recordBytes>buffer_target_-(std::min)(buffer_target_,rows_.bytes()) ||
                    required>budget_.availableWithParent()) flush();
        }
        if (workspace_.bytes() && rows_.empty() && rows_.values().capacity()) {
            // This check belongs after a possible flush: reusing its buffer
            // must not strand a wider incoming row or a competing live owner.
            // Reclaim before moving input if a bounded record has no room.
            const size_t incoming = charge.bytes() ? 0U : size_(record);
            const size_t available = budget_.availableWithParent();
            if (incoming > available || MAX_RECORD_BYTES * 64U > available - incoming)
                rows_.clear();
        }
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
        // No input run will use this capacity again. Release it before merge
        // and owning output compete for the same unchanged hard allowance.
        rows_.clear();
        tracePhase("sort-before-merge");
		for(size_t level=0;runs_.size()>1U;++level){
			if(level==12U)throw std::runtime_error("external Sort exceeded its merge-depth bound");
            traceMergeProgress("begin", level);
			try {
			utils::AdmissionPmrVector<Run> next(&memory_);next.reserve((runs_.size()+1U)/2U);
			for(size_t i=0;i<runs_.size();i+=2U){checkpoint_();if(i+1U==runs_.size())next.push_back(std::move(runs_[i]));else next.push_back(merge(runs_[i],runs_[i+1U]));}
			runs_=std::move(next);
			} catch (...) { traceMergeProgress("failure", level); throw; }
            traceMergeProgress("end", level);
		}
        tracePhase("sort-before-output");
		Cursor cursor(*this,runs_.front());
		while(cursor.current){checkpoint_();auto record=QuerySpillRowCodec::takeOutputRow(*cursor.current);
			const auto bytes=size_(record);if(bytes>workspace_.bytes())throw std::runtime_error("external Sort decoded output exceeded workspace");
			auto charge=workspace_.splitOff(bytes);
			if(!visitor(std::move(record),std::move(charge))){scratch_->finish();return false;}
			workspace_.set(WORKSPACE_BYTES);cursor.advance();
		}
		scratch_->finish();tracePhase("sort-finished");return true;
	}
	[[nodiscard]] bool spilled() const {return scratch_!=nullptr;}
private:
    void traceMergeProgress(const char *phase, size_t level) noexcept {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
        if (!std::getenv("ZYX_CI_RSS_TRACE")) return;
        static std::atomic<unsigned> reports{0U};
        const auto index = reports.fetch_add(1U, std::memory_order_relaxed);
        if (index >= 24U) {
            if (index == 24U) std::fputs("CI_SORT_MERGE_TRUNCATED process_limit=24\n", stderr);
            return;
        }
        // Logical scratch traffic, not physical IO or allocator attribution.
        // Pair these cumulative counters with nonallocating RSS/ASan snapshots.
        std::fprintf(stderr, "CI_SORT_MERGE phase=%s level=%zu runs=%zu input_rows=%zu decoded=%zu read_fragments=%zu read_payload_bytes=%zu written_fragments=%zu written_payload_bytes=%zu materialized_used=%zu\n",
            phase, level, runs_.size(), total_, decoded_records_, read_fragments_, read_payload_bytes_,
            written_fragments_, written_payload_bytes_, budget_.used());
        std::fflush(stderr);
        tracePhase(phase[0] == 'b' ? "sort-merge-level-begin" :
            phase[0] == 'f' ? "sort-merge-level-failure" : "sort-merge-level-end");
#else
        static_cast<void>(phase); static_cast<void>(level);
#endif
    }
    void traceScratchRead(size_t bytes) noexcept {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
        ++read_fragments_; addTraceBytes(read_payload_bytes_, bytes);
#else
        static_cast<void>(bytes);
#endif
    }
    void traceScratchWrite(size_t bytes) noexcept {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
        ++written_fragments_; addTraceBytes(written_payload_bytes_, bytes);
#else
        static_cast<void>(bytes);
#endif
    }
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
    static void addTraceBytes(size_t &total, size_t bytes) noexcept {
        total = bytes > SIZE_MAX - total ? SIZE_MAX : total + bytes;
    }
    size_t decoded_records_ = 0U, read_fragments_ = 0U, read_payload_bytes_ = 0U;
    size_t written_fragments_ = 0U, written_payload_bytes_ = 0U;
#endif
    static void tracePhase(const char *phase) noexcept {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
        // Fixed phase boundaries only. Do not invoke the allocating ASan
        // statistics printer before a guard's observed rejection.
        debug::traceResidentMemory(phase, false);
#else
        static_cast<void>(phase);
#endif
    }
	struct Run {
		explicit Run(std::pmr::memory_resource *resource):handles(resource){}
		utils::AdmissionPmrVector<uint64_t> handles;uint64_t rows=0U;
	};
    struct WriterBuffers { std::vector<std::byte> block, encoded; };
    WriterBuffers &writerBuffers() {
        // Construct only after the spill workspace is admitted. One writer is
        // active at a time, and these private buffers die before its lease.
        if (!writer_buffers_) writer_buffers_.emplace();
        return *writer_buffers_;
    }
	static constexpr uint64_t BLOCK_MAGIC=UINT64_C(0x3154525358595A);
	static void put(std::vector<std::byte> &bytes,uint64_t value,size_t width){for(size_t i=0;i<width;++i)bytes.push_back(static_cast<std::byte>((value>>(8*i))&255U));}
	static uint64_t get(std::span<const std::byte> bytes,size_t &offset,size_t width){if(offset>bytes.size() || width>bytes.size()-offset)throw std::invalid_argument("external Sort block is truncated");uint64_t value=0;for(size_t i=0;i<width;++i)value|=uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(8*i);offset+=width;return value;}
	struct Writer {
		ExternalQuerySort &sort;Run run;std::vector<std::byte> &block, &encoded;
        uint32_t count=0;
		explicit Writer(ExternalQuerySort &owner):sort(owner),run(&owner.memory_),block(owner.writerBuffers().block),encoded(owner.writerBuffers().encoded){block.reserve(storage::RSS_SEGMENT_SIZE-storage::RSS_ENVELOPE_BYTES);start();}
		void start(){block.clear();put(block,BLOCK_MAGIC,8);put(block,0,4);put(block,0,4);count=0;}
		void flush(){if(!count)return;for(size_t i=0;i<4;++i)block[8+i]=static_cast<std::byte>((count>>(8*i))&255U);
			// Reserve route capacity before publishing private bytes.
			if(run.handles.size()==run.handles.capacity())run.handles.reserve(std::max<size_t>(1,run.handles.size()*2));
			sort.publication_checkpoint_();run.handles.push_back(sort.scratch_->write(block));sort.traceScratchWrite(block.size());start();}
        void append(const Record &record){sort.checkpoint_();QuerySpillRowCodec::encodeInto(record,MAX_RECORD_BYTES,encoded);
            // One <=MAX_RECORD_BYTES encoded buffer remains within the existing
            // fixed spill workspace; binding order needs no separate scratch.
			if(block.size()+4U+encoded.size()>sort.scratch_->maximumFragmentBytes())flush();put(block,encoded.size(),4);block.insert(block.end(),encoded.begin(),encoded.end());++count;++run.rows;}
		Run finish(){flush();return std::move(run);}
	};
	struct Cursor {
		ExternalQuerySort &sort;Run &run;size_t handle=0,offset=0;uint32_t remaining=0;std::vector<std::byte> block;std::optional<Record> current;std::optional<Record> prior;uint64_t seen=0U;
		Cursor(ExternalQuerySort &owner,Run &input):sort(owner),run(input){advance();}
		void advance(){sort.checkpoint_();
			if(remaining==0){if(!block.empty() && offset!=block.size())throw std::invalid_argument("external Sort block has trailing bytes");
				if(handle==run.handles.size()){if(seen!=run.rows)throw std::invalid_argument("external Sort run cardinality changed");current.reset();block.clear();return;}
				block=sort.scratch_->read(run.handles[handle++]);offset=0;
                sort.traceScratchRead(block.size());
				if(get(block,offset,8)!=BLOCK_MAGIC)throw std::invalid_argument("external Sort block magic/version is invalid");
				remaining=static_cast<uint32_t>(get(block,offset,4));if(get(block,offset,4)!=0U || remaining==0 || remaining>(block.size()-offset)/(4U+QuerySpillRowCodec::HEADER_BYTES))throw std::invalid_argument("external Sort block count is invalid");
			}
			const auto bytes=get(block,offset,4);if(bytes>MAX_RECORD_BYTES || bytes>block.size()-offset)throw std::invalid_argument("external Sort record is oversized or truncated");
			if (!current) current.emplace();
            QuerySpillRowCodec::decodeInto(std::span(block).subspan(offset,static_cast<size_t>(bytes)),MAX_RECORD_BYTES,*current);offset+=bytes;--remaining;
			if(current->keys.size()!=sort.ascending_.size() || current->ordinal>=sort.total_ || !qualified(*current) || sort.size_(*current)>MAX_RECORD_BYTES*64U)
				throw std::invalid_argument("external Sort decoded record failed its layout/ownership bound");
			if(prior && (sort.precedes(*current,*prior) || prior->ordinal==current->ordinal))throw std::invalid_argument("external Sort run is unordered or has duplicate adjacent ordinals");
			if (!prior) prior.emplace();
            prior->keys=current->keys;prior->ordinal=current->ordinal;++seen;
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
            ++sort.decoded_records_;
#endif
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
	void ensureScratch(){if(scratch_)return;tracePhase("sort-before-first-spill");workspace_.set(WORKSPACE_BYTES);publication_checkpoint_();scratch_=provider_.openQueryScratchSession(maximum_disk_,maximum_fragments_,account_);if(!scratch_)throw std::runtime_error("external Sort cannot spill to a physically read-only file");}
	void flush(){if(!spillable_)throw std::runtime_error("external Sort has no qualified spill codec for its input");ensureScratch();sortRows();Writer writer(*this);
		for(const auto &record:rows_.values())writer.append(record);auto run=writer.finish();if(runs_.size()==4096U)throw std::runtime_error("external Sort exceeded its run bound");runs_.push_back(std::move(run));
        // Payloads die before their grants are reconciled. The PMR array stays
        // live and fully charged, avoiding repeated geometric allocations for
        // subsequent runs without pretending that retained capacity is free.
        rows_.values().clear();rows_.recalculateBytes();
        if(runs_.size()==1U)tracePhase("sort-after-first-spill");}
	Run merge(Run &left,Run &right){Cursor a(*this,left),b(*this,right);Writer writer(*this);
		while(a.current || b.current){checkpoint_();if(!b.current || (a.current && precedes(*a.current,*b.current))){writer.append(*a.current);a.advance();}else{writer.append(*b.current);b.advance();}}
		auto result=writer.finish();
        // Both cursors are exhausted and their read buffers own all bytes.
        // Reclaim each input run in bounded durable batches, rather than
        // rebuilding/publishing allocator metadata for every fragment.
        scratch_->releaseBatch(std::span<const uint64_t>(left.handles.data(),left.handles.size()));
        scratch_->releaseBatch(std::span<const uint64_t>(right.handles.data(),right.handles.size()));
        return result;}
	storage::LogicalQueryScratchProvider &provider_;MaterializedByteBudget &budget_;size_t maximum_rows_,buffer_target_,total_=0;
	MaterializedByteReservation workspace_;BudgetMemoryResource memory_;utils::AdmissionPmrVector<uint8_t> ascending_;
	SizeFunction size_;std::function<void()> checkpoint_,publication_checkpoint_;size_t maximum_disk_,maximum_fragments_;std::shared_ptr<ResourceAccount> account_;utils::AdmissionPmrVector<Run> runs_;BoundedBatch<Record> rows_;
    std::optional<WriterBuffers> writer_buffers_;
	std::unique_ptr<storage::LogicalQueryScratchSession> scratch_;bool spillable_=true,finished_=false;
};
}
