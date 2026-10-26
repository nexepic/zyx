	namespace {

        void validateNativeEntityFreeSummaryEntry(uint64_t indexId,const BTreePageEntry &entry) {
            const auto generation=EntityFreeSpaceIndex::generationKey(),count=EntityFreeSpaceIndex::countKey();
            if(std::ranges::equal(entry.key,generation) || std::ranges::equal(entry.key,count)) {
                if(!entry.value_or_child)throw FormatException(FormatError::FERR_INVALID_HEADER,"native summary source has a zero marker");
                return;
            }
            static_cast<void>(EntityFreeSpaceIndex::decodeEntry(entry,indexId==RSS_NODE_FREE_SPACE_INDEX_ID?
                EntityPageRecordKind::EPRK_NODE:EntityPageRecordKind::EPRK_EDGE));
        }

		// Transaction-local bootstrap is deliberately bounded to one shared-page
		// pool worth of input. Large DDL construction must use the streaming bulk
		// builder, which does not retain every leaf entry in memory.
		constexpr size_t RSS_INITIAL_TREE_MAX_STAGED_ENTRY_BYTES =
				static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) * RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;

		[[nodiscard]] PageOwnerHeader indexPageOwner(uint64_t indexId, PageId pageId, Generation generation,
													 LogSequenceNumber committedLsn) {
			PageOwnerHeader owner;
			owner.page_id = pageId;
			owner.owner_id = indexId;
			owner.kind = PageOwnerKind::POK_INDEX;
			owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
			owner.generation = generation;
			owner.committed_lsn = committedLsn;
			return owner;
		}

		[[nodiscard]] LogSequenceNumber provisionalPlanCheckpointLsn(const MetadataSnapshotPin &snapshot) {
			const LogSequenceNumber publishedLsn = snapshot.superblock().checkpoint_lsn;
			if (!publishedLsn.isValid() || publishedLsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable B+Tree structural plan cannot advance the checkpoint LSN");
			}
			return LogSequenceNumber(publishedLsn.value() + 1U);
		}

		[[nodiscard]] BTreePage makeLeafPage(const BTreePageHeader &source, const PageOwnerHeader &owner,
											 PageId leftSibling, PageId rightSibling,
											 std::span<const BTreePageEntry> entries) {
			BTreePageHeader header = source;
			header.page_id = owner.page_id;
			header.generation = owner.generation;
			header.kind = BTreeNodeKind::BPN_LEAF;
			header.level = 0;
			header.left_sibling = leftSibling;
			header.right_sibling = rightSibling;
			header.leftmost_child = {};
			header.entry_count = 0;
			// Every structural preview and final materialization already has the
			// complete ordered entry set. Encode it once instead of repeatedly
			// replacing the packed payload for each entry.
			return BTreePage::createWithEntries(owner, header, entries);
		}

		[[nodiscard]] BTreePage makeInternalPage(const BTreePageHeader &source, const PageOwnerHeader &owner,
												 std::span<const BTreePageEntry> entries) {
			BTreePageHeader header = source;
			header.page_id = owner.page_id;
			header.generation = owner.generation;
			header.entry_count = 0;
			return BTreePage::createWithEntries(owner, header, entries);
		}

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		// A deep mutation plans every COW replacement from one pinned snapshot.
		// `child_ordinal` identifies the next child selected by this node; the
		// structural insert path uses it to place a promoted separator exactly once.
		struct BTreePathNode {
			BTreePageHeader header;
			PageLocation location;
			LogSequenceNumber owner_lsn{};
			utils::OwnedStdVector<BTreePageEntry> entries;
			size_t child_ordinal = 0;
		};

		// A structural mutation is planned before the metadata writer is entered.
		// References can name an existing stable PageId or one PageId allocated by
		// the single publication batch; this prevents an intermediate tree shape
		// from becoming visible while promotions travel toward the root.
		struct PlannedPageReference {
			PageId existing_page_id{};
			std::optional<size_t> appended_ordinal;

			[[nodiscard]] static PlannedPageReference existing(PageId pageId) {
				if (!pageId.isValid()) { // ZYX_COV_EXCL_LINE: every stable reference comes from a validated B+Tree
					// header or child entry before the internal mutation plan is created.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable B+Tree mutation plan has an invalid stable Page ID");
				}
				return {pageId, std::nullopt};
			}

			[[nodiscard]] static PlannedPageReference appended(size_t ordinal) { return {{}, ordinal}; }

			[[nodiscard]] PageId resolve(std::span<const PageId> appendedPageIds) const {
				if (appended_ordinal.has_value()) {
					if (*appended_ordinal >=
								appendedPageIds.size() || // ZYX_COV_EXCL_LINE: ordinals originate from this
														  // plan's append vector; Metadata allocates every Page ID.
						!appendedPageIds[*appended_ordinal]
								 .isValid()) { // ZYX_COV_EXCL_LINE: Metadata allocated this append ID.
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable B+Tree mutation plan received an invalid appended Page ID");
					}
					return appendedPageIds[*appended_ordinal];
				}
				return existing_page_id;
			}

			[[nodiscard]] PageId preview() const { return appended_ordinal.has_value() ? PageId(1) : existing_page_id; }
		};

		struct PlannedInternalEntry {
			std::vector<std::byte> key;
			PlannedPageReference right_child;
		};

		struct PlannedBTreePage {
			BTreePageHeader source_header;
			PlannedPageReference page_ref;
			LogSequenceNumber expected_owner_lsn{};
			PlannedPageReference left_sibling;
			PlannedPageReference right_sibling;
			PlannedPageReference leftmost_child;
			utils::OwnedStdVector<BTreePageEntry> leaf_entries;
			std::vector<PlannedInternalEntry> internal_entries;
		};

		[[nodiscard]] std::vector<PlannedInternalEntry> planInternalEntries(std::span<const BTreePageEntry> entries) {
			std::vector<PlannedInternalEntry> planned;
			planned.reserve(entries.size());
			for (const BTreePageEntry &entry: entries) {
				planned.push_back({entry.key, PlannedPageReference::existing(PageId(entry.value_or_child))});
			}
			return planned;
		}

		[[nodiscard]] utils::OwnedStdVector<BTreePageEntry>
		previewInternalEntries(std::span<const PlannedInternalEntry> entries) {
			utils::OwnedStdVector<BTreePageEntry> preview;
			preview.reserve(entries.size());
			for (const PlannedInternalEntry &entry: entries) {
				preview.push_back({entry.key, entry.right_child.preview().value()});
			}
			return preview;
		}

		[[nodiscard]] BTreePageHeader previewInternalHeader(const BTreePageHeader &source,
															const PlannedPageReference &leftmostChild) {
			BTreePageHeader header = source;
			header.page_id = PageId(1);
			header.leftmost_child = leftmostChild.preview();
			return header;
		}

		[[nodiscard]] BTreePage materializePlannedPage(uint64_t indexId, const CatalogEntry &pool,
													   const PlannedBTreePage &planned,
													   std::span<const PageId> appendedPageIds,
													   LogSequenceNumber checkpointLsn) {
			BTreePageHeader header = planned.source_header;
			header.page_id = planned.page_ref.resolve(appendedPageIds);
			header.generation = pool.generation;
			const LogSequenceNumber ownerLsn =
					planned.page_ref.appended_ordinal.has_value() ? checkpointLsn : planned.expected_owner_lsn;
			const PageOwnerHeader owner = indexPageOwner(indexId, header.page_id, pool.generation, ownerLsn);
			if (header.kind == BTreeNodeKind::BPN_LEAF) {
				return makeLeafPage(header, owner, planned.left_sibling.resolve(appendedPageIds),
									planned.right_sibling.resolve(appendedPageIds), planned.leaf_entries);
			}
			header.leftmost_child = planned.leftmost_child.resolve(appendedPageIds);
			utils::OwnedStdVector<BTreePageEntry> entries;
			entries.reserve(planned.internal_entries.size());
			for (const PlannedInternalEntry &entry: planned.internal_entries) {
				entries.push_back({entry.key, entry.right_child.resolve(appendedPageIds).value()});
			}
			return makeInternalPage(header, owner, entries);
		}

		[[nodiscard]] std::optional<size_t> findBalancedLeafSplitIndex(const BTreePageHeader &source, uint64_t indexId,
																	   LogSequenceNumber leftOwnerLsn,
																	   LogSequenceNumber rightOwnerLsn,
																	   PageId leftSibling, PageId rightSibling,
																	   std::span<const BTreePageEntry> entries) {
			std::optional<size_t> selected;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			constexpr size_t maximumPackedBytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES -
				RSS_SLOTTED_PAGE_HEADER_BYTES - 2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
			const auto sizes = PackedBTreePageCodec::encodedBoundarySizes(
				BTreeNodeKind::BPN_LEAF, entries, source.leaf_value_layout);
			for (size_t candidate = 1; candidate < entries.size(); ++candidate) {
				try {
					const auto left = sizes.prefixes[candidate];
					const auto right = sizes.suffixes[candidate];
					if (left > maximumPackedBytes || right > maximumPackedBytes) continue;
					// Both pages have the same fixed header and two slots. Their
					// live-byte skew is exactly the packed-payload size difference.
					const uint64_t leftBytes = left;
					const uint64_t rightBytes = right;
					const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
					if (!selected.has_value() || skew < smallestSkew) {
						selected = candidate;
						smallestSkew = skew;
					}
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE) { // ZYX_COV_EXCL_LINE: the locally rebuilt
						// leaf can only reject its variable-size capacity, never persisted bytes.
						throw;
					}
				}
			}
			if (selected) {
				static_cast<void>(makeLeafPage(source,
					indexPageOwner(indexId, PageId(1), source.generation, leftOwnerLsn),
					leftSibling, PageId(2), entries.first(*selected)));
				static_cast<void>(makeLeafPage(source,
					indexPageOwner(indexId, PageId(2), source.generation, rightOwnerLsn),
					PageId(1), rightSibling, entries.subspan(*selected)));
			}
			return selected;
		}

		[[nodiscard]] std::optional<size_t>
		findBalancedInternalSplitIndex(const BTreePageHeader &source, uint64_t indexId, LogSequenceNumber leftOwnerLsn,
									   LogSequenceNumber rightOwnerLsn, std::span<const BTreePageEntry> entries) {
			if (entries.size() < 2U) { // ZYX_COV_EXCL_LINE: this helper runs only after inserting one promoted
				// separator into an already-full internal page.
				return std::nullopt;
			}
			// Validate the common owner/header once; final chosen pages retain
			// complete packed validation. Candidate payloads need exact sizes only.
			static_cast<void>(makeInternalPage(source,
				indexPageOwner(indexId, PageId(1), source.generation, leftOwnerLsn), {}));
			BTreePageHeader emptyRightHeader = source;
			emptyRightHeader.left_sibling = {};
			emptyRightHeader.right_sibling = {};
			static_cast<void>(makeInternalPage(emptyRightHeader,
				indexPageOwner(indexId, PageId(2), source.generation, rightOwnerLsn), {}));
			std::optional<size_t> selected;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			constexpr size_t capacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
				2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
			const auto sizes = PackedBTreePageCodec::encodedBoundarySizes(
				BTreeNodeKind::BPN_INTERNAL, entries, source.leaf_value_layout);
			for (size_t promoted = 0; promoted < entries.size(); ++promoted) {
				BTreePageHeader rightHeader = source;
				rightHeader.page_id = PageId(2);
				rightHeader.left_sibling = {};
				rightHeader.right_sibling = {};
				rightHeader.leftmost_child = PageId(entries[promoted].value_or_child);
				rightHeader.entry_count = 0;
				static_cast<void>(BTreePageHeaderCodec::encode(rightHeader));
				const size_t leftBytes = sizes.prefixes[promoted];
				const size_t rightBytes = sizes.suffixes[promoted + 1U];
				if (leftBytes > capacity || rightBytes > capacity) continue;
				const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
				if (!selected || skew < smallestSkew) {
					selected = promoted;
					smallestSkew = skew;
				}
			}
			if (selected) {
				static_cast<void>(makeInternalPage(source,
					indexPageOwner(indexId, PageId(1), source.generation, leftOwnerLsn), entries.first(*selected)));
				BTreePageHeader rightHeader = source;
				rightHeader.page_id = PageId(2);
				rightHeader.left_sibling = {};
				rightHeader.right_sibling = {};
				rightHeader.leftmost_child = PageId(entries[*selected].value_or_child);
				static_cast<void>(makeInternalPage(rightHeader,
					indexPageOwner(indexId, PageId(2), source.generation, rightOwnerLsn), entries.subspan(*selected + 1U)));
			}
			return selected;
		}

		// The fixed slotted-page header, its live slot directory, and every live
		// record are part of the durable page image. Use that encoded footprint
		// instead of record count so variable-length keys cannot defeat the B+Tree
		// deletion occupancy invariant.
		constexpr uint32_t RSS_BTREE_MIN_NODE_OCCUPIED_BYTES = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES / 2U;

		struct BTreeDeleteMutationNode {
			BTreePageHeader header;
			PageLocation location;
			LogSequenceNumber owner_lsn{};
			utils::OwnedStdVector<BTreePageEntry> entries;
			bool dirty = false;
			bool retired = false;
		};

		[[nodiscard]] BTreePage materializeDeleteMutationNode(uint64_t indexId, const BTreeDeleteMutationNode &node) {
			const PageOwnerHeader owner =
					indexPageOwner(indexId, node.header.page_id, node.header.generation, node.owner_lsn);
			if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
				return makeLeafPage(node.header, owner, node.header.left_sibling, node.header.right_sibling,
									node.entries);
			}
			return makeInternalPage(node.header, owner, node.entries);
		}

		[[nodiscard]] uint32_t encodedNodeOccupancy(const BTreePage &page) {
			const SlottedPageStats stats = page.slottedPage().stats();
			const uint64_t unusedBytes = static_cast<uint64_t>(stats.contiguous_free_bytes) + stats.reclaimable_bytes;
			if (unusedBytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) { // ZYX_COV_EXCL_LINE: SlottedPage validates
				// its locally materialized stats before this occupancy calculation.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable B+Tree page reports an invalid encoded free-space total");
			}
			return static_cast<uint32_t>(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - unusedBytes);
		}

		[[nodiscard]] bool isUnderfullDeleteNode(uint64_t indexId, const BTreeDeleteMutationNode &node) {
			if (node.entries.empty()) {
				return true;
			}
			return encodedNodeOccupancy(materializeDeleteMutationNode(indexId, node)) <
				   RSS_BTREE_MIN_NODE_OCCUPIED_BYTES;
		}

	} // namespace

	struct OverlayPageReference {
		PageId existing_page_id{};
		size_t virtual_ordinal = 0U;
		bool is_virtual = false;

		[[nodiscard]] static OverlayPageReference existing(PageId pageId) { return {pageId, 0U, false}; }

		[[nodiscard]] static OverlayPageReference virtualPage(size_t ordinal) { return {{}, ordinal, true}; }

		constexpr bool operator==(const OverlayPageReference &) const = default;
	};

	struct OverlayInternalEntry {
        using OwnedHeapElement = void;
		utils::OwnedStdVector<std::byte> key;
		OverlayPageReference right_child;
	};

	struct OverlayMutationNode {
		BTreePageHeader header;
		std::optional<OverlayPageReference> left_sibling;
		std::optional<OverlayPageReference> right_sibling;
		std::optional<OverlayPageReference> leftmost_child;
		utils::OwnedStdVector<BTreePageEntry> leaf_entries;
        utils::OwnedStdVector<std::byte> packed_leaf_entries;
		utils::OwnedStdVector<OverlayInternalEntry> internal_entries;
		bool dirty = false;
		bool retired = false;
	};

	// A loaded page is durable only after SharedPagePoolAllocator and BTreePage
	// have verified its physical and typed owner identities. Keep that identity
	// out of virtual mutation nodes so a missing fence is not representable.
	struct ExistingOverlayMutationNode {
		OverlayMutationNode node;
		PageLocation location;
		LogSequenceNumber owner_lsn{};
	};

	class TransactionBTreeOverlay final : public std::enable_shared_from_this<TransactionBTreeOverlay> {
	public:
		TransactionBTreeOverlay(const BTreePageStore &store, RelocatableMetadataStore &metadata,
								const MetadataSnapshotPin &snapshot, uint64_t indexId, bool rebuildOnFrameExhaustion,
								bool forceFreshRebuild, BTreeLeafValueLayout leafValueLayout,
            std::function<void(BTreePage)> nativeSink = {}, std::shared_ptr<std::pmr::memory_resource> nativeMemory = {},
            const CancellationCheckpoint *nativeCancellation = nullptr, bool nativeSchemaKeys = false) :
			store_(store), metadata_(metadata), snapshot_(snapshot), index_id_(indexId),
			leaf_value_layout_(leafValueLayout), rebuild_on_frame_exhaustion_(rebuildOnFrameExhaustion),
			force_fresh_rebuild_(forceFreshRebuild),
            native_credit_(std::move(nativeMemory), nativeSink ? 128U * 1024U : 0U),
            native_sink_(std::move(nativeSink)), native_cancellation_(nativeCancellation), native_schema_keys_(nativeSchemaKeys), root_(loadRoot()) {}

		void apply(const BTreeScalarIndexOperation &operation) {
            checkNative();
			if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_UPSERT) {
				if (leaf_value_layout_ == BTreeLeafValueLayout::BLVL_EMPTY && operation.value != 0U) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable key-only B+Tree overlay cannot upsert a nonempty value");
				}
                constexpr size_t maximumKeyBytes = 16U * 1024U;
                if (operation.key.size() > maximumKeyBytes) {
                    flushPendingUpserts();
                    upsert(operation.key, operation.value);
                    return;
                }
                if (pending_upserts_.size() == 64U || operation.key.size() > maximumKeyBytes - pending_key_bytes_)
                    flushPendingUpserts();
                pending_upserts_.push_back({utils::OwnedStdVector<std::byte>(operation.key.begin(), operation.key.end()), operation.value});
                pending_key_bytes_ += operation.key.size();
                debug::PerfTrace::addValue("native_btree.plan.buffered_upserts", 1);
				return;
			}
			if (operation.kind == BTreeScalarIndexOperationKind::BSIOP_ERASE) {
                flushPendingUpserts();
				leaf_append_size_.reset();
				erase(operation.key);
				return;
			}
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable transaction-local B+Tree overlay received an unknown scalar operation");
		}

		[[nodiscard]] BTreeStructuralMutationPlan buildPlan() {
            flushPendingUpserts();
			struct ExistingReplacement {
				PageLocation location;
				OverlayPageReference reference;
			};
			std::vector<ExistingReplacement> replacements;
			std::vector<IndexTreePageRetirement> retirements;
			for (const auto &[pageId, existing]: existing_nodes_) {
				static_cast<void>(pageId);
				const OverlayMutationNode &node = existing.node;
				if (node.retired) {
					retirements.push_back({existing.location, existing.owner_lsn});
				} else if (node.dirty) {
					replacements.push_back({existing.location, OverlayPageReference::existing(node.header.page_id)});
				}
			}
			std::vector<size_t> liveVirtuals;
			for (size_t ordinal = 0U; ordinal < virtual_nodes_.size(); ++ordinal) {
				if (!virtual_nodes_[ordinal].retired) {
					liveVirtuals.push_back(ordinal);
				}
			}
			const size_t rebuildThreshold = static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) * 4U;
			bool rebuiltDenseTree = false;
			if (!native_sink_ && liveVirtuals.size() > rebuildThreshold) {
				rebuiltDenseTree = rebuildDenseTree(true);
				if (rebuiltDenseTree) {
					liveVirtuals.clear();
					for (size_t ordinal = 0U; ordinal < virtual_nodes_.size(); ++ordinal) {
						if (!virtual_nodes_[ordinal].retired) {
							liveVirtuals.push_back(ordinal);
						}
					}
					replacements.clear();
					retirements.clear();
					for (const auto &[pageId, existing]: existing_nodes_) {
						static_cast<void>(pageId);
						const OverlayMutationNode &node = existing.node;
						if (node.retired) {
							retirements.push_back({existing.location, existing.owner_lsn});
						}
					}
				}
			}

			utils::OwnedMap<uint64_t, size_t> requestOrdinals;
			std::vector<IndexTreeMutationPoolRequest> requests;
			const auto ensureRequest = [&](SegmentId segmentId) {
				const auto [iterator, inserted] = requestOrdinals.emplace(segmentId.value(), requests.size());
				if (inserted) {
					requests.push_back({segmentId, {}, 0U, {}});
				}
				return iterator->second;
			};
			for (const ExistingReplacement &replacement: replacements) {
				requests[ensureRequest(replacement.location.segment_id)].expected_replacements.push_back(
						replacement.location);
			}
			for (const IndexTreePageRetirement &retirement: retirements) {
				requests[ensureRequest(retirement.location.segment_id)].expected_retirements.push_back(retirement);
			}
			const auto rebuildIntoFreshPools = [&] {
				static_cast<void>(rebuildDenseTree(false));
				liveVirtuals.clear();
				for (size_t ordinal = 0U; ordinal < virtual_nodes_.size(); ++ordinal) {
					if (!virtual_nodes_[ordinal].retired) {
						liveVirtuals.push_back(ordinal);
					}
				}
				replacements.clear();
				retirements.clear();
				for (const auto &[pageId, existing]: existing_nodes_) {
					static_cast<void>(pageId);
					retirements.push_back({existing.location, existing.owner_lsn});
				}
				requestOrdinals.clear();
				requests.clear();
				for (const IndexTreePageRetirement &retirement: retirements) {
					requests[ensureRequest(retirement.location.segment_id)].expected_retirements.push_back(retirement);
				}
				fresh_rebuild_applied_ = true;
			};
			// A full source pool does not require rebuilding the complete tree: a
			// bounded dirty-path replacement can be allocated from another compatible
			// Index pool while the source frames remain valid for older snapshots.
			// Reserve destination capacity deterministically so several source pools do
			// not overcommit the same target under one pinned planning snapshot.
			std::vector<PageLocation> reservedDestinationFrames;
			if (!native_sink_ && !force_fresh_rebuild_ && !replacements.empty()) {
				for (IndexTreeMutationPoolRequest &request: requests) {
					if (request.expected_replacements.empty()) {
						continue;
					}
					const CatalogEntry sourcePool = metadata_.findCatalog(snapshot_, request.segment_id);
					if (store_.hasReplacementFrameCapacity(sourcePool, request.expected_replacements.size())) {
						continue;
					}
					const std::optional<SegmentId> destination =
							store_.selectAppendPool(snapshot_, reservedDestinationFrames,
											   request.expected_replacements.size(), sourcePool.owner_id);
					if (!destination.has_value() || *destination == request.segment_id) {
						// Prefer an available destination before paying for reconciliation.
						// Before a complete-tree rebuild, release only source versions
						// invisible to every active snapshot and retry local capacity.
						// Deferred graph-wide admission keeps its own reclamation boundary.
						if (rebuild_on_frame_exhaustion_) {
							store_.reclaimRetiredFrames(request.segment_id);
							if (store_.hasReplacementFrameCapacity(sourcePool, request.expected_replacements.size())) {
								continue;
							}
						}
						continue;
					}
					request.replacement_segment_id = *destination;
					for (const PageLocation &source: request.expected_replacements) {
						// The selection helper only needs a valid segment/frame identity to
						// account for reserved destination capacity; the PageId remains
						// unique because source replacement IDs are globally unique.
						reservedDestinationFrames.push_back(
								{source.page_id, *destination, RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation{1U}, 0U});
					}
				}
			}
			if (force_fresh_rebuild_ && !fresh_rebuild_applied_ && (!replacements.empty() || !liveVirtuals.empty())) {
				rebuildIntoFreshPools();
			} else if (rebuild_on_frame_exhaustion_ &&
					   std::ranges::any_of(requests, [this](const IndexTreeMutationPoolRequest &request) {
						   const CatalogEntry pool = metadata_.findCatalog(snapshot_, request.segment_id);
						   return !request.replacement_segment_id.isValid() &&
								  !store_.hasReplacementFrameCapacity(pool, request.expected_replacements.size());
					   })) {
				rebuildIntoFreshPools();
			}
			if (requests.empty() && liveVirtuals.empty()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay produced no durable page mutation");
			}

			struct VirtualPlacement {
				size_t pool_ordinal = 0U;
				size_t appended_ordinal = 0U;
			};
			std::optional<BTreeNewIndexTreePoolMutation> newPool;
			std::vector<BTreeNewIndexTreePoolMutation> additionalNewPools;
			std::vector<VirtualPlacement> virtualPlacements(virtual_nodes_.size());
			if (!liveVirtuals.empty()) {
				std::vector<PageLocation> expectedLocations;
				expectedLocations.reserve(replacements.size());
				for (const ExistingReplacement &replacement: replacements) {
					expectedLocations.push_back(replacement.location);
				}
				size_t firstUnplaced = 0U;
				const auto appendToCurrentPool = [&](size_t candidate) {
					std::vector<PageLocation> appendReservations = expectedLocations;
					appendReservations.insert(appendReservations.end(), reservedDestinationFrames.begin(),
							reservedDestinationFrames.end());
					const std::optional<SegmentId> selected =
							store_.selectAppendPool(snapshot_, appendReservations, candidate, 0U);
					if (!selected.has_value()) {
						return false;
					}
					const size_t ordinal = ensureRequest(*selected);
					requests[ordinal].appended_page_count = candidate;
					for (size_t index = 0U; index < candidate; ++index) {
						reservedDestinationFrames.push_back(
								{PageId{1U}, *selected, RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation{1U}, 0U});
					}
					for (size_t index = 0U; index < candidate; ++index) {
						virtualPlacements[liveVirtuals[index]] = {ordinal, index};
					}
					firstUnplaced = candidate;
					return true;
				};
				if (!native_sink_ && !fresh_rebuild_applied_ && rebuiltDenseTree) {
					const size_t maximumCurrentPoolAppend =
							std::min(liveVirtuals.size(), static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT));
					for (size_t candidate = maximumCurrentPoolAppend; candidate != 0U; --candidate) {
						if (appendToCurrentPool(candidate)) {
							break;
						}
					}
				} else if (!native_sink_ && !fresh_rebuild_applied_ &&
						   liveVirtuals.size() <= static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)) {
					// A single structural publication can safely use any current typed
					// Index pool selected under the same metadata snapshot.
					static_cast<void>(appendToCurrentPool(liveVirtuals.size()));
				}
				for (size_t first = firstUnplaced; first < liveVirtuals.size();) {
					const size_t count =
							std::min(static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT), liveVirtuals.size() - first);
					const size_t freshOrdinal = newPool.has_value() ? additionalNewPools.size() + 1U : 0U;
					if (!newPool.has_value()) {
						newPool = {index_id_, count};
					} else {
						additionalNewPools.push_back({index_id_, count});
					}
					for (size_t offset = 0U; offset < count; ++offset) {
						virtualPlacements[liveVirtuals[first + offset]] = {requests.size() + freshOrdinal, offset};
					}
					first += count;
				}
			}
			// selectAppendPool() accounts for every stable replacement and appended
			// virtual page under this pinned metadata snapshot. It returns no pool
			// unless that complete batch fits, in which case the loop above reserves
			// a fresh pool. A second capacity check here cannot observe a new state.

			std::optional<IndexTreeMutationAppendedPageReference> replacementRoot;
			std::optional<PageId> stableReplacementRoot;
			if (root_.is_virtual) {
				const auto rootVirtual = std::ranges::find(liveVirtuals, root_.virtual_ordinal);
				if (rootVirtual == liveVirtuals.end()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay root was retired before publication");
				}
				replacementRoot = {virtualPlacements[root_.virtual_ordinal].pool_ordinal,
								   virtualPlacements[root_.virtual_ordinal].appended_ordinal};
			} else {
				stableReplacementRoot = root_.existing_page_id;
			}

			const OverlayMutationNode &finalRoot = nodeFor(root_);
			const bool finalTreeEmpty = finalRoot.header.kind == BTreeNodeKind::BPN_LEAF &&
									finalRoot.leaf_entries.empty() && finalRoot.packed_leaf_entries.empty();
			const IndexRootLocation expectedRoot = expected_root_;
			const BTreeStructuralMutationPreparer preparer = [self = shared_from_this(),
															  replacements = std::move(replacements),
															  retirements = std::move(retirements),
															  liveVirtuals = std::move(liveVirtuals), requests,
															  virtualPlacements = std::move(virtualPlacements),
															  freshPoolCount =
																	  static_cast<size_t>(newPool.has_value()) +
																	  additionalNewPools.size()](
																	 std::vector<IndexTreeMutationPoolBatch> &batches,
																	 LogSequenceNumber checkpointLsn) {
				const size_t expectedBatchCount = requests.size() + freshPoolCount;
				if (batches.size() != expectedBatchCount) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay received an invalid pool batch shape");
				}
				std::vector<PageId> virtualIdsByOrdinal(self->virtual_nodes_.size());
				for (const size_t virtualOrdinal: liveVirtuals) {
					const VirtualPlacement placement = virtualPlacements[virtualOrdinal];
					const IndexTreeMutationPoolBatch &batch = batches.at(placement.pool_ordinal);
					if (placement.appended_ordinal >= batch.appended_page_ids.size()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "relocatable transaction-local B+Tree overlay received an invalid "
											  "virtual Page-ID placement");
					}
					virtualIdsByOrdinal[virtualOrdinal] = batch.appended_page_ids[placement.appended_ordinal];
				}
				for (size_t poolOrdinal = 0U; poolOrdinal < batches.size(); ++poolOrdinal) {
					IndexTreeMutationPoolBatch &batch = batches[poolOrdinal];
					const size_t expectedAppends = poolOrdinal < requests.size()
														   ? requests[poolOrdinal].appended_page_count
														   : batches[poolOrdinal].appended_page_ids.size();
					if (poolOrdinal < requests.size()) {
						const IndexTreeMutationPoolRequest &request = requests[poolOrdinal];
						if (batch.pool.segment_id != request.segment_id) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "relocatable transaction-local B+Tree overlay source-pool fence changed "
												  "before preparation");
						}
						if (batch.expected_replacements != request.expected_replacements) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "relocatable transaction-local B+Tree overlay replacement fence changed "
												  "before preparation");
						}
						if (batch.expected_retirements != request.expected_retirements) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "relocatable transaction-local B+Tree overlay retirement fence changed "
												  "before preparation");
						}
						const SegmentId expectedReplacementPool = request.replacement_segment_id.isValid()
														? request.replacement_segment_id
														: request.segment_id;
						if (batch.replacement_pool.segment_id != expectedReplacementPool) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "relocatable transaction-local B+Tree overlay replacement-pool fence changed "
												  "before preparation");
						}
					}
					if (batch.appended_page_ids.size() != expectedAppends) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable transaction-local B+Tree overlay append count changed before preparation");
					}
                    if (self->native_sink_) {
                        // Source identities and virtual IDs are fenced above.
                        // Emit unplaced images; metadata assigns fresh physical
                        // owners and commits all field/map routes together.
                        for (const auto &location : batch.expected_replacements) {
                            self->checkNative(); const auto &existing = self->existingFor(location.page_id);
                            self->native_sink_(self->materialize(existing.node, location.page_id,
                                existing.node.header.generation, checkpointLsn, virtualIdsByOrdinal));
                        }
                        for (const size_t ordinal : liveVirtuals) {
                            const auto placement = virtualPlacements[ordinal];
                            if (placement.pool_ordinal != poolOrdinal) continue;
                            self->checkNative();
                            self->native_sink_(self->materialize(self->nodeFor(OverlayPageReference::virtualPage(ordinal)),
                                batch.appended_page_ids.at(placement.appended_ordinal), Generation(1), checkpointLsn, virtualIdsByOrdinal));
                        }
                        continue;
                    }
					const CatalogEntry &replacementPool = batch.replacement_pool.segment_id.isValid()
														? batch.replacement_pool
														: batch.pool;
					std::vector<SharedPagePoolPreparedReplacement> preparedReplacements;
					std::vector<SharedPagePoolPreparedCrossPoolReplacement> preparedCrossPoolReplacements;
					preparedReplacements.reserve(batch.expected_replacements.size());
					preparedCrossPoolReplacements.reserve(batch.expected_replacements.size());
					for (const PageLocation &location: batch.expected_replacements) {
						ExistingOverlayMutationNode &existing = self->existingFor(location.page_id);
						OverlayMutationNode &node = existing.node;
						// The batch equality fence above and ExistingOverlayMutationNode's
						// construction from the pinned read prove this durable source.
						BTreePage page = self->materialize(node, location.page_id, replacementPool.generation,
																   existing.owner_lsn, virtualIdsByOrdinal);
						std::vector<std::byte> bytes = page.serialize();
						PageOwnerHeader owner =
								indexPageOwner(self->index_id_, location.page_id, replacementPool.generation, checkpointLsn);
						owner.payload_bytes = static_cast<uint32_t>(bytes.size());
						const uint32_t freeBytes = page.slottedPage().stats().contiguous_free_bytes;
						if (replacementPool.segment_id == batch.pool.segment_id) {
							preparedReplacements.push_back({std::move(owner), existing.owner_lsn, std::move(bytes), freeBytes});
						} else {
							preparedCrossPoolReplacements.push_back(
									{batch.pool, location, existing.owner_lsn, std::move(owner), std::move(bytes), freeBytes});
						}
					}
					std::vector<SharedPagePoolPreparedAppend> preparedAppends;
					std::vector<size_t> batchVirtuals;
					for (const size_t virtualOrdinal: liveVirtuals) {
						if (virtualPlacements[virtualOrdinal].pool_ordinal == poolOrdinal) {
							batchVirtuals.push_back(virtualOrdinal);
						}
					}
					if (!batchVirtuals.empty()) {
						preparedAppends.reserve(batchVirtuals.size());
						for (const size_t virtualOrdinal: batchVirtuals) {
							const VirtualPlacement placement = virtualPlacements[virtualOrdinal];
							OverlayMutationNode &node =
									self->nodeFor(OverlayPageReference::virtualPage(virtualOrdinal));
							const PageId pageId = batch.appended_page_ids[placement.appended_ordinal];
							BTreePage page = self->materialize(node, pageId, batch.pool.generation, checkpointLsn,
															   virtualIdsByOrdinal);
							std::vector<std::byte> bytes = page.serialize();
							PageOwnerHeader owner =
									indexPageOwner(self->index_id_, pageId, batch.pool.generation, checkpointLsn);
							owner.payload_bytes = static_cast<uint32_t>(bytes.size());
							preparedAppends.push_back({std::move(owner), std::move(bytes),
													   page.slottedPage().stats().contiguous_free_bytes});
						}
					}
					std::vector<SharedPagePoolPreparedRetirement> preparedRetirements;
					preparedRetirements.reserve(batch.expected_retirements.size());
					for (const IndexTreePageRetirement &retirement: batch.expected_retirements) {
						preparedRetirements.push_back({retirement.location, self->index_id_, PageOwnerKind::POK_INDEX,
													   retirement.expected_owner_lsn});
					}
					if (batch.newly_reserved) {
						self->pages().prepareNewPoolPages(batch.pool, preparedAppends, batch.appended_locations,
															  checkpointLsn, batch.prepared_new_pool_prefix_count);
					} else if (!preparedCrossPoolReplacements.empty()) {
						self->pages().prepareCrossPoolReplacementPages(replacementPool, preparedCrossPoolReplacements,
																		  batch.replacement_locations, checkpointLsn);
						// A pure cross-pool replacement needs no source write: the new
						// Directory route makes the old frame unreachable while preserving
						// it for older snapshots. Only source-local appends or explicit
						// retirements require a second source-pool mutation call.
						if (!preparedAppends.empty() || !preparedRetirements.empty()) {
							const std::vector<PageLocation> noReplacementSources;
							const std::vector<SharedPagePoolPreparedReplacement> noReplacements;
							std::vector<PageLocation> noReplacementLocations;
							self->pages().prepareMutationPages(batch.pool, noReplacementSources, noReplacements,
																		  preparedAppends, noReplacementLocations,
																		  batch.appended_locations, checkpointLsn,
																		  preparedRetirements);
						}
					} else {
						self->pages().prepareMutationPages(batch.pool, batch.expected_replacements,
														   preparedReplacements, preparedAppends,
														   batch.replacement_locations, batch.appended_locations,
														   checkpointLsn, preparedRetirements);
					}
				}
			};
			return {expectedRoot,		   std::move(requests), newPool, std::move(additionalNewPools), replacementRoot,
					stableReplacementRoot, std::move(preparer), finalTreeEmpty};
		}

	private:
		[[nodiscard]] bool rebuildDenseTree(bool requireReduction) {
            utils::OwnedStdVector<OverlayPageReference> leaves;
            std::set<uint64_t> visitedExisting;
            std::set<size_t> visitedVirtual;
            collectRebuildLeaves(root_, nodeFor(root_).header.level, leaves, visitedExisting, visitedVirtual);
            if (requireReduction && std::ranges::any_of(leaves, [this](const auto &leaf) {
                    return !leaf.is_virtual && !existing_nodes_.contains(leaf.existing_page_id.value());
                })) return false; // Optional densification must not retain an entire cold tree.
            if (leaves.size() == 1U && leaves.front() == root_ && nodeFor(root_).leaf_entries.empty() && nodeFor(root_).packed_leaf_entries.empty()) {
				// An empty root leaf is a valid scalar-index state. When a source pool
				// cannot COW the final erase, retire the old reachable tree and publish
				// one fresh empty root instead of rejecting the logical deletion.
				for (OverlayMutationNode &node: virtual_nodes_) {
					node.retired = true;
					node.dirty = false;
				}
				for (const uint64_t pageId: visitedExisting) {
					OverlayMutationNode &node = existing_nodes_.at(pageId).node;
					node.retired = true;
					node.dirty = false;
				}
				BTreePageHeader emptyRootHeader;
				emptyRootHeader.index_id = index_id_;
				emptyRootHeader.generation = expected_root_.root_generation;
				emptyRootHeader.kind = BTreeNodeKind::BPN_LEAF;
				emptyRootHeader.level = 0U;
				emptyRootHeader.leaf_value_layout = leaf_value_layout_;
				root_ = createVirtual(emptyRootHeader);
				nodeFor(root_).dirty = true;
				return true;
			}
            InitialBTreePlan plan;
            if (requireReduction) {
                // Keep the optional path non-destructive until reduction is proven.
                utils::OwnedStdVector<BTreePageEntry> entries;
                std::set<uint64_t> existing;
                std::set<size_t> virtuals;
                try {
                    collectEntries(root_, nodeFor(root_).header.level, entries, existing, virtuals);
                    plan = InitialBTreeBuilder::plan(index_id_, entries, leaf_value_layout_);
                } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                    return false;
                }
            } else {
                size_t leafOrdinal = 0U, entryOrdinal = 0U;
                std::span<const std::byte> previousKey;
                plan = InitialBTreeBuilder::planFromCursor(index_id_, [&]() -> std::optional<BTreePageEntry> {
                    while (leafOrdinal < leaves.size()) {
                        const auto reference = leaves[leafOrdinal];
                        auto &leaf = loadNode(reference, 0U);
                        if (leaf.header.kind != BTreeNodeKind::BPN_LEAF || leaf.leaf_entries.empty())
                            throw FormatException(FormatError::FERR_INVALID_STATE,
                                "relocatable transaction-local B+Tree rebuild found an empty non-root leaf");
                        BTreePageEntry entry = std::move(leaf.leaf_entries[entryOrdinal++]);
                        const bool invalidValue = leaf_value_layout_ == BTreeLeafValueLayout::BLVL_U64
                            ? entry.value_or_child == 0U : entry.value_or_child != 0U;
                        if (entry.key.empty() || invalidValue || (!previousKey.empty() && !lessKey(previousKey, entry.key)))
                            throw FormatException(FormatError::FERR_INVALID_STATE,
                                "relocatable transaction-local B+Tree rebuild found unordered leaf entries");
                        // Key backing transfers into the immutable plan and stays
                        // alive across leaf growth/compaction until this cursor ends.
                        previousKey = std::span<const std::byte>(entry.key.data(), entry.key.size());
                        if (entryOrdinal == leaf.leaf_entries.size()) {
                            leaf.leaf_entries = {};
                            ++leafOrdinal; entryOrdinal = 0U;
                        }
                        return entry;
                    }
                    return std::nullopt;
                }, leaf_value_layout_, InitialBTreeLeafStorage::IBLS_PACKED);
            }
			const size_t liveVirtualCount = static_cast<size_t>(std::ranges::count_if(
					virtual_nodes_, [](const OverlayMutationNode &node) { return !node.retired; }));
			if (requireReduction && plan.nodes.size() >= liveVirtualCount) {
				return false;
			}

			for (OverlayMutationNode &node: virtual_nodes_) {
				node.retired = true;
				node.dirty = false;
			}
			for (const uint64_t pageId: visitedExisting) {
				OverlayMutationNode &node = existing_nodes_.at(pageId).node;
				node.retired = true;
				node.dirty = false;
			}

			std::vector<OverlayPageReference> rebuilt;
			rebuilt.reserve(plan.nodes.size());
			for (const InitialBTreePlanNode &planned: plan.nodes) {
				BTreePageHeader header;
				header.index_id = index_id_;
				header.generation = expected_root_.root_generation;
				header.kind = planned.level == 0U ? BTreeNodeKind::BPN_LEAF : BTreeNodeKind::BPN_INTERNAL;
				header.level = planned.level;
				if (planned.level == 0U) {
					header.leaf_value_layout = leaf_value_layout_;
				}
				rebuilt.push_back(createVirtual(header));
			}
			for (size_t ordinal = 0U; ordinal < plan.nodes.size(); ++ordinal) {
				const InitialBTreePlanNode &planned = plan.nodes[ordinal];
				OverlayMutationNode &node = nodeFor(rebuilt[ordinal]);
				node.dirty = true;
				if (planned.level == 0U) {
					// Transfer the private plan backing; copying would retain a third
					// complete leaf set alongside the source and replacement tree.
					node.leaf_entries = std::move(plan.nodes[ordinal].leaf_entries);
                    node.packed_leaf_entries = std::move(plan.nodes[ordinal].packed_leaf_entries);
					node.left_sibling = ordinal == 0U ? std::nullopt : std::optional(rebuilt[ordinal - 1U]);
					node.right_sibling =
							ordinal + 1U == plan.leaf_count ? std::nullopt : std::optional(rebuilt[ordinal + 1U]);
					continue;
				}
				node.leftmost_child = rebuilt[planned.children.front()];
				for (size_t child = 1U; child < planned.children.size(); ++child) {
					const size_t childOrdinal = planned.children[child];
					node.internal_entries.push_back({plan.nodes[childOrdinal].first_key, rebuilt[childOrdinal]});
				}
			}
			root_ = rebuilt[plan.root_ordinal];
			return true;
		}

        void collectRebuildLeaves(const OverlayPageReference &reference, uint16_t level,
                utils::OwnedStdVector<OverlayPageReference> &leaves,
                std::set<uint64_t> &existing, std::set<size_t> &virtuals) {
            const bool first = reference.is_virtual ? virtuals.insert(reference.virtual_ordinal).second
                : existing.insert(reference.existing_page_id.value()).second;
            if (!first) throw FormatException(FormatError::FERR_INVALID_STATE,
                "relocatable transaction-local B+Tree rebuild found a cycle or shared child page");
            if (level == 0U) { leaves.push_back(reference); return; }
            auto &node = loadNode(reference, level);
            if (node.header.kind != BTreeNodeKind::BPN_INTERNAL)
                throw FormatException(FormatError::FERR_INVALID_STATE,
                    "relocatable transaction-local B+Tree rebuild found an invalid internal level");
            for (size_t child = 0U; child <= node.internal_entries.size(); ++child)
                collectRebuildLeaves(childAt(node, child), static_cast<uint16_t>(level - 1U), leaves, existing, virtuals);
        }

		void collectEntries(const OverlayPageReference &reference, uint16_t expectedLevel,
							utils::OwnedStdVector<BTreePageEntry> &entries, std::set<uint64_t> &visitedExisting,
							std::set<size_t> &visitedVirtual) {
			const bool firstVisit = reference.is_virtual
											? visitedVirtual.insert(reference.virtual_ordinal).second
											: visitedExisting.insert(reference.existing_page_id.value()).second;
			if (!firstVisit) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree rebuild found a cycle or shared child page");
			}
			OverlayMutationNode &node = loadNode(reference, expectedLevel);
			if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
				if (expectedLevel != 0U || (node.leaf_entries.empty() && reference != root_)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable transaction-local B+Tree rebuild found an empty non-root leaf");
				}
				if (node.leaf_entries.empty()) {
					return;
				}
				for (const BTreePageEntry &entry: node.leaf_entries) {
					const bool invalidValue = leaf_value_layout_ == BTreeLeafValueLayout::BLVL_U64
											  ? entry.value_or_child == 0U
											  : entry.value_or_child != 0U;
					if (entry.key.empty() || invalidValue ||
						(!entries.empty() && !lessKey(entries.back().key, entry.key))) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable transaction-local B+Tree rebuild found unordered leaf entries");
					}
					entries.push_back(entry);
				}
				return;
			}
			if (expectedLevel == 0U || node.header.kind != BTreeNodeKind::BPN_INTERNAL) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree rebuild found an invalid internal level");
			}
			for (size_t child = 0U; child <= node.internal_entries.size(); ++child) {
				collectEntries(childAt(node, child), static_cast<uint16_t>(expectedLevel - 1U), entries,
							   visitedExisting, visitedVirtual);
			}
		}

		[[nodiscard]] OverlayPageReference loadRoot() {
			expected_root_ = metadata_.findIndexRoot(snapshot_, index_id_);
			const OverlayPageReference root = OverlayPageReference::existing(expected_root_.root_page_id);
			OverlayMutationNode &node =
					loadExisting(root.existing_page_id, expected_root_.root_generation.value(), readLevel(root));
			if (node.header.left_sibling.isValid() || node.header.right_sibling.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay root is stale or malformed");
			}
			return root;
		}

		[[nodiscard]] uint16_t readLevel(OverlayPageReference root) const {
			const BTreePage page = store_.read(snapshot_, root.existing_page_id);
			return page.header().level;
		}

		[[nodiscard]] OverlayMutationNode &loadExisting(PageId pageId, uint64_t expectedGeneration,
														uint16_t expectedLevel) {
			const auto existing = existing_nodes_.find(pageId.value());
			if (existing != existing_nodes_.end()) {
				if (existing->second.node.header.level != expectedLevel ||
					existing->second.node.header.generation.value() != expectedGeneration) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay revisited a page with a different identity");
				}
				return existing->second.node;
			}
            admitNativeNode(); checkNative();
			BTreePage page = store_.read(snapshot_, pageId);
			const BTreePageHeader header = page.header();
			const PageLocation location = metadata_.findPageLocation(snapshot_, pageId);
			const LogSequenceNumber ownerLsn = page.slottedPage().ownerCommittedLsn();
			const uint8_t invalid = static_cast<uint8_t>(header.index_id != index_id_) |
									static_cast<uint8_t>(header.page_id != pageId) |
									static_cast<uint8_t>(header.generation.value() != expectedGeneration) |
									static_cast<uint8_t>(header.level != expectedLevel) |
									static_cast<uint8_t>(location.page_id != pageId) |
									static_cast<uint8_t>(location.generation != header.generation) |
									static_cast<uint8_t>(!ownerLsn.isValid());
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay encountered a foreign or malformed page");
			}
			if (header.kind == BTreeNodeKind::BPN_LEAF && header.leaf_value_layout != leaf_value_layout_) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable transaction-local B+Tree overlay found a mismatched leaf layout");
			}
			OverlayMutationNode node;
			node.header = header;
			if (header.kind == BTreeNodeKind::BPN_LEAF) {
				if (header.left_sibling.isValid()) {
					node.left_sibling = OverlayPageReference::existing(header.left_sibling);
				}
				if (header.right_sibling.isValid()) {
					node.right_sibling = OverlayPageReference::existing(header.right_sibling);
				}
				node.leaf_entries = page.entries();
                if (native_sink_)
                    for (const auto &entry : node.leaf_entries) {
                        if(native_schema_keys_ && isEntityFreeSpaceIndexId(index_id_)) {
                            validateNativeEntityFreeSummaryEntry(index_id_,entry);continue;
                        }
                        if (native_schema_keys_ ?
                            (entry.key.size() > RSS_INDEX_KEY_MAX_BYTES ||
                                (isSystemCatalogConstraintDefinitionId(index_id_) && entry.key.size() != RSS_CONSTRAINT_SUPPORT_ENTRY_KEY_BYTES) ||
                                (index_id_ != RSS_SYSTEM_CATALOG_INDEX_ID && IndexKeyCodec::decodeEntryEntityId(entry.key) != entry.value_or_child)) :
                            index_id_ == RSS_LARGE_VALUE_CONTENT_INDEX_ID ?
                            (entry.key.size() < 53U || entry.key.size() > nativeKeyWidth()) : entry.key.size() != nativeKeyWidth())
                            throw FormatException(FormatError::FERR_INVALID_HEADER, "native map image source has a wrong fixed-key width");
                    }
			} else {
				node.leftmost_child = OverlayPageReference::existing(header.leftmost_child);
				for (const BTreePageEntry &entry: page.entries()) {
                    if (native_sink_ && entry.key.size() > nativeKeyWidth())
                        throw FormatException(FormatError::FERR_INVALID_HEADER, "native map separator exceeds its fixed-key width");
					node.internal_entries.push_back(
							{entry.key, OverlayPageReference::existing(PageId(entry.value_or_child))});
				}
			}
			return existing_nodes_
					.emplace(pageId.value(), ExistingOverlayMutationNode{std::move(node), location, ownerLsn})
					.first->second.node;
		}

		[[nodiscard]] ExistingOverlayMutationNode &existingFor(PageId pageId) {
			return existing_nodes_.at(pageId.value());
		}

		[[nodiscard]] OverlayMutationNode &nodeFor(const OverlayPageReference &reference) {
			if (reference.is_virtual) {
				return virtual_nodes_.at(reference.virtual_ordinal);
			}
			return existingFor(reference.existing_page_id).node;
		}

		[[nodiscard]] OverlayMutationNode &loadNode(const OverlayPageReference &reference, uint16_t expectedLevel) {
			if (reference.is_virtual) {
                auto &node = nodeFor(reference);
                if (!node.packed_leaf_entries.empty()) {
                    if (node.header.kind != BTreeNodeKind::BPN_LEAF || !node.leaf_entries.empty())
                        throw FormatException(FormatError::FERR_INVALID_STATE, "overlay leaf has mixed private representations");
                    node.leaf_entries = PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF,
                        node.packed_leaf_entries, leaf_value_layout_);
                    node.packed_leaf_entries = {};
                }
				return node;
			}
            // Evacuation and cross-pool COW may leave any tree with children
            // from different physical generations. Keep the exact root fence;
            // authenticate each child through its own pinned directory route.
            const auto generation = reference.existing_page_id != expected_root_.root_page_id ?
                metadata_.findPageLocation(snapshot_, reference.existing_page_id).generation : expected_root_.root_generation;
            return loadExisting(reference.existing_page_id, generation.value(), expectedLevel);
		}

		[[nodiscard]] OverlayPageReference createVirtual(BTreePageHeader header) {
            admitNativeNode(); checkNative();
			header.page_id = PageId(1U);
			header.generation = expected_root_.root_generation;
			header.entry_count = 0U;
			OverlayMutationNode node;
			node.header = std::move(header);
			virtual_nodes_.push_back(std::move(node));
			return OverlayPageReference::virtualPage(virtual_nodes_.size() - 1U);
		}

		[[nodiscard]] static OverlayPageReference childAt(const OverlayMutationNode &parent, size_t ordinal) {
			return ordinal == 0U ? parent.leftmost_child.value() : parent.internal_entries.at(ordinal - 1U).right_child;
		}

		[[nodiscard]] static size_t childOrdinal(const OverlayMutationNode &parent, const OverlayPageReference &child) {
			for (size_t ordinal = 0U; ordinal <= parent.internal_entries.size(); ++ordinal) {
				if (childAt(parent, ordinal) == child) {
					return ordinal;
				}
			}
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable transaction-local B+Tree overlay parent lost its selected child");
		}

		[[nodiscard]] static PageId preview(const OverlayPageReference &reference) {
			return reference.is_virtual ? PageId(1U) : reference.existing_page_id;
		}

		[[nodiscard]] static PageId resolve(const OverlayPageReference &reference,
											const std::vector<PageId> &virtualIds) {
			if (!reference.is_virtual) {
				return reference.existing_page_id;
			}
			const PageId pageId = virtualIds.at(reference.virtual_ordinal);
			if (!pageId.isValid()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay could not resolve a virtual Page ID");
			}
			return pageId;
		}

		[[nodiscard]] BTreePage materialize(const OverlayMutationNode &node, PageId pageId, Generation generation,
											LogSequenceNumber ownerLsn, const std::vector<PageId> &virtualIds) const {
			const PageOwnerHeader owner = indexPageOwner(index_id_, pageId, generation, ownerLsn);
			if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
				const PageId left = node.left_sibling.has_value() ? resolve(*node.left_sibling, virtualIds) : PageId{};
				const PageId right =
						node.right_sibling.has_value() ? resolve(*node.right_sibling, virtualIds) : PageId{};
                std::optional<utils::OwnedStdVector<BTreePageEntry>> decoded;
                if (!node.packed_leaf_entries.empty()) {
                    if (!node.leaf_entries.empty()) throw FormatException(FormatError::FERR_INVALID_STATE,
                        "overlay leaf has mixed private representations");
                    decoded.emplace(PackedBTreePageCodec::decode(BTreeNodeKind::BPN_LEAF,
                        node.packed_leaf_entries, node.header.leaf_value_layout));
                }
				return makeLeafPage(node.header, owner, left, right, decoded ? *decoded : node.leaf_entries);
			}
			BTreePageHeader header = node.header;
			header.leftmost_child = resolve(*node.leftmost_child, virtualIds);
			utils::OwnedStdVector<BTreePageEntry> entries;
			entries.reserve(node.internal_entries.size());
			for (const OverlayInternalEntry &entry: node.internal_entries) {
				entries.push_back({entry.key, resolve(entry.right_child, virtualIds).value()});
			}
			return makeInternalPage(header, owner, entries);
		}

		[[nodiscard]] BTreePage materializePreview(const OverlayMutationNode &node) const {
			// Internal child IDs use varints. Metadata assigns virtual IDs only
			// during publication, so a one-byte placeholder can understate the
			// final payload and admit a page that cannot be materialized. Reserve
			// the largest legal width, as the initial-tree planner already does.
			std::vector<PageId> virtualIds(virtual_nodes_.size(), PageId(std::numeric_limits<uint64_t>::max()));
			return materialize(node, node.header.page_id, node.header.generation, LogSequenceNumber(1U), virtualIds);
		}

		[[nodiscard]] bool underfull(const OverlayMutationNode &node) const {
			const bool empty = node.header.kind == BTreeNodeKind::BPN_LEAF ? node.leaf_entries.empty() && node.packed_leaf_entries.empty()
																		   : node.internal_entries.empty();
			return empty || encodedNodeOccupancy(materializePreview(node)) < RSS_BTREE_MIN_NODE_OCCUPIED_BYTES;
		}

		[[nodiscard]] size_t validatedLeafOccupancy(const OverlayMutationNode &node) const {
			const auto referenceId = [&](const std::optional<OverlayPageReference> &reference) {
				if (!reference) return PageId{};
				if (reference->is_virtual) static_cast<void>(virtual_nodes_.at(reference->virtual_ordinal));
				return preview(*reference);
			};
			BTreePageHeader header = node.header;
			header.level = 0;
			header.entry_count = 0;
			header.leftmost_child = {};
			header.left_sibling = referenceId(node.left_sibling);
			header.right_sibling = referenceId(node.right_sibling);
			// Keep owner/header/reference validation, without materializing
			// and decoding a packed copy of every growing leaf prefix.
			BTreePage::validateNewPage(indexPageOwner(index_id_, header.page_id,
				header.generation, LogSequenceNumber(1U)), header);
			constexpr size_t overhead = RSS_SLOTTED_PAGE_HEADER_BYTES +
				2U * RSS_SLOTTED_PAGE_SLOT_BYTES + RSS_BTREE_PAGE_HEADER_BYTES;
            if (!node.packed_leaf_entries.empty()) return overhead + node.packed_leaf_entries.size();
			return overhead + PackedBTreePageCodec::encodedSize(header.kind, node.leaf_entries,
				header.leaf_value_layout);
		}
		[[nodiscard]] bool fits(const OverlayMutationNode &node) const {
			try {
				if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
					return validatedLeafOccupancy(node) <= RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
				}
				static_cast<void>(materializePreview(node));
				return true;
			} catch (const FormatException &) {
				return false;
			}
		}

		[[nodiscard]] std::vector<OverlayPageReference> pathFor(std::span<const std::byte> key) {
			std::vector<OverlayPageReference> path;
			OverlayPageReference current = root_;
			uint16_t expectedLevel = nodeFor(root_).header.level;
			for (;;) {
				OverlayMutationNode &node = loadNode(current, expectedLevel);
				path.push_back(current);
				if (node.header.kind == BTreeNodeKind::BPN_LEAF) {
					return path;
				}
				// Separators are authenticated and remain ordered in the overlay.
				// Equality follows the right child, matching the persisted router.
				const auto upper = std::upper_bound(node.internal_entries.begin(), node.internal_entries.end(), key,
					[](std::span<const std::byte> value, const OverlayInternalEntry &entry) {
						return lessKey(value, entry.key);
					});
				const OverlayPageReference next = childAt(node,
					static_cast<size_t>(upper - node.internal_entries.begin()));
				current = next;
				--expectedLevel;
			}
		}

        // Pending keys own their input: streamed emitters may reuse the source
        // operation immediately after apply returns. ERASE is a flush barrier.
        struct PendingUpsert {
            using OwnedHeapElement = void;
            utils::OwnedStdVector<std::byte> key;
            uint64_t value = 0U;
        };

        size_t mergeUpsertsIntoLeaf(std::span<const PendingUpsert *const> operations) {
            checkNative();
            auto path = pathFor(operations.front()->key);
            auto &leaf = nodeFor(path.back());
            std::optional<std::span<const std::byte>> upper;
            for (size_t depth = path.size() - 1U; depth > 0U; --depth) {
                const auto &parent = nodeFor(path[depth - 1U]);
                const size_t ordinal = childOrdinal(parent, path[depth]);
                if (ordinal < parent.internal_entries.size()) {
                    upper = parent.internal_entries[ordinal].key;
                    break;
                }
            }
            size_t count = 1U;
            while (count < operations.size() && (!upper || lessKey(operations[count]->key, *upper))) ++count;
            // Ordered tail growth already has an admitted geometric backing and
            // constant-time packed-size deltas. Preserve that existing path.
            if (leaf.leaf_entries.empty() || lessKey(leaf.leaf_entries.back().key, operations.front()->key)) {
                for (size_t i = 0U; i < count; ++i) {
                    checkNative();
                    upsert(operations[i]->key, operations[i]->value);
                }
                return count;
            }
            constexpr size_t capacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
                2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
            while (count > 1U) {
                utils::OwnedStdVector<BTreePageEntry> merged;
                merged.reserve(leaf.leaf_entries.size() + count);
                size_t old = 0U, next = 0U;
                while (old < leaf.leaf_entries.size() || next < count) {
                    checkNative();
                    if (next == count || (old < leaf.leaf_entries.size() &&
                            lessKey(leaf.leaf_entries[old].key, operations[next]->key))) {
                        merged.push_back(leaf.leaf_entries[old++]);
                        debug::PerfTrace::addValue("native_btree.plan.batch_copied_source_entries", 1);
                        continue;
                    }
                    const auto *operation = operations[next++];
                    while (next < count && std::ranges::equal(operation->key, operations[next]->key))
                        operation = operations[next++]; // Original equal-key order: last write wins.
                    if (old < leaf.leaf_entries.size() && std::ranges::equal(leaf.leaf_entries[old].key, operation->key)) ++old;
                    merged.push_back({operation->key, operation->value});
                }
                const auto bytes = PackedBTreePageCodec::encodedSize(BTreeNodeKind::BPN_LEAF, merged, leaf_value_layout_);
                const bool fitsOne = bytes <= capacity;
                if (!fitsOne && !findBalancedLeafSplitIndex(leaf.header, index_id_, LogSequenceNumber(1U),
                        LogSequenceNumber(1U), leaf.left_sibling ? preview(*leaf.left_sibling) : PageId{},
                        leaf.right_sibling ? preview(*leaf.right_sibling) : PageId{}, merged)) {
                    // Wide keys may require more than two pages. Reduce this
                    // bounded group before changing any private node.
                    count = (count + 1U) / 2U;
                    continue;
                }
                leaf_append_size_.reset();
                leaf.leaf_entries = std::move(merged);
                leaf.dirty = true;
                debug::PerfTrace::addValue("native_btree.plan.leaf_batch_merges", 1);
                debug::PerfTrace::addValue("native_btree.plan.batch_merged_upserts", count);
                if (fitsOne) leaf_append_size_ = LeafAppendSize{path.back(), leaf.leaf_entries.size(), bytes};
                else splitLeaf(path);
                return count;
            }
            upsert(operations.front()->key, operations.front()->value);
            return 1U;
        }

        void flushPendingUpserts() {
            if (pending_upserts_.empty()) return;
            std::array<const PendingUpsert *, 64U> ordered{};
            for (size_t i = 0U; i < pending_upserts_.size(); ++i) ordered[i] = &pending_upserts_[i];
            auto remaining = std::span<const PendingUpsert *const>(ordered.data(), pending_upserts_.size());
            std::ranges::sort(std::span(ordered.data(), pending_upserts_.size()), [](const auto *left, const auto *right) {
                if (lessKey(left->key, right->key)) return true;
                if (lessKey(right->key, left->key)) return false;
                return left < right; // Same contiguous buffer, preserving source statement order.
            });
            while (!remaining.empty()) remaining = remaining.subspan(mergeUpsertsIntoLeaf(remaining));
            pending_upserts_.clear();
            pending_key_bytes_ = 0U;
        }

		void upsert(std::span<const std::byte> key, uint64_t value) {
			std::vector<OverlayPageReference> path = pathFor(key);
			OverlayMutationNode &leaf = nodeFor(path.back());
			// Authenticated leaves and every overlay mutation preserve key order.
			// Locate once instead of scanning then sorting the complete leaf after
			// each insertion; ordered bulk appends therefore avoid full-leaf sorts.
			const auto position = std::lower_bound(leaf.leaf_entries.begin(), leaf.leaf_entries.end(), key,
				[](const BTreePageEntry &entry, std::span<const std::byte> value) {
					return lessKey(entry.key, value);
				});
			const bool append = position == leaf.leaf_entries.end();
			std::optional<size_t> appendedSize;
			BTreePageEntry next{std::vector<std::byte>(key.begin(), key.end()), value};
			if (append && leaf_append_size_ && leaf_append_size_->page == path.back() &&
				leaf_append_size_->count == leaf.leaf_entries.size()) {
				static_assert(RSS_BTREE_PACKED_RESTART_INTERVAL > 1U);
				size_t increment;
				if (leaf.leaf_entries.size() % RSS_BTREE_PACKED_RESTART_INTERVAL == 0U) {
					increment = PackedBTreePageCodec::encodedSize(BTreeNodeKind::BPN_LEAF,
						std::span<const BTreePageEntry>(&next, 1U), leaf_value_layout_) - RSS_BTREE_PACKED_HEADER_BYTES;
				} else {
					// The last/new pair has the exact non-restart prefix cost. Its
					// restart table is identical to the singleton's and cancels out.
					const std::array pair{leaf.leaf_entries.back(), next};
					increment = PackedBTreePageCodec::encodedSize(BTreeNodeKind::BPN_LEAF, pair, leaf_value_layout_) -
						PackedBTreePageCodec::encodedSize(BTreeNodeKind::BPN_LEAF,
							std::span<const BTreePageEntry>(&pair[0], 1U), leaf_value_layout_);
				}
				if (increment > std::numeric_limits<size_t>::max() - leaf_append_size_->bytes) {
					throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "B+Tree append size overflowed");
				}
				appendedSize = leaf_append_size_->bytes + increment;
				debug::PerfTrace::addValue("native_btree.plan.cached_leaf_append_size", 1);
			}
			leaf_append_size_.reset();
			if (position != leaf.leaf_entries.end() && std::ranges::equal(position->key, key)) {
				position->value_or_child = value;
			} else if (append) {
				// OwnedStdVector's general insertion keeps a private copy for
				// throwing element moves. Appending has no shifted suffix and can
				// use its admitted, geometrically grown backing directly.
				leaf.leaf_entries.push_back(std::move(next));
			} else {
				leaf.leaf_entries.insert(position, std::move(next));
			}
			leaf.dirty = true;
			constexpr size_t capacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES -
				2U * RSS_SLOTTED_PAGE_SLOT_BYTES - RSS_BTREE_PAGE_HEADER_BYTES;
			const bool fitsNow = appendedSize ? *appendedSize <= capacity : fits(leaf);
			if (fitsNow) {
				const size_t bytes = appendedSize ? *appendedSize : PackedBTreePageCodec::encodedSize(
					BTreeNodeKind::BPN_LEAF, leaf.leaf_entries, leaf_value_layout_);
				leaf_append_size_ = LeafAppendSize{path.back(), leaf.leaf_entries.size(), bytes};
				return;
			}
			splitLeaf(path);
		}

		void splitLeaf(std::vector<OverlayPageReference> &path) {
			const OverlayPageReference leftRef = path.back();
			OverlayMutationNode &left = nodeFor(leftRef);
			utils::OwnedStdVector<BTreePageEntry> entries = left.leaf_entries;
			const std::optional<size_t> split = findBalancedLeafSplitIndex(
					left.header, index_id_, LogSequenceNumber(1U), LogSequenceNumber(1U),
					left.left_sibling.has_value() ? preview(*left.left_sibling) : PageId{},
					left.right_sibling.has_value() ? preview(*left.right_sibling) : PageId{}, entries);
			if (!split.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay cannot split an oversized leaf");
			}
			BTreePageHeader rightHeader = left.header;
			rightHeader.page_id = PageId(1U);
			OverlayPageReference rightRef = createVirtual(rightHeader);
			OverlayMutationNode &leftAfter = nodeFor(leftRef);
			OverlayMutationNode &right = nodeFor(rightRef);
			right.leaf_entries.assign(entries.begin() + static_cast<ptrdiff_t>(*split), entries.end());
			right.left_sibling = leftRef;
			right.right_sibling = leftAfter.right_sibling;
			leftAfter.leaf_entries.assign(entries.begin(), entries.begin() + static_cast<ptrdiff_t>(*split));
			leftAfter.right_sibling = rightRef;
			leftAfter.dirty = true;
			if (right.right_sibling.has_value()) {
				OverlayMutationNode &next = loadNode(*right.right_sibling, 0U);
				if (!next.left_sibling.has_value() || *next.left_sibling != leftRef) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable transaction-local B+Tree overlay found a broken leaf successor link");
				}
				next.left_sibling = rightRef;
				next.dirty = true;
			}
			insertPromoted(path, path.size() - 1U, right.leaf_entries.front().key, rightRef);
		}

		void insertPromoted(std::vector<OverlayPageReference> &path, size_t childPathIndex,
							utils::OwnedStdVector<std::byte> separator, OverlayPageReference rightChild) {
			if (childPathIndex == 0U) {
				OverlayMutationNode &oldRoot = nodeFor(path.front());
				BTreePageHeader rootHeader;
				rootHeader.index_id = index_id_;
				rootHeader.page_id = PageId(1U);
				rootHeader.generation = expected_root_.root_generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = static_cast<uint16_t>(oldRoot.header.level + 1U);
				rootHeader.leftmost_child = PageId(1U);
				const OverlayPageReference newRoot = createVirtual(rootHeader);
				OverlayMutationNode &root = nodeFor(newRoot);
				root.leftmost_child = path.front();
				root.internal_entries.push_back({std::move(separator), rightChild});
				root_ = newRoot;
				return;
			}
			const OverlayPageReference parentRef = path[childPathIndex - 1U];
			OverlayMutationNode &parent = nodeFor(parentRef);
			const size_t ordinal = childOrdinal(parent, path[childPathIndex]);
			parent.internal_entries.insert(parent.internal_entries.begin() + static_cast<ptrdiff_t>(ordinal),
										   {std::move(separator), rightChild});
			parent.dirty = true;
			if (fits(parent)) {
				return;
			}
			splitInternal(path, childPathIndex - 1U);
		}

		void splitInternal(std::vector<OverlayPageReference> &path, size_t pathIndex) {
			const OverlayPageReference leftRef = path[pathIndex];
			OverlayMutationNode &left = nodeFor(leftRef);
			utils::OwnedStdVector<OverlayInternalEntry> combined = left.internal_entries;
			std::optional<size_t> selected;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			// Variable-length keys can make an endpoint separator the only split
			// that fits. This matches the durable deep-split planner and preserves
			// the child reference carried by a unary internal page.
			for (size_t promoted = 0U; promoted < combined.size(); ++promoted) {
				OverlayMutationNode leftCandidate = left;
				leftCandidate.internal_entries.assign(combined.begin(),
													  combined.begin() + static_cast<ptrdiff_t>(promoted));
				OverlayMutationNode rightCandidate = left;
				rightCandidate.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(promoted + 1U),
													   combined.end());
				rightCandidate.leftmost_child = combined[promoted].right_child;
				try {
					const uint32_t leftBytes = encodedNodeOccupancy(materializePreview(leftCandidate));
					const uint32_t rightBytes = encodedNodeOccupancy(materializePreview(rightCandidate));
					const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
					if (!selected.has_value() || skew < smallestSkew) {
						selected = promoted;
						smallestSkew = skew;
					}
				} catch (const FormatException &) {
				}
			}
			if (!selected.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot split an oversized internal page");
			}
			const OverlayInternalEntry promoted = combined[*selected];
			BTreePageHeader rightHeader = left.header;
			rightHeader.page_id = PageId(1U);
			OverlayPageReference rightRef = createVirtual(rightHeader);
			OverlayMutationNode &leftAfter = nodeFor(leftRef);
			OverlayMutationNode &right = nodeFor(rightRef);
			right.leftmost_child = promoted.right_child;
			right.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(*selected + 1U), combined.end());
			leftAfter.internal_entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(*selected));
			leftAfter.dirty = true;
			insertPromoted(path, pathIndex, promoted.key, rightRef);
		}

		void erase(std::span<const std::byte> key) {
			std::vector<OverlayPageReference> path = pathFor(key);
			OverlayMutationNode &leaf = nodeFor(path.back());
			const auto erased = std::ranges::find_if(leaf.leaf_entries, [&key](const BTreePageEntry &entry) {
				return std::ranges::equal(entry.key, key);
			});
			if (erased == leaf.leaf_entries.end()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable transaction-local B+Tree overlay cannot erase an absent scalar key");
			}
			const bool erasedFirst = erased == leaf.leaf_entries.begin();
			leaf.leaf_entries.erase(erased);
			leaf.dirty = true;
			if (erasedFirst && !leaf.leaf_entries.empty() && path.size() > 1U) {
				OverlayMutationNode &parent = nodeFor(path[path.size() - 2U]);
				const size_t ordinal = childOrdinal(parent, path.back());
				if (ordinal != 0U) {
					parent.internal_entries[ordinal - 1U].key = leaf.leaf_entries.front().key;
					parent.dirty = true;
				}
			}
			// Removing a key shifts packed-prefix restart positions. The remaining
			// entries can therefore occupy more bytes despite a smaller count.
			// Split an overflowing leaf before occupancy-based erase rebalancing
			// attempts to materialize it; insertion and deletion share the same
			// byte-capacity and parent/sibling publication rules.
			const size_t leafOccupancy = validatedLeafOccupancy(leaf);
			if (leafOccupancy > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
				splitLeaf(path);
				return;
			}
			rebalanceAfterErase(path, leafOccupancy);
		}

		void retire(const OverlayPageReference &reference) {
			OverlayMutationNode &node = nodeFor(reference);
			node.retired = true;
			node.dirty = false;
		}

		void rebalanceAfterErase(std::vector<OverlayPageReference> &path, size_t initialLeafOccupancy) {
			size_t pathIndex = path.size() - 1U;
			for (;;) {
				const OverlayPageReference currentRef = path[pathIndex];
				OverlayMutationNode &current = nodeFor(currentRef);
				if (pathIndex == 0U) {
					if (current.header.kind == BTreeNodeKind::BPN_INTERNAL && current.internal_entries.empty()) {
						const OverlayPageReference child = *current.leftmost_child;
						retire(currentRef);
						root_ = child;
						collapseUnaryRoot();
					}
					return;
				}
				// Reuse the exact authenticated size already computed after erase.
				// Later ancestors and modified siblings retain their normal checks.
				const bool currentUnderfull = pathIndex == path.size() - 1U
					? current.leaf_entries.empty() || initialLeafOccupancy < RSS_BTREE_MIN_NODE_OCCUPIED_BYTES
					: underfull(current);
				if (!currentUnderfull) {
					return;
				}
				OverlayMutationNode &parent = nodeFor(path[pathIndex - 1U]);
				if (parent.internal_entries.empty()) {
					--pathIndex;
					continue;
				}
				const size_t currentOrdinal = childOrdinal(parent, currentRef);
				const size_t leftOrdinal =
						currentOrdinal < parent.internal_entries.size() ? currentOrdinal : currentOrdinal - 1U;
				const OverlayPageReference leftRef = childAt(parent, leftOrdinal);
				const OverlayPageReference rightRef = childAt(parent, leftOrdinal + 1U);
				OverlayMutationNode &left = loadNode(leftRef, current.header.level);
				OverlayMutationNode &right = loadNode(rightRef, current.header.level);
				if (current.header.kind == BTreeNodeKind::BPN_LEAF) {
					if (!rebalanceLeaf(parent, leftOrdinal, leftRef, rightRef, left, right)) {
						// Variable-length keys can leave one nonempty leaf below the
						// byte occupancy target while neither a legal redistribution nor
						// a one-page merge fits. Keeping it is the only valid layout.
						return;
					}
				} else {
					rebalanceInternal(parent, leftOrdinal, leftRef, rightRef, left, right, pathIndex);
				}
				if (!nodeFor(path[pathIndex]).retired && !underfull(nodeFor(path[pathIndex]))) {
					// A merge can leave the surviving child healthy while removing the
					// final separator from its parent. Continue to that unary parent so
					// the root-collapse path cannot publish an unnecessary tree level.
					if (!nodeFor(path[pathIndex - 1U]).internal_entries.empty()) {
						return;
					}
				}
				--pathIndex;
			}
		}

		[[nodiscard]] bool rebalanceLeaf(OverlayMutationNode &parent, size_t leftOrdinal,
										 const OverlayPageReference &leftRef, const OverlayPageReference &rightRef,
										 OverlayMutationNode &left, OverlayMutationNode &right) {
			if (!left.right_sibling.has_value() || !right.left_sibling.has_value() || *left.right_sibling != rightRef ||
				*right.left_sibling != leftRef) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay found leaf siblings with broken links");
			}
			utils::OwnedStdVector<BTreePageEntry> combined = left.leaf_entries;
			combined.insert(combined.end(), right.leaf_entries.begin(), right.leaf_entries.end());
			std::optional<size_t> redistribution;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			// Authenticate the common headers and references once. Exact packed
			// boundary sizes include each candidate's independently aligned restart
			// table; constructing two complete pages per split only repeats copies,
			// encoding and validation of the same immutable keys.
			static_cast<void>(validatedLeafOccupancy(left));
			static_cast<void>(validatedLeafOccupancy(right));
			constexpr size_t overhead = RSS_SLOTTED_PAGE_HEADER_BYTES +
				2U * RSS_SLOTTED_PAGE_SLOT_BYTES + RSS_BTREE_PAGE_HEADER_BYTES;
			constexpr size_t capacity = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - overhead;
			const auto sizes = PackedBTreePageCodec::encodedBoundarySizes(
				BTreeNodeKind::BPN_LEAF, combined, leaf_value_layout_);
			for (size_t split = 1U; split < combined.size(); ++split) {
				if (sizes.prefixes[split] > capacity || sizes.suffixes[split] > capacity) continue;
				const size_t leftBytes = overhead + sizes.prefixes[split];
				const size_t rightBytes = overhead + sizes.suffixes[split];
				if (leftBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES &&
					rightBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES) {
					const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
					if (!redistribution.has_value() || skew < smallestSkew) {
						redistribution = split;
						smallestSkew = skew;
					}
				}
			}
			if (redistribution.has_value()) {
				left.leaf_entries.assign(combined.begin(), combined.begin() + static_cast<ptrdiff_t>(*redistribution));
				right.leaf_entries.assign(combined.begin() + static_cast<ptrdiff_t>(*redistribution), combined.end());
				static_cast<void>(materializePreview(left));
				static_cast<void>(materializePreview(right));
				left.dirty = true;
				right.dirty = true;
				parent.internal_entries[leftOrdinal].key = right.leaf_entries.front().key;
				parent.dirty = true;
				return true;
			}
			const bool retainRight = left.leaf_entries.empty() && !right.leaf_entries.empty();
			OverlayMutationNode merged = retainRight ? right : left;
			merged.leaf_entries = std::move(combined);
			if (retainRight) {
				merged.left_sibling = left.left_sibling;
			} else {
				merged.right_sibling = right.right_sibling;
			}
			if (!fits(merged)) {
				if (!left.leaf_entries.empty() && !right.leaf_entries.empty()) {
					// The current nonempty leaf remains readable and ordered. A later
					// delete may make a merge possible; publishing no rebalance avoids
					// manufacturing an invalid variable-length page layout.
					return false;
				}
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot merge an empty underfull leaf");
			}
			if (retainRight) {
				right.leaf_entries = std::move(merged.leaf_entries);
				right.left_sibling = merged.left_sibling;
				right.dirty = true;
				retire(leftRef);
				if (right.left_sibling.has_value()) {
					OverlayMutationNode &previous = loadNode(*right.left_sibling, 0U);
					previous.right_sibling = rightRef;
					previous.dirty = true;
				}
				if (leftOrdinal == 0U) {
					parent.leftmost_child = rightRef;
					parent.internal_entries.erase(parent.internal_entries.begin());
				} else {
					parent.internal_entries.erase(parent.internal_entries.begin() +
												  static_cast<ptrdiff_t>(leftOrdinal - 1U));
				}
			} else {
				left.leaf_entries = std::move(merged.leaf_entries);
				left.right_sibling = merged.right_sibling;
				left.dirty = true;
				retire(rightRef);
				if (left.right_sibling.has_value()) {
					OverlayMutationNode &next = loadNode(*left.right_sibling, 0U);
					next.left_sibling = leftRef;
					next.dirty = true;
				}
				parent.internal_entries.erase(parent.internal_entries.begin() + static_cast<ptrdiff_t>(leftOrdinal));
			}
			parent.dirty = true;
			return true;
		}

		void rebalanceInternal(OverlayMutationNode &parent, size_t leftOrdinal, const OverlayPageReference &leftRef,
							   const OverlayPageReference &rightRef, OverlayMutationNode &left,
							   OverlayMutationNode &right, size_t) {
			OverlayInternalEntry bridge = parent.internal_entries[leftOrdinal];
			bridge.right_child = *right.leftmost_child;
			utils::OwnedStdVector<OverlayInternalEntry> combined = left.internal_entries;
			combined.push_back(bridge);
			combined.insert(combined.end(), right.internal_entries.begin(), right.internal_entries.end());
			std::optional<size_t> redistribution;
			uint64_t smallestSkew = std::numeric_limits<uint64_t>::max();
			for (size_t promoted = 1U; promoted + 1U < combined.size(); ++promoted) {
				OverlayMutationNode leftCandidate = left;
				OverlayMutationNode rightCandidate = right;
				leftCandidate.internal_entries.assign(combined.begin(),
													  combined.begin() + static_cast<ptrdiff_t>(promoted));
				rightCandidate.leftmost_child = combined[promoted].right_child;
				rightCandidate.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(promoted + 1U),
													   combined.end());
				try {
					const uint32_t leftBytes = encodedNodeOccupancy(materializePreview(leftCandidate));
					const uint32_t rightBytes = encodedNodeOccupancy(materializePreview(rightCandidate));
					if (leftBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES &&
						rightBytes >= RSS_BTREE_MIN_NODE_OCCUPIED_BYTES) {
						const uint64_t skew = leftBytes > rightBytes ? leftBytes - rightBytes : rightBytes - leftBytes;
						if (!redistribution.has_value() || skew < smallestSkew) {
							redistribution = promoted;
							smallestSkew = skew;
						}
					}
				} catch (const FormatException &error) {
					if (error.error() != FormatError::FERR_INVALID_STATE) {
						throw;
					}
				}
			}
			if (redistribution.has_value()) {
				left.internal_entries.assign(combined.begin(),
											 combined.begin() + static_cast<ptrdiff_t>(*redistribution));
				right.leftmost_child = combined[*redistribution].right_child;
				right.internal_entries.assign(combined.begin() + static_cast<ptrdiff_t>(*redistribution + 1U),
											  combined.end());
				left.dirty = true;
				right.dirty = true;
				parent.internal_entries[leftOrdinal].key = combined[*redistribution].key;
				parent.dirty = true;
				return;
			}
			OverlayMutationNode merged = left;
			merged.internal_entries = std::move(combined);
			if (!fits(merged)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot merge an underfull internal pair");
			}
			left.internal_entries = std::move(merged.internal_entries);
			left.dirty = true;
			retire(rightRef);
			parent.internal_entries.erase(parent.internal_entries.begin() + static_cast<ptrdiff_t>(leftOrdinal));
			parent.dirty = true;
		}

		void collapseUnaryRoot() {
			while (true) {
				OverlayMutationNode &candidate = nodeFor(root_);
				if (candidate.header.kind != BTreeNodeKind::BPN_INTERNAL || !candidate.internal_entries.empty()) {
					break;
				}
				const OverlayPageReference child = *candidate.leftmost_child;
				retire(root_);
				root_ = child;
			}
			OverlayMutationNode &root = nodeFor(root_);
			if (root.header.kind == BTreeNodeKind::BPN_LEAF &&
				(root.left_sibling.has_value() || root.right_sibling.has_value())) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable transaction-local B+Tree overlay cannot publish a root leaf with siblings");
			}
		}

		[[nodiscard]] SlottedPagePool &pages() const { return const_cast<SlottedPagePool &>(store_.pages_); }

		const BTreePageStore &store_;
		RelocatableMetadataStore &metadata_;
		const MetadataSnapshotPin &snapshot_;
		uint64_t index_id_ = 0U;
		BTreeLeafValueLayout leaf_value_layout_ = BTreeLeafValueLayout::BLVL_U64;
		bool rebuild_on_frame_exhaustion_ = false;
		bool force_fresh_rebuild_ = false;
		bool fresh_rebuild_applied_ = false;
		IndexRootLocation expected_root_;
        // Declared before the governed std maps, vectors and callback so all
        // retained storage dies before its conservative credit is returned.
        utils::OwnedHeapReservation native_credit_;
        std::function<void(BTreePage)> native_sink_;
        const CancellationCheckpoint *native_cancellation_ = nullptr;
        bool native_schema_keys_ = false;
        void checkNative() const { if (native_cancellation_) native_cancellation_->check(); }
        [[nodiscard]] size_t nativeKeyWidth() const {
            if (native_schema_keys_) return RSS_INDEX_KEY_MAX_BYTES;
            if (index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID) return RSS_GLOBAL_ADJACENCY_KEY_BYTES;
            if (index_id_ == RSS_LARGE_VALUE_CONTENT_INDEX_ID) return 53U + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES;
            if (index_id_ == RSS_HYBRID_PROPERTY_INDEX_ID_FIRST +
                static_cast<uint8_t>(HybridPropertyRootOrdinal::HPRO_NODE_SCAN)) return 24U;
            const auto role = (index_id_ - RSS_HYBRID_PROPERTY_INDEX_ID_FIRST) % 4U;
            return role == 0 ? 16U : role == 3 ? 24U : 28U;
        }
        void admitNativeNode() {
            if (!native_sink_) return;
            // Decoded keys, leaf/internal vectors, the virtual deque and
            // existing-node map own exact admissions. This remaining envelope
            // covers standard API plan/request/frontier vectors: no phase has
            // more than sixteen simultaneously retained/growing copies of any
            // listed per-node record. The fixed credit covers path and callback
            // frontiers; page images retain their independent owned admissions.
            constexpr size_t planRecordEnvelope = 16U * (sizeof(IndexTreeMutationPoolRequest) +
                sizeof(IndexTreePageRetirement) + sizeof(PageLocation) +
                sizeof(BTreeNewIndexTreePoolMutation) + sizeof(OverlayPageReference) + sizeof(size_t));
            native_credit_.set(utils::detail::addStandardHeapBytes(native_credit_.bytes(), planRecordEnvelope));
        }
		// loadRoot() runs while root_ is initialized and immediately populates
		// this map, so it must be constructed before root_.
		utils::OwnedMap<uint64_t, ExistingOverlayMutationNode> existing_nodes_;
		OverlayPageReference root_;
		struct LeafAppendSize {
			OverlayPageReference page;
			size_t count;
			size_t bytes;
		};
		// Transaction-local, last-leaf only. Erases/non-appends and splits
		// invalidate it; no persisted bytes or cross-snapshot proof is cached.
		std::optional<LeafAppendSize> leaf_append_size_;
        utils::OwnedStdVector<PendingUpsert> pending_upserts_;
        size_t pending_key_bytes_ = 0U;
		std::deque<OverlayMutationNode, utils::OwnedPmrAllocator<OverlayMutationNode>> virtual_nodes_;
	};

	namespace {

		struct RootLeafMutationSource {
			IndexRootLocation root;
			CatalogEntry pool;
			PageLocation location;
			BTreePage page;
			BTreePageHeader header;
			LogSequenceNumber owner_lsn{};
		};

		[[nodiscard]] RootLeafMutationSource loadRootLeafMutationSource(const BTreePageStore &store,
																		RelocatableMetadataStore &metadata,
																		const MetadataSnapshotPin &snapshot,
																		uint64_t indexId) {
			if (!snapshot.isValid() || indexId == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable root-leaf mutation plan requires a live snapshot and Index ID");
			}
			const IndexRootLocation root = metadata.findIndexRoot(snapshot, indexId);
			BTreePage page = store.read(snapshot, root.root_page_id);
			const BTreePageHeader header = page.header();
			const uint8_t invalidRoot = static_cast<uint8_t>(header.index_id != indexId) |
										static_cast<uint8_t>(header.page_id != root.root_page_id) |
										static_cast<uint8_t>(header.generation != root.root_generation) |
										static_cast<uint8_t>(header.kind != BTreeNodeKind::BPN_LEAF) |
										static_cast<uint8_t>(header.left_sibling.isValid()) |
										static_cast<uint8_t>(header.right_sibling.isValid());
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable root-leaf mutation plan requires an unsplit current leaf root");
			}
			const PageLocation location = metadata.findPageLocation(snapshot, root.root_page_id);
			const CatalogEntry pool = metadata.findCatalog(snapshot, location.segment_id);
			const SlottedPage &slotted = page.slottedPage();
			const LogSequenceNumber ownerLsn = slotted.ownerCommittedLsn();
			const uint8_t invalidLocation = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
											static_cast<uint8_t>(pool.generation != location.generation) |
											static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
											static_cast<uint8_t>(location.segment_id != pool.segment_id) |
											static_cast<uint8_t>(slotted.pageId() != root.root_page_id) |
											static_cast<uint8_t>(slotted.pageGeneration() != location.generation) |
											static_cast<uint8_t>(slotted.ownerKind() != PageOwnerKind::POK_INDEX) |
											static_cast<uint8_t>(slotted.ownerId() != indexId) |
											static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
											static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(!ownerLsn.isValid());
			if (invalidLocation != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable root-leaf mutation plan found an unavailable or incompatible index page");
			}
			return {root, pool, location, std::move(page), header, ownerLsn};
		}

		[[nodiscard]] BTreeRootLeafMutationPlan makeRootLeafMutationPlan(const RootLeafMutationSource &source,
																		 std::span<const BTreePageEntry> entries,
																		 const char *structuralMessage) {
			const PageOwnerHeader owner = indexPageOwner(source.root.index_id, source.header.page_id,
														 source.header.generation, source.owner_lsn);
			try {
				const BTreePage replacement = makeLeafPage(source.header, owner, {}, {}, entries);
				return {source.root,
						source.pool,
						source.location,
						source.owner_lsn,
						replacement.serialize(),
						replacement.slottedPage().stats().contiguous_free_bytes};
			} catch (const FormatException &error) {
				if (error.error() != FormatError::FERR_INVALID_STATE) {
					throw;
				}
				throw FormatException(FormatError::FERR_INVALID_STATE, structuralMessage);
			}
		}

	} // namespace
