#pragma once
#include "PropertySubblockFieldTransition.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"

namespace zyx::benchmark::subblock {
// A synchronous query-owned plan: the caller's budget must outlive it. It cannot
// be copied/moved to an uncharged holder. This binds the pure kernel to actual
// account admission; canonical extension bodies and future native publication
// buffers still require their own reservations.
class ReservedFieldTransition final {
    using Reservation = graph::query::execution::MaterializedByteReservation;
    Reservation reservation_;
    FieldTransition transition_;
    static Reservation reserve(graph::query::execution::MaterializedByteBudget &budget) {
        Reservation reservation(&budget);
        // Include the holder and worst-case named-return temporary overlap even
        // when the compiler does not elide the pure kernel's return copy.
        reservation.set(sizeof(ReservedFieldTransition) + sizeof(FieldTransition));
        return reservation;
    }
public:
    ReservedFieldTransition(graph::query::execution::MaterializedByteBudget &budget,
        const Column &source,unsigned slot,FieldCell value,uint64_t live,uint64_t generation,size_t nativeLimit)
        : reservation_(reserve(budget)), transition_(planFieldChange(source,slot,value,live,generation,
            sizeof(FieldTransition),nativeLimit)) {}
    ReservedFieldTransition(const ReservedFieldTransition &) = delete;
    ReservedFieldTransition &operator=(const ReservedFieldTransition &) = delete;
    [[nodiscard]] const FieldTransition &get() const & noexcept { return transition_; }
    const FieldTransition &get() const && = delete;
    [[nodiscard]] size_t retainedCharge() const noexcept { return reservation_.bytes(); }
};
}
