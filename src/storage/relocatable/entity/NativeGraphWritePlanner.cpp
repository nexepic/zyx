#include "NativeGraphWritePlannerContext.hpp"

namespace graph::storage {

	NativeGraphWriteTransaction::NativeGraphWriteTransaction(std::shared_ptr<NativeGraphWritePlannerContext> context,
															 MetadataSnapshotPin snapshot) :
			context_(std::move(context)), snapshot_(std::move(snapshot)),
			state_(NativeGraphWriteTransactionState::NGWTS_ACTIVE) {
		next_vector_id_ = snapshot_.superblock().next_logical_vector_id;
	}

	NativeGraphWriteTransaction::~NativeGraphWriteTransaction() { rollback(); }

	NativeGraphWriteTransaction::NativeGraphWriteTransaction(NativeGraphWriteTransaction &&other) noexcept :
		context_(std::move(other.context_)), snapshot_(std::move(other.snapshot_)), state_(other.state_),
		next_vector_id_(other.next_vector_id_), native_graph_coordinated_(other.native_graph_coordinated_), native_schema_coordinated_(other.native_schema_coordinated_), node_id_ranges_(std::move(other.node_id_ranges_)),
		edge_id_ranges_(std::move(other.edge_id_ranges_)), nodes_(std::move(other.nodes_)),
		edges_(std::move(other.edges_)), global_adjacency_operations_(std::move(other.global_adjacency_operations_)),
		global_adjacency_operations_canonical_(other.global_adjacency_operations_canonical_),
		global_adjacency_plan_(std::move(other.global_adjacency_plan_)),
        global_adjacency_images_(std::move(other.global_adjacency_images_)),
		global_adjacency_retirement_plan_(std::move(other.global_adjacency_retirement_plan_)),
		global_adjacency_initial_plan_(std::move(other.global_adjacency_initial_plan_)),
		global_adjacency_deferred_bulk_(other.global_adjacency_deferred_bulk_),
		properties_(std::move(other.properties_)), hybrid_catalog_credit_(std::move(other.hybrid_catalog_credit_)), hybrid_catalog_tokens_(std::move(other.hybrid_catalog_tokens_)), scalar_index_mutations_(std::move(other.scalar_index_mutations_)),
		initial_scalar_index_mutations_(std::move(other.initial_scalar_index_mutations_)),
        hybrid_vectors_(std::move(other.hybrid_vectors_)), hybrid_vector_pending_(other.hybrid_vector_pending_),
		vector_bootstraps_(std::move(other.vector_bootstraps_)), vector_deltas_(std::move(other.vector_deltas_)),
        hybrid_conversion_(std::move(other.hybrid_conversion_)), hybrid_columns_(std::move(other.hybrid_columns_)), covered_adjacency_sealed_(other.covered_adjacency_sealed_),
        covered_adjacency_pending_(other.covered_adjacency_pending_),
        covered_adjacency_request_(other.covered_adjacency_request_) {
        free_summary_credit_ = std::move(other.free_summary_credit_);
        rebuild_entity_free_summaries_ = other.rebuild_entity_free_summaries_;
        free_summary_checkpoint_ = std::move(other.free_summary_checkpoint_);
		other.state_ = NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK;
		other.next_vector_id_ = 0U;
		other.global_adjacency_operations_canonical_ = false;
		other.global_adjacency_deferred_bulk_ = false;
	}

	NativeGraphWriteTransaction &NativeGraphWriteTransaction::operator=(NativeGraphWriteTransaction &&other) noexcept {
		if (this == &other) {
			return *this;
		}
		// A live target owns snapshot and logical-ID capabilities that must be
		// abandoned before replacement; default move assignment would leak them.
		rollback();
		context_ = std::move(other.context_);
		snapshot_ = std::move(other.snapshot_);
		state_ = other.state_;
		next_vector_id_ = other.next_vector_id_;
        native_graph_coordinated_ = other.native_graph_coordinated_;
        native_schema_coordinated_ = other.native_schema_coordinated_;
		node_id_ranges_ = std::move(other.node_id_ranges_);
		edge_id_ranges_ = std::move(other.edge_id_ranges_);
		nodes_ = std::move(other.nodes_);
		edges_ = std::move(other.edges_);
		global_adjacency_operations_ = std::move(other.global_adjacency_operations_);
		global_adjacency_operations_canonical_ = other.global_adjacency_operations_canonical_;
		global_adjacency_plan_ = std::move(other.global_adjacency_plan_);
        global_adjacency_images_ = std::move(other.global_adjacency_images_);
		global_adjacency_retirement_plan_ = std::move(other.global_adjacency_retirement_plan_);
		global_adjacency_initial_plan_ = std::move(other.global_adjacency_initial_plan_);
		global_adjacency_deferred_bulk_ = other.global_adjacency_deferred_bulk_;
		properties_ = std::move(other.properties_);
        if (other.hybrid_catalog_credit_) hybrid_catalog_credit_.emplace(std::move(*other.hybrid_catalog_credit_));
        if (other.hybrid_catalog_tokens_) hybrid_catalog_tokens_.emplace(std::move(*other.hybrid_catalog_tokens_));
		scalar_index_mutations_ = std::move(other.scalar_index_mutations_);
		initial_scalar_index_mutations_ = std::move(other.initial_scalar_index_mutations_);
        if (other.hybrid_vectors_) hybrid_vectors_.emplace(std::move(*other.hybrid_vectors_));
        hybrid_vector_pending_ = other.hybrid_vector_pending_;
		vector_bootstraps_ = std::move(other.vector_bootstraps_);
		vector_deltas_ = std::move(other.vector_deltas_);
        hybrid_conversion_ = std::move(other.hybrid_conversion_);
        hybrid_columns_ = std::move(other.hybrid_columns_);
        covered_adjacency_sealed_ = other.covered_adjacency_sealed_;
        covered_adjacency_pending_ = other.covered_adjacency_pending_;
        covered_adjacency_request_ = other.covered_adjacency_request_;
        free_summary_credit_ = std::move(other.free_summary_credit_);
        rebuild_entity_free_summaries_ = other.rebuild_entity_free_summaries_;
        free_summary_checkpoint_ = std::move(other.free_summary_checkpoint_);
		other.state_ = NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK;
		other.next_vector_id_ = 0U;
		other.global_adjacency_operations_canonical_ = false;
		other.global_adjacency_deferred_bulk_ = false;
		return *this;
	}

	bool NativeGraphWriteTransaction::isActive() const {
		return state_ == NativeGraphWriteTransactionState::NGWTS_ACTIVE;
	}

	const Superblock &NativeGraphWriteTransaction::baseSuperblock() const {
		requireActive("inspect its base Superblock", true);
		return snapshot_.superblock();
	}

	const MetadataSnapshotPin &NativeGraphWriteTransaction::baseSnapshot() const {
		requireActive("inspect its pinned metadata snapshot", true);
		return snapshot_;
	}

	void NativeGraphWriteTransaction::requireActive(const char *operation, bool allowHybridConversion) const {
		if (!isActive()) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write transaction is no longer active");
		}
        if (covered_adjacency_sealed_ && !allowHybridConversion)
            fail(FormatError::FERR_INVALID_STATE, "covered candidate finalization sealed further graph staging");
        if ((hybrid_conversion_ || hybrid_columns_) && !allowHybridConversion)
            fail(FormatError::FERR_INVALID_STATE, "isolated native property mutation cannot mix with other transaction operations");
		static_cast<void>(operation);
	}

    void NativeGraphWriteTransaction::stageHybridPropertyColumns(
        std::span<const NativeHybridPropertyColumnReplacement> replacements,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation, bool fieldLifecycle) {
        stageHybridPropertyMutation(replacements, {}, std::move(memory), cancellation, fieldLifecycle);
    }
    bool NativeGraphWriteTransaction::isHybridPropertyOwner(EntityPropertyKey owner,
        const CancellationCheckpoint *cancellation) const {
        requireActive("probe native property owner");
        return context_->properties->isHybridPropertyOwner(snapshot_, owner, cancellation);
    }

    void NativeGraphWriteTransaction::stageHybridPropertyMaps(std::span<const PropertyCollectionRoot> roots,
        NativeHybridPropertyMapMode mode, std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation) {
        requireActive("stage native property maps");
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            fail(FormatError::FERR_UNSUPPORTED_VERSION, "native map staging is disabled");
        const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory);
        constexpr size_t maximumBytes = 1024U * 1024U;
        if (!account || account->used() > maximumBytes || account->available() > maximumBytes - account->used() ||
            roots.empty() || roots.size() > RSS_HYBRID_GROUP_CAPACITY ||
            (mode != NativeHybridPropertyMapMode::NHPMM_REPLACE && mode != NativeHybridPropertyMapMode::NHPMM_MERGE))
            fail(FormatError::FERR_INVALID_HEADER, "native map staging requires a bounded account, owner set and mode");
        if (!nodes_.empty() || !edges_.empty() || !properties_.empty() || !node_id_ranges_.empty() || !edge_id_ranges_.empty() ||
            !scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() || !vector_bootstraps_.empty() || !vector_deltas_.empty())
            fail(FormatError::FERR_INVALID_STATE, "native map staging requires an otherwise empty transaction");
        const std::function<void()> check = [cancellation] { if (cancellation) cancellation->check(); };
        check(); std::array<EntityPropertyKey, RSS_HYBRID_GROUP_CAPACITY> owners{}; size_t memberships = 0, inputBytes = 0;
        // Validate the complete bounded input metadata before optional heap
        // admission can conceal a later duplicate owner or malformed token.
        for (size_t i = 0; i < roots.size(); ++i) {
            check(); const auto &root = roots[i];
            if ((root.owner_entity_type != Node::typeId && root.owner_entity_type != Edge::typeId) ||
                root.entries.size() > 4096U - memberships)
                fail(FormatError::FERR_INVALID_HEADER, "native map input has an invalid owner kind or exceeds4096actual fields");
            memberships += root.entries.size(); owners[i] = {root.owner_entity_type == Node::typeId ? EntityPropertyOwnerKind::EPOK_NODE :
                EntityPropertyOwnerKind::EPOK_EDGE, root.owner_entity_id};
            if (!owners[i].isValid())
                fail(FormatError::FERR_INVALID_HEADER, "native map input requires a valid owner namespace and ID");
            for (size_t prior = 0; prior < i; ++prior) if (owners[prior] == owners[i])
                fail(FormatError::FERR_INVALID_HEADER, "native map input repeats an owner");
            uint32_t previous = 0;
            for (const auto &entry : root.entries) {
                check(); if (!entry.key_token || entry.key_token <= previous)
                    fail(FormatError::FERR_INVALID_HEADER, "native map input tokens must be strictly ordered");
                previous = entry.key_token; const auto bytes = CanonicalPropertyValueCodec::encodedSize(entry.value);
                if (bytes > maximumBytes - inputBytes)
                    fail(FormatError::FERR_INVALID_STATE, "native map canonical input exceeds its one MiB planning bound");
                inputBytes += bytes;
            }
        }
        utils::OwnedAllocationScope allocation(memory);
        utils::OwnedHeapReservation boundary(memory);
        const SelectedPropertyReadAdmission admitBoundary = [&](size_t bytes) { boundary.set(bytes); };
        for (size_t i = 0; i < roots.size(); ++i) {
            check(); if (!context_->properties->isHybridPropertyOwner(snapshot_, owners[i], cancellation, &admitBoundary))
                fail(FormatError::FERR_INVALID_STATE, "native map staging requires live owners with authenticated hybrid routing");
            boundary.set(0);
            if (owners[i].owner_kind == EntityPropertyOwnerKind::EPOK_NODE) static_cast<void>(context_->entities->readNode(snapshot_, owners[i].entity_id));
            else static_cast<void>(context_->entities->readEdge(snapshot_, owners[i].entity_id));
        }
        StagedHybridColumns staged{utils::OwnedVector<NativeHybridPropertyColumnReplacement>(
            utils::OwnedPmrAllocator<NativeHybridPropertyColumnReplacement>(memory)), memory, cancellation, true,
            utils::OwnedVector<EntityPropertyKey>(utils::OwnedPmrAllocator<EntityPropertyKey>(memory))};
        const auto append = [&](NativeHybridPropertyColumnReplacement cell) {
            if (staged.cells.size() == RSS_HYBRID_GROUP_CAPACITY)
                fail(FormatError::FERR_INVALID_STATE, "native map transaction exceeds64changed fields");
            staged.cells.push_back(std::move(cell));
        };
        for (size_t i = 0; i < roots.size(); ++i) {
            check(); const auto &root = roots[i]; const auto owner = owners[i];
            for (const auto &entry : root.entries) {
                check(); detail::PropertyDecodeWorkspace decoded(memory, &check);
                const auto previous = context_->properties->readEntityPropertyOptional(snapshot_, owner, entry.key_token);
                if (!previous || !(previous->value == entry.value))
                    append({owner, entry.key_token, OwnedPropertyValue(entry.value), false});
            }
            if (mode == NativeHybridPropertyMapMode::NHPMM_REPLACE) {
                utils::OwnedHeapReservation tokenCredit(memory), transientCredit(memory);
                const SelectedPropertyReadAdmission admitTokens = [&](size_t bytes) { tokenCredit.set(bytes); };
                const SelectedPropertyReadAdmission admitTransient = [&](size_t bytes) { transientCredit.set(bytes); };
                const auto tokens = context_->properties->readEntityPropertyTokens(snapshot_, owner, admitTokens, admitTransient, cancellation);
                transientCredit.set(0);
                for (const auto token : tokens) {
                    check(); const auto found = std::lower_bound(root.entries.begin(), root.entries.end(), token,
                        [](const PropertyCollectionRootEntry &entry, uint32_t key) { return entry.key_token < key; });
                    if (found == root.entries.end() || found->key_token != token) append({owner, token, std::monostate{}, true});
                }
            }
        }
        // All-equal maps and empty merges deliberately stage a fenced no-op;
        // old field staging still rejects an empty request from public callers.
        hybrid_columns_.emplace(std::move(staged));
    }

    void NativeGraphWriteTransaction::stageHybridPropertyOwnerDeletion(std::span<const EntityPropertyKey> owners,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation) {
        stageHybridPropertyMutation({}, owners, std::move(memory), cancellation, true);
    }
    void NativeGraphWriteTransaction::stageHybridPropertyMutation(
        std::span<const NativeHybridPropertyColumnReplacement> replacements, std::span<const EntityPropertyKey> deletions,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation, bool fieldLifecycle) {
        requireActive("stage native field or owner mutations");
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            fail(FormatError::FERR_UNSUPPORTED_VERSION, "native column publication is disabled");
        const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory);
        constexpr size_t maximumBytes = 1024U * 1024U;
        if (!account || account->used() > maximumBytes || account->available() > maximumBytes - account->used() ||
            (replacements.empty() == deletions.empty()) || replacements.size() > RSS_HYBRID_GROUP_CAPACITY ||
            deletions.size() > RSS_HYBRID_GROUP_CAPACITY ||
            !snapshot_.superblock().hybrid_property_descriptor_root.isValid())
            fail(FormatError::FERR_INVALID_HEADER, "native column staging requires a bounded set/account and native source");
        if (!nodes_.empty() || !edges_.empty() || !properties_.empty() || !node_id_ranges_.empty() || !edge_id_ranges_.empty() ||
            !scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() ||
            !vector_bootstraps_.empty() || !vector_deltas_.empty())
            fail(FormatError::FERR_INVALID_STATE, "native column staging requires an otherwise empty transaction");
        if (cancellation) cancellation->check();
        utils::OwnedAllocationScope allocationScope(memory);
        StagedHybridColumns staged{utils::OwnedVector<NativeHybridPropertyColumnReplacement>(
            utils::OwnedPmrAllocator<NativeHybridPropertyColumnReplacement>(memory)), memory, cancellation, fieldLifecycle,
            utils::OwnedVector<EntityPropertyKey>(utils::OwnedPmrAllocator<EntityPropertyKey>(memory))};
        staged.cells.reserve(replacements.size());
        for (const auto &cell : replacements) {
            if (cancellation) cancellation->check();
            if (cell.erase) { staged.cells.push_back({cell.owner, cell.token, std::monostate{}, true}); continue; }
            PropertyValue scratch;
            if (!fieldLifecycle && CanonicalPropertyValueCodec::encodedSize(cell.propertyValue(scratch)) >
                RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES - RSS_HYBRID_EXTENSION_HEADER_BYTES)
                fail(FormatError::FERR_INVALID_STATE, "oversized native cells require explicit field lifecycle");
            staged.cells.push_back(cell);
        }
        staged.deletions.assign(deletions.begin(), deletions.end());
        static_assert(std::is_nothrow_move_constructible_v<StagedHybridColumns> && std::is_nothrow_move_assignable_v<StagedHybridColumns>);
        hybrid_columns_.emplace(std::move(staged));
    }

    void NativeGraphWriteTransaction::adoptHybridPropertyCatalog(NativeGraphWriteTransaction &&catalog,
        utils::OwnedHeapReservation &&credit) {
        requireActive("coordinate native property dictionary", true);
        if (!hybrid_columns_ || !hybrid_vectors_ || hybrid_vector_pending_ || hybrid_catalog_credit_ ||
            !hybrid_columns_->deletions.empty() || !credit.governed() || credit.bytes() < 65536U || credit.owner() != hybrid_columns_->memory)
            fail(FormatError::FERR_INVALID_STATE, "native dictionary coordination requires admitted field/Vector staging");
        hybrid_vector_pending_ = true;
        if (!catalog.isActive() || catalog.context_ != context_ || catalog.baseSuperblock() != baseSuperblock() ||
            !catalog.nodes_.empty() || !catalog.edges_.empty() || !catalog.properties_.empty() ||
            !catalog.node_id_ranges_.empty() || !catalog.edge_id_ranges_.empty() ||
            !catalog.initial_scalar_index_mutations_.empty() || catalog.scalar_index_mutations_.size() != 1U ||
            !catalog.vector_bootstraps_.empty() || !catalog.vector_deltas_.empty() || catalog.hybrid_columns_ || catalog.hybrid_conversion_)
            fail(FormatError::FERR_INVALID_STATE, "native dictionary has foreign or unrelated temporary work");
        const auto &index = catalog.scalar_index_mutations_.front();
        if (index.index_id != RSS_SYSTEM_CATALOG_INDEX_ID || index.index != context_->indexes ||
            index.leaf_value_layout != BTreeLeafValueLayout::BLVL_U64 || index.operations.empty() || index.operations.size() > 192U ||
            index.plan || index.retirement_plan ||
            std::ranges::any_of(scalar_index_mutations_, [](const auto &entry) { return entry.index_id == RSS_SYSTEM_CATALOG_INDEX_ID; }))
            fail(FormatError::FERR_INVALID_STATE, "native dictionary needs one existing canonical Catalog tree");
        utils::OwnedVector<uint32_t> tokens{utils::OwnedPmrAllocator<uint32_t>(hybrid_columns_->memory)};
        for (const auto &operation : index.operations) {
            const auto kind = operation.key.empty() ? 255U : std::to_integer<unsigned>(operation.key.front());
            if (operation.kind != BTreeScalarIndexOperationKind::BSIOP_UPSERT ||
                (kind != 1U && kind != 2U && !(kind == 5U && operation.key.size() == 2U && operation.key[1] == std::byte{1})))
                fail(FormatError::FERR_INVALID_STATE, "native dictionary cannot change schema/index/constraint controls");
            if (kind == 2U) {
                if (operation.key.size() <= 5U || !operation.value || operation.value > UINT32_MAX || tokens.size() == 64U)
                    fail(FormatError::FERR_INVALID_STATE, "native dictionary has invalid token identity or cardinality");
                uint32_t encoded = 0;
                for (size_t i = 1; i <= 4; ++i) encoded = (encoded << 8U) | std::to_integer<uint32_t>(operation.key[i]);
                if (encoded != operation.value) fail(FormatError::FERR_INVALID_STATE, "native dictionary reverse token identity disagrees");
                tokens.push_back(encoded);
            }
        }
        std::ranges::sort(tokens);
        if (tokens.empty() || std::ranges::adjacent_find(tokens) != tokens.end())
            fail(FormatError::FERR_INVALID_STATE, "native dictionary has missing or duplicated token allocations");
        hybrid_catalog_tokens_.emplace(std::move(tokens));
        hybrid_catalog_credit_.emplace(std::move(credit));
        scalar_index_mutations_.push_back(std::move(catalog.scalar_index_mutations_.front()));
        std::vector<StagedScalarIndexMutation>().swap(catalog.scalar_index_mutations_);
        catalog.rollback(); hybrid_vector_pending_ = false;
    }

    void NativeGraphWriteTransaction::coordinateHybridPropertyVectors(VectorPageStore &rawPages,
        VectorManifestStore &manifests, NativeGraphVectorDeltaOptions options) {
        requireActive("coordinate native fields and Vector state", true);
        if (!hybrid_columns_ || hybrid_vectors_ || hybrid_vector_pending_ ||
            !rawPages.usesMetadataStore(*context_->metadata) || !manifests.usesMetadataStore(*context_->metadata) ||
            !options.first_slot.isValid() || options.last_slot < options.first_slot ||
            !options.maximum_records || options.maximum_records > 128 || !options.maximum_raw_pages || options.maximum_raw_pages > 128 ||
            !options.rebuild_threshold || options.rebuild_threshold > options.maximum_overlay_entries ||
            options.maximum_overlay_entries > 4096 || !options.maximum_delta_pages || options.maximum_delta_pages > 128 ||
            options.maximum_overlay_entries > VectorDeltaMapping::maximumEntriesForPageBudget(options.maximum_delta_pages))
            fail(FormatError::FERR_INVALID_HEADER, "native Vector coordination requires one staged field mutation and bounded same-domain stores");
        auto &fields = *hybrid_columns_;
        const std::function<void()> check = [&] { if (fields.cancellation) fields.cancellation->check(); };
        check(); hybrid_vector_pending_ = true;
        utils::OwnedAllocationScope allocation(fields.memory);
        hybrid_vectors_.emplace(StagedHybridVectors{utils::OwnedHeapReservation(fields.memory), {}});
        auto &staged = *hybrid_vectors_;
        const auto admit = [&](size_t bytes) {
            constexpr size_t limit = 1024U * 1024U;
            if (bytes > limit - staged.credit.bytes()) throw query::execution::MaterializedByteLimitExceeded();
            staged.credit.set(staged.credit.bytes() + bytes);
        };
        SystemCatalog schema(*context_->indexes, *context_->metadata);
        auto definitions = schema.readIndexDefinitions(snapshot_, 4096);
        std::erase_if(definitions, [](const auto &d) { return d.kind != IndexDefinitionKind::IDK_VECTOR; });
        utils::OwnedStdVector<uint64_t> owners;
        for (const auto &cell : fields.cells) if (cell.owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE &&
            std::ranges::find(owners, cell.owner.entity_id) == owners.end()) owners.push_back(cell.owner.entity_id);
        for (const auto owner : fields.deletions) if (owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE) owners.push_back(owner.entity_id);
        const auto erased = [&](uint64_t id) { return std::ranges::any_of(fields.deletions, [&](const auto owner) {
            return owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE && owner.entity_id == id;
        }); };
        const auto affected = [&](uint64_t id, uint32_t token) {
            return erased(id) || std::ranges::any_of(fields.cells, [&](const auto &cell) {
                return cell.owner == EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, id} && cell.token == token;
            });
        };
        // Admit retained semantic/raw buffers and the ordinary planner's bounded
        // map/manifest frontier before invoking that planner. This is not an RSS
        // or production admission claim; the existing one-MiB account remains.
        for (const auto &definition : definitions) {
            check(); const auto token = schema.findPropertyToken(snapshot_, definition.property_names.front());
            if (!token || definition.vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS)
                fail(FormatError::FERR_INVALID_STATE, "native Vector dependency has an unresolved token or excessive dimensions");
            const size_t count = std::ranges::count_if(owners, [&](uint64_t id) { return affected(id, *token); });
            if (!count) continue;
            admit(65536U + count * (size_t(definition.vector_dimensions) * 8U + 1024U));
            const auto root = context_->metadata->findIndexRootOptional(snapshot_, definition.index_id);
            const auto manifest = context_->metadata->findVectorManifestOptional(snapshot_, definition.index_id);
            if (bool(root) != bool(manifest)) fail(FormatError::FERR_INVALID_STATE, "native Vector state has an incomplete mapping/Manifest pair");
            if (manifest) admit(size_t(manifests.read(snapshot_, definition.index_id).manifest().delta_entry_count) * 512U);
        }
        NativeVectorMutationPlanner vector(std::move(definitions),
            [&](const MetadataSnapshotPin &pin, std::string_view name) { return schema.findPropertyToken(pin, name); },
            {4096, options.maximum_records});
        for (const auto id : owners) {
            check(); detail::PropertyDecodeWorkspace decoded(fields.memory, &check);
            utils::OwnedMap<uint32_t, std::optional<PropertyCollectionRootEntry>> previous;
            utils::OwnedHeapReservation labelsCredit(fields.memory);
            const auto labels = context_->labels->readNodeLabels(snapshot_, context_->entities->readNode(snapshot_, id),
                [&](size_t bytes) { labelsCredit.set(bytes); });
            std::array<PropertyValue, RSS_HYBRID_GROUP_CAPACITY> scratch;
            const auto oldValue = [&](uint32_t token) -> const PropertyValue * {
                check(); if (!affected(id, token)) return nullptr;
                auto found = previous.find(token);
                if (found == previous.end()) found = previous.emplace(token,
                    context_->properties->readEntityPropertyOptional(snapshot_, {EntityPropertyOwnerKind::EPOK_NODE, id}, token)).first;
                return found->second ? &found->second->value : nullptr;
            };
            const auto newValue = [&](uint32_t token) -> const PropertyValue * {
                check(); if (erased(id) || !affected(id, token)) return nullptr;
                for (size_t i = 0; i < fields.cells.size(); ++i) {
                    const auto &cell = fields.cells[i];
                    if (cell.owner == EntityPropertyKey{EntityPropertyOwnerKind::EPOK_NODE, id} && cell.token == token) {
                        if (cell.erase) return nullptr;
                        const auto *value = &cell.propertyValue(scratch[i]);
                        const auto *previous = oldValue(token);
                        // Field writes use ROW equality before Vector encoding:
                        // signed-zero differences alone must not create a delta
                        // for a property mutation that the field writer omits.
                        return previous && *previous == *value ? previous : value;
                    }
                }
                return oldValue(token);
            };
            for (auto mutation : vector.planNodeValues(snapshot_, id, labels, oldValue,
                erased(id) ? std::span<const uint64_t>{} : std::span<const uint64_t>(labels), newValue)) {
                if (staged.semantics.size() == options.maximum_records)
                    fail(FormatError::FERR_INVALID_STATE, "native Vector coordination exceeds its combined semantic operation bound");
                staged.semantics.push_back(std::move(mutation));
            }
        }
        std::ranges::sort(staged.semantics, {}, [](const auto &v) {
            return std::tuple(v.index_id, v.node_id, static_cast<uint8_t>(v.kind));
        });
        // Keep ordinary staging permission confined to a separate transaction.
        // Cancellation callbacks never gain permission to append unrelated work
        // to this sealed native field transaction.
        NativeGraphWriteTransaction vectorTransaction(context_, context_->metadata->openSnapshot());
        if (vectorTransaction.baseSuperblock() != snapshot_.superblock())
            fail(FormatError::FERR_INVALID_STATE, "native Vector planning source changed before coordination");
        for (size_t first = 0; first < staged.semantics.size();) {
            size_t last = first + 1;
            while (last < staged.semantics.size() && staged.semantics[last].index_id == staged.semantics[first].index_id) ++last;
            const auto index = staged.semantics[first].index_id;
            const auto definition = std::ranges::find(vector.definitions(), index, &IndexDefinition::index_id);
            if (definition == vector.definitions().end()) fail(FormatError::FERR_INVALID_STATE, "native Vector coordination lost its schema");
            const auto mutations = std::span(staged.semantics).subspan(first, last - first);
            if (context_->metadata->findIndexRootOptional(snapshot_, index)) {
                vectorTransaction.stageVectorIndexDelta(*context_->indexes, rawPages, manifests, *definition, mutations, options);
            } else {
                std::vector<NativeGraphVectorBootstrapRecord, utils::OwnedPmrAllocator<NativeGraphVectorBootstrapRecord>> records;
                for (const auto &mutation : mutations) {
                    if (mutation.kind != NativeVectorMutationKind::NVMK_INSERT)
                        fail(FormatError::FERR_INVALID_STATE, "native Vector bootstrap cannot erase absent state");
                    records.push_back({mutation.node_id, mutation.encoded_bfloat16});
                }
                NativeGraphVectorBootstrapOptions bootstrap{options.first_slot, options.last_slot,
                    options.maximum_records, options.maximum_raw_pages + 8U, check};
                vectorTransaction.stageVectorIndexBootstrap(*context_->indexes, rawPages, manifests, *definition, records, bootstrap);
            }
            first = last;
        }
        check();
        scalar_index_mutations_ = std::move(vectorTransaction.scalar_index_mutations_);
        initial_scalar_index_mutations_ = std::move(vectorTransaction.initial_scalar_index_mutations_);
        vector_bootstraps_ = std::move(vectorTransaction.vector_bootstraps_);
        vector_deltas_ = std::move(vectorTransaction.vector_deltas_);
        next_vector_id_ = vectorTransaction.next_vector_id_;
        hybrid_vector_pending_ = false;
    }

    void NativeGraphWriteTransaction::stageHybridPropertyConversion(EntityPropertyOwnerKind kind, uint64_t groupId,
            std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation, bool publishCandidate,
            NativeHybridPropertyConversionDirection direction, bool compactColumnMemberships,
            std::optional<NativeHybridPropertyAllocationAdmission> allocationAdmission, uint8_t groupCount,
            bool coldRowRemainder) {
        requireActive("stage a native property group conversion");
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            fail(FormatError::FERR_UNSUPPORTED_VERSION, "native property group conversion is disabled");
        const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory);
        constexpr size_t maximumBytes = 1024U * 1024U;
        const bool privateBuilding = RSS_SCAN_PATH_FORMAT_ENABLED &&
            (snapshot_.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) != 0;
        if (coldRowRemainder && (!compactColumnMemberships ||
            direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID ||
            (publishCandidate && !allocationAdmission && !privateBuilding)))
            fail(FormatError::FERR_INVALID_HEADER, "cold ROW conversion requires compact forward layout and whole-database admission");
        if (!groupCount || groupCount > 8U || groupId > RSS_HYBRID_MAX_GROUP_ID - (groupCount - 1U) ||
            (groupCount > 1U && (!compactColumnMemberships || (!allocationAdmission && !privateBuilding) || !publishCandidate ||
                direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID)))
            fail(FormatError::FERR_INVALID_HEADER, "native cohort requires1..8groups and explicit gated compact forward publication");
        if (!hybrid_property_detail::validKind(kind) || !groupId || groupId > RSS_HYBRID_MAX_GROUP_ID ||
            (direction != NativeHybridPropertyConversionDirection::NHPCD_ROW_TO_HYBRID &&
                direction != NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW) || !account ||
            account->used() > maximumBytes || account->available() > maximumBytes - account->used())
            fail(FormatError::FERR_INVALID_HEADER, "native property conversion requires one bounded group/account");
        if (!nodes_.empty() || !edges_.empty() || !properties_.empty() || !node_id_ranges_.empty() ||
            !edge_id_ranges_.empty() || !scalar_index_mutations_.empty() || !initial_scalar_index_mutations_.empty() ||
            !vector_bootstraps_.empty() || !vector_deltas_.empty())
            fail(FormatError::FERR_INVALID_STATE, "native property conversion requires an otherwise empty transaction");
        const auto &source = snapshot_.superblock();
        const auto &placement = context_->options.property_roots;
        if (cancellation) cancellation->check();
        hybrid_conversion_ = NativeGraphHybridPropertyRootMutation{source.database_id,
            source.hybrid_property_descriptor_root, NativeHybridPropertyRootMode::NHPRM_REPLACE, {},
            NativeGraphHybridPropertyConversion{kind, groupId, std::move(memory), cancellation,
                placement.first_slot, placement.last_slot, placement.page_owner_id, publishCandidate, direction,
                std::move(allocationAdmission), compactColumnMemberships, groupCount, coldRowRemainder}};
    }

	NativeGraphWriteTransaction::NodeChange &NativeGraphWriteTransaction::mutableNode(uint64_t nodeId) {
		requireActive("modify a Node");
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph write requires a nonzero Node ID");
		}
		const auto existing = nodes_.find(nodeId);
		if (existing != nodes_.end()) {
			if (existing->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot modify an erased Node");
			}
			return existing->second;
		}
		NodeChange change;
		change.record = context_->entities->readNode(snapshot_, nodeId);
		auto [iterator, inserted] = nodes_.emplace(nodeId, std::move(change));
		static_cast<void>(inserted);
		return iterator->second;
	}

	NativeGraphWriteTransaction::EdgeChange &NativeGraphWriteTransaction::mutableEdge(uint64_t edgeId) {
		requireActive("modify an Edge");
		if (edgeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph write requires a nonzero Edge ID");
		}
		const auto existing = edges_.find(edgeId);
		if (existing != edges_.end()) {
			if (existing->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot modify an erased Edge");
			}
			return existing->second;
		}
		EdgeChange change;
		change.record = context_->entities->readEdge(snapshot_, edgeId);
		auto [iterator, inserted] = edges_.emplace(edgeId, std::move(change));
		static_cast<void>(inserted);
		return iterator->second;
	}

	uint64_t NativeGraphWriteTransaction::createNode(std::span<const uint64_t> labels) {
		requireActive("create a Node");
		validateLabels(labels);
		leaseAndAppendLogicalIdRanges(*context_, node_id_ranges_, true, 1U);
		const uint64_t nodeId = node_id_ranges_.back().first_id;
		NodeChange change;
		change.inserted = true;
		change.labels = std::vector<uint64_t>(labels.begin(), labels.end());
		const bool inserted = nodes_.emplace(nodeId, std::move(change)).second;
		if (!inserted) { // ZYX_COV_EXCL_LINE: each newly leased logical ID is unique and cannot already exist in this transaction map.
			fail(FormatError::FERR_INVALID_STATE, "native graph write allocated a duplicate Node ID");
		}
		return nodeId;
	}

	uint64_t NativeGraphWriteTransaction::createEdge(uint64_t sourceNodeId, uint64_t targetNodeId, uint64_t typeId) {
		requireActive("create an Edge");
		// Reserve both directions before changing any staged Entity state. This keeps
		// a rejected bounded write from leaving a one-direction adjacency mutation.
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 2U,
										  context_->options.maximum_global_adjacency_write_entries);
		static_cast<void>(mutableNode(sourceNodeId));
		if (sourceNodeId != targetNodeId) {
			static_cast<void>(mutableNode(targetNodeId));
		}
		leaseAndAppendLogicalIdRanges(*context_, edge_id_ranges_, false, 1U);
		const uint64_t edgeId = edge_id_ranges_.back().first_id;
		EdgeChange change;
		change.inserted = true;
		change.record.source_node_id = sourceNodeId;
		change.record.target_node_id = targetNodeId;
		change.record.type_id = typeId;
		const bool inserted = edges_.emplace(edgeId, std::move(change)).second;
		if (!inserted) { // ZYX_COV_EXCL_LINE: an adopted lease is validated as unique by the metadata capability registry.
			fail(FormatError::FERR_INVALID_STATE, "native graph write allocated a duplicate Edge ID");
		}
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_OUT, sourceNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_IN, targetNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		return edgeId;
	}

	void NativeGraphWriteTransaction::adoptLogicalIdReservation(NativeGraphLogicalIdReservation reservation) {
		requireActive("adopt logical-ID reservations");
		if (!nodes_.empty() || !edges_.empty() || !node_id_ranges_.empty() || !edge_id_ranges_.empty()) {
			fail(FormatError::FERR_INVALID_STATE,
					 "native graph logical-ID reservations must be adopted before staging Entity mutations");
		}
		// Validate both entity kinds before changing either retained range. An
		// externally issued reservation stays whole when one kind is malformed.
		std::vector<NativeGraphLogicalIdLeaseRange> nextNodeRanges = node_id_ranges_;
		std::vector<NativeGraphLogicalIdLeaseRange> nextEdgeRanges = edge_id_ranges_;
		appendLogicalIdRanges(nextNodeRanges, reservation.node_ranges, context_->options.maximum_logical_id_reservation);
		appendLogicalIdRanges(nextEdgeRanges, reservation.edge_ranges, context_->options.maximum_logical_id_reservation);
		node_id_ranges_.swap(nextNodeRanges);
		edge_id_ranges_.swap(nextEdgeRanges);
	}

	uint64_t NativeGraphWriteTransaction::createNodeWithReservedId(uint64_t nodeId, std::span<const uint64_t> labels) {
		requireActive("create a Node with a logical-ID reservation");
		validateLabels(labels);
		if (nodeId == 0U ||
			!std::ranges::any_of(node_id_ranges_, [nodeId](const NativeGraphLogicalIdLeaseRange &range) {
				return range.contains(nodeId);
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write attempted to create a Node outside its adopted logical-ID lease");
		}
		NodeChange change;
		change.inserted = true;
		change.labels = std::vector<uint64_t>(labels.begin(), labels.end());
		if (!nodes_.emplace(nodeId, std::move(change)).second) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write adopted a duplicate Node ID");
		}
		return nodeId;
	}

	uint64_t NativeGraphWriteTransaction::createEdgeWithReservedId(uint64_t edgeId, uint64_t sourceNodeId,
																   uint64_t targetNodeId, uint64_t typeId) {
		requireActive("create an Edge with a logical-ID reservation");
		requireAdjacencyOperationCapacity(global_adjacency_operations_.size(), 2U,
										  context_->options.maximum_global_adjacency_write_entries);
		if (edgeId == 0U ||
			!std::ranges::any_of(edge_id_ranges_, [edgeId](const NativeGraphLogicalIdLeaseRange &range) {
				return range.contains(edgeId);
			})) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph write attempted to create an Edge outside its adopted logical-ID lease");
		}
		static_cast<void>(mutableNode(sourceNodeId));
		if (sourceNodeId != targetNodeId) {
			static_cast<void>(mutableNode(targetNodeId));
		}
		EdgeChange change;
		change.inserted = true;
		change.record.source_node_id = sourceNodeId;
		change.record.target_node_id = targetNodeId;
		change.record.type_id = typeId;
		if (!edges_.emplace(edgeId, std::move(change)).second) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write adopted a duplicate Edge ID");
		}
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_OUT, sourceNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		stageGlobalAdjacencyMutation(GlobalAdjacencyDirection::GAD_IN, targetNodeId, edgeId,
									 BTreeScalarIndexOperationKind::BSIOP_UPSERT);
		return edgeId;
	}

	void NativeGraphWriteTransaction::reserveNodeIds(uint64_t count) {
		requireActive("reserve Node IDs");
		if (count == 0U) {
			return;
		}
		leaseAndAppendLogicalIdRanges(*context_, node_id_ranges_, true, count);
	}

	void NativeGraphWriteTransaction::reserveEdgeIds(uint64_t count) {
		requireActive("reserve Edge IDs");
		if (count == 0U) {
			return;
		}
		leaseAndAppendLogicalIdRanges(*context_, edge_id_ranges_, false, count);
	}

	std::vector<uint64_t> NativeGraphWriteTransaction::collectIncidentEdgeIds(uint64_t nodeId,
																			  size_t maximumEdges) const {
		requireActive("enumerate a Node's incident Edges");
		if (nodeId == 0U || maximumEdges == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph incident Edge enumeration requires a Node ID and positive bound");
		}
		const auto validateNode = [this, nodeId] {
			const auto staged = nodes_.find(nodeId);
			if (staged != nodes_.end()) {
				if (staged->second.erased) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native graph incident Edge enumeration references an erased Node");
				}
				return;
			}
			static_cast<void>(context_->entities->readNode(snapshot_, nodeId));
		};
		validateNode();
		// The snapshot still contains Edges erased earlier in this write
		// transaction. Account for each final-state tombstone before choosing the
		// bounded raw scan, then apply the complete adjacency overlay before
		// evaluating the logical incident-Edge limit.
		std::set<uint64_t> erasedIncidentEdges;
		for (const BTreeScalarIndexOperation &operation: global_adjacency_operations_) {
			if (operation.kind != BTreeScalarIndexOperationKind::BSIOP_ERASE) {
				continue;
			}
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(operation.key);
			if (key.node_id == nodeId) {
				erasedIncidentEdges.insert(key.edge_id);
			}
		}
		if (erasedIncidentEdges.size() > std::numeric_limits<size_t>::max() - maximumEdges) {
			fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
				 "native graph incident Edge enumeration cannot derive its bounded overlay scan");
		}
		const size_t rawScanLimit = maximumEdges + erasedIncidentEdges.size();
		std::set<uint64_t> incident;
		const auto collectDirection = [this, nodeId, rawScanLimit, &incident](GlobalAdjacencyDirection direction) {
			const GlobalAdjacencyRange range = GlobalAdjacencyIndex(*context_->indexes, *context_->metadata)
															 .scan(snapshot_, direction, nodeId, rawScanLimit);
			for (const uint64_t edgeId: range.edge_ids) {
				incident.insert(edgeId);
			}
			return range.continuation.has_value();
		};
		const bool outgoingHasMore = collectDirection(GlobalAdjacencyDirection::GAD_OUT);
		const bool incomingHasMore = collectDirection(GlobalAdjacencyDirection::GAD_IN);
		for (const BTreeScalarIndexOperation &operation: global_adjacency_operations_) {
			const GlobalAdjacencyKey key = AdjacencyKeyCodec::decode(operation.key);
			if (key.node_id != nodeId) {
				continue;
			}
			if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT) {
				incident.insert(key.edge_id);
			} else if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_ERASE) {
				incident.erase(key.edge_id);
			} else {
				fail(FormatError::FERR_INVALID_STATE,
					 "native graph global adjacency stream has an unknown operation");
			}
		}
		if (outgoingHasMore || incomingHasMore || incident.size() > maximumEdges) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph incident Edge enumeration exceeded its caller-supplied adjacency bound");
		}
		return {incident.begin(), incident.end()};
	}

	bool NativeGraphWriteTransaction::nodeHasIncidentEdges(uint64_t nodeId) const {
		requireActive("inspect a Node's relationship heads");
		if (nodeId == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph relationship-head inspection requires a Node ID");
		}
		return !collectIncidentEdgeIds(nodeId, context_->options.maximum_adjacency_read_edges).empty();
	}

	void NativeGraphWriteTransaction::replaceNodeLabels(uint64_t nodeId, std::span<const uint64_t> labels) {
		requireActive("replace Node labels");
		validateLabels(labels);
		NodeChange &change = mutableNode(nodeId);
		change.labels = std::vector<uint64_t>(labels.begin(), labels.end());
	}

	void NativeGraphWriteTransaction::replaceNodeProperty(PropertyCollectionRoot root) {
		requireActive("replace Node Properties");
			if (root.owner_entity_type != Node::typeId || root.owner_entity_id == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph write Node Property roots require a direct owner");
		}
		const auto nodeChange = nodes_.find(root.owner_entity_id);
		if (nodeChange != nodes_.end()) {
			if (nodeChange->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot attach Properties to an erased Node");
			}
		} else {
			// Property-only writes do not COW an unchanged fixed Entity record.
			static_cast<void>(context_->entities->readNode(snapshot_, root.owner_entity_id));
		}
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, root.owner_entity_id};
		properties_.insert_or_assign(key.directoryId(), std::move(root));
	}

	void NativeGraphWriteTransaction::replaceEdgeProperty(PropertyCollectionRoot root) {
		requireActive("replace Edge Properties");
			if (root.owner_entity_type != Edge::typeId || root.owner_entity_id == 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native graph write Edge Property roots require a direct owner");
		}
		const auto edgeChange = edges_.find(root.owner_entity_id);
		if (edgeChange != edges_.end()) {
			if (edgeChange->second.erased) {
				fail(FormatError::FERR_INVALID_STATE, "native graph write cannot attach Properties to an erased Edge");
			}
		} else {
			// Property-only writes do not COW an unchanged fixed Entity record.
			static_cast<void>(context_->entities->readEdge(snapshot_, root.owner_entity_id));
		}
		const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_EDGE, root.owner_entity_id};
		properties_.insert_or_assign(key.directoryId(), std::move(root));
	}

    void NativeGraphWriteTransaction::stageCoveredAdjacencyCandidate(uint64_t indexId, size_t maximumChangedEdges,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *stageCancellation,
        const NativeScalarIndexMutationPlanner *typeIndexes, SystemCatalog *statistics) {
        requireActive("finalize covered adjacency candidate");
        const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory);
        if (!indexId || (indexId >> 62U) || !maximumChangedEdges || maximumChangedEdges > 64U || !account ||
            account->used() > 1024U * 1024U || account->available() > 1024U * 1024U - account->used())
            fail(FormatError::FERR_INVALID_HEADER, "covered candidate finalization requires a lab namespace and bounded account");
        const CoveredAdjacencyRequest participants{indexId, typeIndexes, statistics};
        if (covered_adjacency_pending_ && covered_adjacency_request_ != participants)
            fail(FormatError::FERR_INVALID_STATE, "covered candidate retry cannot change its root or derived-state participants");
        covered_adjacency_request_ = participants;
        covered_adjacency_pending_ = true;
        const auto check = [&] { if (stageCancellation) stageCancellation->check(); };
        check();
        if (std::ranges::any_of(scalar_index_mutations_, [indexId](const auto &entry) { return entry.index_id == indexId; }) ||
            std::ranges::any_of(initial_scalar_index_mutations_, [indexId](const auto &entry) { return entry.index_id == indexId; }))
            fail(FormatError::FERR_INVALID_STATE, "covered candidate cannot mix manual and coordinator index changes");
        size_t count = 0;
        for (const auto &[id, edge] : edges_) {
            check(); static_cast<void>(id);
            if (edge.inserted == edge.erased) continue;
            if (++count > maximumChangedEdges)
                fail(FormatError::FERR_INVALID_STATE, "covered candidate finalization exceeds its changed Edge bound");
        }
        size_t changedNodes = 0;
        for (const auto &[id, node] : nodes_) {
            check(); static_cast<void>(id);
            if ((node.inserted || node.erased || node.labels.has_value()) && ++changedNodes > maximumChangedEdges - count)
                fail(FormatError::FERR_INVALID_STATE, "covered candidate finalization exceeds its combined Entity bound");
        }
        utils::OwnedAllocationScope allocation(memory); utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        using Candidate = CoveredAdjacencyCandidate;
        Candidate::Map map(indexId, *context_->indexes, *context_->metadata);
        if (!context_->metadata->findIndexRootOptional(snapshot_, indexId))
            fail(FormatError::FERR_INVALID_STATE, "covered candidate coordinator requires its existing root");
        std::optional<FixedBTreeMapMutation<RSS_GLOBAL_ADJACENCY_KEY_BYTES, 16>> changes;
        if (count) changes.emplace(*this, *context_->indexes, *context_->metadata, indexId, count * 2U, *memory);
        else {
            // Bind/authenticate the existing root and first leaf even though
            // this batch will not change any candidate entries (including
            // property-only and logical-ID-reservation publications).
            static_cast<void>(map.visitAll(snapshot_, 1, [](const auto &) { return false; }, admit, stageCancellation));
            boundary.set(0);
        }
        // Authenticate every old direction before staging any derived delta.
        // Completeness of unrelated entries still belongs to verifyIntegrity.
        for (const auto &[id, edge] : edges_) {
            check(); if (edge.inserted == edge.erased) continue;
            for (const auto direction : {GlobalAdjacencyDirection::GAD_OUT, GlobalAdjacencyDirection::GAD_IN}) {
                check(); const auto expected = Candidate::entry(direction, id, edge.record.source_node_id,
                    edge.record.target_node_id, edge.record.type_id);
                const auto previous = map.find(snapshot_, expected.key, admit); boundary.set(0);
                if (edge.inserted ? previous.has_value() : previous != std::optional(expected.value))
                    fail(FormatError::FERR_INVALID_STATE, "covered candidate old direction disagrees with authoritative Edge");
                if (edge.erased) changes->erase(expected.key, admit);
                else changes->set(expected.key, expected.value, admit);
                boundary.set(0);
            }
        }
        // Keep all derived state in this transaction. Missing statistics stay unknown;
        // a complete token namespace is updated only from its pinned counts.
        if ((typeIndexes && !typeIndexes->usesMetadataStore(*context_->metadata)) ||
            (statistics && !statistics->usesMetadataStore(*context_->metadata)))
            fail(FormatError::FERR_INVALID_STATE, "covered candidate derived state has a foreign metadata domain");
        if (typeIndexes) {
            for (const auto &definition : typeIndexes->definitions())
                if (definition.index_id == indexId)
                    fail(FormatError::FERR_INVALID_STATE, "covered candidate cannot alias a schema index");
        }
        if (statistics) {
            const auto graph = statistics->readGraphCardinalityStatistics(snapshot_);
            const auto kind = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
            const auto labelKind = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
            const bool complete = statistics->hasCompleteTokenCardinality(snapshot_, kind);
            const bool labelsComplete = statistics->hasCompleteTokenCardinality(snapshot_, labelKind);
            auto updated = graph;
            using Counter = std::pair<SystemCatalogSchemaTokenKind, uint64_t>;
            std::pmr::map<Counter, int64_t> deltas(memory.get());
            const auto changeCount = [&](uint64_t &value, bool erased) {
                if ((erased && !value) || (!erased && value >= UINT64_MAX - 1U))
                    fail(FormatError::FERR_INVALID_STATE, "covered candidate graph count underflows or overflows");
                value = erased ? value - 1U : value + 1U;
            };
            for (const auto &[id, edge] : edges_) {
                check(); static_cast<void>(id); if (edge.inserted == edge.erased) continue;
                if (updated) changeCount(updated->edge_count, edge.erased);
                if (complete) deltas[{kind, edge.record.type_id}] += edge.erased ? -1 : 1;
            }
            for (const auto &[id, node] : nodes_) {
                check(); static_cast<void>(id);
                if (!node.inserted && !node.erased && !node.labels) continue;
                if (updated && node.inserted != node.erased) changeCount(updated->node_count, node.erased);
                if (!labelsComplete) continue;
                if (!node.inserted) {
                    {
                        const auto previous = context_->labels->readNodeLabels(snapshot_, node.record, admit);
                        for (const auto token : previous) { check(); --deltas[{labelKind, token}]; }
                    }
                    boundary.set(0);
                }
                if (!node.erased && node.labels)
                    for (const auto token : *node.labels) { check(); ++deltas[{labelKind, token}]; }
            }
            // Validate all counters before staging any catalog updates.
            std::pmr::map<Counter, uint64_t> counts(memory.get());
            for (const auto &[counter, delta] : deltas) {
                check(); if (!delta) continue; const auto previous = statistics->readTokenCardinality(snapshot_, counter.first, counter.second);
                if (!previous || (delta < 0 && *previous < uint64_t(-delta)) ||
                    (delta > 0 && *previous > UINT64_MAX - 1U - uint64_t(delta)))
                    fail(FormatError::FERR_INVALID_STATE, "covered candidate token count is unknown or overflows");
                counts[counter] = delta < 0 ? *previous - uint64_t(-delta) : *previous + uint64_t(delta);
            }
            if (updated != graph || !counts.empty()) {
                auto session = statistics->beginWrite(*this);
                if (updated != graph) session.setGraphCardinalityStatistics(*updated);
                for (const auto &[counter, value] : counts) { check(); session.setTokenCardinality(counter.first, counter.second, value); }
                session.finish();
            }
        }
        if (typeIndexes) for (const auto &[id, edge] : edges_) {
            check(); if (edge.inserted != edge.erased)
                typeIndexes->stageEdgeType(*this, id, edge.record.type_id, edge.erased);
        }
        check(); if (changes) changes->stage();
        covered_adjacency_sealed_ = true;
        covered_adjacency_pending_ = false;
    }

	void NativeGraphWriteTransaction::stageScalarIndexUpsert(BTreePageStore &index, uint64_t indexId,
															 std::span<const std::byte> key, uint64_t value, BTreeLeafValueLayout leafValueLayout) {
		requireActive("stage a scalar Index upsert");
		if ((leafValueLayout != BTreeLeafValueLayout::BLVL_U64 && leafValueLayout != BTreeLeafValueLayout::BLVL_EMPTY) ||
			(leafValueLayout == BTreeLeafValueLayout::BLVL_EMPTY && value != 0U)) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph staged Index has an invalid leaf value/layout");
		}
		if (indexId == 0U || isReservedInternalIndexId(indexId) || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index upsert requires a non-reserved Index in this metadata domain");
		}
		const auto initial =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (initial != initial_scalar_index_mutations_.end() && initial->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for first publication");
		}
		auto existing = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existing == scalar_index_mutations_.end() || existing->index_id != indexId) {
			existing = scalar_index_mutations_.insert(existing, {&index, indexId, {}, std::nullopt, std::nullopt, leafValueLayout});
		}
		if (existing->index != &index || existing->leaf_value_layout != leafValueLayout) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index mutation changed its page-store identity or leaf layout");
		}
		if (existing->retirement_plan.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for whole-tree retirement");
		}
		existing->operations.push_back(
				{BTreeScalarIndexOperationKind::BSIOP_UPSERT, std::vector<std::byte>(key.begin(), key.end()), value});
		existing->plan.reset();
	}

	void NativeGraphWriteTransaction::stageScalarIndexErase(BTreePageStore &index, uint64_t indexId,
															std::span<const std::byte> key, BTreeLeafValueLayout leafValueLayout) {
		requireActive("stage a scalar Index erase");
		if (leafValueLayout != BTreeLeafValueLayout::BLVL_U64 && leafValueLayout != BTreeLeafValueLayout::BLVL_EMPTY) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph staged Index has an invalid leaf layout");
		}
		if (indexId == 0U || isReservedInternalIndexId(indexId) || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index erase requires a non-reserved Index in this metadata domain");
		}
		const auto initial =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (initial != initial_scalar_index_mutations_.end() && initial->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for first publication");
		}
		auto existing = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existing == scalar_index_mutations_.end() || existing->index_id != indexId) {
			existing = scalar_index_mutations_.insert(existing, {&index, indexId, {}, std::nullopt, std::nullopt, leafValueLayout});
		}
		if (existing->index != &index || existing->leaf_value_layout != leafValueLayout) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index mutation changed its page-store identity or leaf layout");
		}
		if (existing->retirement_plan.has_value()) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot mutate a scalar Index staged for whole-tree retirement");
		}
		existing->operations.push_back(
				{BTreeScalarIndexOperationKind::BSIOP_ERASE, std::vector<std::byte>(key.begin(), key.end()), 0U});
		existing->plan.reset();
	}

	void NativeGraphWriteTransaction::stageScalarIndexCreate(BTreePageStore &index, uint64_t indexId,
															 std::span<const BTreePageEntry> entries) {
		requireActive("stage a scalar Index creation");
		if (indexId == 0U || isReservedInternalIndexId(indexId) || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index creation requires a non-reserved Index in this metadata domain");
		}
		const auto existingMutation = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existingMutation != scalar_index_mutations_.end() && existingMutation->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot create and mutate the same scalar Index in one transaction");
		}
		auto existingCreation =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (existingCreation != initial_scalar_index_mutations_.end() && existingCreation->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index creation duplicates one logical Index ID");
		}
		BTreeInitialTreeMutationPlan plan = index.planInitialTreeMutation(snapshot_, indexId, entries);
		stageScalarIndexCreate(index, std::move(plan));
	}

	void NativeGraphWriteTransaction::stageScalarIndexCreate(BTreePageStore &index, BTreeInitialTreeMutationPlan plan) {
		requireActive("stage a scalar Index creation plan");
		if (plan.index_id == 0U || isReservedInternalIndexId(plan.index_id) || !plan.preparer ||
			plan.new_pool_mutations.empty() || !index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER, "native graph staged scalar Index creation plan is invalid, "
												   "reserved, or belongs to another metadata domain");
		}
		auto existingMutation = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), plan.index_id,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existingMutation != scalar_index_mutations_.end() && existingMutation->index_id == plan.index_id) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot create and mutate the same scalar Index in one transaction");
		}
		auto existingCreation =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 plan.index_id, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (existingCreation != initial_scalar_index_mutations_.end() && existingCreation->index_id == plan.index_id) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph staged scalar Index creation duplicates one logical Index ID");
		}
		initial_scalar_index_mutations_.insert(existingCreation, {&index, plan.index_id, std::move(plan)});
	}

	void NativeGraphWriteTransaction::stageScalarIndexDrop(BTreePageStore &index, uint64_t indexId,
														   size_t maximumPages) {
		requireActive("stage a scalar Index whole-tree retirement");
		if (indexId == 0U || isReservedInternalIndexId(indexId) || maximumPages == 0U ||
			!index.usesMetadataStore(*context_->metadata)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph staged scalar Index retirement requires a non-reserved local Index, ID, and page bound");
		}
		const auto initial =
				std::lower_bound(initial_scalar_index_mutations_.begin(), initial_scalar_index_mutations_.end(),
								 indexId, [](const StagedInitialScalarIndexMutation &mutation, uint64_t value) {
									 return mutation.index_id < value;
								 });
		if (initial != initial_scalar_index_mutations_.end() && initial->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot retire a scalar Index staged for first publication");
		}
		auto existing = std::lower_bound(
				scalar_index_mutations_.begin(), scalar_index_mutations_.end(), indexId,
				[](const StagedScalarIndexMutation &mutation, uint64_t value) { return mutation.index_id < value; });
		if (existing != scalar_index_mutations_.end() && existing->index_id == indexId) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native graph cannot retire and mutate the same scalar Index in one transaction");
		}
		BTreeTreeRetirementPlan plan = index.planTreeRetirement(snapshot_, indexId, maximumPages);
		scalar_index_mutations_.insert(existing, {&index, indexId, {}, std::nullopt, std::move(plan)});
	}

} // namespace graph::storage
