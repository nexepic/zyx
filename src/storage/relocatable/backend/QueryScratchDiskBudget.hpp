#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace graph::storage::detail {

// Counts private allocated extents, including uncertain writes until exclusive
// writable recovery proves reclamation. Metadata COW storage is a separate owner.
// All admission is nonblocking. No IO or caller callback runs under this mutex.
class QueryScratchDiskBudget final : public std::enable_shared_from_this<QueryScratchDiskBudget> {
public:
	struct Usage { size_t bytes = 0, orphan_bytes = 0, sessions = 0, domains = 0; };
	class Session final {
	public:
		Session(const Session &) = delete;
		Session &operator=(const Session &) = delete;
		Session(Session &&other) noexcept
			: owner_(std::move(other.owner_)), slot_(other.slot_), bytes_(std::exchange(other.bytes_,0)), maximum_(other.maximum_) {}
		Session &operator=(Session &&other) noexcept {
			if(this!=&other){abandon();owner_=std::move(other.owner_);slot_=other.slot_;bytes_=std::exchange(other.bytes_,0);maximum_=other.maximum_;}
			return *this;
		}
		~Session(){abandon();}
		void acquire(size_t bytes) {
			if(!owner_)throw std::logic_error("query scratch disk session is finished");
			std::lock_guard lock(owner_->mutex_);
			auto &domain=owner_->domains_[slot_];
			if(bytes>maximum_-bytes_ || bytes>domain.maximum-domain.bytes || bytes>owner_->maximum_-owner_->bytes_)
				throw std::runtime_error("query scratch exceeded hierarchical disk quota");
			bytes_+=bytes;domain.bytes+=bytes;owner_->bytes_+=bytes;
		}
		// Call only after durable reclamation or before any IO has begun.
		void release(size_t bytes) {
			if(!owner_ || bytes>bytes_)throw std::logic_error("invalid query scratch disk refund");
			std::lock_guard lock(owner_->mutex_);
			bytes_-=bytes;owner_->domains_[slot_].bytes-=bytes;owner_->bytes_-=bytes;
		}
		[[nodiscard]] size_t bytes() const noexcept{return bytes_;}
		// Unreclaimed bytes become orphans; successful cleanup leaves zero charge.
		void close() noexcept {abandon();}
	private:
		friend class QueryScratchDiskBudget;
		Session(std::shared_ptr<QueryScratchDiskBudget> owner,size_t slot,size_t maximum)
			:owner_(std::move(owner)),slot_(slot),maximum_(maximum){}
		void abandon() noexcept {
			if(!owner_)return;
			{
				std::lock_guard lock(owner_->mutex_);
				auto &domain=owner_->domains_[slot_];
				domain.orphan_bytes+=bytes_;--domain.sessions;
				if(domain.sessions==0 && domain.bytes==0)domain.identity.clear();
			}
			bytes_=0;owner_.reset();
		}
		std::shared_ptr<QueryScratchDiskBudget> owner_;
		size_t slot_,bytes_=0,maximum_;
	};

	explicit QueryScratchDiskBudget(size_t maximum,size_t maximumSessions=64)
		:maximum_(maximum),maximum_sessions_(maximumSessions){
		if(maximum==0 || maximumSessions==0)throw std::invalid_argument("query scratch disk budget requires finite limits");
	}
	[[nodiscard]] Session open(std::string identity,size_t databaseMaximum,size_t queryMaximum) {
		if(identity.empty() || databaseMaximum==0 || databaseMaximum>maximum_ || queryMaximum==0 || queryMaximum>databaseMaximum)
			throw std::invalid_argument("invalid query scratch disk domain limits");
		auto owner=shared_from_this();
		std::lock_guard lock(mutex_);
		size_t slot=domains_.size(),empty=domains_.size();
		for(size_t i=0;i<domains_.size();++i){
			if(domains_[i].identity==identity){slot=i;break;}
			if(domains_[i].identity.empty())empty=i;
		}
		if(slot==domains_.size()){
			if(empty==domains_.size())throw std::runtime_error("query scratch disk domain slots exhausted");
			slot=empty;domains_[slot].identity=std::move(identity);domains_[slot].maximum=databaseMaximum;
		}
		auto &domain=domains_[slot];
		if(domain.maximum!=databaseMaximum)throw std::invalid_argument("query scratch database disk quota differs between owners");
		if(domain.sessions>=maximum_sessions_)throw std::runtime_error("query scratch session slots exhausted");
		++domain.sessions;
		return Session(std::move(owner),slot,queryMaximum);
	}
	// Caller holds the exclusive scratch-file recovery lease and has already
	// authenticated/reclaimed every orphan. A failed recovery must not call this.
	void recovered(const std::string &identity) {
		std::lock_guard lock(mutex_);
		for(auto &domain:domains_)if(domain.identity==identity){
			if(domain.sessions!=0)throw std::logic_error("query scratch disk recovery has live sessions");
			bytes_-=domain.orphan_bytes;domain.bytes-=domain.orphan_bytes;domain.orphan_bytes=0;
			if(domain.bytes==0)domain.identity.clear();
			return;
		}
	}
	[[nodiscard]] Usage usage() const {
		std::lock_guard lock(mutex_);Usage result;result.bytes=bytes_;
		for(const auto &domain:domains_)if(!domain.identity.empty()){
			++result.domains;result.sessions+=domain.sessions;result.orphan_bytes+=domain.orphan_bytes;
		}
		return result;
	}
	static std::shared_ptr<QueryScratchDiskBudget> processBudget(){
		static auto owner=std::make_shared<QueryScratchDiskBudget>(size_t{2}*1024*1024*1024);
		return owner;
	}
private:
	struct Domain {std::string identity;size_t maximum=0,bytes=0,orphan_bytes=0,sessions=0;};
	const size_t maximum_,maximum_sessions_;
	mutable std::mutex mutex_;
	size_t bytes_=0;
	std::array<Domain,64> domains_{};
};
} // namespace graph::storage::detail
