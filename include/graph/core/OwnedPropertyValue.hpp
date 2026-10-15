/**
 * @file OwnedPropertyValue.hpp
 * @brief Immutable, admitted payloads for retained logical-plan literals.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "graph/core/PropertyHeapRequestBytes.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/utils/OwnedSharedPtr.hpp"

namespace graph {
// The public view is const PropertyValue; replacements swap complete immutable
// payloads. Copies within one authority share data. Crossing a scoped authority
// clones before publication, so CACHE cannot retain a private QUERY account.
class OwnedPropertyValue final {
    struct Payload {
        Payload(std::shared_ptr<std::pmr::memory_resource> owner, const PropertyValue &source)
            : charge(std::move(owner), property_heap::copyRequestUpper(source)), value(source) {
            const auto retained = property_heap::retainedRequestUpper(value);
            if (retained > charge.bytes())
                throw std::logic_error("Property copy exceeded its audited heap request bound");
            charge.set(retained);
        }
        template<class ListSource>
        Payload(std::shared_ptr<std::pmr::memory_resource> owner, const ListSource &source)
            : charge(std::move(owner), source.copyRequestUpper()), value(source.copy()) {
            const auto retained = property_heap::retainedRequestUpper(value);
            if (retained > charge.bytes()) throw std::logic_error("Property list copy exceeded its audited request bound");
            charge.set(retained);
        }
        utils::OwnedHeapReservation charge;
        const PropertyValue value;
    };
public:
    // Getter returns CopySource views into stable immutable inputs. It is called
    // once for sizing and once for copying, with no payload allocation in sizing.
    template<class Getter>
    static OwnedPropertyValue fromList(size_t count, Getter &&getter) {
        struct Source {
            size_t count;
            Getter &get;
            size_t copyRequestUpper() const {
                auto total = property_heap::detail::copiedVectorBytes<PropertyValue>(count);
                // Root PropertyValue's by-value vector constructor makes two
                // moves; each can create one emptied-source Debug proxy.
                total = utils::detail::addStandardHeapBytes(total, 2U * utils::detail::standardContainerProxyBytes());
                for (size_t index = 0U; index < count; ++index)
                    total = utils::detail::addStandardHeapBytes(total, get(index).copyRequestUpper());
                return total;
            }
            PropertyValue copy() const {
                std::vector<PropertyValue> values; values.reserve(count);
                for (size_t index = 0U; index < count; ++index) get(index).appendTo(values);
                return PropertyValue(std::move(values));
            }
        };
        OwnedPropertyValue result;
        const Source source{count, getter};
        auto allocationOwner = result.owner_;
        if (!allocationOwner) allocationOwner = std::shared_ptr<std::pmr::memory_resource>(
            std::shared_ptr<std::pmr::memory_resource>{}, std::pmr::new_delete_resource());
        result.payload_ = std::allocate_shared<Payload>(utils::OwnedPmrAllocator<Payload>(allocationOwner), result.owner_, source);
        return result;
    }
    OwnedPropertyValue() : owner_(currentOwner()) {}
    OwnedPropertyValue(const PropertyValue &source) : OwnedPropertyValue(currentOwner(), source) {}
    OwnedPropertyValue(const OwnedPropertyValue &source) : owner_(currentOwner(source.owner_)) {
        if (owner_ == source.owner_) payload_ = source.payload_;
        else if (source.payload_) payload_ = makePayload(owner_, source.get());
    }
    OwnedPropertyValue(OwnedPropertyValue &&source) noexcept
        : owner_(source.owner_), payload_(std::move(source.payload_)) {}
    OwnedPropertyValue &operator=(const OwnedPropertyValue &source) {
        if (this != &source) { OwnedPropertyValue copy(source); swap(copy); }
        return *this;
    }
    OwnedPropertyValue &operator=(OwnedPropertyValue &&source) noexcept {
        if (this != &source) { OwnedPropertyValue moved(std::move(source)); swap(moved); }
        return *this;
    }
    OwnedPropertyValue &operator=(const PropertyValue &source) {
        OwnedPropertyValue copy(currentOwner(owner_), source); swap(copy); return *this;
    }
    void swap(OwnedPropertyValue &other) noexcept {
        owner_.swap(other.owner_); payload_.swap(other.payload_);
    }
    friend void swap(OwnedPropertyValue &left, OwnedPropertyValue &right) noexcept { left.swap(right); }
    [[nodiscard]] const PropertyValue &get() const noexcept {
        static const PropertyValue nullValue;
        return payload_ ? payload_->value : nullValue;
    }
    operator const PropertyValue &() const noexcept { return get(); }
    [[nodiscard]] std::string toString() const { return get().toString(); }
private:
    OwnedPropertyValue(std::shared_ptr<std::pmr::memory_resource> owner, const PropertyValue &source)
        : owner_(std::move(owner)) {
        if (source.getType() != PropertyType::NULL_TYPE) payload_ = makePayload(owner_, source);
    }
    static std::shared_ptr<std::pmr::memory_resource> currentOwner(
            const std::shared_ptr<std::pmr::memory_resource> &fallback = {}) {
        return utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : fallback;
    }
    static std::shared_ptr<const Payload> makePayload(
            const std::shared_ptr<std::pmr::memory_resource> &owner, const PropertyValue &source) {
        auto allocationOwner = owner;
        if (!allocationOwner) allocationOwner = std::shared_ptr<std::pmr::memory_resource>(
            std::shared_ptr<std::pmr::memory_resource>{}, std::pmr::new_delete_resource());
        return std::allocate_shared<Payload>(utils::OwnedPmrAllocator<Payload>(std::move(allocationOwner)), owner, source);
    }
    std::shared_ptr<std::pmr::memory_resource> owner_;
    std::shared_ptr<const Payload> payload_;
};
} // namespace graph
