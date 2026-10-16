/**
 * @file OwnedFunction.hpp
 * @brief Copyable callbacks with admitted closure storage and trivial inline targets.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include "graph/utils/OwnedSharedPtr.hpp"

namespace graph::utils {
namespace detail {
template<class T> struct IsStandardFunction : std::false_type {};
template<class R, class... A> struct IsStandardFunction<std::function<R(A...)>> : std::true_type {};
}

// The box and its control block are governed here. Captured payloads and
// pointees must retain their own authority, just as with makeSharedOwned.
// A borrowed external callback does not acquire ownership of its referents.
template<class Signature> class OwnedFunction;
template<class R, class... A>
class OwnedFunction<R(A...)> {
    struct Callable {
        std::shared_ptr<std::pmr::memory_resource> owner;
        explicit Callable(std::shared_ptr<std::pmr::memory_resource> value) : owner(std::move(value)) {}
        virtual ~Callable() = default;
        virtual R invoke(A... args) = 0;
        virtual std::shared_ptr<Callable> clone() const = 0;
        virtual const std::type_info &type() const noexcept = 0;
        virtual void *address(const std::type_info &) noexcept = 0;
    };
    using Heap = std::shared_ptr<Callable>;
    template<class F> static R invokeTarget(F &function, A &&...args) {
        if constexpr (std::is_void_v<R>) std::invoke(function, std::forward<A>(args)...);
        else return std::invoke(function, std::forward<A>(args)...);
    }

    template<class F> struct Model final : Callable {
        F function;
        template<class Source> Model(std::shared_ptr<std::pmr::memory_resource> owner, Source &&source)
            : Callable(std::move(owner)), function(std::forward<Source>(source)) {}
        R invoke(A... args) override { return invokeTarget(function, std::forward<A>(args)...); }
        std::shared_ptr<Callable> clone() const override {
            auto selected = OwnedAllocationScope::active() ? sharedAllocationOwner() : this->owner;
            OwnedAllocationScope scope(selected);
            return std::allocate_shared<Model>(OwnedPmrAllocator<Model>(selected), selected, function);
        }
        const std::type_info &type() const noexcept override { return typeid(F); }
        void *address(const std::type_info &requested) noexcept override {
            return requested == typeid(F) ? std::addressof(function) : nullptr;
        }
    };
    static constexpr size_t inline_bytes = 2U * sizeof(void *);
    static constexpr size_t storage_bytes = inline_bytes > sizeof(Heap) ? inline_bytes : sizeof(Heap);
    struct Storage { alignas(Heap) std::byte bytes[storage_bytes]; };
    template<class F> static F *slot(Storage &value) noexcept { return reinterpret_cast<F *>(value.bytes); }
    template<class F> static F *object(Storage &value) noexcept { return std::launder(slot<F>(value)); }
    template<class F> static const F *object(const Storage &value) noexcept {
        return std::launder(reinterpret_cast<const F *>(value.bytes));
    }
    struct Operations {
        R (*invoke)(Storage &, A &&...);
        void (*copy)(Storage &, const Storage &);
        void (*move)(Storage &, Storage &) noexcept;
        void (*destroy)(Storage &) noexcept;
        const std::type_info &(*type)(const Storage &) noexcept;
        void *(*address)(Storage &, const std::type_info &) noexcept;
    };
    template<class F> static const Operations &inlineOperations() noexcept {
        static const Operations operations{
            [](Storage &value, A &&...args) -> R { return invokeTarget(*object<F>(value), std::forward<A>(args)...); },
            [](Storage &to, const Storage &from) { std::construct_at(slot<F>(to), *object<F>(from)); },
            [](Storage &to, Storage &from) noexcept {
                std::construct_at(slot<F>(to), std::move(*object<F>(from))); std::destroy_at(object<F>(from));
            },
            [](Storage &value) noexcept { std::destroy_at(object<F>(value)); },
            [](const Storage &) noexcept -> const std::type_info & { return typeid(F); },
            [](Storage &value, const std::type_info &requested) noexcept -> void * {
                return requested == typeid(F) ? object<F>(value) : nullptr;
            }};
        return operations;
    }
    static const Operations &heapOperations() noexcept {
        static const Operations operations{
            [](Storage &value, A &&...args) -> R { return (*object<Heap>(value))->invoke(std::forward<A>(args)...); },
            [](Storage &to, const Storage &from) { std::construct_at(slot<Heap>(to), (*object<Heap>(from))->clone()); },
            [](Storage &to, Storage &from) noexcept {
                std::construct_at(slot<Heap>(to), std::move(*object<Heap>(from))); std::destroy_at(object<Heap>(from));
            },
            [](Storage &value) noexcept { std::destroy_at(object<Heap>(value)); },
            [](const Storage &value) noexcept -> const std::type_info & { return (*object<Heap>(value))->type(); },
            [](Storage &value, const std::type_info &requested) noexcept -> void * {
                return (*object<Heap>(value))->address(requested);
            }};
        return operations;
    }
public:
    using result_type = R;
    OwnedFunction() noexcept = default;
    OwnedFunction(std::nullptr_t) noexcept {}
    template<class F> requires (!std::is_same_v<std::remove_cvref_t<F>, OwnedFunction> &&
        std::is_copy_constructible_v<std::decay_t<F>> && std::is_invocable_r_v<R, std::decay_t<F> &, A...>)
    OwnedFunction(F &&function) {
        using Target = std::decay_t<F>;
        if constexpr (std::is_pointer_v<Target> || std::is_member_pointer_v<Target> || detail::IsStandardFunction<Target>::value)
            if (!function) return;
        if constexpr (sizeof(Target) <= inline_bytes && alignof(Target) <= alignof(Heap) &&
            std::is_trivially_copyable_v<Target> && std::is_nothrow_move_constructible_v<Target>) {
            std::construct_at(slot<Target>(storage_), std::forward<F>(function));
            operations_ = &inlineOperations<Target>();
        } else {
            auto owner = sharedAllocationOwner();
            OwnedAllocationScope scope(owner);
            auto model = std::allocate_shared<Model<Target>>(OwnedPmrAllocator<Model<Target>>(owner), owner,
                std::forward<F>(function));
            std::construct_at(slot<Heap>(storage_), std::move(model));
            operations_ = &heapOperations();
        }
    }
    OwnedFunction(const OwnedFunction &source) {
        if (source.operations_) { source.operations_->copy(storage_, source.storage_); operations_ = source.operations_; }
    }
    OwnedFunction(OwnedFunction &&source) noexcept { take(source); }
    ~OwnedFunction() { clear(); }
    OwnedFunction &operator=(const OwnedFunction &source) {
        if (this != &source) { OwnedFunction copy(source); swap(copy); } return *this;
    }
    OwnedFunction &operator=(OwnedFunction &&source) noexcept {
        if (this != &source) { clear(); take(source); } return *this;
    }
    OwnedFunction &operator=(std::nullptr_t) noexcept { clear(); return *this; }
    template<class F> requires (!std::is_same_v<std::remove_cvref_t<F>, OwnedFunction> &&
        std::is_copy_constructible_v<std::decay_t<F>> && std::is_invocable_r_v<R, std::decay_t<F> &, A...>)
    OwnedFunction &operator=(F &&function) {
        OwnedFunction next(std::forward<F>(function)); swap(next); return *this;
    }
    explicit operator bool() const noexcept { return operations_ != nullptr; }
    R operator()(A... args) const {
        if (!operations_) throw std::bad_function_call();
        return operations_->invoke(storage_, std::forward<A>(args)...);
    }
    const std::type_info &target_type() const noexcept { return operations_ ? operations_->type(storage_) : typeid(void); }
    template<class T> T *target() noexcept {
        if constexpr (!std::is_object_v<T>) return nullptr;
        else return operations_ ? static_cast<T *>(operations_->address(storage_, typeid(T))) : nullptr;
    }
    template<class T> const T *target() const noexcept {
        if constexpr (!std::is_object_v<T>) return nullptr;
        else return operations_ ? static_cast<const T *>(operations_->address(storage_, typeid(T))) : nullptr;
    }
    void swap(OwnedFunction &other) noexcept {
        if (this == &other) return;
        OwnedFunction previous(std::move(*this)); *this = std::move(other); other = std::move(previous);
    }
    friend void swap(OwnedFunction &a, OwnedFunction &b) noexcept { a.swap(b); }
    friend bool operator==(const OwnedFunction &value, std::nullptr_t) noexcept { return !value; }
    friend bool operator==(std::nullptr_t, const OwnedFunction &value) noexcept { return !value; }
private:
    void clear() noexcept { if (operations_) { operations_->destroy(storage_); operations_ = nullptr; } }
    void take(OwnedFunction &source) noexcept {
        if (source.operations_) {
            source.operations_->move(storage_, source.storage_); operations_ = std::exchange(source.operations_, nullptr);
        }
    }
    const Operations *operations_ = nullptr;
    mutable Storage storage_;
};
} // namespace graph::utils
