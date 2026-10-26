        // Only the sealed graph/field coordinator consumes this cold-aware
        // source view. Generic mutation group lookup stays guarded elsewhere.
        const auto topologyGroup = [&](EntityPropertyKey owner, const std::function<void(size_t)> &admission,
            SlottedSlotReference *reference = nullptr) {
            return topologyFields->findGroup(owner, admission, reference, true, true);
        };
        std::array<EntityPropertyKey, 64> pendingNativeOwners{};
        size_t pendingNativeOwnerCount = 0;
        if (preservingNativeFields) {
            utils::OwnedHeapReservation boundary(utils::OwnedAllocationScope::owner());
            const auto independent = [&](EntityPropertyKey owner) {
                if (topologyGroup(owner, [&](size_t bytes) { boundary.set(bytes); }))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                        "native group owner insertion/deletion/property mutation requires its field lifecycle coordinator");
                boundary.set(0);
            };
            for (const auto &owner : hybridTopology->property_owners_) independent(owner);
            const auto inserted = [&](EntityPropertyKey owner) {
                const auto group = topologyGroup(owner, [&](size_t bytes) { boundary.set(bytes); }); boundary.set(0);
                if (!group) return;
                if (pendingNativeOwnerCount == pendingNativeOwners.size() ||
                    (group->live & (UINT64_C(1) << hybridPropertySlotFor(owner))))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native creation requires one bounded absent owner set");
                pendingNativeOwners[pendingNativeOwnerCount++] = owner;
            };
            for (const auto &owner : insertedNodes) inserted({EntityPropertyOwnerKind::EPOK_NODE, owner.entity_id});
            for (const auto &owner : insertedEdges) inserted({EntityPropertyOwnerKind::EPOK_EDGE, owner.entity_id});
            std::array<EntityPropertyKey, 64> nativeOwners = pendingNativeOwners; size_t nativeOwnerCount = pendingNativeOwnerCount;
            const auto accountOwner = [&](EntityPropertyKey owner) {
                const auto selected = std::span(nativeOwners).first(nativeOwnerCount);
                if (std::ranges::find(selected, owner) != selected.end()) return;
                if (nativeOwnerCount == nativeOwners.size())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native logical lifecycle exceeds64distinct affected owners");
                nativeOwners[nativeOwnerCount++] = owner;
            };
            for (const auto owner : hybridTopology->owner_deletions_) accountOwner(owner);
            for (const auto &cell : hybridTopology->field_changes_) {
                accountOwner(cell.owner);
                const bool pending = std::ranges::find(std::span(pendingNativeOwners).first(pendingNativeOwnerCount), cell.owner) !=
                    std::span(pendingNativeOwners).first(pendingNativeOwnerCount).end();
                const auto group = topologyGroup(cell.owner, [&](size_t bytes) { boundary.set(bytes); }); boundary.set(0);
                if (!cell.token || !group || (pending ? cell.erase : !(group->live & (UINT64_C(1) << hybridPropertySlotFor(cell.owner)))))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native graph field has no authenticated live or pending owner");
            }
            for (size_t i = 0; i < hybridTopology->owner_deletions_.size(); ++i) {
                const auto owner = hybridTopology->owner_deletions_[i];
                const auto group = topologyGroup(owner, [&](size_t bytes) { boundary.set(bytes); }); boundary.set(0);
                const auto &erased = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? erasedNodes : erasedEdges;
                if (!owner.isValid() || !std::ranges::binary_search(erased, owner.entity_id) || !group ||
                    !(group->live & (UINT64_C(1) << hybridPropertySlotFor(owner))) ||
                    std::ranges::find(hybridTopology->owner_deletions_.first(i), owner) != hybridTopology->owner_deletions_.first(i).end())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native layout deletion has no unique live Entity removal");
            }
            const auto erased = [&](EntityPropertyKey owner) {
                if (std::ranges::find(hybridTopology->owner_deletions_, owner) == hybridTopology->owner_deletions_.end()) independent(owner);
            };
            for (const auto owner : erasedNodes) erased({EntityPropertyOwnerKind::EPOK_NODE, owner});
            for (const auto owner : erasedEdges) erased({EntityPropertyOwnerKind::EPOK_EDGE, owner});
            // Inline Property rewrites are represented in the sealed owner's
            // list even when they do not create a ROW directory entry.
            for (const auto key : touchedPropertyKeys) independent(EntityPropertyKey::fromDirectoryId(key));
        }
        const bool nativeScanWork = RSS_SCAN_PATH_FORMAT_ENABLED && hybridTopology && !hybridTopology->scan_changes_.empty();
        const bool nativeFieldWork = pendingNativeOwnerCount || (hybridTopology &&
            (!hybridTopology->field_changes_.empty() || !hybridTopology->owner_deletions_.empty()));
        // A Node-only scan transition creates its source account here. Prepay
        // it from the caller; declaration order keeps that credit until every
        // child buffer and the metadata batch have been destroyed.
        std::optional<utils::OwnedHeapReservation> scanSourceCredit;
        if (nativeScanWork && !hybridTopology->fields_memory_) {
            scanSourceCredit.emplace(utils::OwnedAllocationScope::owner(), 1024U * 1024U);
            if (!scanSourceCredit->governed())
                throw std::invalid_argument("scan graph mutation needs an admitted caller source grant");
        }
        std::shared_ptr<std::pmr::memory_resource> pendingNativeMemory = nativeFieldWork || nativeScanWork ?
            (hybridTopology->fields_memory_ ? hybridTopology->fields_memory_ :
                std::make_shared<query::execution::MaterializedByteBudget>(1024U * 1024U - (nativeScanWork ? 4096U : 0U))) : nullptr;
        MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn,
            pendingNativeMemory ? pendingNativeMemory : hybridVectors ? hybridVectors->fields_.memory : hybridPropertyRootMutation && hybridPropertyRootMutation->conversion ?
                hybridPropertyRootMutation->conversion->memory : std::shared_ptr<std::pmr::memory_resource>{});
        // Caller-assigned logical identities must precede every private image
        // allocation, even though their physical pools are materialized later.
        for (const auto pageId : pageIds) batch.observeLogicalPageId(PageId(pageId));
        for (const auto &request : newPoolRequests) batch.observeLogicalSegmentId(request.reservation.segment_id);
        for (const auto &[id, destination] : freshReplacementPools) batch.observeLogicalSegmentId(destination.segment_id);
        // ROW references were assigned from this source before field planning.
        // Reserve their exact range once; native proposals follow it, and both
        // sets feed the metadata-owned content-index preparation below.
        const auto jointRowLargeFirst = jointNativeContent ? batch.allocateLargeValueIdRange(largeValues.size()) : LargeValueId{};
        std::optional<MetadataSnapshotPin> hybridSource;
        std::unique_ptr<HybridPropertyPoolPlacement> hybridPlacement;
        std::unique_ptr<HybridPropertyPoolPlacement> streamedAdjacencyPlacement;
        std::unique_ptr<HybridPropertyPoolPlacement> streamedMembershipPlacement;
        std::optional<MetadataSnapshotPin> ordinaryIndexSource;
        std::unique_ptr<HybridPropertyPoolPlacement> ordinaryIndexPlacement;

        if (hybridVectors) {
            const auto &fields = hybridVectors->fields_;
            hybridSource.emplace(snapshots_.pinCurrentUnderWrite(writer, current));
            hybridPlacement.reset(new HybridPropertyPoolPlacement(*this, batch, writer, *hybridSource,
                fields.first_slot, fields.last_slot, fields.memory, [checkpoint = fields.cancellation] { if (checkpoint) checkpoint->check(); }));
            if (!prepareHybridPropertyColumns(batch, writer, *hybridSource, fields, fields.replacements,
                *conversionPages, *conversionIndexes, *conversionLargeValues, *conversionEntities, *hybridLabels, *hybridPlacement, hybridVectors))
                throw FormatException(FormatError::FERR_INVALID_STATE, "native Vector publication has no authoritative field transition");
        }
        std::unique_ptr<PreparedScanMembershipMutation> scanMembership;
        if (nativeScanWork) scanMembership = prepareScanMembershipMutation(batch, writer, *topologySource,
            *hybridTopology, pendingNativeMemory, *conversionEntities, *hybridLabels, *conversionIndexes);
        bool nativeFieldsPrepared = false;
        if (nativeFieldWork) {
            utils::OwnedAllocationScope pendingScope(pendingNativeMemory);
            SlottedSlotReference groupReference; utils::OwnedHeapReservation boundary(pendingNativeMemory);
            if (!topologyGroup(pendingNativeOwnerCount ? pendingNativeOwners[0] : !hybridTopology->field_changes_.empty() ?
                hybridTopology->field_changes_.front().owner : hybridTopology->owner_deletions_.front(), [&](size_t bytes) { boundary.set(bytes); }, &groupReference))
                throw FormatException(FormatError::FERR_INVALID_STATE, "native pending owner lost its source group");
            boundary.set(0);
            const auto groupPage = locations_.findPage(current, groupReference.page_id);
            const auto fieldPool = catalog_.find(current, groupPage.segment_id);
            hybridPlacement.reset(new HybridPropertyPoolPlacement(*this, batch, writer, *topologySource,
                PhysicalSlotId(1), PhysicalSlotId(UINT64_MAX), pendingNativeMemory, [] {}));
            NativeHybridPropertyColumnWrite fields{current.database_id, current.generation,
                current.hybrid_property_descriptor_root, hybridTopology->field_changes_, pendingNativeMemory, PhysicalSlotId(1), PhysicalSlotId(UINT64_MAX),
                fieldPool.owner_id, nullptr, true, hybridTopology->owner_deletions_, std::span(pendingNativeOwners).first(pendingNativeOwnerCount)};
            nativeFieldsPrepared = prepareHybridPropertyColumns(batch, writer, *topologySource, fields, fields.replacements, *conversionPages,
                *conversionIndexes, *conversionLargeValues, *conversionEntities, *hybridLabels, *hybridPlacement, nullptr, hybridTopology, jointNativeContent ? largeValues : std::span<const NativeGraphLargeValueMutation>{});
            if (!nativeFieldsPrepared && pendingNativeOwnerCount)
                throw FormatException(FormatError::FERR_INVALID_STATE, "native pending owner liveness did not prepare a transition");
        }
        if (reclamationEvidence) batch.hybrid_property_publication_ = true;
        if (hybridPropertyRootMutation && hybridPropertyRootMutation->conversion) {
            const auto &conversion = *hybridPropertyRootMutation->conversion;
            const std::function<void()> checkpoint = [&conversion] {
                if (conversion.cancellation) conversion.cancellation->check();
            };
            // Declare the decode admission before all borrowed ROW values:
            // decoded heaps and vector backing die before their credit refunds.
            utils::OwnedAllocationScope allocationScope(conversion.memory);
            std::optional<detail::PropertyDecodeWorkspace> headerWorkspace;
            headerWorkspace.emplace(conversion.memory, &checkpoint);
            auto source = snapshots_.pinCurrentUnderWrite(writer, current);
            if (conversion.direction == NativeHybridPropertyConversionDirection::NHPCD_HYBRID_TO_ROW)
                return publishHybridPropertyRows(batch, writer, source, conversion, *conversionEntities,
                    *conversionPages, *conversionIndexes, *conversionLargeValues, options);
            if (current.hybrid_property_descriptor_root.isValid()) {
                HybridPropertyReadContext existing(io_, *conversionPages, *this, source, conversion.cancellation);
                for (uint64_t offset = 0; offset < conversion.group_count; ++offset)
                    if (existing.groupRecord({conversion.owner_kind, (conversion.group_id + offset - 1U) * 64U + 1U}))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "ROW conversion group is already native");
            }
            std::optional<NativeStorageAllocationCensus> allocationCensus;
            std::optional<NativeStorageAllocationFootprint> sourceCost;
            NativeStorageAllocationCensusOptions censusOptions;
            if (conversion.allocation_admission) {
                censusOptions = conversion.allocation_admission->census;
                censusOptions.cancellation = conversion.cancellation;
                allocationCensus.emplace(io_, *this, conversion.memory);
                try { sourceCost = allocationCensus->inspectCurrentUnderWrite(writer, source, censusOptions); }
                catch (const query::execution::MaterializedByteLimitExceeded &) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                if (!sourceCost || !sourceCost->fullyAttributed()) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                const auto &limits = *conversion.allocation_admission;
                if (sourceCost->totalAllocatedBytes() > limits.maximum_admitted_peak_bytes || sourceCost->file_bytes > limits.maximum_admitted_file_bytes) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_resource_refusal", 1); return current;
                }
            }
            struct SourceOwner { EntityPropertyKey key; size_t fields; };
            auto owners = utils::OwnedVector<SourceOwner>(utils::OwnedPmrAllocator<SourceOwner>(conversion.memory));
            owners.reserve(size_t(conversion.group_count) * 64U);
            size_t memberships = 0;
            const uint64_t firstId = (conversion.group_id - 1U) * 64U + 1U;
            for (uint64_t slot = 0; slot < uint64_t(conversion.group_count) * 64U; ++slot) {
                checkpoint();
                if (slot % 64U == 0) memberships = 0;
                const auto id = firstId + slot;
                if (id > RSS_HYBRID_MAX_ENTITY_ID) break;
                const bool node = conversion.owner_kind == EntityPropertyOwnerKind::EPOK_NODE;
                const auto location = node ? locations_.findNodePageLocationOptional(current, id) :
                    locations_.findEdgePageLocationOptional(current, id);
                if (!location) continue;
                if (node) static_cast<void>(conversionEntities->readNodeAt(source, *location));
                else static_cast<void>(conversionEntities->readEdgeAt(source, *location));
                const EntityPropertyKey key{conversion.owner_kind, id};
                // Admit the fixed work bound before decoding any values from an
                // over-wide source. Count lookup authenticates owner framing.
                const auto count = conversionRoots->readEntityPropertyCount(source, key).value_or(0);
                if (count > HybridPropertyGroupConversion::MAX_MEMBERSHIPS - memberships)
                    throw std::length_error("native hybrid conversion exceeds its membership bound");
                memberships += count;
                owners.push_back({key, count});
            }
            if (owners.empty())
                throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid conversion group has no live owners");
            // Header-only temporaries have died. Decode/classify/encode ROW
            // values in a separate lifetime: field images and immutable value
            // proposals own their bytes and borrow no input after preparation.
            // Releasing source values before index/physical planning keeps the
            // documented maximal group inside the same 1 MiB account.
            headerWorkspace.reset();
            auto largeFields = utils::OwnedVector<HybridPropertyLargeFieldPreparation>(
                utils::OwnedPmrAllocator<HybridPropertyLargeFieldPreparation>(conversion.memory));
            std::shared_ptr<HybridPropertyConversionPreparation> fields;
            for (uint64_t groupOffset = 0; groupOffset < conversion.group_count; ++groupOffset) {
            const auto groupId = conversion.group_id + groupOffset;
            auto preparedGroup = [&] {
                detail::PropertyDecodeWorkspace sourceWorkspace(conversion.memory, &checkpoint);
                // Keep decoded values admitted for the complete source lifetime,
                // but release per-owner LargeValue wire buffers after decoding.
                // Without explicit slot admission those transient bodies join
                // the retained decoder ledger once per owner.
                size_t retainedSlotBytes = 0;
                utils::OwnedHeapReservation sourceSlotCredit(conversion.memory);
                const SelectedPropertyReadAdmission admitSourceSlots = [&](size_t bytes) {
                    constexpr size_t frontierBytes = 4096U;
                    if (bytes > (std::numeric_limits<size_t>::max)() - retainedSlotBytes - frontierBytes)
                        throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "native source slot admission overflows");
                    sourceSlotCredit.set(retainedSlotBytes + bytes + frontierBytes);
                };
                // Complete the header-only bound check before materializing any
                // source values. Both passes remain within the same fenced pin.
                auto roots = utils::OwnedVector<PropertyCollectionRoot>(
                    utils::OwnedPmrAllocator<PropertyCollectionRoot>(conversion.memory));
                roots.reserve(64U);
                auto cursor = conversionRoots->openScanCursor(source, conversion.cancellation);
                for (const auto &owner : owners) {
                    if ((owner.key.entity_id - 1U) / 64U + 1U != groupId) continue;
                    checkpoint();
                    auto root = cursor.readEntityOptional(source, owner.key, nullptr, &admitSourceSlots).value_or(PropertyCollectionRoot{
                        owner.key.entity_id, conversion.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId, {}});
                    if (root.entries.size() != owner.fields)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid source count disagrees with its ROW payload");
                    retainedSlotBytes += utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(root.entries.capacity() * sizeof(PropertyCollectionRootEntry),
                            alignof(PropertyCollectionRootEntry));
                    roots.push_back(std::move(root));
                    sourceSlotCredit.set(retainedSlotBytes);
                    debug::PerfTrace::addValue("native_graph.hybrid.source_owners_decoded", 1);
                }
                HybridPropertyGroupConversion plan(conversion.memory, roots, conversion.owner_kind,
                    groupId, batch.generation(), RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, checkpoint);
                if (conversion.group_count > 1U && !conversion.cold_row_remainder && (plan.columns().empty() ||
                    std::ranges::any_of(plan.members(), [](const auto &member) { return member.source != HybridPropertySource::HPS_COLUMN; })))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native hybrid cohort requires compact pure-column groups");
                debug::PerfTrace::addValue("native_graph.hybrid.source_classification_completed", 1);
                // Directory sources retain their immutable final locator records,
                // rather than borrow the ROW/planner or keep a second row copy.
                const auto retainProposal = [&](HybridPropertyLargeFieldPreparation proposal) -> const HybridPropertyLargeFieldPreparation & {
                    const HybridPropertyLargeFieldPreparation *selected = &proposal;
                    // Content created earlier in this same private batch is absent
                    // from the published index. Compare the complete carrier bytes
                    // before allocating another ID; hash equality alone is not enough.
                    // Shared existing values also need one retained proposal
                    // per complete payload, rather than one copy per owner.
                    for (const auto &previous : largeFields) {
                        checkpoint();
                        if (std::ranges::equal(previous.payload(), proposal.payload())) {
                            selected = &previous; break;
                        }
                    }
                    if (selected == &proposal) {
                        // Reserve the holder before consuming a batch-local ID.
                        if (largeFields.size() == largeFields.capacity())
                            largeFields.reserve(largeFields.empty() ? 1U : largeFields.size() * 2U);
                        if (!proposal.reused() && batch.allocateLargeValueId() != proposal.reference().value_id)
                            throw FormatException(FormatError::FERR_INVALID_STATE, "native field value lost its batch ID sequence");
                        largeFields.push_back(std::move(proposal)); selected = &largeFields.back();
                    }
                    return *selected;
                };
                const auto resolveLarge = [&](EntityPropertyKey owner, uint32_t token, const PropertyValue &value) {
                    checkpoint();
                    auto proposed = HybridPropertyLargeFieldPreparation::prepare(*this, source, *conversionLargeValues,
                        owner, token, value, LargeValueId(batch.candidate().next_large_value_id), conversion.memory,
                        {}, LargeValueExtentClass::LVEC_32K, RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, conversion.cancellation);
                    debug::PerfTrace::addValue("native_graph.hybrid.large_field_proposals_prepared", 1);
                    const auto *selected = &retainProposal(std::move(proposed));
                    const size_t encodedBytes = RSS_LARGE_VALUE_REF_HEADER_BYTES + selected->reference().media_type.size();
                    utils::OwnedHeapReservation encodingCredit(conversion.memory,
                        utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(encodedBytes, alignof(std::byte)));
                    const auto encoded = LargeValueRefCodec::encode(selected->reference());
                    utils::OwnedHeapReservation decodedCredit(conversion.memory);
                    const auto admitReference = [&](size_t bytes) { decodedCredit.set(bytes); };
                    return HybridPropertyExtensionView::encodeReference(owner, token, batch.generation(), encoded,
                        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, *conversion.memory, std::ref(admitReference));
                };
                const auto resolveRoot = [&](EntityPropertyKey owner, std::span<const BorrowedPropertyCollectionRootEntry> entries) {
                    checkpoint();
                    const auto &proposal = retainProposal(HybridPropertyLargeFieldPreparation::prepareRootBorrowed(*this, source,
                        *conversionLargeValues, owner, entries, LargeValueId(batch.candidate().next_large_value_id),
                        conversion.memory, conversion.cancellation));
                    debug::PerfTrace::addValue("native_graph.hybrid.large_root_proposals_prepared", 1);
                    // The existing owner-keyed codec has two bounded standard
                    // buffers; charge their full supported media/proxy overlap.
                    constexpr size_t maximumReference = RSS_LARGE_VALUE_REF_HEADER_BYTES + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES;
                    utils::OwnedHeapReservation credit(conversion.memory,
                        2U * utils::detail::standardContainerProxyBytes() +
                        utils::detail::standardAllocatorRequestBytes(maximumReference, alignof(std::byte)) +
                        utils::detail::standardAllocatorRequestBytes(RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES +
                            maximumReference, alignof(std::byte)));
                    const auto type = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
                    const auto encoded = EntityPropertyLargeValueReferenceCodec::encode({owner.entity_id, type, proposal.reference()});
                    return std::pmr::vector<std::byte>(encoded.begin(), encoded.end(), conversion.memory.get());
                };
                const HybridPropertyConversionPreparation::LargeFieldResolver largeResolver = std::ref(resolveLarge);
                const HybridPropertyConversionPreparation::WholeRootResolver rootResolver = std::ref(resolveRoot);
                return utils::shareOwned(std::unique_ptr<HybridPropertyConversionPreparation>(
                    new HybridPropertyConversionPreparation(batch, plan, checkpoint, largeResolver,
                        conversion.compact_column_memberships, conversion.cold_row_remainder, rootResolver)));
            }();
            if (!fields) fields = std::move(preparedGroup);
            else {
                const auto append = [](auto &target, auto &part) {
                    target.reserve(target.size() + part.size());
                    target.insert(target.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
                };
                append(fields->pages_, preparedGroup->pages_);
                append(fields->owners_, preparedGroup->owners_);
                append(fields->columns_, preparedGroup->columns_);
                append(fields->extensions_, preparedGroup->extensions_);
                append(fields->groups_, preparedGroup->groups_);
            }
            }
            const auto sourceFor = [fields, memory = conversion.memory, checkpoint]<class Map>(
                std::span<const typename Map::Entry> entries) -> OrderedBTreeEntrySource {
                return [fields, memory, entries, checkpoint]() -> OrderedBTreeEntryCursor {
                    utils::OwnedAllocationScope sourceScope(memory);
                    return [fields, memory, entries, checkpoint, position = size_t{0}]() mutable -> std::optional<BTreePageEntry> {
                        utils::OwnedAllocationScope cursorScope(memory);
                        checkpoint();
                        if (position == entries.size()) return {};
                        const auto &entry = entries[position++];
                        const auto record = Map::encode(entry.key, entry.value);
                        BTreePageEntry result;
                        result.key.assign(record.begin(), record.end());
                        return result;
                    };
                };
            };
            using PreparedIndexImage = HybridPropertyContentIndexPreparation::Image;
            auto indexImages = utils::makeSharedOwned<utils::OwnedVector<PreparedIndexImage>>(
                utils::OwnedPmrAllocator<PreparedIndexImage>(conversion.memory));
            const auto collectIndexImages = [indexImages, checkpoint](const CatalogEntry &pool,
                std::span<BTreePage> images, std::span<PageLocation> locations, LogSequenceNumber lsn) {
                if (images.size() != locations.size() || images.empty() || pool.committed_lsn != lsn)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "private hybrid index images lost their placement");
                for (size_t i = 0; i < images.size(); ++i) {
                    checkpoint();
                    const auto header = images[i].header();
                    if (header.index_id != pool.owner_id || header.page_id != locations[i].page_id ||
                        header.generation != pool.generation || locations[i].segment_id != pool.segment_id ||
                        locations[i].page_index != 0 || images[i].slottedPage().ownerCommittedLsn() != lsn)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "private hybrid index image changed its assigned identity");
                    locations[i].page_index = uint16_t(i + RSS_PAGE_POOL_FIRST_DATA_FRAME);
                    locations[i].free_bytes = images[i].slottedPage().stats().contiguous_free_bytes;
                    indexImages->push_back({pool, locations[i], std::move(images[i])});
                }
            };
            std::array<std::optional<BTreeInitialTreeMutationPlan>, 4> directoryPlans;
            const size_t namespaceBase = conversion.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
            // Select real, disjoint allocator-free destinations under the same
            // writer, but do not write or mark any pool reachable before complete
            // cost/lifecycle admission. This local cursor is monotonic across
            // field and Index pools; it cannot reuse another private destination.
            auto pools = utils::OwnedVector<CatalogEntry>(utils::OwnedPmrAllocator<CatalogEntry>(conversion.memory));
            HybridPropertyPoolPlacement placement(*this, batch, writer, source, conversion.first_slot,
                conversion.last_slot, conversion.memory, checkpoint);
            // Two directly neighbouring groups provide a bounded cold-open
            // hint. The descriptor/map/body and actual Catalog route remain
            // authoritative; no database-wide pool search is needed.
            if (current.hybrid_property_descriptor_root.isValid()) {
                const auto prior = pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor;
                if (prior.roots[namespaceBase + 3U].page_id.isValid()) {
                    const auto binding = HybridPropertyDirectoryCodec::bindRoot(prior, source,
                        static_cast<HybridPropertyRootOrdinal>(namespaceBase + 3U));
                    HybridPropertyGroupDirectoryMap groups(binding.root.index_id, *conversionIndexes, *this, binding);
                    HybridPropertyReadContext reader(io_, *conversionPages, *this, source);
                    utils::OwnedHeapReservation boundary(conversion.memory);
                    for (const auto neighbor : {conversion.group_id > 1U ? conversion.group_id - 1U : conversion.group_id,
                        conversion.group_id < RSS_HYBRID_MAX_GROUP_ID ? conversion.group_id + 1U : conversion.group_id}) {
                        if (neighbor == conversion.group_id) continue;
                        const auto locator = groups.find(source, HybridPropertyDirectoryCodec::groupKey(neighbor),
                            [&](size_t bytes) { boundary.set(bytes); }, conversion.cancellation);
                        boundary.set(0); if (!locator) continue;
                        const auto neighborLive = reader.ownerLiveness(
                            {conversion.owner_kind, (neighbor - 1U) * 64U + 1U},
                            [&](size_t bytes) { boundary.set(bytes); });
                        hybrid_property_detail::require(neighborLive.has_value(),
                            "native neighbor pool has no authenticated group");
                        boundary.set(0);
                        const auto slot = HybridPropertyDirectoryCodec::decodeLocator(*locator);
                        placement.candidate(locations_.findPage(current, slot.page_id).segment_id,
                            SegmentKind::SGK_PROPERTY_PAGE_POOL, conversion.field_pool_owner_id, true);
                    }
                }
            }
            const auto allocatePool = [&](SegmentKind kind, uint64_t owner, size_t count = RSS_PAGE_POOL_DATA_FRAME_COUNT, bool native = false) {
                const bool forceFresh = kind == SegmentKind::SGK_INDEX_PAGE_POOL && !locations_.findIndexRootOptional(current, owner);
                const auto pool = placement.allocate(kind, owner, count, native, forceFresh);
                if (std::none_of(pools.begin(), pools.end(), [&](const auto &p) { return p.segment_id == pool.segment_id; })) pools.push_back(pool);
                return pool;
            };
            struct PreparedFieldImage { CatalogEntry pool; PageLocation location; SlottedPage page; };
            auto fieldImages = utils::OwnedVector<PreparedFieldImage>(utils::OwnedPmrAllocator<PreparedFieldImage>(conversion.memory));
            fieldImages.reserve(fields->pages_.size());
            CatalogEntry fieldPool; size_t fieldPoolUsed = 0;
            for (size_t i = 0; i < fields->pages_.size(); ++i) {
                checkpoint();
                const auto selected = allocatePool(SegmentKind::SGK_PROPERTY_PAGE_POOL, conversion.field_pool_owner_id, 1U, true);
                if (selected.segment_id != fieldPool.segment_id) fieldPoolUsed = 0;
                fieldPool = selected;
                auto &image = fields->pages_[i];
                PageOwnerHeader owner;
                owner.page_id = image.pageId(); owner.owner_id = fieldPool.owner_id;
                owner.kind = PageOwnerKind::POK_HYBRID_PROPERTY; owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
                owner.generation = fieldPool.generation; owner.committed_lsn = batch.checkpointLsn();
                utils::OwnedHeapReservation referenceCredit(conversion.memory);
                const auto admitReference = [&](size_t retained) { referenceCredit.set(retained); };
                auto bound = std::move(image).bindPrivateOwner(owner, fieldPool, batch.checkpointLsn(), std::ref(admitReference));
                const PageLocation location{owner.page_id, fieldPool.segment_id,
                    uint16_t(fieldPoolUsed++ + RSS_PAGE_POOL_FIRST_DATA_FRAME),
                    fieldPool.generation, bound.stats().contiguous_free_bytes};
                fieldImages.push_back({fieldPool, location, std::move(bound)});
            }
            HybridPropertyDescriptor descriptor = current.hybrid_property_descriptor_root.isValid() ?
                pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor : HybridPropertyDescriptor{};
            descriptor.database_id = current.database_id;
            std::array<std::optional<IndexRootLocation>, 4> directoryRoots;
            utils::OwnedVector<uint64_t> supersededDirectories{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
            utils::OwnedVector<uint64_t> erasedDirectories{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
            const auto prepareDirectory = [&]<class Map>(size_t ordinal, std::span<const typename Map::Entry> entries) {
                checkpoint();
                const auto index = hybridPropertyIndexId(static_cast<HybridPropertyRootOrdinal>(namespaceBase + ordinal));
                const auto existing = findIndexRootOptional(source, index);
                if (existing) {
                    if (!descriptor.roots[namespaceBase + ordinal].page_id.isValid())
                        throw FormatException(FormatError::FERR_INVALID_STATE,
                            "ROW conversion found an existing hybrid directory without its descriptor binding");
                    const auto binding = HybridPropertyDirectoryCodec::bindRoot(descriptor, source,
                        static_cast<HybridPropertyRootOrdinal>(namespaceBase + ordinal));
                    if (entries.empty()) return;
                    HybridPropertyDirectoryPreparation prepared(batch, source, *this, *conversionIndexes, binding,
                        entries, std::span<const typename Map::Key>{}, conversion.memory, conversion.cancellation,
                        allocatePool, checkpoint, std::type_identity<Map>{});
                    if (!prepared.root_)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "new native group lost a shared directory");
                    directoryRoots[ordinal] = *prepared.root_;
                    descriptor.roots[namespaceBase + ordinal] = {prepared.root_->root_page_id, prepared.root_->root_generation};
                    for (auto &image : prepared.images_) indexImages->push_back({image.pool, image.location, std::move(image.page)});
                    supersededDirectories.insert(supersededDirectories.end(), prepared.superseded_.begin(), prepared.superseded_.end());
                    erasedDirectories.insert(erasedDirectories.end(), prepared.erased_.begin(), prepared.erased_.end());
                    return;
                }
                if (descriptor.roots[namespaceBase + ordinal].page_id.isValid())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native descriptor has a missing Index Root binding");
                if (entries.empty()) return; // absent is canonical, no empty root/pool
                const BulkBTreeBuildOptions options{conversion.first_slot, conversion.last_slot, index,
                    entries.size(), entries.size(), BTreeLeafValueLayout::BLVL_EMPTY};
                directoryPlans[ordinal] = BulkBTreeBuilder::planNativeTreeWithImageSink(*conversionIndexes, *this, source,
                    index, sourceFor.template operator()<Map>(entries), options, collectIndexImages);
                if (directoryPlans[ordinal]->entry_count != entries.size())
                    throw FormatException(FormatError::FERR_INVALID_STATE, "hybrid directory plan lost a final field locator");
            };
            fields->pages_.clear(); fields->pages_.shrink_to_fit();
            prepareDirectory.template operator()<HybridPropertyOwnerMap>(0, fields->owners());
            prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(1, fields->columns());
            prepareDirectory.template operator()<HybridPropertyFieldDirectoryMap>(2, fields->extensions());
            prepareDirectory.template operator()<HybridPropertyGroupDirectoryMap>(3, std::span<const HybridPropertyGroupDirectoryMap::Entry>(fields->groups_));

            for (size_t ordinal = 0; ordinal < directoryPlans.size(); ++ordinal) {
                if (!directoryPlans[ordinal]) continue;
                const auto &directory = *directoryPlans[ordinal];
                // Pre-admit the exact standard-vector assignment backing and
                // five nested empty proxies before constructing the API batch.
                // These temporary assignments die before this credit refunds.
                size_t assignmentBytes = utils::detail::addStandardHeapBytes(utils::detail::standardContainerProxyBytes(),
                    utils::detail::standardAllocatorRequestBytes(directory.new_pool_mutations.size() * sizeof(IndexTreeMutationPoolBatch),
                        alignof(IndexTreeMutationPoolBatch)));
                for (const auto &request : directory.new_pool_mutations) {
                    assignmentBytes = utils::detail::addStandardHeapBytes(assignmentBytes,
                        5U * utils::detail::standardContainerProxyBytes());
                    assignmentBytes = utils::detail::addStandardHeapBytes(assignmentBytes,
                        utils::detail::standardAllocatorRequestBytes(request.appended_page_count * sizeof(PageId), alignof(PageId)));
                    assignmentBytes = utils::detail::addStandardHeapBytes(assignmentBytes,
                        utils::detail::standardAllocatorRequestBytes(request.appended_page_count * sizeof(PageLocation), alignof(PageLocation)));
                }
                utils::OwnedHeapReservation assignmentCredit(conversion.memory, assignmentBytes);
                std::vector<IndexTreeMutationPoolBatch> assignments(directory.new_pool_mutations.size());
                for (size_t i = 0; i < assignments.size(); ++i) {
                    const auto &request = directory.new_pool_mutations[i]; auto &assignment = assignments[i];
                    assignment.pool = allocatePool(SegmentKind::SGK_INDEX_PAGE_POOL, directory.index_id, request.appended_page_count);
                    assignment.replacement_pool = assignment.pool; assignment.newly_reserved = true;
                    assignment.appended_page_ids.reserve(request.appended_page_count);
                    assignment.appended_locations.reserve(request.appended_page_count);
                    if (assignment.appended_page_ids.capacity() != request.appended_page_count ||
                        assignment.appended_locations.capacity() != request.appended_page_count)
                        throw std::logic_error("private hybrid tree assignments exceeded their admitted capacity");
                    for (size_t page = 0; page < request.appended_page_count; ++page) {
                        const auto id = batch.allocateLogicalPageId();
                        assignment.appended_page_ids.push_back(id);
                        assignment.appended_locations.push_back({id, assignment.pool.segment_id, 0, assignment.pool.generation, 0});
                    }
                }
                directory.preparer(assignments, batch.checkpointLsn());
                const auto &root = assignments.at(directory.root_page.pool_ordinal);
                descriptor.roots[namespaceBase + ordinal] = {
                    root.appended_locations.at(directory.root_page.appended_page_ordinal).page_id, root.pool.generation};
                directoryRoots[ordinal] = IndexRootLocation{directory.index_id,
                    root.appended_locations.at(directory.root_page.appended_page_ordinal).page_id, root.pool.generation};
            }
            descriptor.semantic_generation = batch.generation();
            // Validate the final namespace bindings and publication identity,
            // without allocating or writing a descriptor metadata frame yet.
            static_cast<void>(HybridPropertyDescriptorCodec::encode({batch.generation(), batch.checkpointLsn(), descriptor}));
            const HybridPropertyImmutablePreparation immutableImages(batch, *conversionLargeValues,
                largeFields, conversion.memory, allocatePool, checkpoint);
            // These are private preparation counts, not committed work or total
            // conversion cost. Count every whole reserved pool, including its
            // header and unused frames; final metadata/retirement is additional.
            debug::PerfTrace::addValue("native_graph.hybrid.private_large_values", int64_t(largeFields.size()));
            size_t newLargeValues = 0;
            for (const auto &value : largeFields) newLargeValues += !value.reused();
            debug::PerfTrace::addValue("native_graph.hybrid.private_new_large_values", int64_t(newLargeValues));
            debug::PerfTrace::addValue("native_graph.hybrid.private_field_frames", int64_t(fieldImages.size()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_index_frames", int64_t(indexImages->size()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_large_data_frames", int64_t(immutableImages.dataFrameCount()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_large_manifest_frames", int64_t(immutableImages.manifestFrameCount()));
            debug::PerfTrace::addValue("native_graph.hybrid.private_data_pool_bytes", int64_t(std::count_if(placement.reservations_.begin(), placement.reservations_.end(), [](const auto &r) { return r.fresh; }) * RSS_SEGMENT_SIZE));
            checkpoint();
            // Do not publish an empty descriptor and consumed IDs as a false
            // success. Physical placement, final maps and every graph route
            // must replace this guard together with nonempty-root admission.
            if (!conversion.publish_candidate)
                throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                    "native hybrid private field preparation completed; directory plans prepared; physical images and final roots prepared; physical publication is not installed for preparation-only requests");
            const size_t contentImageStart = indexImages->size();
            HybridPropertyContentIndexPreparation content(batch, source, conversion, *this, *conversionIndexes,
                *conversionLargeValues, largeFields, allocatePool, checkpoint);
            auto contentRoot = content.root_;
            auto supersededContentPages = std::move(content.superseded_);
            auto erasedContentPages = std::move(content.erased_);
            supersededContentPages.insert(supersededContentPages.end(), supersededDirectories.begin(), supersededDirectories.end());
            erasedContentPages.insert(erasedContentPages.end(), erasedDirectories.begin(), erasedDirectories.end());
            for (auto &image : content.images_) indexImages->push_back(std::move(image));
            debug::PerfTrace::addValue("native_graph.hybrid.private_content_index_frames",
                int64_t(indexImages->size() - contentImageStart));
            checkpoint();
            // Remove converted ROW slots from CURRENT physical page images as
            // well as their owner directory. Preserve every unrelated owner and
            // stable slot locator; old pins keep their original physical frames.
            // Group authenticated slot references before decoding pages. Fully
            // removed pages retain only their IDs; keeping their 4KiB bodies
            // until publication needlessly consumes the conversion workspace.
            utils::OwnedVector<SlottedSlotReference> rowSlots{utils::OwnedPmrAllocator<SlottedSlotReference>(conversion.memory)};
            rowSlots.reserve(owners.size());
            for (const auto &owner : owners) {
                checkpoint(); const auto location = locations_.findEntityPropertyOptional(current, owner.key);
                if (location) rowSlots.push_back({location->page_id, location->slot_id, location->slot_generation});
            }
            std::sort(rowSlots.begin(), rowSlots.end(), [](const auto &left, const auto &right) {
                return left.page_id < right.page_id || (left.page_id == right.page_id && left.slot_id < right.slot_id);
            });
            utils::OwnedVector<SlottedPage> rowPages{utils::OwnedPmrAllocator<SlottedPage>(conversion.memory)};
            utils::OwnedVector<PageId> changedRowPages{utils::OwnedPmrAllocator<PageId>(conversion.memory)};
            changedRowPages.reserve(rowSlots.size());
            utils::OwnedVector<uint64_t> erasedRowPages{utils::OwnedPmrAllocator<uint64_t>(conversion.memory)};
            erasedRowPages.reserve(rowSlots.size() + erasedContentPages.size());
            erasedRowPages.insert(erasedRowPages.end(), erasedContentPages.begin(), erasedContentPages.end());
            for (size_t begin = 0; begin < rowSlots.size();) {
                checkpoint();
                auto page = conversionPages->read(source, rowSlots[begin].page_id);
                size_t end = begin;
                while (end < rowSlots.size() && rowSlots[end].page_id == page.pageId()) {
                    checkpoint();
                    if (end > begin && rowSlots[end].slot_id == rowSlots[end - 1].slot_id)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native conversion ROW owners alias one source slot");
                    page.erase(rowSlots[end++]);
                }
                changedRowPages.push_back(page.pageId());
                if (!page.stats().live_slot_count) erasedRowPages.push_back(page.pageId().value());
                else rowPages.push_back(std::move(page));
                begin = end;
            }
            // A pool may retire only when every currently routed frame is
            // replaced or removed by this conversion. Inspect at most31 frame
            // owners per source pool; never scan the database Page Directory.
            utils::OwnedVector<CatalogEntry> retiredRowPools{utils::OwnedPmrAllocator<CatalogEntry>(conversion.memory)};
            retiredRowPools.reserve(changedRowPages.size() + supersededContentPages.size());
            for (const auto id : supersededContentPages) {
                const auto location = locations_.findPage(current, PageId(id));
                retiredRowPools.push_back(catalog_.find(current, location.segment_id));
            }
            for (const auto id : changedRowPages) {
                const auto location = locations_.findPage(current, id);
                retiredRowPools.push_back(catalog_.find(current, location.segment_id));
            }
            std::sort(retiredRowPools.begin(), retiredRowPools.end(), [](const auto &a, const auto &b) { return a.segment_id < b.segment_id; });
            retiredRowPools.erase(std::unique(retiredRowPools.begin(), retiredRowPools.end(), [](const auto &a, const auto &b) {
                return a.segment_id == b.segment_id;
            }), retiredRowPools.end());
            SharedPagePoolAllocator sourceRowPools(io_, *this, source);
            for (auto pool = retiredRowPools.begin(); pool != retiredRowPools.end();) {
                checkpoint(); bool detached = pool->state == SegmentState::SGS_SEALED &&
                    (pool->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL ||
                        (pool->kind == SegmentKind::SGK_INDEX_PAGE_POOL && (pool->owner_id == RSS_LARGE_VALUE_CONTENT_INDEX_ID || isHybridPropertyIndexId(pool->owner_id)))) &&
                    allocator_.isAllocated(current, pool->physical_slot);
                if (detached) {
                    const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, *pool);
                    for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
                        checkpoint(); const auto owner = hybrid_property_detail::currentPoolFrameOwner(*pool, header, frame,
                            [&](uint8_t index) { return sourceRowPools.readPageOwner(*pool, index); },
                            [&](PageId id) { return locations_.findPageOptional(current, id); });
                        if (owner) detached = detached && (pool->kind == SegmentKind::SGK_PROPERTY_PAGE_POOL ?
                            std::binary_search(changedRowPages.begin(), changedRowPages.end(), owner->page_id) :
                            std::find(supersededContentPages.begin(), supersededContentPages.end(), owner->page_id.value()) != supersededContentPages.end());
                    }
                }
                if (!detached || placement.destination(pool->segment_id)) { pool = retiredRowPools.erase(pool); continue; }
                batch.reserveDataSlot(pool->physical_slot); pool->state = SegmentState::SGS_RETIRED; ++pool;
            }
            utils::OwnedVector<PreparedFieldImage> rowImages{utils::OwnedPmrAllocator<PreparedFieldImage>(conversion.memory)};
            rowImages.reserve(rowPages.size());
            CatalogEntry rowPool; size_t rowFrames = RSS_PAGE_POOL_DATA_FRAME_COUNT;
            for (auto &page : rowPages) {
                checkpoint();
                if (rowFrames == RSS_PAGE_POOL_DATA_FRAME_COUNT || rowPool.owner_id != page.ownerId()) {
                    rowPool = allocatePool(SegmentKind::SGK_PROPERTY_PAGE_POOL, page.ownerId()); rowFrames = 0;
                }
                const PageOwnerHeader owner{page.pageId(), page.ownerId(), PageOwnerKind::POK_PROPERTY, 0,
                    RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, rowPool.generation, batch.checkpointLsn()};
                page.rebindOwner(owner);
                const PageLocation location{page.pageId(), rowPool.segment_id,
                    uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + rowFrames++), rowPool.generation, page.stats().contiguous_free_bytes};
                rowImages.push_back({rowPool, location, std::move(page)});
            }
            auto published = utils::OwnedVector<PageLocation>(utils::OwnedPmrAllocator<PageLocation>(conversion.memory));
            published.reserve(fieldImages.size() + indexImages->size() + rowImages.size() + immutableImages.frames_.size());
            for (const auto &pool : pools) {
                checkpoint();
                const auto inPool = [&](const auto &image) { return image.pool.segment_id == pool.segment_id; };
                const auto frameCount = size_t(std::count_if(fieldImages.begin(), fieldImages.end(), inPool)) +
                    size_t(std::count_if(indexImages->begin(), indexImages->end(), inPool)) +
                    size_t(std::count_if(rowImages.begin(), rowImages.end(), inPool)) +
                    size_t(std::count_if(immutableImages.frames_.begin(), immutableImages.frames_.end(), inPool));
                const bool hybridPool = std::any_of(fieldImages.begin(), fieldImages.end(), inPool);
                utils::OwnedHeapReservation payloadCredit(conversion.memory, frameCount *
                    (utils::detail::standardAllocatorRequestBytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, alignof(std::byte)) +
                        3U * utils::detail::standardContainerProxyBytes()));
                utils::OwnedVector<SharedPagePoolPreparedAppend> appends{
                    utils::OwnedPmrAllocator<SharedPagePoolPreparedAppend>(conversion.memory)};
                utils::OwnedVector<PageLocation> locations{utils::OwnedPmrAllocator<PageLocation>(conversion.memory)};
                appends.reserve(frameCount); locations.reserve(frameCount);
                const auto append = [&](const auto &prepared, PageOwnerKind kind, const SlottedPage &page) {
                    checkpoint();
                    if (prepared.pool.segment_id != pool.segment_id) return;
                    const auto bytes = page.serializeOwned(conversion.memory);
                    PageOwnerHeader owner{prepared.location.page_id, pool.owner_id, kind, 0,
                        RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, pool.generation, batch.checkpointLsn()};
                    SharedPagePoolPreparedAppend preparedAppend;
                    preparedAppend.owner = owner; preparedAppend.free_bytes = prepared.location.free_bytes;
                    preparedAppend.payload.assign(bytes.begin(), bytes.end());
                    appends.push_back(std::move(preparedAppend));
                    auto location = prepared.location; location.page_index = 0;
                    locations.push_back(location);
                };
                if (pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL) {
                    for (const auto &prepared : fieldImages) append(prepared, PageOwnerKind::POK_HYBRID_PROPERTY, prepared.page);
                    for (const auto &prepared : rowImages) append(prepared, PageOwnerKind::POK_PROPERTY, prepared.page);
                } else if (pool.kind == SegmentKind::SGK_INDEX_PAGE_POOL)
                    for (const auto &prepared : *indexImages) append(prepared, PageOwnerKind::POK_INDEX, prepared.page.slottedPage());
                else if (pool.kind == SegmentKind::SGK_LARGE_VALUE_DATA_POOL || pool.kind == SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL) {
                    for (const auto &frame : immutableImages.frames_) if (inPool(frame)) {
                        SharedPagePoolPreparedAppend prepared; prepared.owner = frame.owner;
                        prepared.payload.assign(frame.payload.begin(), frame.payload.end()); appends.push_back(std::move(prepared));
                        auto location = frame.location; location.page_index = 0; locations.push_back(location);
                    }
                } else throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid candidate has an unsupported pool");
                // A declined optional admission can leave private appends in
                // a Catalog-visible ROW content-index pool. Its next writer
                // must recover these even though no hybrid root was published.
                if (conversion.allocation_admission) native_pool_recovery_required_ = true;
                placement.write(pool, appends, locations, hybridPool);
                for (const auto &location : locations) published.push_back(location);
            }
            // Physical frames precede every reachable metadata root. All
            // allocator, Catalog, Page Directory, field directories and ROW
            // authority changes belong to this ONE final A/B publication.
            for (const auto &reservation : placement.reservations_) if (reservation.fresh) {
                const auto &pool = reservation.pool; allocator_.prepareSetAllocated(batch, pool.physical_slot, true);
                catalog_.prepareUpsert(batch, std::span(&pool, 1));
            }
            std::sort(published.begin(), published.end(), [](const auto &left, const auto &right) {
                return left.page_id < right.page_id;
            });
            std::sort(erasedRowPages.begin(), erasedRowPages.end());
            locations_.prepareReplacePages(batch, published, erasedRowPages);
            utils::OwnedVector<RetireQueueEntry> rowRetirements{utils::OwnedPmrAllocator<RetireQueueEntry>(conversion.memory)};
            rowRetirements.reserve(retiredRowPools.size());
            for (const auto &pool : retiredRowPools)
                rowRetirements.push_back({pool.physical_slot, pool.segment_id, batch.generation(), RetireQueueDisposition::RQD_RETIRED_SEGMENT});
            std::sort(rowRetirements.begin(), rowRetirements.end(), [](const auto &a, const auto &b) { return a.physical_slot < b.physical_slot; });
            if (!retiredRowPools.empty()) {
                catalog_.prepareUpsert(batch, retiredRowPools); retirement_.prepareQueueInsertions(batch, rowRetirements);
            }
            utils::OwnedVector<IndexRootLocation> installedRoots{utils::OwnedPmrAllocator<IndexRootLocation>(conversion.memory)};
            installedRoots.reserve(5);
            if (contentRoot) installedRoots.push_back(*contentRoot);
            for (const auto &root : directoryRoots) if (root) installedRoots.push_back(*root);
            std::sort(installedRoots.begin(), installedRoots.end(), [](const auto &a, const auto &b) { return a.index_id < b.index_id; });
            locations_.prepareReplaceIndexRoots(batch, installedRoots, {});
            if (!immutableImages.manifests_.empty()) locations_.prepareReplaceLargeValueManifests(batch, immutableImages.manifests_, {});
            utils::OwnedVector<EntityPropertyKey> erasedRows{utils::OwnedPmrAllocator<EntityPropertyKey>(conversion.memory)};
            erasedRows.reserve(owners.size());
            for (const auto &owner : owners)
                if (locations_.findEntityPropertyOptional(current, owner.key)) erasedRows.push_back(owner.key);
            if (!erasedRows.empty()) locations_.prepareReplaceEntityProperties(batch, {}, erasedRows);
            pages_.prepareHybridPropertyDescriptorImpl(batch, descriptor, true);
            if (fields->compact_) batch.enableHybridColumnMembership();
            batch.hybrid_property_publication_ = true;
            checkpoint();
            if (conversion.allocation_admission) {
                // Prepare final metadata/routes once. The private future graph
                // remains unreachable until the following A/B decision.
                prepareBatchPublication(batch); checkpoint();
                // Drain dirty private cache pages before the stable-epoch
                // census; read eviction must not become a hidden writeback.
                io_->flushStream(); checkpoint();
                std::optional<NativeStorageAllocationFootprint> prepared;
                try { prepared = allocationCensus->inspectPreparedCandidate(writer, batch, censusOptions); }
                catch (const query::execution::MaterializedByteLimitExceeded &) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                if (!prepared || !prepared->fullyAttributed()) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_unknown", 1); return current;
                }
                const auto row = sourceCost->steadyAllocatedBytes(), candidate = prepared->steadyAllocatedBytes();
                const auto peak = std::max(sourceCost->totalAllocatedBytes(), prepared->totalAllocatedBytes());
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_row_bytes", int64_t(row));
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_candidate_bytes", int64_t(candidate));
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_peak_bytes", int64_t(peak));
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_costs_prepared", 1); checkpoint();
                const auto &limits = *conversion.allocation_admission;
                if (peak > limits.maximum_admitted_peak_bytes || prepared->file_bytes > limits.maximum_admitted_file_bytes) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_resource_refusal", 1); return current;
                }
                const uint64_t threshold = row / 10U * 9U + row % 10U * 9U / 10U;
                if (candidate > threshold) {
                    debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_insufficient_saving", 1); return current;
                }
                debug::PerfTrace::addValue("native_graph.hybrid.allocation_admission_passed", 1);
            }
            publishBatch(batch, writer, options);
            placement.remember();
            return batch.candidate();
        }
		if (derivedState.external_private_tail_reservation.has_value()) {
			// Keep the source outside every metadata allocator decision, but do not
			// publish it as allocated: it remains unreachable on both outcomes.
			batch.reserveDataSlotRange(*derivedState.external_private_tail_reservation);
		}
		for (const EntityPageLocation &location: insertedNodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: insertedEdges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		// Reserve every private or public destination in this writer-owned set.
		// `MetadataWriteBatch` fences the physical choice for this generation even
		// though unpublished private scratch is intentionally absent from Catalog.
		std::set<uint64_t> reservedNewPoolSlots;
		// Allocation state does not change while this writer fence is held. Keep a
		// per-range cursor so a compact scratch plan does not restart an allocator
		// radix walk from its first slot for every candidate segment.
		std::map<std::pair<uint64_t, uint64_t>, std::optional<uint64_t>> nextFreeSlotByRange;
		const auto findFirstFreeNewPoolSlot = [this, &batch, &current, &derivedState, &reservedNewPoolSlots,
																		  &nextFreeSlotByRange](
																 PhysicalSlotId first, PhysicalSlotId last) {
			const auto cursor = nextFreeSlotByRange.try_emplace({first.value(), last.value()}, first.value()).first;
			if (!cursor->second.has_value()) {
				return std::optional<PhysicalSlotId>{};
			}
			PhysicalSlotId candidate(*cursor->second);
			while (candidate <= last) {
				const std::optional<PhysicalSlotId> found = allocator_.findFirstFree(current, candidate, last);
				if (!found.has_value()) {
					cursor->second.reset();
					return std::optional<PhysicalSlotId>{};
				}
				const auto advanceCursor = [&cursor, found, last] {
					if (*found == last || found->value() == std::numeric_limits<uint64_t>::max()) {
						cursor->second.reset();
					} else {
						cursor->second = found->value() + 1U;
					}
				};
				const bool isPrivateSourceSlot = derivedState.external_private_tail_reservation.has_value() &&
																 derivedState.external_private_tail_reservation->contains(*found);
				if (!isPrivateSourceSlot && !batch.isDataSlotReserved(*found) && !batch.isMetadataPoolSlot(*found) &&
					reservedNewPoolSlots.insert(found->value()).second) {
					advanceCursor();
					return found;
				}
				advanceCursor();
				if (!cursor->second.has_value()) {
					return std::optional<PhysicalSlotId>{};
				}
				candidate = PhysicalSlotId(*cursor->second);
			}
			return std::optional<PhysicalSlotId>{};
		};
		const auto reserveDistinctNewPoolSlot = [&findFirstFreeNewPoolSlot](PhysicalSlotId first, PhysicalSlotId last,
																						 const char *message) {
			const std::optional<PhysicalSlotId> found = findFirstFreeNewPoolSlot(first, last);
			if (!found.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE, message);
			}
			return *found;
		};
		std::optional<WriterScopedCompactPrivateScratch> compactPrivateScratch;
		if (derivedState.private_segment_plan.has_value()) {
			const NativeGraphPrivateSegmentPlan &plan = *derivedState.private_segment_plan;
			if (plan.segment_count > std::numeric_limits<uint64_t>::max()) {
				throw FormatException(
						FormatError::FERR_ARITHMETIC_OVERFLOW,
						"native graph compact private scratch plan exceeds its durable identity namespace");
			}
			const uint64_t count = static_cast<uint64_t>(plan.segment_count);
			CatalogEntry firstEntry;
			firstEntry.segment_id = batch.allocateLogicalSegmentIdRange(count);
			firstEntry.kind = plan.kind;
			firstEntry.owner_id = plan.owner_id;
			firstEntry.generation = Generation(1U);
			firstEntry.state = SegmentState::SGS_SEALED;
			firstEntry.committed_lsn = checkpointLsn;
			compactPrivateScratch = {firstEntry, {}, plan.first_slot, plan.last_slot, plan.segment_count, plan.header_bytes,
								 plan.header_extension};
		}
		auto privateScratchState = std::make_shared<WriterScopedPrivateScratchState>();
		privateScratchState->superblock = current;
		std::shared_ptr<NativeGraphPrivateSegmentSet> privateSegmentSet;
		if (compactPrivateScratch.has_value()) {
			privateScratchState->compact_candidate = *compactPrivateScratch;
			const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
			privateSegmentSet = std::make_shared<NativeGraphPrivateSegmentSet>(
					compact.first_entry.segment_id, compact.first_entry.generation, compact.count,
					compact.first_entry.owner_id);
		} else if (!derivedState.private_segments.empty()) {
			privateScratchState->candidates.reserve(privateSegmentCount);
			for (const NativeGraphPrivateSegmentRequest &request: derivedState.private_segments) {
				CatalogEntry entry;
				entry.segment_id = batch.allocateLogicalSegmentId();
				entry.physical_slot = reserveDistinctNewPoolSlot(
						request.first_slot, request.last_slot,
						"native graph private segment has no free physical slot in its bounded range");
				entry.kind = request.kind;
				entry.owner_id = request.owner_id;
				entry.generation = Generation(1U);
				entry.state = SegmentState::SGS_SEALED;
				entry.committed_lsn = checkpointLsn;
				if (catalog_.findOptional(current, entry.segment_id).has_value()) { // the writer-held batch allocates a fresh monotonic logical Segment ID.
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph private segment allocated an already-published Segment ID");
				}
				batch.reserveDataSlot(entry.physical_slot);
				privateScratchState->candidates.push_back({entry, request.header_bytes, 0U});
			}
			const CatalogEntry &first = privateScratchState->candidates.front().entry;
			privateSegmentSet = std::make_shared<NativeGraphPrivateSegmentSet>(
					first.segment_id, first.generation, privateScratchState->candidates.size(), first.owner_id);
		}
		const auto privateEntryAt = [privateScratchState](size_t ordinal) {
			if (privateScratchState->compact_candidate.has_value()) {
				const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
				const auto entry = compact.materialized_entries.find(ordinal);
				if (ordinal >= compact.count || entry == compact.materialized_entries.end()) { // compact private scratch materialization emits one entry for every bounded ordinal.
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch ordinal is not materialized");
				}
				return entry->second;
			}
			return privateScratchState->candidates.at(ordinal).entry;
		};
		const auto privateHeaderBytesAt = [privateScratchState](size_t ordinal) {
			if (privateScratchState->compact_candidate.has_value()) {
				const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
				if (ordinal >= compact.count) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph compact private scratch ordinal is out of range");
				}
				return compact.header_bytes;
			}
			return privateScratchState->candidates.at(ordinal).header_bytes;
		};
		// Every reservation is a complete self-authenticating Segment image. Reuse
		// one bounded buffer rather than allocating a 128 KiB image per candidate.
		std::vector<std::byte> privateScratchImage(RSS_SEGMENT_SIZE);
		// Legacy callers already supply one physical request per candidate. Compact
		// streamed plans materialize reservations lazily in candidateWriter below.
		for (size_t ordinal = 0U;
			 !derivedState.private_segment_plan.has_value() && ordinal < privateSegmentCount; ++ordinal) {
			const NativeGraphPrivateSegmentRequest &legacyRequest = derivedState.private_segments.at(ordinal);
			const uint16_t headerBytes = legacyRequest.header_bytes;
			const std::vector<std::byte> &headerExtension = legacyRequest.header_extension;
			const CatalogEntry entry = privateEntryAt(ordinal);
			std::vector<std::byte> payload = privateSegmentImagePreparer
															 ? privateSegmentImagePreparer(*privateSegmentSet, ordinal)
															 : legacyRequest.payload;
			if (payload.empty() || payload.size() > RSS_SEGMENT_SIZE - headerBytes) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph private scratch image preparation returned an invalid bounded payload");
			}
			std::fill(privateScratchImage.begin(), privateScratchImage.end(), std::byte{});
			SegmentEnvelope envelope;
			envelope.header_bytes = headerBytes;
			envelope.kind = entry.kind;
			envelope.state = SegmentState::SGS_SEALED;
			envelope.segment_id = entry.segment_id;
			envelope.owner_id = entry.owner_id;
			envelope.generation = entry.generation;
			envelope.committed_lsn = entry.committed_lsn;
			envelope.payload_bytes = static_cast<uint32_t>(payload.size());
			envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
			const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
			std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), privateScratchImage.begin());
			std::copy(headerExtension.begin(), headerExtension.end(), privateScratchImage.begin() + RSS_ENVELOPE_BYTES);
			std::copy(payload.begin(), payload.end(), privateScratchImage.begin() + headerBytes);
			const uint64_t offset =
					PhysicalSegmentAllocator::checkedSlotOffset(current.data_region_offset, entry.physical_slot);
			io_->writePagesThrough(
					{offset, entry.generation.value()}, privateScratchImage.data(), privateScratchImage.size());
		}
		const auto materializeCompactPrivateEntry =
				[this, privateScratchState, privateSegmentSet, &batch, &reserveDistinctNewPoolSlot,
					 privateSegmentImagePreparer, checkpointLsn](size_t ordinal) {
					if (!privateScratchState->compact_candidate.has_value() || privateSegmentSet == nullptr) {
						return false;
					}
					WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
					if (ordinal >= compact.count) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch ordinal is out of range");
					}
					if (compact.materialized_entries.contains(ordinal)) {
						return false;
					}
					if (!privateSegmentImagePreparer) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch has no reservation image preparer");
					}
					CatalogEntry entry = compact.first_entry;
					entry.segment_id = SegmentId(entry.segment_id.value() + ordinal);
					entry.physical_slot = reserveDistinctNewPoolSlot(
							compact.first_slot, compact.last_slot,
							"native graph compact private scratch plan has no allocator-visible free slot");
					batch.reserveDataSlot(entry.physical_slot);
					const std::vector<std::byte> payload = privateSegmentImagePreparer(*privateSegmentSet, ordinal);
					if (payload.empty() || payload.size() > RSS_SEGMENT_SIZE - compact.header_bytes) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph compact private scratch image preparation returned an invalid bounded payload");
					}
					std::vector<std::byte> image(RSS_SEGMENT_SIZE);
					SegmentEnvelope envelope;
					envelope.header_bytes = compact.header_bytes;
					envelope.kind = entry.kind;
					envelope.state = SegmentState::SGS_SEALED;
					envelope.segment_id = entry.segment_id;
					envelope.owner_id = entry.owner_id;
					envelope.generation = entry.generation;
					envelope.committed_lsn = checkpointLsn;
					envelope.payload_bytes = static_cast<uint32_t>(payload.size());
					envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
					const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
					std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), image.begin());
					std::copy(compact.header_extension.begin(), compact.header_extension.end(),
							  image.begin() + RSS_ENVELOPE_BYTES);
					std::copy(payload.begin(), payload.end(), image.begin() + compact.header_bytes);
					const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
							privateScratchState->superblock.data_region_offset, entry.physical_slot);
					io_->writePagesThrough({offset, entry.generation.value()}, image.data(), image.size());
					compact.materialized_entries.emplace(ordinal, entry);
					return true;
				};
		const NativeGraphPrivateSegmentWriter candidateWriter = [this, privateScratchState, privateSegmentSet,
															 privateEntryAt, privateHeaderBytesAt, materializeCompactPrivateEntry](
																	 const NativeGraphPrivateSegmentMaterialization
																			 &identity,
																	 std::vector<std::byte> payload) {
			debug::ScopedPerfTimer timer("native_graph.streamed.private_scratch_write");
			if (!privateScratchState->active || privateSegmentSet == nullptr || !identity.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch writer escaped its planning scope");
			}
			const size_t ordinal = privateSegmentSet->ordinalOf(identity);
			const bool materializedNow = materializeCompactPrivateEntry(ordinal);
			const CatalogEntry entry = privateEntryAt(ordinal);
			const uint16_t headerBytes = privateHeaderBytesAt(ordinal);
			if (payload.empty() || payload.size() > RSS_SEGMENT_SIZE - headerBytes) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph private scratch writer received an invalid bounded payload");
			}
			const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
					privateScratchState->superblock.data_region_offset, entry.physical_slot);
			if (materializedNow) {
				// This writer just emitted the authenticated reservation under the same
				// writer fence. Avoid a redundant full-segment read before replacing it.
				const WriterScopedCompactPrivateScratch &compact = *privateScratchState->compact_candidate;
				std::vector<std::byte> image(RSS_SEGMENT_SIZE);
				SegmentEnvelope envelope;
				envelope.header_bytes = headerBytes;
				envelope.kind = entry.kind;
				envelope.state = SegmentState::SGS_SEALED;
				envelope.segment_id = entry.segment_id;
				envelope.owner_id = entry.owner_id;
				envelope.generation = entry.generation;
				envelope.committed_lsn = entry.committed_lsn;
				envelope.payload_bytes = static_cast<uint32_t>(payload.size());
				envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
				const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
				std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), image.begin());
				std::copy(compact.header_extension.begin(), compact.header_extension.end(),
						  image.begin() + RSS_ENVELOPE_BYTES);
				std::copy(payload.begin(), payload.end(), image.begin() + headerBytes);
				io_->writePagesThrough({offset, entry.generation.value()}, image.data(), image.size());
				privateScratchState->candidate_payloads_rewritten = true;
				return;
			}
			std::vector<std::byte> currentImage(RSS_SEGMENT_SIZE);
			const size_t bytesRead = io_->readPages({offset, entry.generation.value()}, currentImage.data(),
					currentImage.size(), BufferPoolReadHint::BPRH_SEQUENTIAL);
			if (bytesRead != currentImage.size()) {
				throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
								  "native graph private scratch writer found a truncated candidate segment");
			}
			const SegmentEnvelope previous =
					SegmentEnvelopeCodec::decode(std::span<const std::byte>(currentImage.data(), RSS_ENVELOPE_BYTES));
			const uint8_t invalidPrevious =
					static_cast<uint8_t>(previous.kind != SegmentKind::SGK_BULK_SCRATCH) |
					static_cast<uint8_t>(previous.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(previous.segment_id != entry.segment_id) |
					static_cast<uint8_t>(previous.owner_id != entry.owner_id) |
					static_cast<uint8_t>(previous.generation != entry.generation) |
					static_cast<uint8_t>(previous.committed_lsn != entry.committed_lsn) |
					static_cast<uint8_t>(previous.header_bytes != headerBytes) |
					static_cast<uint8_t>(previous.payload_bytes == 0U) |
					static_cast<uint8_t>(previous.payload_bytes > RSS_SEGMENT_SIZE - headerBytes);
			if (invalidPrevious != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch writer found an invalid candidate identity");
			}
			const std::span<const std::byte> previousPayload(currentImage.data() + headerBytes, previous.payload_bytes);
			if (utils::calculateCrc(previousPayload.data(), previousPayload.size()) != previous.payload_crc ||
				!isPrivateScratchReservation(previousPayload)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch writer may replace only a reserved candidate once");
			}
			// Preserve the fixed header extension and unused bytes exactly; the
			// writer is authorized to replace only the reserved payload.
			std::vector<std::byte> image = std::move(currentImage);
			SegmentEnvelope envelope = previous;
			envelope.payload_bytes = static_cast<uint32_t>(payload.size());
			envelope.payload_crc = utils::calculateCrc(payload.data(), payload.size());
			const std::array<std::byte, RSS_ENVELOPE_BYTES> encodedEnvelope = SegmentEnvelopeCodec::encode(envelope);
			std::copy(encodedEnvelope.begin(), encodedEnvelope.end(), image.begin());
			std::copy(payload.begin(), payload.end(), image.begin() + headerBytes);
			io_->writePagesThrough({offset, entry.generation.value()}, image.data(), image.size());
			privateScratchState->candidate_payloads_rewritten = true;
		};
		const NativeGraphPrivateSegmentReader candidateReader =
			[this, privateScratchState, privateSegmentSet, privateEntryAt, privateHeaderBytesAt,
			 materializeCompactPrivateEntry](
					const NativeGraphPrivateSegmentMaterialization &identity) -> std::vector<std::byte> {
				debug::ScopedPerfTimer timer("native_graph.streamed.private_scratch_read");
			if (!privateScratchState->active) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch reader escaped its writer scope");
			}
			if (!identity.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "native graph private scratch reader received an invalid identity");
			}
			if (privateSegmentSet == nullptr) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph private scratch reader cannot resolve a non-candidate segment");
			}
			CatalogEntry entry;
			uint16_t headerBytes = 0U;
			{
				debug::ScopedPerfTimer resolveTimer("native_graph.streamed.private_scratch_resolve");
				const size_t ordinal = privateSegmentSet->ordinalOf(identity);
				// A reader may be the first consumer of a compact candidate. Materialize
				// its authenticated reservation so the normal reservation validation below
				// remains the fail-closed result for an unwritten payload.
				materializeCompactPrivateEntry(ordinal);
				entry = privateEntryAt(ordinal);
				headerBytes = privateHeaderBytesAt(ordinal);
			}
			std::vector<std::byte> image(RSS_SEGMENT_SIZE);
			const uint64_t offset = PhysicalSegmentAllocator::checkedSlotOffset(
					privateScratchState->superblock.data_region_offset, entry.physical_slot);
			size_t bytesRead = 0U;
			{
				debug::ScopedPerfTimer readTimer("native_graph.streamed.private_scratch_physical_read");
				bytesRead = io_->readPages({offset, entry.generation.value()}, image.data(), image.size(),
						BufferPoolReadHint::BPRH_SEQUENTIAL);
			}
			if (bytesRead != image.size()) {
				throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL,
								  "native graph private scratch reader found a truncated candidate segment");
			}
			debug::ScopedPerfTimer validationTimer("native_graph.streamed.private_scratch_validate_copy");
			const SegmentEnvelope envelope =
					SegmentEnvelopeCodec::decode(std::span<const std::byte>(image.data(), RSS_ENVELOPE_BYTES));
			const uint8_t invalid =
					static_cast<uint8_t>(envelope.kind != SegmentKind::SGK_BULK_SCRATCH) |
					static_cast<uint8_t>(envelope.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(envelope.segment_id != entry.segment_id) |
					static_cast<uint8_t>(envelope.owner_id != entry.owner_id) |
					static_cast<uint8_t>(envelope.generation != entry.generation) |
					static_cast<uint8_t>(envelope.committed_lsn != entry.committed_lsn) |
					static_cast<uint8_t>(envelope.header_bytes != headerBytes) |
					static_cast<uint8_t>(envelope.payload_bytes == 0U) |
					static_cast<uint8_t>(envelope.payload_bytes > RSS_SEGMENT_SIZE - envelope.header_bytes);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch candidate does not match its assigned identity");
			}
			std::vector<std::byte> payload(image.begin() + envelope.header_bytes,
										   image.begin() + envelope.header_bytes + envelope.payload_bytes);
			if (utils::calculateCrc(payload.data(), payload.size()) != envelope.payload_crc) {
				throw FormatException(FormatError::FERR_CHECKSUM_MISMATCH,
									  "native graph private scratch candidate payload checksum does not match");
			}
			if (isPrivateScratchReservation(payload)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch reader cannot expose an unfilled reservation");
			}
			return payload;
		};
		const NativeGraphPrivateSegmentFence candidateFence = [this, privateScratchState] {
			debug::ScopedPerfTimer timer("native_graph.streamed.private_scratch_fence");
			if (!privateScratchState->active) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph private scratch fence escaped its planning scope");
			}
			io_->sync();
		};
		std::vector<IndexTreeMultiPoolMutationPreparer> deferredIndexPreparers;
		std::vector<NativeGraphDeferredIndexPlan> deferredPlans;
		const NativeGraphDeferredIndexPlanSink deferIndexPlan = [&deferredPlans](NativeGraphDeferredIndexPlan plan) {
			if (!plan.preparer && !plan.images) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "native graph early publication submitted an empty deferred Index plan");
			}
			deferredPlans.push_back(std::move(plan));
		};
