from pathlib import Path
p=Path('/workspace/zyx/include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text();a=s.index('    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlocks(');b=s.index('    // Restricted same-batch preparation',a);c=s[a:b]
c=c.replace('''std::span<const HybridPropertyLargeFieldPreparation> newRoots = {}) {''','''std::span<const HybridPropertyLargeFieldPreparation> newRoots = {},
        std::span<const HybridPropertyColumnDelta *const> demotions = {}) {''',1).replace('''require(!blocks.empty() && blocks.size() <= 64 && hotChanges.size() <= 64,''','''require((!blocks.empty() || !demotions.empty()) && blocks.size() <= 64 && hotChanges.size() <= 64 && demotions.size() <= 64,''',1).replace('''size_t count = hotChanges.size(); uint64_t changed = 0;''','''// Whole-column demotion expands a bounded user write to at most64
        // actual owners. Structural column plans are not additional owners.
        // Existing ordinary cold/hot callers retain their original count limit.
        size_t count = demotions.empty() ? hotChanges.size() : 0; uint64_t changed = 0;''',1)
needle='''        for (const auto &block : blocks)
            result = prepareColdBlockImpl''';assert needle in c
c=c.replace(needle,'''        utils::OwnedVector<uint32_t> removedTokens{utils::OwnedPmrAllocator<uint32_t>(memory)};
        if (!demotions.empty()) {
            const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
            require(demotions.front() && demotions.front()->isValid(), "cold demotion has no authenticated first plan");
            const auto &source = demotions.front()->sourceGroup();
            const auto groupReference = demotions.front()->groupReference();
            require(source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
                context.snapshot_.superblock() == batch.current(), "cold demotion requires its original source group and batch");
            utils::OwnedHeapReservation boundary(memory); const auto admit = [&](size_t bytes) { boundary.set(bytes); };
            require(context.findGroup(demotions.front()->owner(), admit, nullptr, true, true) == source,
                "cold demotion source Entity/ROW authority changed"); boundary.set(0);
            if (!result.group_.group_id) result.group_ = source;
            require(result.group_.owner_kind == source.owner_kind && result.group_.group_id == source.group_id &&
                result.group_.columns == source.columns && result.group_.live == source.live,
                "cold demotion compatible plans change source group identity or liveness");
            result.pages_.reserve(result.pages_.size() + demotions.size() + 1U);
            const auto image = [&](PageId id) -> HybridPropertyPageImage & {
                for (auto &page : result.pages_) if (page.pageId() == id) return page;
                result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, id, batch.generation(), memory));
                return result.pages_.back();
            };
            const auto previousGroup = result.group_;
            auto matrix = image(groupReference.page_id).readColumnMembership(previousGroup.column_membership,
                previousGroup, batch.generation());
            uint64_t requiredOwners = 0; size_t emitted = 0;
            for (const auto *plan : demotions) {
                check(); require(plan && plan->isValid() && plan->action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD &&
                    plan->databaseId() == batch.current().database_id && plan->sourceGeneration() == batch.current().generation &&
                    plan->nextGeneration() == batch.generation() && plan->sourceGroup() == source &&
                    plan->groupReference() == groupReference && !plan->effects().clear_live && !plan->effects().set_live &&
                    plan->effects().columns == -1, "cold demotion has a foreign source or liveness plan");
                const auto token = plan->sourceColumn().identity.token;
                require(matrix.presence(token) == plan->sourceColumn().presence && matrix.columnLocator(token) == plan->columnReference(),
                    "cold demotion column contradicts its source membership");
                require(image(plan->columnReference().page_id).readColumn(plan->columnReference(),
                    plan->sourceColumn().identity, source, batch.generation()) == plan->sourceColumn(),
                    "cold demotion source column bytes changed");
                for (unsigned slot = 0; slot < 64; ++slot)
                    if (plan->demotedCells()[slot].kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING) {
                        require(emitted < 4096U, "cold demotion exceeds4096actual emitted memberships"); ++emitted;
                        requiredOwners |= UINT64_C(1) << slot;
                    }
                removedTokens.push_back(token); matrix.eraseColumn(token);
                image(plan->columnReference().page_id).erase(plan->columnReference());
            }
            require(!(requiredOwners & ~changed), "cold demotion omitted an actual surviving column owner");
            std::sort(removedTokens.begin(), removedTokens.end());
            // Prove every surviving/removed demoted membership against complete
            // target roots, including original same-batch unpublished carriers.
            for (const auto &block : blocks) for (const auto &row : block.rows) {
                check(); const EntityPropertyKey owner{source.owner_kind, (source.group_id - 1U) * 64U + row.owner_slot + 1U};
                const uint32_t type = source.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
                std::array<bool, 64> present{};
                const auto remember = [&](uint32_t token) {
                    const auto found = std::lower_bound(removedTokens.begin(), removedTokens.end(), token);
                    if (found != removedTokens.end() && *found == token) present[size_t(found - removedTokens.begin())] = true;
                };
                if (!row.root.empty() && !row.large_value_reference)
                    EntityPropertyCollectionRootCodec::visitSelectedEncoded(row.root, owner.entity_id, type, removedTokens,
                        [&](size_t ordinal, auto bytes) { present[ordinal] = bool(bytes); });
                else if (!row.root.empty()) {
                    utils::OwnedHeapReservation referenceCredit(memory, row.root.size() + 4U * utils::detail::standardContainerProxyBytes());
                    const auto reference = EntityPropertyLargeValueReferenceCodec::decode(row.root);
                    if (reference.value.value_id.value() >= batch.current().next_large_value_id) {
                        require(admitPreparedColdRoot(batch, row, owner, std::span(matrix.tokens).first(matrix.column_count),
                            newRoots, memory, cancellation), "cold demotion target has no original reserved carrier proposal");
                        const auto found = std::find_if(newRoots.begin(), newRoots.end(), [&](const auto &proposal) {
                            return proposal.reference() == reference.value;
                        });
                        require(found != newRoots.end(), "cold demotion lost its immutable carrier holder");
                        const auto directory = EntityPropertyLargeValuePayloadCodec::decodeDirectory(found->payload(), &admit, cancellation);
                        for (const auto &entry : directory) remember(entry.key_token);
                        boundary.set(0);
                    } else {
                        HybridPropertyColdRowInspection inspected(context.snapshot_, row, owner, batch.current().generation,
                            std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation);
                        for (size_t i = 0; i < inspected.count(); ++i) remember(inspected.token(i));
                    }
                }
                for (const auto *plan : demotions) {
                    const auto ordinal = size_t(std::lower_bound(removedTokens.begin(), removedTokens.end(),
                        plan->sourceColumn().identity.token) - removedTokens.begin());
                    require(present[ordinal] == (plan->demotedCells()[row.owner_slot].kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING),
                        "cold demotion target contradicts an actual final column membership");
                }
            }
            matrix.semantic_generation = batch.generation();
            result.group_.columns = matrix.column_count; result.group_.memberships = matrix.totalMemberships();
            result.group_.semantic_generation = batch.generation();
            image(groupReference.page_id).replaceColumnMembership(previousGroup.column_membership, matrix,
                previousGroup, result.group_, batch.generation());
            image(groupReference.page_id).replaceGroup(groupReference, result.group_, batch.generation());
        }
        for (const auto &block : blocks)
            result = prepareColdBlockImpl''',1)
c=c.replace('''cancellation, std::move(result), newRoots);''','''cancellation, std::move(result), newRoots, removedTokens);''',1)
s=s[:a]+c+s[b:]
a=s.index('    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlockImpl(');b=s.index('\npublic:',a);c=s[a:b]
c=c.replace('''std::span<const HybridPropertyLargeFieldPreparation> newRoots) {''','''std::span<const HybridPropertyLargeFieldPreparation> newRoots,
        std::span<const uint32_t> removedTokens = {}) {''',1)
c=c.replace('''result.group_.columns == source->columns && result.group_.column_membership.page_id == groupReference.page_id,''','''removedTokens.size() <= source->columns && result.group_.columns == source->columns - removedTokens.size() &&
                result.group_.column_membership.page_id == groupReference.page_id,''',1)
c=c.replace('''        require(matrix.tokens == matrixSource.tokens && matrix.locators == matrixSource.locators,''','''        auto expectedHot = matrixSource;
        for (const auto token : removedTokens) expectedHot.eraseColumn(token);
        require(matrix.tokens == expectedHot.tokens && matrix.locators == expectedHot.locators,''',1)
c=c.replace('''const auto inspect = [&](HybridColdRowInput row, Generation generation) {''','''const auto inspect = [&](HybridColdRowInput row, Generation generation, std::span<const uint32_t> hotTokens) {''',1)
c=c.replace('''std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation);''','''hotTokens, memory, values, cancellation);''',1)
c=c.replace('''inspect(*row, block.semantic_generation);''','''inspect(*row, block.semantic_generation, std::span(matrixSource.tokens).first(matrixSource.column_count));''',1)
c=c.replace('''inspect(row, batch.current().generation);''','''inspect(row, batch.current().generation, std::span(matrix.tokens).first(matrix.column_count));''',1)
s=s[:a]+c+s[b:];p.write_text(s)
