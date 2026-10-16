/**
 * @file LogicalProjectionCatalog.hpp
 * @brief Database-owned catalog for immutable logical graph projections.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/query/gds/LogicalGraphProjection.hpp"
#include "graph/utils/OwnedLazyUnorderedMap.hpp"

namespace graph::query::gds {

	class LogicalProjectionCatalogTestAccess;

	using LogicalProjectionCatalogCheckpoint = std::function<void()>;

	struct LogicalProjectionDescriptor {
        using OwnedHeapElement = void;
        utils::OwnedStdString name;
		size_t node_count = 0U;
		size_t edge_count = 0U;
		// Projection catalog entries are process-local. These values describe the
		// completed in-memory build and are deliberately not database timestamps.
		int64_t created_at_epoch_millis = 0;
		int64_t build_millis = 0;
		size_t estimated_memory_bytes = 0U;
		bool weighted = false;
		bool has_csr = false;
		size_t csr_memory_bytes = 0U;
		bool stale = false;
		// These tokens are assigned by this process-local catalog. They preserve
		// the public stale-revision contract without exposing Superblock, page, or
		// Segment generations from the native storage format.
		uint64_t source_revision = 0U;
		uint64_t current_revision = 0U;
		utils::OwnedStringVector node_labels;
		utils::OwnedStdVector<LogicalRelationshipProjection> relationships;

		bool operator==(const LogicalProjectionDescriptor &) const = default;
	};

	class PreparedLogicalProjectionCatalogMutation {
	public:
		virtual ~PreparedLogicalProjectionCatalogMutation() = default;

		// Publishes the catalog mutation after its caller has materialized the
		// complete procedure result. Destruction without commit() abandons it.
		virtual void commit() = 0;
	};

	struct LogicalProjectionCatalogCreatePreparation {
		LogicalProjectionDescriptor descriptor;
		std::unique_ptr<PreparedLogicalProjectionCatalogMutation> mutation;
	};

	struct LogicalProjectionCatalogDropAllPreparation {
		size_t dropped_count = 0U;
		std::unique_ptr<PreparedLogicalProjectionCatalogMutation> mutation;
	};

	class LogicalProjectionCatalog final {
	public:
		[[nodiscard]] LogicalProjectionCatalogCreatePreparation
		prepareCreate(const LogicalGraphProjectionSpec &spec, const storage::LogicalGraphReadSnapshot &snapshot,
					  LogicalProjectionBuildBounds bounds, const LogicalProjectionBuildCheckpoint &checkpoint = {});
		[[nodiscard]] std::unique_ptr<PreparedLogicalProjectionCatalogMutation>
		prepareDrop(const std::string &name);
		[[nodiscard]] LogicalProjectionCatalogDropAllPreparation prepareDropAll();
		[[nodiscard]] LogicalProjectionDescriptor create(const LogicalGraphProjectionSpec &spec,
														 const storage::LogicalGraphReadSnapshot &snapshot,
														 LogicalProjectionBuildBounds bounds,
														 const LogicalProjectionBuildCheckpoint &checkpoint = {});
		[[nodiscard]] bool drop(const std::string &name);
		[[nodiscard]] size_t dropAll() noexcept;
		[[nodiscard]] bool exists(const std::string &name) const;
		[[nodiscard]] std::shared_ptr<const LogicalGraphProjection> get(const std::string &name) const;
		[[nodiscard]] LogicalProjectionDescriptor describe(const std::string &name,
														   const storage::LogicalGraphReadSnapshot &snapshot) const;
		[[nodiscard]] std::vector<LogicalProjectionDescriptor>
		list(const storage::LogicalGraphReadSnapshot &snapshot, size_t maximumEntries,
			 const LogicalProjectionCatalogCheckpoint &checkpoint = {}) const;

        [[nodiscard]] utils::OwnedStdVector<LogicalProjectionDescriptor>
        listOwned(const storage::LogicalGraphReadSnapshot &snapshot, size_t maximumEntries,
            const LogicalProjectionCatalogCheckpoint &checkpoint = {}) const;

	private:
		friend class LogicalProjectionCatalogTestAccess;

		struct Entry {
            using OwnedHeapElement = void;
			std::shared_ptr<const LogicalGraphProjection> projection;
			LogicalGraphProjectionSpec spec;
			storage::LogicalGraphMutationFence revision;
			uint64_t source_revision = 0U;
			int64_t created_at_epoch_millis = 0;
			int64_t build_millis = 0;
		};

		struct MutationFenceHash {
			[[nodiscard]] size_t operator()(const storage::LogicalGraphMutationFence &fence) const noexcept;
		};

		struct RevisionToken {
			uint64_t token = 0U;
			size_t source_references = 0U;
		};

		[[nodiscard]] static storage::LogicalGraphMutationFence
		requireRevision(const storage::LogicalGraphReadSnapshot &snapshot);
		[[nodiscard]] static LogicalProjectionDescriptor
		makeDescriptor(const std::string &name, const Entry &entry,
					   const storage::LogicalGraphMutationFence &currentRevision, uint64_t currentRevisionToken);
		[[nodiscard]] uint64_t allocateRevisionTokenLocked() const;
		[[nodiscard]] uint64_t retainSourceRevisionLocked(const storage::LogicalGraphMutationFence &revision) const;
		void releaseSourceRevisionLocked(const storage::LogicalGraphMutationFence &revision) const;
		[[nodiscard]] uint64_t currentRevisionTokenLocked(const storage::LogicalGraphMutationFence &revision) const;

		mutable std::shared_mutex mutex_;
		struct NameHash {
            using is_transparent = void;
            size_t operator()(std::string_view name) const noexcept { return std::hash<std::string_view>{}(name); }
        };
        struct NameEqual {
            using is_transparent = void;
            bool operator()(std::string_view left, std::string_view right) const noexcept { return left == right; }
        };
        utils::OwnedLazyUnorderedMap<utils::OwnedStdString, Entry, NameHash, NameEqual> entries_;
		// Source revisions are retained only while a live projection refers to
		// them. The one-entry current-revision cache avoids growing memory when a
		// long-lived catalog observes many graph generations between projections.
		mutable utils::OwnedLazyUnorderedMap<storage::LogicalGraphMutationFence, RevisionToken, MutationFenceHash>
				source_revision_tokens_;
		mutable std::optional<storage::LogicalGraphMutationFence> last_observed_revision_;
		mutable uint64_t last_observed_revision_token_ = 0U;
		mutable uint64_t next_revision_token_ = 1U;
	};

} // namespace graph::query::gds
