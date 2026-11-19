#pragma once
// Fixed-capacity test diagnostics: no fields are added to production owners.
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace graph::debug {
namespace ci_budget_detail {
struct Entry { const void *owner=nullptr;const char *kind=nullptr;size_t id=0,events=0,suppressed=0,peak=0; };
struct State {
    std::atomic_flag lock=ATOMIC_FLAG_INIT;
    Entry entries[512]{};
    size_t next=1,events=0,summaries=0,successes=0,notices=0,active=0;
    bool overflow=false,overflow_reported=false;
};
inline State state;
struct Lock {
    Lock() noexcept {while(state.lock.test_and_set(std::memory_order_acquire)) {}}
    ~Lock(){state.lock.clear(std::memory_order_release);}
};
inline Entry *find(const void *owner,const char *kind,bool create) noexcept {
    if(!create && state.active==0U)return nullptr;
    Entry *empty=nullptr;
    const auto start=(reinterpret_cast<uintptr_t>(owner)>>4U)%512U;
    for(size_t n=0;n<512U;++n){auto &e=state.entries[(start+n)%512U];
        if(e.owner==owner && e.kind && std::strcmp(e.kind,kind)==0)return &e;
        if(!e.owner && !empty)empty=&e;}
    if(!create)return nullptr;
    if(!empty){state.overflow=true;return nullptr;}
    *empty={owner,kind,state.next++,0U,0U,0U};++state.active;return empty;
}
}
class CiBudgetTrace final {
public:
    explicit CiBudgetTrace(const char *kind) noexcept : owner_(this),kind_(kind) {}
    CiBudgetTrace(const void *owner,const char *kind) noexcept : owner_(owner),kind_(kind) {}
    size_t id() const noexcept {
        if(!enabled())return 0U;
        ci_budget_detail::Lock lock;const auto *e=ci_budget_detail::find(owner_,kind_,true);return e ? e->id : 0U;
    }
    bool active() const noexcept {
        if(!enabled())return false;
        ci_budget_detail::Lock lock;const auto *e=ci_budget_detail::find(owner_,kind_,false);return e && e->events;
    }
    void observePeak(size_t used) const noexcept {
        if(!enabled())return;
        ci_budget_detail::Lock lock;if(auto *e=ci_budget_detail::find(owner_,kind_,true))if(used>e->peak)e->peak=used;
    }
    bool allow() const noexcept {
        if(!enabled())return false;
        bool permitted=false,notice=false,overflow=false;size_t id=0;
        {ci_budget_detail::Lock lock;auto &s=ci_budget_detail::state;
            if(auto *e=ci_budget_detail::find(owner_,kind_,true)){
                id=e->id;const auto local=e->events++,global=s.events++;
                permitted=local<40U && global<400U;if(!permitted)++e->suppressed;
                if((local==40U || global==400U) && s.notices++<20U)notice=true;
            }
            if(s.overflow && !s.overflow_reported){s.overflow_reported=true;overflow=true;}
        }
        if(notice)std::fprintf(stderr,"CI_TRACE_TRUNCATED kind=%s owner=%zu owner_limit=40 process_limit=400 notice_limit=20\n",kind_,id);
        if(overflow)std::fputs("CI_TRACE_TRUNCATED active_owner_registry_limit=512\n",stderr);
        if(notice || overflow)std::fflush(stderr);
        return permitted;
    }
    void discard() const noexcept {
        ci_budget_detail::Lock lock;
        if(auto *e=ci_budget_detail::find(owner_,kind_,false)){*e={};--ci_budget_detail::state.active;}
    }
    void summary(size_t used,size_t peak,size_t maximum,size_t parent=0U) const noexcept {
        if(!enabled()){discard();return;}
        ci_budget_detail::Entry snapshot;bool report=false,notice=false,overflow=false;size_t limit=0;
        {ci_budget_detail::Lock lock;auto &s=ci_budget_detail::state;
            if(auto *e=ci_budget_detail::find(owner_,kind_,true)){
                snapshot=*e;if(peak>snapshot.peak)snapshot.peak=peak;
                if(snapshot.events || snapshot.peak){limit=snapshot.events ? 80U : 20U;
                    auto &count=snapshot.events ? s.summaries : s.successes;
                    const auto n=count++;report=n<limit;notice=n==limit;}
                *e={};--s.active; // Lifetime ended: pointer reuse receives a fresh opaque ID.
            }
            if(s.overflow && !s.overflow_reported){s.overflow_reported=true;overflow=true;}
        }
        if(report)std::fprintf(stderr,"CI_BUDGET_SUMMARY kind=%s owner=%zu phase=before_member_release used=%zu peak=%zu maximum=%zu parent_charge=%zu events=%zu suppressed=%zu\n",
            kind_,snapshot.id,used,snapshot.peak,maximum,parent,snapshot.events,snapshot.suppressed);
        if(notice)std::fprintf(stderr,"CI_TRACE_TRUNCATED summary_kind=%s process_limit=%zu\n",snapshot.events ? "pressure" : "successful",limit);
        if(overflow)std::fputs("CI_TRACE_TRUNCATED active_owner_registry_limit=512\n",stderr);
        if(report || notice || overflow)std::fflush(stderr);
    }
private:
    static bool enabled() noexcept {return std::getenv("ZYX_CI_BUDGET_TRACE")!=nullptr;}
    const void *owner_;const char *kind_;
};
}
#endif
