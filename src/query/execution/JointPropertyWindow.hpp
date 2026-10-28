#pragma once

#include <exception>
#include "graph/storage/LogicalGraphJointPropertyRead.hpp"
#include "query/execution/DirectPropertyColumns.hpp"

namespace graph::query::execution::detail {
// Query-local construction; nothing is visible until all presence cells and
// values have been produced, the snapshot revalidated and the owner sealed.
// A failed, declined-after-delivery or reentered attempt is never reusable.
class JointPropertyWindow final {
public:
    JointPropertyWindow(const storage::LogicalGraphReadSnapshot &snapshot, size_t rows,
        size_t valueColumns, size_t presenceColumns, MaterializedByteBudget &budget)
        : snapshot_(snapshot), fence_(snapshot.mutationFence()), schema_(snapshot.bindingSchemaIdentity()),
          tokenSupport_(snapshot.supportsPropertyTokenResolution()), rows_(checkedRows(snapshot, rows, valueColumns, presenceColumns)), presenceColumns_(presenceColumns),
          memory_(budget), columns_(snapshot, rows_, valueColumns, budget), seen_(&memory_), nonnull_(&memory_) {
        seen_.resize(rows * presenceColumns, 0U);
        nonnull_.resize(rows * presenceColumns, 0U);
    }
    template<class Guard>
    bool read(std::span<const uint64_t> ids, std::span<const std::string_view> valueKeys,
        std::span<const std::string_view> presenceKeys, const std::function<void(size_t)> &admit, Guard guard) {
        const auto *provider = dynamic_cast<const storage::LogicalGraphJointPropertyReadProvider *>(&snapshot_);
        storage::JointNodePropertyReadSource source;
        if (provider) source = [provider](auto requested, auto values, auto presence, const auto &sinks, const auto &admission) {
            return provider->tryReadJointNodePropertyWindow(requested, values, presence, sinks, admission);
        };
        return readFromSource(ids, valueKeys, presenceKeys, admit, source, guard);
    }
    // Call only during the producer's synchronous authenticated scan visitor.
    // The source and every borrowed input/callback expire when this call returns.
    template<class Guard>
    bool readFromSource(std::span<const uint64_t> ids, std::span<const std::string_view> valueKeys,
        std::span<const std::string_view> presenceKeys, const std::function<void(size_t)> &admit,
        const storage::JointNodePropertyReadSource &source, Guard guard) {
        if (reading_) { failed_ = true; throw std::invalid_argument("joint window read reentered"); }
        if (failed_ || sealed_ || ids.size() != rows_ || valueKeys.size() != columns_.columnCount() ||
            presenceKeys.size() != presenceColumns_ || !admit) throw std::invalid_argument("joint window shape/state changed");
        validate();
        uint64_t previous = 0;
        for (auto id : ids) { guard(); if (!id || id <= previous) throw std::invalid_argument("joint window requires sorted unique IDs"); previous = id; }
        for (auto key : valueKeys) if (key.empty()) throw std::invalid_argument("joint window has empty value key");
        for (auto key : presenceKeys) if (key.empty()) throw std::invalid_argument("joint window has empty presence key");
        if (!source) return false;
        reading_ = true;
        try {
            storage::JointNodePropertyReadSinks sinks;
            sinks.values = [&](size_t row, size_t column, std::optional<PropertyValue> value) {
                try { Callback callback(*this); columns_.append(row, column, std::move(value), guard); }
                catch (...) { rememberFailure(); throw; }
            };
            sinks.runs.scalars = [&](std::span<const size_t> rows, size_t column, PropertyType type, std::span<const uint64_t> words) {
                try { Callback callback(*this); columns_.appendScalarRun(rows, column, type, words, guard); }
                catch (...) { rememberFailure(); throw; }
            };
            sinks.runs.texts = [&](std::span<const size_t> rows, size_t column, std::span<const std::string_view> texts) {
                try { Callback callback(*this); columns_.appendTextRun(rows, column, texts, guard); }
                catch (...) { rememberFailure(); throw; }
            };
            sinks.nonnull = [&](size_t row, size_t column, bool present) {
                try {
                    Callback callback(*this); guard(); validate();
                    if (row >= rows_ || column >= presenceColumns_) throw std::invalid_argument("joint presence ordinal outside window");
                    const auto index = column * rows_ + row;
                    if (seen_[index]) throw std::invalid_argument("duplicate joint presence cell");
                    seen_[index] = 1U; nonnull_[index] = present;
                } catch (...) { rememberFailure(); throw; }
            };
            const std::function<void(size_t)> sourceAdmission = [&](size_t bytes) {
                try {
                    if (!reading_ || active_ || failed_) throw std::invalid_argument("joint admission reentered or expired");
                    guard(); validate(); admit(bytes); validate(); sourceAdmitted_ = true;
                } catch (...) { rememberFailure(); throw; }
            };
            const bool supported = source(ids, valueKeys, presenceKeys, sinks, sourceAdmission);
            if (firstError_) std::rethrow_exception(firstError_);
            validate();
            if (!supported) {
                if (delivered_) throw std::invalid_argument("unsupported joint provider delivered cells");
                reading_ = false; failed_ = true; return false;
            }
            for (size_t i = 0; i < seen_.size(); ++i) {
                if ((i & 15U) == 0U) guard();
                if (!seen_[i]) throw std::invalid_argument("joint presence result is incomplete");
            }
            columns_.seal(); validate(); reading_ = false; sealed_ = true; return true;
        } catch (...) {
            reading_ = false; failed_ = true;
            if (firstError_) std::rethrow_exception(firstError_);
            throw;
        }
    }
    [[nodiscard]] const DirectPropertyColumns &values() const {
        validate(); if (!sealed_) throw std::invalid_argument("joint window is not sealed"); return columns_;
    }
    [[nodiscard]] bool nonnull(size_t row, size_t column) const {
        validate(); if (!sealed_ || row >= rows_ || column >= presenceColumns_) throw std::invalid_argument("joint presence unavailable");
        return nonnull_[column * rows_ + row] != 0U;
    }
private:
    struct Callback {
        JointPropertyWindow &owner;
        int exceptions = std::uncaught_exceptions();
        explicit Callback(JointPropertyWindow &value) : owner(value) {
            owner.delivered_ = true;
            if (!owner.reading_ || !owner.sourceAdmitted_ || owner.failed_ || owner.active_) { owner.failed_ = true; throw std::invalid_argument("joint callback reentered or expired"); }
            owner.active_ = true;
        }
        ~Callback() { owner.active_ = false; if (std::uncaught_exceptions() > exceptions) owner.failed_ = true; }
    };
    static size_t checkedRows(const storage::LogicalGraphReadSnapshot &snapshot, size_t rows, size_t valueColumns, size_t presenceColumns) {
        if (!snapshot.hasImmutableReadIdentity() || rows == 0 || rows > storage::LogicalGraphJointPropertyReadProvider::MAXIMUM_ROWS ||
            valueColumns > 64U || presenceColumns == 0 || presenceColumns > 64U || rows > SIZE_MAX / presenceColumns)
            throw std::invalid_argument("joint window requires immutable bounded shape");
        return rows;
    }
    void rememberFailure() noexcept {
        failed_ = true;
        if (!firstError_) firstError_ = std::current_exception();
    }
    void validate() const {
        if (failed_ || snapshot_.mutationFence() != fence_ || snapshot_.bindingSchemaIdentity() != schema_ ||
            snapshot_.supportsPropertyTokenResolution() != tokenSupport_) throw std::invalid_argument("joint snapshot identity changed");
    }
    const storage::LogicalGraphReadSnapshot &snapshot_;
    std::optional<storage::LogicalGraphMutationFence> fence_;
    std::optional<storage::LogicalGraphSchemaIdentity> schema_;
    bool tokenSupport_;
    size_t rows_, presenceColumns_;
    BudgetMemoryResource memory_;
    DirectPropertyColumns columns_;
    std::pmr::vector<uint8_t> seen_, nonnull_;
    std::exception_ptr firstError_;
    bool reading_ = false, active_ = false, delivered_ = false, sealed_ = false, failed_ = false, sourceAdmitted_ = false;
};
}
