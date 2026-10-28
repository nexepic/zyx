/**
 * @file LogicalProjectionCatalog.cpp
 * @brief Immutable logical graph projection catalog implementation.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/utils/OwnedFunction.hpp"
#include "graph/utils/OwnedPmrObject.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace graph::query::gds {

	namespace {

		class PreparedProjectionCatalogMutation final : public PreparedLogicalProjectionCatalogMutation, public utils::OwnedPmrObject {
		public:
			PreparedProjectionCatalogMutation(std::unique_lock<std::shared_mutex> lock, utils::OwnedFunction<void()> commit,
										  utils::OwnedFunction<void()> abort) :
				lock_(std::move(lock)), commit_(std::move(commit)), abort_(std::move(abort)) {}

			~PreparedProjectionCatalogMutation() override {
				if (!finished_) {
					abort_();
				}
			}

			void commit() override {
				if (finished_) {
					throw std::logic_error("prepared logical projection catalog mutation was committed more than once");
				}
				commit_();
				finished_ = true;
			}

		private:
			std::unique_lock<std::shared_mutex> lock_;
			utils::OwnedFunction<void()> commit_;
			utils::OwnedFunction<void()> abort_;
			bool finished_ = false;
		};

	} // namespace

	size_t LogicalProjectionCatalog::MutationFenceHash::operator()(
			const storage::LogicalGraphMutationFence &fence) const noexcept {
		// The fence is opaque to callers. This hash only provides process-local
		// identity for catalog revision tokens; equality still compares every
		// graph-visible root token.
		size_t value = std::hash<uint64_t>{}(fence.backend_token);
		const auto combine = [&value](const uint64_t token) {
			value ^= std::hash<uint64_t>{}(token) + size_t{0x9e3779b9U} + (value << 6U) + (value >> 2U);
		};
		combine(fence.format_token);
		for (const uint64_t token: fence.graph_state_tokens) {
			combine(token);
		}
		for (const uint64_t token: fence.data_page_root_tokens) combine(token);
		combine(fence.logical_revision);
		return value;
	}

	storage::LogicalGraphMutationFence
	LogicalProjectionCatalog::requireRevision(const storage::LogicalGraphReadSnapshot &snapshot) {
		const std::optional<storage::LogicalGraphMutationFence> revision = snapshot.mutationFence();
		if (!revision.has_value() || !revision->isValid()) {
			throw std::runtime_error("logical projection catalog requires a valid snapshot revision");
		}
		return *revision;
	}

	LogicalProjectionDescriptor
	LogicalProjectionCatalog::makeDescriptor(const std::string &name, const Entry &entry,
											 const storage::LogicalGraphMutationFence &currentRevision,
											 uint64_t currentRevisionToken) {
		const LogicalProjectionCsrCacheState csrCacheState = entry.projection->csrCacheState();
		return {.name = name,
				.node_count = entry.projection->nodeCount(),
				.edge_count = entry.projection->edgeCount(),
				.created_at_epoch_millis = entry.created_at_epoch_millis,
				.build_millis = entry.build_millis,
				.estimated_memory_bytes = entry.projection->estimatedMemoryBytes(),
				.weighted = entry.projection->isWeighted(),
				.has_csr = csrCacheState.materialized,
				.csr_memory_bytes = csrCacheState.memory_bytes,
				.stale = entry.revision != currentRevision,
				.source_revision = entry.source_revision,
				.current_revision = currentRevisionToken,
				.node_labels = entry.spec.node_labels,
				.relationships = entry.spec.relationships};
	}

	uint64_t LogicalProjectionCatalog::allocateRevisionTokenLocked() const {
		if (next_revision_token_ == 0U) {
			throw std::overflow_error("logical projection catalog revision token overflowed");
		}
		const uint64_t token = next_revision_token_;
		++next_revision_token_;
		return token;
	}

	uint64_t
	LogicalProjectionCatalog::retainSourceRevisionLocked(const storage::LogicalGraphMutationFence &revision) const {
		if (const auto found = source_revision_tokens_.find(revision); found != source_revision_tokens_.end()) {
			if (found->second.source_references == std::numeric_limits<size_t>::max()) {
				throw std::overflow_error("logical projection catalog source revision reference count overflowed");
			}
			++found->second.source_references;
			last_observed_revision_ = revision;
			last_observed_revision_token_ = found->second.token;
			return found->second.token;
		}
		const uint64_t token = last_observed_revision_.has_value() && *last_observed_revision_ == revision
									   ? last_observed_revision_token_
									   : allocateRevisionTokenLocked();
		source_revision_tokens_.emplace(revision, RevisionToken{.token = token, .source_references = 1U});
		last_observed_revision_ = revision;
		last_observed_revision_token_ = token;
		return token;
	}

	void
	LogicalProjectionCatalog::releaseSourceRevisionLocked(const storage::LogicalGraphMutationFence &revision) const {
		const auto found = source_revision_tokens_.find(revision);
		if (found == source_revision_tokens_.end() || found->second.source_references == 0U) {
			throw std::logic_error("logical projection catalog source revision accounting became inconsistent");
		}
		if (found->second.source_references == 1U) {
			source_revision_tokens_.erase(found);
            if (source_revision_tokens_.empty()) source_revision_tokens_.clear();
			return;
		}
		--found->second.source_references;
	}

	uint64_t
	LogicalProjectionCatalog::currentRevisionTokenLocked(const storage::LogicalGraphMutationFence &revision) const {
		if (const auto found = source_revision_tokens_.find(revision); found != source_revision_tokens_.end()) {
			return found->second.token;
		}
		if (last_observed_revision_.has_value() && *last_observed_revision_ == revision) {
			return last_observed_revision_token_;
		}
		const uint64_t token = allocateRevisionTokenLocked();
		last_observed_revision_ = revision;
		last_observed_revision_token_ = token;
		return token;
	}

	LogicalProjectionCatalogCreatePreparation LogicalProjectionCatalog::prepareCreate(
			const LogicalGraphProjectionSpec &spec, const storage::LogicalGraphReadSnapshot &snapshot,
			const LogicalProjectionBuildBounds bounds, const LogicalProjectionBuildCheckpoint &checkpoint) {
		if (checkpoint) {
			checkpoint();
		}
		if (spec.name.empty()) {
			throw std::invalid_argument("logical projection catalog name must not be empty");
		}
		const storage::LogicalGraphMutationFence revision = requireRevision(snapshot);
		const auto checkPreparation = [&] {
			if (checkpoint) checkpoint();
			if (requireRevision(snapshot) != revision) {
				throw std::runtime_error("logical projection snapshot changed during catalog preparation");
			}
		};
		const auto buildStart = std::chrono::steady_clock::now();
		std::shared_ptr<const LogicalGraphProjection> projection = utils::makeSharedOwned<LogicalGraphProjection>(
				LogicalGraphProjection::build(snapshot, spec, bounds, checkpoint));
		const int64_t buildMillis = static_cast<int64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - buildStart)
						.count());
		const int64_t createdAtEpochMillis =
				static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
											 std::chrono::system_clock::now().time_since_epoch())
											 .count());
		checkPreparation();
		std::unique_lock lock(mutex_);
		checkPreparation();
		if (entries_.contains(spec.name)) {
			throw std::invalid_argument("logical projection catalog already contains this name");
		}
        bool retainedRevision = false;
        bool transferred = false;
        try {
            // Bind cold structural storage to this CACHE preparation, so commit
            // cannot select its caller's QUERY account for persistent metadata.
            entries_.reserve(entries_.size() + 1U);
            const uint64_t revisionToken = retainSourceRevisionLocked(revision);
            retainedRevision = true;
            auto entry = utils::makeSharedOwned<Entry>(Entry{.projection = std::move(projection), .spec = spec,
                .revision = revision, .source_revision = revisionToken,
                .created_at_epoch_millis = createdAtEpochMillis, .build_millis = buildMillis});
            LogicalProjectionCatalogCreatePreparation preparation;
            preparation.descriptor = makeDescriptor(spec.name, *entry, revision, revisionToken);
            preparation.mutation = std::unique_ptr<PreparedLogicalProjectionCatalogMutation>(
                new (utils::sharedAllocationOwner()) PreparedProjectionCatalogMutation(std::move(lock),
                    [this, name = spec.name, entry] { static_cast<void>(entries_.emplace(name, std::move(*entry))); },
                    [this, revision] {
                        releaseSourceRevisionLocked(revision);
                        if (entries_.empty()) entries_.clear();
                    }));
            transferred = true;
            return preparation;
        } catch (...) {
            // A throwing callable/constructor argument can already have moved
            // and released the lock. Reacquire before restoring revision state.
            if (!lock.owns_lock()) lock = std::unique_lock(mutex_);
            if (retainedRevision && !transferred) releaseSourceRevisionLocked(revision);
            if (entries_.empty()) entries_.clear();
            if (source_revision_tokens_.empty()) source_revision_tokens_.clear();
            throw;
        }
	}

	LogicalProjectionDescriptor LogicalProjectionCatalog::create(const LogicalGraphProjectionSpec &spec,
																		 const storage::LogicalGraphReadSnapshot &snapshot,
																		 const LogicalProjectionBuildBounds bounds,
																		 const LogicalProjectionBuildCheckpoint &checkpoint) {
		LogicalProjectionCatalogCreatePreparation preparation = prepareCreate(spec, snapshot, bounds, checkpoint);
		preparation.mutation->commit();
		return std::move(preparation.descriptor);
	}

	std::unique_ptr<PreparedLogicalProjectionCatalogMutation> LogicalProjectionCatalog::prepareDrop(const std::string &name) {
		if (name.empty()) {
			throw std::invalid_argument("logical projection catalog name must not be empty");
		}
		std::unique_lock lock(mutex_);
		const auto found = entries_.find(name);
		if (found == entries_.end()) {
			return nullptr;
		}
		return std::unique_ptr<PreparedLogicalProjectionCatalogMutation>(new (utils::sharedAllocationOwner()) PreparedProjectionCatalogMutation(
				std::move(lock),
				[this, name = utils::OwnedStdString(name), revision = found->second.revision] {
					// The prepared mutation owns the write lock, so the entry observed
					// above remains the exact entry being removed at publication.
					releaseSourceRevisionLocked(revision);
					entries_.erase(entries_.find(name));
                    if (entries_.empty()) entries_.clear();
				},
				[] {}));
	}

	bool LogicalProjectionCatalog::drop(const std::string &name) {
		std::unique_ptr<PreparedLogicalProjectionCatalogMutation> preparation = prepareDrop(name);
		if (!preparation) {
			return false;
		}
		preparation->commit();
		return true;
	}

	LogicalProjectionCatalogDropAllPreparation LogicalProjectionCatalog::prepareDropAll() {
		std::unique_lock lock(mutex_);
		LogicalProjectionCatalogDropAllPreparation preparation;
		preparation.dropped_count = entries_.size();
		preparation.mutation = std::unique_ptr<PreparedLogicalProjectionCatalogMutation>(new (utils::sharedAllocationOwner()) PreparedProjectionCatalogMutation(
				std::move(lock),
				[this] {
					entries_.clear();
					source_revision_tokens_.clear();
				},
				[] {}));
		return preparation;
	}

	size_t LogicalProjectionCatalog::dropAll() noexcept {
		// The convenience API is used during noexcept engine teardown and has no
		// result pipeline to defer. Keep it allocation-free rather than routing it
		// through the prepared mutation used by the procedure runtime.
		std::unique_lock lock(mutex_);
		const size_t dropped = entries_.size();
		entries_.clear();
		source_revision_tokens_.clear();
		return dropped;
	}

	bool LogicalProjectionCatalog::exists(const std::string &name) const {
		if (name.empty()) {
			return false;
		}
		std::shared_lock lock(mutex_);
		return entries_.contains(name);
	}

	std::shared_ptr<const LogicalGraphProjection> LogicalProjectionCatalog::get(const std::string &name) const {
		if (name.empty()) {
			throw std::invalid_argument("logical projection catalog name must not be empty");
		}
		std::shared_lock lock(mutex_);
		const auto found = entries_.find(name);
		if (found == entries_.end()) {
			throw std::out_of_range("logical projection catalog does not contain this name");
		}
		return found->second.projection;
	}

	LogicalProjectionDescriptor
	LogicalProjectionCatalog::describe(const std::string &name,
									   const storage::LogicalGraphReadSnapshot &snapshot) const {
		if (name.empty()) {
			throw std::invalid_argument("logical projection catalog name must not be empty");
		}
		const storage::LogicalGraphMutationFence revision = requireRevision(snapshot);
		uint64_t revisionToken = 0U;
		{
			std::unique_lock lock(mutex_);
			revisionToken = currentRevisionTokenLocked(revision);
		}
		std::shared_lock lock(mutex_);
		const auto found = entries_.find(name);
		if (found == entries_.end()) {
			throw std::out_of_range("logical projection catalog does not contain this name");
		}
		return makeDescriptor(found->first, found->second, revision, revisionToken);
	}

	utils::OwnedStdVector<LogicalProjectionDescriptor>
	LogicalProjectionCatalog::listOwned(const storage::LogicalGraphReadSnapshot &snapshot, const size_t maximumEntries,
								   const LogicalProjectionCatalogCheckpoint &checkpoint) const {
		if (maximumEntries == 0U) {
			throw std::invalid_argument("logical projection catalog list requires a positive entry bound");
		}
		const storage::LogicalGraphMutationFence revision = requireRevision(snapshot);
		uint64_t revisionToken = 0U;
		{
			std::unique_lock lock(mutex_);
			revisionToken = currentRevisionTokenLocked(revision);
		}
		utils::OwnedStdVector<LogicalProjectionDescriptor> result;
		{
			std::shared_lock lock(mutex_);
			if (entries_.size() > maximumEntries) {
				throw std::runtime_error("logical projection catalog list exceeded its caller-supplied entry bound");
			}
			result.reserve(entries_.size());
			for (const auto &[name, entry]: entries_) {
				if (checkpoint) {
					checkpoint();
				}
				result.push_back(makeDescriptor(name, entry, revision, revisionToken));
			}
		}
		std::ranges::sort(result, {}, &LogicalProjectionDescriptor::name);
		return result;
	}

    std::vector<LogicalProjectionDescriptor> LogicalProjectionCatalog::list(
            const storage::LogicalGraphReadSnapshot &snapshot, size_t maximumEntries,
            const LogicalProjectionCatalogCheckpoint &checkpoint) const {
        auto owned = listOwned(snapshot, maximumEntries, checkpoint);
        // Compatibility output belongs to the legacy caller. Native procedures
        // use listOwned to preserve the array's allocation authority.
        return std::move(static_cast<std::vector<LogicalProjectionDescriptor> &>(owned));
    }

} // namespace graph::query::gds
