#include "NativeGraphWritePlannerContext.hpp"

namespace graph::storage {

	void NativeGraphWriteTransaction::rollback() {
		if (state_ == NativeGraphWriteTransactionState::NGWTS_ACTIVE) {
			// Rollback is also the staging-allocation-failure path. Borrow the
			// already retained ranges instead of allocating a second reservation.
			context_->metadata->abandonNativeGraphLogicalIdReservation(node_id_ranges_, edge_id_ranges_);
			snapshot_.release();
			state_ = NativeGraphWriteTransactionState::NGWTS_ROLLED_BACK;
		}
        if (hybrid_vectors_) {
            std::vector<StagedVectorBootstrap>().swap(vector_bootstraps_);
            std::vector<StagedVectorDelta>().swap(vector_deltas_);
            std::vector<StagedScalarIndexMutation>().swap(scalar_index_mutations_);
            std::vector<StagedInitialScalarIndexMutation>().swap(initial_scalar_index_mutations_);
            hybrid_vectors_.reset();
        }
        hybrid_conversion_.reset(); hybrid_columns_.reset(); hybrid_catalog_tokens_.reset(); hybrid_catalog_credit_.reset();
	}

	NativeGraphWritePlanner::NativeGraphWritePlanner(RelocatableMetadataStore &metadata, EntityPageStore &entities,
													 EntityLabelSetStore &labels,
													 PropertyCollectionRootStore &properties, BTreePageStore &indexes,
													 NativeGraphWritePlannerOptions options) :
		context_(std::make_shared<NativeGraphWritePlannerContext>()) {
		validatePlannerOptions(options);
		const uint8_t foreignDomain = static_cast<uint8_t>(!entities.usesMetadataStore(metadata)) |
									  static_cast<uint8_t>(!labels.usesMetadataStore(metadata)) |
									  static_cast<uint8_t>(!properties.usesMetadataStore(metadata)) |
									  static_cast<uint8_t>(!indexes.usesMetadataStore(metadata));
		if (foreignDomain != 0U) {
			throw std::invalid_argument("native graph write planner requires one metadata storage domain");
		}
		context_->metadata = &metadata;
		context_->entities = &entities;
		context_->labels = &labels;
		context_->properties = &properties;
		context_->indexes = &indexes;
		context_->options = options;
	}

	NativeGraphWriteTransaction NativeGraphWritePlanner::beginTransaction() {
		if (context_ == nullptr) {
			fail(FormatError::FERR_INVALID_STATE, "native graph write planner is detached from storage");
		}
		return NativeGraphWriteTransaction(context_, context_->metadata->openSnapshot());
	}

    NativeGraphWriteCommitResult NativeGraphWritePlanner::rebuildEntityFreeSpaceSummaries(
        const std::function<void()> &checkpoint) {
        auto transaction = beginTransaction();
        transaction.rebuild_entity_free_summaries_ = true;
        transaction.native_schema_coordinated_ = true;
        transaction.free_summary_checkpoint_ = checkpoint;
        return transaction.commit();
    }

	NativeGraphLogicalIdReservation NativeGraphWritePlanner::reserveLogicalIds(uint64_t nodeCount, uint64_t edgeCount) {
		if (context_ == nullptr || (nodeCount == 0U && edgeCount == 0U) ||
			nodeCount > static_cast<uint64_t>(context_->options.maximum_logical_id_reservation) ||
			edgeCount > static_cast<uint64_t>(context_->options.maximum_logical_id_reservation)) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native graph logical transaction identity reservation exceeds its configured work bound");
		}
		return context_->metadata->acquireNativeGraphLogicalIds(
				nodeCount, edgeCount, static_cast<uint64_t>(context_->options.logical_id_lease_block_size));
	}

} // namespace graph::storage
