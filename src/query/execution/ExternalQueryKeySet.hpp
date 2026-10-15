#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <functional>
#include <memory_resource>
#include <string>
#include <unordered_set>
#include "graph/query/execution/StandardContainerHeapBytes.hpp"
#include "graph/storage/LogicalQueryScratch.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "query/execution/BudgetMemoryResource.hpp"

namespace graph::query::execution::detail {
// Exact, streaming DISTINCT over already canonical keys. Eight hash partitions
// retain append buffers and bounded Bloom filters, then probe authenticated
// scratch blocks only on a possible match. Bloom collisions never drop rows.
// There is no replay, row decoding, output reordering or sidecar file.
class ExternalQueryKeySet final {
public:
    static constexpr size_t MINIMUM_QUERY_BYTES = 4U * 1024U * 1024U;
    ExternalQueryKeySet(storage::LogicalQueryScratchProvider &provider, MaterializedByteBudget &budget,
            BudgetMemoryResource &memory, size_t maximumKeys, size_t residentBytes,
            std::function<void()> checkpoint, std::function<void()> publicationCheckpoint,
            size_t maximumDiskBytes = 128U * 1024U * 1024U, size_t maximumFragments = 1024U,
            std::shared_ptr<ResourceAccount> account = {})
        : provider_(provider), budget_(budget), memory_(memory), maximum_keys_(maximumKeys),
          resident_bytes_(residentBytes), checkpoint_(std::move(checkpoint)),
          publication_checkpoint_(std::move(publicationCheckpoint)), maximum_disk_(maximumDiskBytes),
          maximum_fragments_(maximumFragments), account_(std::move(account)), cached_(&memory), partitions_(&memory) {
        if (!maximumKeys || !residentBytes || !checkpoint_ || !publication_checkpoint_ || !maximumFragments || maximumFragments > 4096U ||
                maximumDiskBytes < storage::RSS_SEGMENT_SIZE)
            throw std::invalid_argument("external DISTINCT requires finite bounds and guards");
    }
    [[nodiscard]] bool insert(std::pmr::string key) {
        if (finished_ || failed_) throw std::logic_error("external DISTINCT owner is terminal");
        try {
            checkpoint_();
            if (!scratch_) {
                if (cached_.contains(key)) return false;
                requireKeySlot();
                // Downstream retained output shares this budget. Start the
                // transition while its buffers still fit, even if this key set
                // has not reached its own resident threshold. This is advisory;
                // every allocation still performs actual hierarchical admission.
                const size_t transitionHeadroom = addStandardHeapBytes(resident_bytes_,
                    PARTITIONS * (storage::RSS_SEGMENT_SIZE + sizeof(Partition)));
                if (!cached_.empty() && budget_.available() < transitionHeadroom) spill();
                if (!scratch_) {
                    cached_.insert(std::move(key));
                    ++size_;
                    if (memory_.used() > resident_bytes_) spill();
                    return true;
                }
            }
            validateKey(key);
            const auto hash = hashKey(key);
            auto &partition = partitions_[hash % PARTITIONS];
            if (mayContain(partition, hash) && contains(partition, key)) return false;
            requireKeySlot();
            append(partition, key, hash);
            ++size_;
            return true;
        } catch (...) { failed_ = true; throw; }
    }
    void finish() {
        if (finished_) return;
        try { if (scratch_) scratch_->finish(); }
        catch (...) { failed_ = true; throw; }
        finished_ = true;
    }
    [[nodiscard]] bool spilled() const noexcept { return scratch_ != nullptr; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
private:
    static constexpr size_t PARTITIONS = 8U;
    static constexpr size_t BLOOM_WORDS = 1024U;
    static constexpr size_t HEADER_BYTES = 16U;
    static constexpr uint64_t MAGIC = UINT64_C(0x3159454B58595A); // ZYXKEY1
    struct Partition {
        explicit Partition(std::pmr::memory_resource *memory) : block(memory), handles(memory) {}
        std::pmr::vector<std::byte> block;
        std::pmr::vector<uint64_t> handles;
        std::array<uint64_t, BLOOM_WORDS> bloom{};
        uint32_t count = 0U;
    };
    static uint64_t hashKey(std::string_view key) noexcept {
        uint64_t hash = UINT64_C(14695981039346656037);
        for (const unsigned char byte : key) { hash ^= byte; hash *= UINT64_C(1099511628211); }
        hash ^= hash >> 33U; hash *= UINT64_C(0xff51afd7ed558ccd);
        hash ^= hash >> 33U; hash *= UINT64_C(0xc4ceb9fe1a85ec53);
        return hash ^ (hash >> 33U);
    }
    static std::pair<size_t, size_t> bloomBits(uint64_t hash) noexcept {
        constexpr size_t mask = BLOOM_WORDS * 64U - 1U;
        return {static_cast<size_t>(hash >> 3U) & mask, static_cast<size_t>(hash >> 32U) & mask};
    }
    static bool mayContain(const Partition &partition, uint64_t hash) noexcept {
        const auto [a, b] = bloomBits(hash);
        return (partition.bloom[a / 64U] & (UINT64_C(1) << (a % 64U))) &&
            (partition.bloom[b / 64U] & (UINT64_C(1) << (b % 64U)));
    }
    static void remember(Partition &partition, uint64_t hash) noexcept {
        const auto [a, b] = bloomBits(hash);
        partition.bloom[a / 64U] |= UINT64_C(1) << (a % 64U);
        partition.bloom[b / 64U] |= UINT64_C(1) << (b % 64U);
    }
    static void put(std::pmr::vector<std::byte> &bytes, uint64_t value, size_t width) {
        for (size_t i = 0U; i < width; ++i) bytes.push_back(static_cast<std::byte>((value >> (8U * i)) & 255U));
    }
    static uint64_t get(std::span<const std::byte> bytes, size_t &offset, size_t width) {
        if (offset > bytes.size() || width > bytes.size() - offset)
            throw std::invalid_argument("external DISTINCT block is truncated");
        uint64_t value = 0U;
        for (size_t i = 0U; i < width; ++i)
            value |= uint64_t(std::to_integer<unsigned>(bytes[offset++])) << (8U * i);
        return value;
    }
    void requireKeySlot() const {
        if (size_ == maximum_keys_) throw std::runtime_error("external DISTINCT exceeded its unique-key bound");
    }
    void validateKey(std::string_view key) const {
        if (key.empty() || key.size() > UINT32_MAX || maximum_fragment_ < HEADER_BYTES + 4U ||
                key.size() > maximum_fragment_ - HEADER_BYTES - 4U)
            throw std::runtime_error("external DISTINCT key exceeded its spill byte bound");
    }
    void start(Partition &partition) {
        partition.block.clear();
        put(partition.block, MAGIC, 8U); put(partition.block, 0U, 4U); put(partition.block, 0U, 4U);
        partition.count = 0U;
    }
    void flush(Partition &partition) {
        if (!partition.count) return;
        // Allocate the cleanup route before publishing bytes, including the
        // transient route-vector overlap. push_back(uint64_t) then cannot fail.
        if (partition.handles.size() == partition.handles.capacity()) {
            const size_t capacity = partition.handles.empty() ? 1U : partition.handles.size() * 2U;
            partition.handles.reserve((std::min)(capacity, maximum_fragments_));
        }
        if (sealed_fragments_ == maximum_fragments_)
            throw std::runtime_error("external DISTINCT exceeded its scratch-handle bound");
        publication_checkpoint_();
        const auto handle = scratch_->write(partition.block);
        partition.handles.push_back(handle); ++sealed_fragments_;
        start(partition);
    }
    void append(Partition &partition, std::string_view key, uint64_t hash) {
        validateKey(key);
        if (key.size() + 4U > maximum_fragment_ - partition.block.size()) flush(partition);
        put(partition.block, key.size(), 4U);
        for (const unsigned char byte : key) partition.block.push_back(static_cast<std::byte>(byte));
        ++partition.count;
        for (size_t i = 0U; i < 4U; ++i)
            partition.block[8U + i] = static_cast<std::byte>((partition.count >> (8U * i)) & 255U);
        remember(partition, hash);
    }
    bool scan(std::span<const std::byte> block, std::string_view key, bool allowEmpty = false) const {
        size_t offset = 0U;
        if (block.size() > maximum_fragment_ || get(block, offset, 8U) != MAGIC)
            throw std::invalid_argument("external DISTINCT block magic or size is invalid");
        const auto count = get(block, offset, 4U);
        if (get(block, offset, 4U) != 0U || (!allowEmpty && count == 0U) || count > (block.size() - offset) / 5U)
            throw std::invalid_argument("external DISTINCT block count is invalid");
        bool found = false;
        const auto expectedPartition = hashKey(key) % PARTITIONS;
        for (size_t i = 0U; i < count; ++i) {
            if ((i & 255U) == 0U) checkpoint_();
            const auto length = get(block, offset, 4U);
            if (!length || length > block.size() - offset)
                throw std::invalid_argument("external DISTINCT key is truncated");
            const std::string_view stored(reinterpret_cast<const char *>(block.data() + offset), static_cast<size_t>(length));
            if (hashKey(stored) % PARTITIONS != expectedPartition)
                throw std::invalid_argument("external DISTINCT key is in the wrong partition");
            found = found || stored == key;
            offset += static_cast<size_t>(length);
        }
        if (offset != block.size()) throw std::invalid_argument("external DISTINCT block has trailing bytes");
        return found;
    }
    bool contains(Partition &partition, std::string_view key) {
        if (scan(partition.block, key, true)) return true;
        for (const auto handle : partition.handles) {
            checkpoint_();
            // The returned byte vector is the scratch API's bounded read owner.
            // Destroy it before refunding the external allocation reservation.
            MaterializedByteReservation readCharge(&budget_);
            readCharge.set(addStandardHeapBytes(standardContainerProxyBytes(),
                standardAllocatorRequestBytes(maximum_fragment_, alignof(std::byte))));
            auto bytes = scratch_->read(handle);
            if (bytes.capacity() > maximum_fragment_)
                throw std::runtime_error("external DISTINCT scratch reader exceeded its capacity contract");
            if (scan(bytes, key)) return true;
        }
        return false;
    }
    void spill() {
        // Admission of resident buffers precedes file permission and owner
        // publication. Unsupported key widths fail before persisting any key.
        maximum_fragment_ = storage::RSS_SEGMENT_SIZE - storage::RSS_ENVELOPE_BYTES;
        for (const auto &key : cached_) validateKey(key);
        partitions_.reserve(PARTITIONS);
        for (size_t i = 0U; i < PARTITIONS; ++i) {
            partitions_.emplace_back(&memory_);
            auto &partition = partitions_.back();
            partition.block.reserve(maximum_fragment_); start(partition);
        }
        publication_checkpoint_();
        scratch_ = provider_.openQueryScratchSession(maximum_disk_, maximum_fragments_, account_);
        if (!scratch_) throw std::runtime_error("external DISTINCT cannot spill to a physically read-only file");
        if (scratch_->maximumFragmentBytes() != maximum_fragment_)
            throw std::runtime_error("external DISTINCT scratch fragment layout is unsupported");
        for (const auto &key : cached_) {
            checkpoint_(); const auto hash = hashKey(key);
            append(partitions_[hash % PARTITIONS], key, hash);
        }
        for (auto &partition : partitions_) flush(partition);
        cached_.clear();
    }
    storage::LogicalQueryScratchProvider &provider_;
    MaterializedByteBudget &budget_;
    BudgetMemoryResource &memory_;
    size_t maximum_keys_, resident_bytes_;
    std::function<void()> checkpoint_, publication_checkpoint_;
    size_t maximum_disk_, maximum_fragments_, maximum_fragment_ = 0U, sealed_fragments_ = 0U, size_ = 0U;
    std::shared_ptr<ResourceAccount> account_;
    std::pmr::unordered_set<std::pmr::string> cached_;
    std::pmr::vector<Partition> partitions_;
    std::unique_ptr<storage::LogicalQueryScratchSession> scratch_;
    bool failed_ = false, finished_ = false;
};
} // namespace graph::query::execution::detail
