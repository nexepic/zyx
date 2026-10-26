	utils::OwnedStdVector<IndexDefinition> RelocatableStorageBackend::readAllIndexDefinitions(const MetadataSnapshotPin &snapshot,
																									size_t maximumDefinitions) const {
		if (maximumDefinitions == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Index definition read requires a positive caller-provided bound");
		}
		const utils::OwnedStdVector<IndexDefinition> canonical =
				system_catalog_->readIndexDefinitions(snapshot, maximumDefinitions);
		for (const IndexDefinition &definition: canonical) {
			if (definition.kind != IndexDefinitionKind::IDK_VECTOR &&
				!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable canonical Index definition has no published physical root");
			}
		}
		// The canonical catalog remains authoritative when the definition set is
		// empty; consulting a second registry here could resurrect dropped state.
		return canonical;
	}

	utils::OwnedStdVector<ConstraintDefinition>
	RelocatableStorageBackend::readAllConstraintDefinitions(const MetadataSnapshotPin &snapshot,
															size_t maximumDefinitions) const {
		if (maximumDefinitions == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable constraint definition read requires a positive bound and initialized catalog");
		}
		return system_catalog_->readConstraintDefinitions(snapshot, maximumDefinitions);
	}

	RelocatableStorageIOStats RelocatableStorageBackend::ioStats() const {
		requireOpen();
		return {data_file_->io()->stats(), data_file_->io()->bufferPoolStats()};
	}

	void RelocatableStorageBackend::resetIoStats() {
		requireOpen();
		data_file_->io()->resetStats();
		data_file_->io()->resetBufferPoolStats();
	}

	std::optional<PropertyValue> RelocatableStorageBackend::findConfiguration(std::string_view name) const {
		requireOpen();
        std::optional<utils::OwnedAllocationScope> privateRead;
        if (!utils::OwnedAllocationScope::active())
            privateRead.emplace(query::execution::detail::ProcessResourceDomains::defaultOwner()->controlGrant(resource_domain_));
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		return system_catalog_->findConfiguration(snapshot, name);
	}

	std::vector<LogicalGraphConfiguration> RelocatableStorageBackend::readConfigurations(size_t maximumRecords) const {
		requireOpen();
        std::optional<utils::OwnedAllocationScope> privateRead;
        if (!utils::OwnedAllocationScope::active())
            privateRead.emplace(query::execution::detail::ProcessResourceDomains::defaultOwner()->controlGrant(resource_domain_));
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::vector<NativeConfigurationRecord> records =
				system_catalog_->readConfigurations(snapshot, maximumRecords);
		std::vector<LogicalGraphConfiguration> result;
		result.reserve(records.size());
		for (const NativeConfigurationRecord &record: records) {
			result.push_back({record.name, record.value});
		}
		return result;
	}

	void RelocatableStorageBackend::setConfiguration(std::string_view name, const PropertyValue &value) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot update durable configuration");
		}
		requireNoIndeterminateWrite();
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession session = system_catalog_->beginWrite(transaction);
		session.setConfiguration(name, value);
		session.finish();
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable durable configuration lost its native publication fence");
		}
	}

	std::vector<LogicalGraphRuntimeStatistic> RelocatableStorageBackend::runtimeStatistics() const {
		const RelocatableStorageIOStats statistics = ioStats();
		const auto asPropertyValue = [](const uint64_t value) {
			return PropertyValue(static_cast<int64_t>(
					std::min<uint64_t>(value, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))));
		};
		return {
				{"io", "read_bytes", asPropertyValue(statistics.data.read_bytes)},
				{"io", "write_bytes", asPropertyValue(statistics.data.write_bytes)},
				{"io", "reserved_bytes", asPropertyValue(statistics.data.reserved_bytes)},
				{"io", "read_calls", asPropertyValue(statistics.data.read_calls)},
				{"io", "write_calls", asPropertyValue(statistics.data.write_calls)},
				{"io", "reserve_calls", asPropertyValue(statistics.data.reserve_calls)},
				{"io", "sync_calls", asPropertyValue(statistics.data.sync_calls)},
				{"io", "data_sync_calls", asPropertyValue(statistics.data.data_sync_calls)},
				{"io", "superblock_sync_calls", asPropertyValue(statistics.data.superblock_sync_calls)},
				{"buffer_pool", "capacity_bytes", asPropertyValue(statistics.buffer_pool.capacity_bytes)},
				{"buffer_pool", "resident_bytes", asPropertyValue(statistics.buffer_pool.resident_bytes)},
				{"buffer_pool", "pinned_bytes", asPropertyValue(statistics.buffer_pool.pinned_bytes)},
				{"buffer_pool", "dirty_bytes", asPropertyValue(statistics.buffer_pool.dirty_bytes)},
				{"buffer_pool", "hits", asPropertyValue(statistics.buffer_pool.hits)},
				{"buffer_pool", "misses", asPropertyValue(statistics.buffer_pool.misses)},
				{"buffer_pool", "evictions", asPropertyValue(statistics.buffer_pool.evictions)},
				{"buffer_pool", "prefetch_requests", asPropertyValue(statistics.buffer_pool.prefetch_requests)},
				{"buffer_pool", "prefetch_hits", asPropertyValue(statistics.buffer_pool.prefetch_hits)},
				{"buffer_pool", "disk_read_bytes", asPropertyValue(statistics.buffer_pool.disk_read_bytes)},
				{"buffer_pool", "disk_write_bytes", asPropertyValue(statistics.buffer_pool.disk_write_bytes)},
		};
	}

	std::optional<LogicalGraphCardinalityStatistics>
	RelocatableStorageBackend::readLogicalGraphCardinalityStatistics() const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::optional<NativeGraphCardinalityStatistics> statistics =
				system_catalog_->readGraphCardinalityStatistics(snapshot);
		if (!statistics.has_value()) {
			return std::nullopt;
		}
		return LogicalGraphCardinalityStatistics{statistics->node_count, statistics->edge_count};
	}

	namespace {
		std::optional<uint64_t> estimateSnapshotLabels(SystemCatalog &catalog,
			const MetadataSnapshotPin &snapshot, uint64_t nodes, std::span<const std::string> labels) {
			uint64_t estimate = nodes;
			for (const auto &label : labels) {
				if (label.empty()) return std::nullopt;
				const auto token = catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
				if (!token) return std::nullopt;
				const auto count = catalog.readTokenCardinality(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *token);
				if (!count || *count > nodes) return std::nullopt;
				estimate = std::min(estimate, *count);
			}
			return estimate;
		}
	}

	std::optional<uint64_t> RelocatableStorageBackend::estimateLogicalNodeCardinality(
			std::span<const std::string> labels) const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const auto statistics = system_catalog_->readGraphCardinalityStatistics(snapshot);
		if (!statistics) return std::nullopt;
		return estimateSnapshotLabels(*system_catalog_, snapshot, statistics->node_count, labels);
	}

	LogicalGraphPlanningStatistics RelocatableStorageBackend::readLogicalPlanningStatistics(
			std::span<const std::vector<std::string>> labels) const {
        return readLogicalPlanningStatistics(labels, {});
    }

    LogicalGraphPlanningStatistics RelocatableStorageBackend::readLogicalPlanningStatistics(
            std::span<const std::vector<std::string>> labels, std::span<const std::string> types) const {
		requireOpen();
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		LogicalGraphPlanningStatistics result;
		result.node_counts.resize(labels.size());
        result.edge_type_counts.resize(types.size());
		const auto statistics = system_catalog_->readGraphCardinalityStatistics(snapshot);
		if (!statistics) return result;
		result.graph = LogicalGraphCardinalityStatistics{statistics->node_count, statistics->edge_count};
        // Planning requests are bounded by the optimizer; authenticate optional
        // type counts in this same pin, never by opening a graph read snapshot.
        // Duplicate names share a lookup; malformed inputs remain unavailable.
        std::map<std::string, std::optional<uint64_t>> typeCounts;
        for (size_t i = 0; i < types.size(); ++i) {
            if (types[i].empty()) continue;
            auto [entry, inserted] = typeCounts.try_emplace(types[i], std::nullopt);
            if (inserted) try {
                const auto token = system_catalog_->findSchemaToken(snapshot,
                    SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, types[i]);
                if (token) {
                    const auto count = system_catalog_->readTokenCardinality(snapshot,
                        SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, *token);
                    if (count && *count <= statistics->edge_count) entry->second = count;
                }
            } catch (...) { }
            result.edge_type_counts[i] = entry->second;
        }
		std::map<std::string, std::optional<uint64_t>> names;
		std::vector<uint64_t> tokens;
		for (const auto &combination : labels) for (const auto &label : combination)
			if (!label.empty()) names.try_emplace(label, std::nullopt);
		std::vector<std::string> orderedNames;
		orderedNames.reserve(names.size());
		for (const auto &[name, token] : names) orderedNames.push_back(name);
		try {
			const auto resolved = system_catalog_->findSchemaTokens(snapshot,
				SystemCatalogSchemaTokenKind::SCSTK_LABEL, orderedNames);
			for (size_t i = 0U; i < orderedNames.size(); ++i) names.at(orderedNames[i]) = resolved.at(i);
		} catch (...) {
			// A malformed name must not disable unrelated validated inputs.
			for (auto &[name, token] : names) {
				try { token = system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name); }
				catch (...) { }
			}
		}
		for (const auto &[name, token] : names) if (token) tokens.push_back(*token);
		std::sort(tokens.begin(), tokens.end());
		tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
		std::vector<std::optional<uint64_t>> counts;
		try {
			if (!tokens.empty()) counts = system_catalog_->readTokenCardinalities(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, tokens);
		} catch (...) {
			// Preserve unrelated valid estimates if a corrupt token breaks the
			// combined read. This slower path still uses the same pinned snapshot.
			for (size_t i = 0U; i < labels.size(); ++i) {
				try { result.node_counts[i] = estimateSnapshotLabels(*system_catalog_, snapshot, statistics->node_count, labels[i]); }
				catch (...) { }
			}
			return result;
		}
		for (size_t i = 0U; i < labels.size(); ++i) {
			uint64_t estimate = statistics->node_count;
			bool available = true;
			for (const auto &label : labels[i]) {
				const auto found = names.find(label);
				if (found == names.end() || !found->second) { available = false; break; }
				const size_t position = std::lower_bound(tokens.begin(), tokens.end(), *found->second) - tokens.begin();
				const auto count = counts.at(position);
				if (!count || *count > statistics->node_count) { available = false; break; }
				estimate = std::min(estimate, *count);
			}
			if (available) result.node_counts[i] = estimate;
		}
		return result;
	}

	void RelocatableStorageBackend::resetRuntimeStatistics() { resetIoStats(); }

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeIndex(const LogicalGraphNodeIndexCreateRequest &request) {
		std::vector<LogicalGraphIndexDefinition> created =
				createLogicalNodeIndexes(std::span<const LogicalGraphNodeIndexCreateRequest>(&request, 1U));
		if (created.size() != 1U) {
			throw std::logic_error("relocatable logical node Index creation did not return exactly one definition");
		}
		return std::move(created.front());
	}

    void RelocatableStorageBackend::rebuildNativeEntityFreeSpaceSummaries(const std::function<void()> &checkpoint) {
        rebuildNativeEntityFreeSpaceSummaries(checkpoint, {});
    }

    void RelocatableStorageBackend::rebuildNativeEntityFreeSpaceSummaries(const std::function<void()> &checkpoint,
        const std::shared_ptr<query::execution::MaterializedByteBudget> &boundedWorkspace) {
        requireOpen();
        if (isReadOnly()) throw std::logic_error("free-space initialization requires a writable backend");
        requireNoIndeterminateWrite();
        constexpr size_t maintenanceLimit = 7U * 1024U * 1024U;
        if (boundedWorkspace && (boundedWorkspace->used() > maintenanceLimit ||
            boundedWorkspace->available() > maintenanceLimit - boundedWorkspace->used()))
            throw std::invalid_argument("bounded free-space initialization exceeds its maintenance account");
        auto workspaceGate = query::execution::detail::ProcessResourceDomains::defaultOwner()->lockWriteWorkspace(resource_domain_);
        std::shared_ptr<std::pmr::memory_resource> workspace;
        if (boundedWorkspace) workspace = boundedWorkspace;
        else workspace = mutationWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope allocation(workspace);
        std::lock_guard lock(native_publication_mutex_);
        const auto publicationBase = metadata_->openSnapshot().superblock();
        try {
            const auto result = native_graph_write_planner_->rebuildEntityFreeSpaceSummaries(checkpoint);
            if (!result.committed()) throw LogicalGraphMutationConflict("free-space initialization source changed");
        } catch (...) {
            markReopenRequiredIfPublicationAdvanced(publicationBase);
            throw;
        }
    }

	void RelocatableStorageBackend::rebuildLogicalTokenCardinalityStatistics(const std::function<void()> &checkpoint) {
		requireOpen();
		if (isReadOnly()) throw std::runtime_error("token statistics rebuilding requires a writable backend");
		requireNoIndeterminateWrite();
		if (checkpoint) checkpoint();
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		auto snapshot = metadata_->openSnapshot();
		const Superblock base = snapshot.superblock();
		if (base.checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
			throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "statistics rebuild checkpoint overflowed");
		}
		const LogSequenceNumber lsn(base.checkpoint_lsn.value() + 1U);
		ExternalBTreeEntrySorter sorter({options_.maximum_online_index_entries,
				options_.online_index_sort_run_bytes, options_.maximum_online_index_sort_runs});
		NativeGraphCardinalityStatistics totals;
		NativeReadSession session(snapshot); session.bind(snapshot);
		const auto addMember = [&](SystemCatalogSchemaTokenKind kind, uint64_t token, uint64_t entity) {
			auto key = SystemCatalogTokenCardinalityCodec::countKeyOwned(kind, token);
			for (int shift = 56; shift >= 0; shift -= 8) key.push_back(static_cast<std::byte>((entity >> shift) & 0xffU));
			sorter.add({std::move(key), entity});
		};
		std::vector<uint64_t> nodeIds;
		nodeIds.reserve(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
		const auto flush = [&] {
			if (checkpoint) checkpoint();
			const auto labels = native_graph_reads_->readNodeLabelTokensSorted(snapshot, nodeIds, &session);
			for (size_t i = 0U; i < nodeIds.size(); ++i) {
				for (uint64_t token : labels[i]) addMember(SystemCatalogSchemaTokenKind::SCSTK_LABEL, token, nodeIds[i]);
			}
			nodeIds.clear();
		};
		const bool nodesComplete = native_graph_reads_->visitNodeRefsUntil(snapshot,
				options_.maximum_online_index_scan_nodes, [&](const EntityRef &ref) {
					++totals.node_count; nodeIds.push_back(ref.entity_id);
					if (nodeIds.size() == BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES) flush(); return true;
				}, &session);
		if (!nodesComplete) throw std::runtime_error("token statistics Node scan exceeded its bound");
		if (!nodeIds.empty()) flush();
		const bool edgesComplete = native_graph_reads_->visitEdgeRefsUntil(snapshot,
				options_.maximum_online_index_entries, [&](const EntityRef &ref) {
					if (totals.edge_count % BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES == 0U && checkpoint) checkpoint();
					++totals.edge_count;
					addMember(SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, ref.type_token, ref.entity_id); return true;
				}, &session);
		if (!edgesComplete) throw std::runtime_error("token statistics Edge scan exceeded its bound");
		const auto sorted = sorter.finish();
		const OrderedBTreeEntrySource statistics = [sorted, totals, checkpoint] {
			auto cursor = sorted();
			return [cursor = std::move(cursor), totals, checkpoint, phase = 0U, scanned = size_t{0U},
					pending = std::optional<BTreePageEntry>{}]() mutable -> std::optional<BTreePageEntry> {
				if (phase == 0U) { ++phase; return BTreePageEntry{SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned(),
						SystemCatalogGraphCardinalityCodec::encodeCount(totals.node_count)}; }
				if (phase == 1U) { ++phase; return BTreePageEntry{SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned(),
						SystemCatalogGraphCardinalityCodec::encodeCount(totals.edge_count)}; }
				if (phase == 2U) {
					if (!pending) pending = cursor();
					if (pending) {
						if (pending->key.size() != 18U || pending->value_or_child == 0U)
							throw FormatException(FormatError::FERR_INVALID_STATE, "statistics sorter returned an invalid member");
						std::vector<std::byte> key(pending->key.begin(), pending->key.begin() + 10U);
						uint64_t count = 0U;
						do {
							if (scanned++ % BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES == 0U && checkpoint) checkpoint();
							++count; pending = cursor();
						} while (pending && pending->key.size() == 18U && std::equal(key.begin(), key.end(), pending->key.begin()));
						return BTreePageEntry{std::move(key), SystemCatalogGraphCardinalityCodec::encodeCount(count)};
					}
					++phase;
				}
				if (phase == 3U) { ++phase; return BTreePageEntry{SystemCatalogTokenCardinalityCodec::completeKeyOwned(
						SystemCatalogSchemaTokenKind::SCSTK_LABEL), 1U}; }
				if (phase == 4U) { ++phase; return BTreePageEntry{SystemCatalogTokenCardinalityCodec::completeKeyOwned(
						SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE), 1U}; }
				return std::nullopt;
			};
		};
		OrderedBTreeEntrySource preserved;
		if (metadata_->findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID)) {
			const auto existing = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, *btree_pages_, *metadata_)
					.orderedSource(snapshot, options_.maximum_bulk_edge_index_entries);
			preserved = [existing, checkpoint] {
				auto cursor = existing();
				return [cursor = std::move(cursor), checkpoint, count = size_t{0U}]() mutable -> std::optional<BTreePageEntry> {
					while (auto entry = cursor()) {
						if (count++ % BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES == 0U && checkpoint) checkpoint();
						if (entry->key.size() >= 2U && entry->key[0] == std::byte{13U} &&
								entry->key[1] >= std::byte{1U} && entry->key[1] <= std::byte{6U}) continue;
						return entry;
					}
					return std::nullopt;
				};
			};
		} else {
			preserved = [] {
				return [done = false]() mutable -> std::optional<BTreePageEntry> {
					if (done) return std::nullopt; done = true;
					return BTreePageEntry{SystemCatalogTokenKeyCodec::bootstrapKeyOwned(), RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE};
				};
			};
		}
		const auto entries = NativeBulkEdgeCatalogTokenMaterializer::overlayEntries(preserved, statistics);
		const NativeStreamedGraphCatalogIndexPlanningOptions options{PhysicalSlotId(1U),
				PhysicalSlotId(std::numeric_limits<uint64_t>::max()), options_.maximum_bulk_edge_index_entries,
				options_.maximum_online_index_pages};
		auto plan = NativeStreamedGraphCatalogIndexPlanner::plan(*system_catalog_, *metadata_, entries, true, options, snapshot, lsn);
		if (!plan) throw std::logic_error("token statistics rebuild produced no Catalog plan");
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(plan->mutation);
		if (checkpoint) checkpoint();
		try {
			static_cast<void>(metadata_->mutateNativeGraphPagesWithLargeValues({}, {}, {}, {}, {}, {}, derived, lsn,
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
						std::span<EntityPageNewPoolBatchMaterialization>, std::span<NativeGraphPropertyPageReplacementMaterialization>,
						std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
					[preparer = plan->preparer](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
						if (batches.size() != 1U) throw std::logic_error("statistics rebuild changed its Catalog partition");
						preparer(batches.front());
					}, {}, {}, {}, {}, {}, {}, {}, base.generation));
		} catch (...) {
			markReopenRequiredIfPublicationAdvanced(base); throw;
		}
	}

	std::vector<uint64_t> RelocatableStorageBackend::ensureLogicalMembershipIndexes(
			const std::function<void()> &checkpoint) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) throw std::runtime_error("membership Index creation requires a writable backend");
		requireNoIndeterminateWrite();
		if (options_.maximum_online_index_entries == 0U || options_.maximum_online_index_scan_nodes == 0U ||
				options_.maximum_online_index_pages == 0U || options_.online_index_sort_run_bytes == 0U ||
				options_.maximum_online_index_sort_runs == 0U) {
			throw std::invalid_argument("membership Index creation requires positive work bounds");
		}
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		auto transaction = native_graph_write_planner_->beginTransaction();
		const auto &snapshot = transaction.baseSnapshot();
		const auto existing = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		std::array<IndexDefinition, 2U> definitions;
		std::array<bool, 2U> missing{true, true};
		for (const auto &definition : existing) {
			if (definition.kind != IndexDefinitionKind::IDK_LABEL) continue;
			const size_t ordinal = definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE ? 0U : 1U;
			if (missing[ordinal]) { definitions[ordinal] = definition; missing[ordinal] = false; }
		}
		if (!missing[0] && !missing[1]) return {definitions[0].index_id, definitions[1].index_id};
		const size_t additions = static_cast<size_t>(missing[0]) + static_cast<size_t>(missing[1]);
		if (additions > options_.maximum_index_definitions - existing.size()) {
			throw std::runtime_error("membership Index definitions exceed the configured bound");
		}
		auto catalogSession = system_catalog_->beginWrite(transaction);
		std::array<std::unique_ptr<ExternalBTreeEntrySorter>, 2U> sorters;
		for (size_t ordinal = 0U; ordinal < 2U; ++ordinal) {
			if (!missing[ordinal]) continue;
			auto &definition = definitions[ordinal];
			definition.entity_kind = ordinal == 0U ? IndexDefinitionEntityKind::IDEK_NODE : IndexDefinitionEntityKind::IDEK_EDGE;
			definition.kind = IndexDefinitionKind::IDK_LABEL;
			const std::string base = ordinal == 0U ? "__zyx_node_membership" : "__zyx_edge_membership";
			definition.name = base;
			size_t suffix = 0U;
			while (std::ranges::any_of(existing, [&](const auto &other) { return other.name == definition.name; })) {
				definition.name = base + "_" + std::to_string(++suffix);
			}
			definition = catalogSession.createIndexDefinition(std::move(definition));
			sorters[ordinal] = std::make_unique<ExternalBTreeEntrySorter>(ExternalBTreeEntrySorterOptions{
					options_.maximum_online_index_entries, options_.online_index_sort_run_bytes,
					options_.maximum_online_index_sort_runs});
		}
		NativeGraphCardinalityStatistics scannedCounts;
		NativeReadSession session(snapshot);
		session.bind(snapshot);
		const auto addMember = [&](size_t ordinal, uint64_t token, uint64_t id) {
			if (token > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw FormatException(FormatError::FERR_INVALID_STATE, "membership token exceeds scalar encoding");
			}
			sorters[ordinal]->add({IndexKeyCodec::encodeEntryKey(PropertyValue(static_cast<int64_t>(token)), id), id});
		};
		if (missing[0]) {
			std::vector<uint64_t> ids;
			ids.reserve(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
			const auto flush = [&] {
				if (checkpoint) checkpoint();
				const auto labels = native_graph_reads_->readNodeLabelTokensSorted(snapshot, ids, &session);
				for (size_t i = 0U; i < ids.size(); ++i) {
					for (uint64_t token : labels[i]) addMember(0U, token, ids[i]);
				}
				ids.clear();
			};
			const bool complete = native_graph_reads_->visitNodeRefsUntil(snapshot,
					options_.maximum_online_index_scan_nodes, [&](const EntityRef &reference) {
						++scannedCounts.node_count;
						ids.push_back(reference.entity_id);
						if (ids.size() == BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES) flush();
						return true;
					}, &session);
			if (!complete) throw std::runtime_error("membership Index source exceeded its scan bound");
			if (!ids.empty()) flush();
		}
		if (missing[1]) {
			const bool complete = native_graph_reads_->visitEdgeRefsUntil(snapshot,
					options_.maximum_online_index_entries, [&](const EntityRef &reference) {
						if (checkpoint) checkpoint();
						++scannedCounts.edge_count;
						if (reference.type_token != 0U) addMember(1U, reference.type_token, reference.entity_id);
						return true;
					}, &session);
			if (!complete) throw std::runtime_error("membership Index source exceeded its scan bound");
		}
		for (size_t ordinal = 0U; ordinal < 2U; ++ordinal) {
			if (!missing[ordinal]) continue;
			if (checkpoint) checkpoint();
			const auto unchecked = sorters[ordinal]->finish();
			OrderedBTreeEntrySource source = unchecked;
			if (checkpoint) source = [unchecked, checkpoint] {
				auto cursor = unchecked();
				return [cursor = std::move(cursor), checkpoint, count = size_t{0U}]() mutable {
					if (count++ % 256U == 0U) checkpoint();
					return cursor();
				};
			};
			const BulkBTreeBuildOptions build{PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
					definitions[ordinal].index_id, options_.maximum_online_index_entries, options_.maximum_online_index_pages};
			transaction.stageScalarIndexCreate(*btree_pages_, BulkBTreeBuilder::planNativeTree(
					*btree_pages_, *metadata_, snapshot, definitions[ordinal].index_id, source, build));
		}
		// Both exhaustive sources proved completion under the same snapshot.
		// Backfill live totals only with this proof, never from high-water IDs.
		if (missing[0] && missing[1] && !system_catalog_->readGraphCardinalityStatistics(snapshot).has_value()) {
			catalogSession.setGraphCardinalityStatistics(scannedCounts);
		}
		catalogSession.finish();
		if (checkpoint) checkpoint();
		if (!commitNativeGraphWriteTransaction(transaction).committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "membership Index creation lost its publication fence");
		}
		return {definitions[0].index_id, definitions[1].index_id};
	}

	LogicalGraphIndexDefinition RelocatableStorageBackend::createLogicalNodeIndexWithCheckpoint(
		const LogicalGraphNodeIndexCreateRequest &request, const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		auto result = createLogicalNodeIndexesWithCheckpoint(std::span(&request, 1U), checkpoint, propertyAdmission);
		return std::move(result.front());
	}

	std::vector<LogicalGraphIndexDefinition> RelocatableStorageBackend::createLogicalNodeIndexes(
			const std::span<const LogicalGraphNodeIndexCreateRequest> requests) {
		return createLogicalNodeIndexesWithCheckpoint(requests, {});
	}

	std::vector<LogicalGraphIndexDefinition>
	RelocatableStorageBackend::createLogicalNodeIndexesWithCheckpoint(std::span<const LogicalGraphNodeIndexCreateRequest> requests,
		const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical Index");
		}
		requireNoIndeterminateWrite();
        auto writeWorkspace = ddlWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope writeAllocation(writeWorkspace);
		if (requests.empty() || requests.size() > options_.maximum_index_definitions ||
			options_.maximum_online_index_entries == 0U || options_.maximum_online_index_scan_nodes == 0U ||
			options_.maximum_online_index_pages == 0U || options_.online_index_sort_run_bytes == 0U ||
			options_.maximum_online_index_sort_runs == 0U) {
			throw std::invalid_argument(
					"relocatable logical node Index batch creation requires bounded nonempty schema input");
		}

		// Validate the complete request set before allocating labels or physical
		// trees, so invalid batches cannot leave a logical schema side effect.
        utils::OwnedStdVector<utils::OwnedStdString> requestedNames;
        requestedNames.reserve(requests.size());
        utils::OwnedStdVector<size_t> ordinals;
        ordinals.reserve(requests.size());
        for (const auto &request : requests) {
            if (request.name.size() > RSS_INDEX_DEFINITION_MAX_NAME_BYTES ||
                request.label.empty() || request.label.size() > RSS_TOKEN_DEFINITION_MAX_NAME_BYTES ||
                request.property_keys.empty() || request.property_keys.size() > RSS_INDEX_DEFINITION_MAX_PROPERTIES)
                throw std::invalid_argument("relocatable logical node Index has an invalid label or Property-key sequence");
            for (size_t i = 0; i < request.property_keys.size(); ++i) {
                const auto &property = request.property_keys[i];
                if (property.empty() || property.size() > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES ||
                    std::find(request.property_keys.begin(), request.property_keys.begin() + i, property) !=
                        request.property_keys.begin() + i)
                    throw std::invalid_argument("relocatable logical node Index has an invalid Property-key sequence");
            }
            requestedNames.push_back(request.name.empty() ? defaultNodeIndexName(request.label, request.property_keys)
                                                         : utils::OwnedStdString(request.name));
            if (requestedNames.back().empty() || requestedNames.back().size() > RSS_INDEX_DEFINITION_MAX_NAME_BYTES)
                throw std::invalid_argument("relocatable logical node Index batch contains an invalid name");
            ordinals.push_back(ordinals.size());
        }
        // Sort borrowed schema/name ordinals instead of copying request strings
        // into unaccounted tree nodes. Per-request property count is format-bounded.
        std::sort(ordinals.begin(), ordinals.end(), [&](size_t a, size_t b) { return requestedNames[a] < requestedNames[b]; });
        for (size_t i = 1; i < ordinals.size(); ++i)
            if (requestedNames[ordinals[i-1]] == requestedNames[ordinals[i]])
                throw std::invalid_argument("relocatable logical node Index batch contains a duplicate name");
        std::sort(ordinals.begin(), ordinals.end(), [&](size_t a, size_t b) {
            return std::tie(requests[a].label, requests[a].property_keys) < std::tie(requests[b].label, requests[b].property_keys);
        });
        for (size_t i = 1; i < ordinals.size(); ++i) {
            const auto &a = requests[ordinals[i-1]], &b = requests[ordinals[i]];
            if (a.label == b.label && a.property_keys == b.property_keys)
                throw std::invalid_argument("relocatable logical node Index batch contains a duplicate schema");
        }

		std::vector<LogicalGraphIndexDefinition> result;
		result.reserve(requests.size());
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		const utils::OwnedStdVector<IndexDefinition> existing =
				readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		if (requests.size() > options_.maximum_index_definitions - existing.size()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical node Index batch exceeds the configured definition capacity");
		}

		utils::OwnedStdVector<IndexDefinition> definitions;
		std::vector<std::vector<uint32_t>> propertyTokens;
		definitions.reserve(requests.size());
		propertyTokens.reserve(requests.size());
		for (size_t requestIndex = 0U; requestIndex < requests.size(); ++requestIndex) {
			const LogicalGraphNodeIndexCreateRequest &request = requests[requestIndex];
			IndexDefinition definition;
			definition.name = requestedNames[requestIndex];
			definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
			definition.kind = request.property_keys.size() == 1U ? IndexDefinitionKind::IDK_PROPERTY
																 : IndexDefinitionKind::IDK_COMPOSITE;
			definition.label_token_id =
					catalogSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, request.label);
			definition.property_names = request.property_keys;
			std::vector<uint32_t> tokens;
			tokens.reserve(request.property_keys.size());
			for (const std::string &property: request.property_keys) {
				tokens.push_back(catalogSession.ensurePropertyToken(property));
			}
			definition = catalogSession.createIndexDefinition(std::move(definition));
			IndexDefinitionCodec::validate(definition);
			if (std::ranges::any_of(existing,
									[&](const IndexDefinition &candidate) {
										return candidate.name == definition.name ||
											   sameIndexSchema(candidate, definition.entity_kind, definition.kind,
															   definition.label_token_id, definition.property_names);
									}) ||
				std::ranges::any_of(definitions, [&](const IndexDefinition &candidate) {
					return candidate.name == definition.name ||
						   sameIndexSchema(candidate, definition.entity_kind, definition.kind,
										   definition.label_token_id, definition.property_names);
				})) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable logical node Index name or physical schema already exists");
			}
			result.push_back({definition.index_id,
							  definition.name,
							  logicalIndexKind(definition.kind),
							  request.label,
							  definition.property_names,
							  0U,
							  {}});
			definitions.push_back(std::move(definition));
			propertyTokens.push_back(std::move(tokens));
		}

		// Build every requested index from one bounded Node/label/Property scan.
		// The previous per-definition loop repeated the Entity Map descent and
		// Property Directory merge for each index in a DDL batch, which made a
		// multi-index schema operation scale with the number of definitions rather
		// than with the graph. Each sorter remains independent, so index ordering,
		// entry limits, and external-sort memory bounds are unchanged.
		struct IndexBuildState {
			ExternalBTreeEntrySorter sorter;
			std::vector<std::pair<uint32_t, size_t>> token_order;
			std::vector<uint32_t> sorted_tokens;
			uint64_t indexed_entries = 0U;

			IndexBuildState(ExternalBTreeEntrySorterOptions options, const std::vector<uint32_t> &tokens) :
					sorter(options) {
				token_order.reserve(tokens.size());
				for (size_t tokenIndex = 0U; tokenIndex < tokens.size(); ++tokenIndex) {
					token_order.emplace_back(tokens[tokenIndex], tokenIndex);
				}
				std::ranges::sort(token_order, {}, &std::pair<uint32_t, size_t>::first);
				for (size_t index = 1U; index < token_order.size(); ++index) {
					if (token_order[index - 1U].first == token_order[index].first) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable logical node Index creation resolved duplicate Property tokens");
					}
				}
				sorted_tokens.reserve(token_order.size());
				for (const auto &[token, originalIndex]: token_order) {
					static_cast<void>(originalIndex);
					sorted_tokens.push_back(token);
				}
			}
		};

		std::vector<IndexBuildState> indexStates;
		indexStates.reserve(definitions.size());
		for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
			indexStates.emplace_back(
					ExternalBTreeEntrySorterOptions{options_.maximum_online_index_entries,
															 options_.online_index_sort_run_bytes,
															 options_.maximum_online_index_sort_runs},
					propertyTokens[definitionIndex]);
		}

		constexpr size_t ONLINE_INDEX_NODE_BATCH_ENTRIES = 512U;
		std::vector<uint64_t> nodeBatch;
		nodeBatch.reserve(ONLINE_INDEX_NODE_BATCH_ENTRIES);
		NativeReadSession readSession(snapshot);
		const auto flushNodeBatch = [&]() {
			if (checkpoint) checkpoint();
			if (nodeBatch.empty()) {
				return;
			}
			const std::vector<std::vector<uint64_t>> labels =
					native_graph_reads_->readNodeLabelTokensSorted(snapshot, nodeBatch, &readSession);
			if (labels.size() != nodeBatch.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable logical node Index creation returned an invalid label batch");
			}

			std::vector<std::vector<size_t>> candidatePositions(definitions.size());
			std::vector<uint64_t> propertyNodeIds;
			std::vector<uint32_t> propertyTokensUnion;
			propertyNodeIds.reserve(nodeBatch.size());
			for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
				const IndexDefinition &definition = definitions[definitionIndex];
				std::vector<size_t> &positions = candidatePositions[definitionIndex];
				positions.reserve(nodeBatch.size());
				for (size_t position = 0U; position < nodeBatch.size(); ++position) {
					if (std::ranges::binary_search(labels[position], definition.label_token_id)) {
						positions.push_back(position);
						propertyNodeIds.push_back(nodeBatch[position]);
					}
				}
				propertyTokensUnion.insert(propertyTokensUnion.end(), indexStates[definitionIndex].sorted_tokens.begin(),
														 indexStates[definitionIndex].sorted_tokens.end());
			}
			std::ranges::sort(propertyNodeIds);
			propertyNodeIds.erase(std::ranges::unique(propertyNodeIds).begin(), propertyNodeIds.end());
			std::ranges::sort(propertyTokensUnion);
			propertyTokensUnion.erase(std::ranges::unique(propertyTokensUnion).begin(), propertyTokensUnion.end());

			std::vector<std::vector<std::optional<graph::PropertyValue>>> columns;
			if (!propertyNodeIds.empty()) {
				auto admission = makeSelectedPropertyBatchAdmission(propertyAdmission);
				columns = native_graph_reads_->readNodePropertyColumnsSorted(
						snapshot, propertyNodeIds, propertyTokensUnion, &readSession, admission ? &admission : nullptr);
				if (columns.size() != propertyTokensUnion.size() ||
						std::ranges::any_of(columns, [&](const auto &column) {
							return column.size() != propertyNodeIds.size();
						})) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable logical node Index creation returned an invalid Property column batch");
				}
			}

			for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
				IndexBuildState &state = indexStates[definitionIndex];
				const IndexDefinition &definition = definitions[definitionIndex];
				const std::vector<uint32_t> &tokens = propertyTokens[definitionIndex];
				for (const size_t nodePosition : candidatePositions[definitionIndex]) {
					const auto propertyPosition = std::ranges::lower_bound(propertyNodeIds, nodeBatch[nodePosition]);
					if (propertyPosition == propertyNodeIds.end() || *propertyPosition != nodeBatch[nodePosition]) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable logical node Index creation lost a candidate ID");
					}
					const size_t row = static_cast<size_t>(propertyPosition - propertyNodeIds.begin());
					std::vector<graph::PropertyValue> values(tokens.size());
					bool complete = true;
					for (size_t sortedIndex = 0U; sortedIndex < state.token_order.size(); ++sortedIndex) {
						const size_t unionIndex = static_cast<size_t>(
								std::ranges::lower_bound(propertyTokensUnion, state.sorted_tokens[sortedIndex]) -
								propertyTokensUnion.begin());
						if (unionIndex >= columns.size() || !columns[unionIndex][row].has_value()) {
							complete = false;
							break;
						}
						values[state.token_order[sortedIndex].second] = *columns[unionIndex][row];
					}
					if (!complete) {
						continue;
					}
					if (state.indexed_entries == options_.maximum_online_index_entries) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable logical node Index exceeds its configured entry bound");
					}
					const std::optional<std::vector<std::byte>> key =
							definition.kind == IndexDefinitionKind::IDK_PROPERTY
									? IndexKeyCodec::tryEncodeEntryKey(values.front(), nodeBatch[nodePosition])
									: IndexKeyCodec::tryEncodeCompositeEntryKey(values, nodeBatch[nodePosition]);
					if (key.has_value()) {
						state.sorter.add({std::move(*key), nodeBatch[nodePosition]});
						++state.indexed_entries;
					}
				}
			}
			nodeBatch.clear();
		};

		const bool scannedAllNodes = native_graph_reads_->visitNodeRefsUntil(
				snapshot, options_.maximum_online_index_scan_nodes,
				[&](const EntityRef &reference) {
					nodeBatch.push_back(reference.entity_id);
					if (nodeBatch.size() == ONLINE_INDEX_NODE_BATCH_ENTRIES) {
						flushNodeBatch();
					}
					return true;
				}, &readSession);
		flushNodeBatch();
		if (!scannedAllNodes) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable logical node Index graph scan exceeded its configured bounded DDL work");
		}

		for (size_t definitionIndex = 0U; definitionIndex < definitions.size(); ++definitionIndex) {
			if (checkpoint) checkpoint();
			const IndexDefinition &definition = definitions[definitionIndex];
			const OrderedBTreeEntrySource uncheckedSource = indexStates[definitionIndex].sorter.finish();
			OrderedBTreeEntrySource source = uncheckedSource;
			if (checkpoint) source = [uncheckedSource, checkpoint] {
				auto cursor = uncheckedSource();
				return [cursor = std::move(cursor), checkpoint, count = size_t{0U}]() mutable {
					if (count++ % 256U == 0U && checkpoint) checkpoint();
					return cursor();
				};
			};
			const BulkBTreeBuildOptions build = {
					PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()), definition.index_id,
					options_.maximum_online_index_entries, options_.maximum_online_index_pages};
			transaction.stageScalarIndexCreate(*btree_pages_,
												 BulkBTreeBuilder::planNativeTree(*btree_pages_, *metadata_, snapshot,
																									 definition.index_id, source, build));
		}
		if (checkpoint) checkpoint();
		catalogSession.finish();
		if (checkpoint) checkpoint();
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical node Index batch lost its native publication fence");
		}
		return result;
	}

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeVectorIndex(const LogicalGraphNodeVectorIndexCreateRequest &request) {
		return createLogicalNodeVectorIndexWithCheckpoint(request, {});
	}

	LogicalGraphIndexDefinition
	RelocatableStorageBackend::createLogicalNodeVectorIndexWithCheckpoint(const LogicalGraphNodeVectorIndexCreateRequest &request,
		const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical Vector Index");
		}
		requireNoIndeterminateWrite();
        auto writeWorkspace = ddlWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope writeAllocation(writeWorkspace);
        if (request.label.size() > RSS_TOKEN_DEFINITION_MAX_NAME_BYTES || request.name.size() > RSS_INDEX_DEFINITION_MAX_NAME_BYTES)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "logical schema name exceeds its durable byte bound");
		if (request.label.empty() || request.property_key.empty() ||
			request.property_key.size() > RSS_INDEX_DEFINITION_MAX_PROPERTY_NAME_BYTES || request.dimensions == 0U ||
			request.dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS || options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_scan_nodes == 0U || options_.maximum_online_index_pages == 0U) {
			throw std::invalid_argument(
					"relocatable logical Vector Index creation requires bounded nonempty node schema and dimensions");
		}
		const utils::OwnedStdString metric = normalizeVectorMetric(request.metric);


		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
		IndexDefinition definition;
		definition.name = request.name.empty() ? defaultNodeVectorIndexName(request.label, request.property_key,
																			request.dimensions, metric)
											   : utils::OwnedStdString(request.name);
		definition.entity_kind = IndexDefinitionEntityKind::IDEK_NODE;
		definition.kind = IndexDefinitionKind::IDK_VECTOR;
		definition.label_token_id =
				catalogSession.ensureSchemaToken(SystemCatalogSchemaTokenKind::SCSTK_LABEL, request.label);
		definition.property_names.push_back(request.property_key);
		definition.vector_dimensions = request.dimensions;
		definition.vector_metric = metric;
		const uint32_t propertyToken = catalogSession.ensurePropertyToken(request.property_key);
		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		const utils::OwnedStdVector<IndexDefinition> existing =
				readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		if (existing.size() >= options_.maximum_index_definitions) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector Index catalog reached its configured definition bound");
		}
		if (std::ranges::any_of(existing, [&](const IndexDefinition &candidate) {
				return candidate.name == definition.name ||
					   sameIndexSchema(candidate, definition.entity_kind, definition.kind, definition.label_token_id,
									   definition.property_names);
			})) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector Index name or physical schema already exists");
		}
		definition = catalogSession.createIndexDefinition(std::move(definition));
		const VectorDistanceMetric distanceMetric = VectorSemanticCodec::metricFor(definition);

		std::vector<NativeGraphVectorBootstrapRecord> records;
		records.reserve(std::min<size_t>(options_.maximum_online_index_entries, 1024U));
		// Vector bootstrap needs one Property column, not complete logical Nodes.
		// Use the same bounded label-reference scan and sorted selected read as
		// scalar indexes so large imports do not decode every owner's root.
		constexpr size_t ONLINE_VECTOR_PROPERTY_BATCH_ENTRIES = 512U;
		std::vector<uint64_t> pendingNodeIds;
		pendingNodeIds.reserve(ONLINE_VECTOR_PROPERTY_BATCH_ENTRIES);
		NativeReadSession readSession(snapshot);
		const auto flushPropertyBatch = [&]() {
			if (checkpoint) checkpoint();
			if (pendingNodeIds.empty()) {
				return;
			}
			std::ranges::sort(pendingNodeIds);
			pendingNodeIds.erase(std::ranges::unique(pendingNodeIds).begin(), pendingNodeIds.end());
			auto admission = makeSelectedPropertyBatchAdmission(propertyAdmission);
			const std::array<uint32_t, 1U> selectedTokens{propertyToken};
			auto columns = native_graph_reads_->readNodePropertyColumnsSorted(snapshot, pendingNodeIds,
				selectedTokens, &readSession, admission ? &admission : nullptr);
			if (columns.size() != 1U) throw std::runtime_error("vector bootstrap Property column changed cardinality");
			auto values = std::move(columns.front());
			if (values.size() != pendingNodeIds.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable logical Vector Index creation returned an invalid Property batch");
			}
			for (size_t index = 0U; index < pendingNodeIds.size(); ++index) {
				if (!values[index].has_value()) {
					continue;
				}
				if (records.size() == options_.maximum_online_index_entries) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable logical Vector Index exceeds its configured historical Vector bound");
				}
				const std::vector<std::byte> encoded = VectorSemanticCodec::encodeBFloat16ForLogicalWrite(
						values[index].value(), definition.vector_dimensions);
				if (distanceMetric == VectorDistanceMetric::VDM_COSINE &&
						!VectorSemanticCodec::hasNonZeroFiniteL2Norm(encoded)) {
					throw std::invalid_argument(
							"relocatable logical cosine Vector Index cannot include a zero-norm property");
				}
				records.push_back({pendingNodeIds[index], encoded});
			}
			pendingNodeIds.clear();
		};
		const std::array<uint64_t, 1U> labelTokens{definition.label_token_id};
		size_t scannedForCheckpoint = 0U;
		const bool scannedAllNodes = native_graph_reads_->visitNodeRefsWithLabelTokenMatchesUntil(
				snapshot, options_.maximum_online_index_scan_nodes, labelTokens,
				[&](const EntityRef &reference, const bool labelsMatch) {
					if (++scannedForCheckpoint % 256U == 0U && checkpoint) checkpoint();
					if (labelsMatch) {
						pendingNodeIds.push_back(reference.entity_id);
						if (pendingNodeIds.size() == ONLINE_VECTOR_PROPERTY_BATCH_ENTRIES) {
							flushPropertyBatch();
						}
					}
					return true;
				}, &readSession);
		flushPropertyBatch();
		if (!scannedAllNodes) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable logical Vector Index graph scan exceeded its configured bounded DDL work");
		}
		if (checkpoint) checkpoint();
		if (!records.empty()) {
			NativeGraphVectorBootstrapOptions bootstrap;
			bootstrap.first_slot = PhysicalSlotId(1U);
			bootstrap.last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
			bootstrap.maximum_vectors = options_.maximum_online_index_entries;
			bootstrap.maximum_pages = options_.maximum_online_index_pages;
			bootstrap.checkpoint = checkpoint;
			transaction.stageVectorIndexBootstrap(*btree_pages_, *vector_pages_, *vector_manifests_, definition,
												  records, bootstrap);
		}
		if (checkpoint) checkpoint();
		catalogSession.finish();
		if (checkpoint) checkpoint();
        LogicalGraphIndexDefinition result{definition.index_id,	 definition.name,			LogicalGraphIndexKind::LGIK_VECTOR,
				request.label,			 definition.property_names, definition.vector_dimensions,
				definition.vector_metric};
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector Index lost its native publication fence");
		}
		return result;
	}

	bool RelocatableStorageBackend::dropLogicalIndexByName(std::string_view name) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot drop a logical Index");
		}
		if (name.empty()) {
			throw std::invalid_argument("relocatable logical Index drop requires a nonempty name");
		}
		requireNoIndeterminateWrite();
        auto writeWorkspace = ddlWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope writeAllocation(writeWorkspace);
		bool dropped = false;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const std::optional<IndexDefinition> canonical = system_catalog_->findIndexDefinitionByName(snapshot, name);
			if (canonical.has_value()) {
				snapshot.release();
				NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
				SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
				// Scalar trees can be retired in the DDL generation itself. Vector
				// state is dependency-ordered and remains for the dedicated reclaimer.
				if (canonical->kind != IndexDefinitionKind::IDK_VECTOR &&
					metadata_->findIndexRootOptional(transaction.baseSnapshot(), canonical->index_id).has_value()) {
					transaction.stageScalarIndexDrop(*btree_pages_, canonical->index_id,
													 options_.maximum_online_index_pages);
				}
				catalogSession.dropIndexDefinition(*canonical);
				catalogSession.finish();
				const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
				if (!committed.committed()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable canonical Index drop lost its native publication fence");
				}
				dropped = true;
				return true;
			}
			// The canonical catalog is authoritative. An absent name means there
			// is no definition to drop; legacy registry trees are never consulted.
			snapshot.release();
		}
		return dropped;
	}

	bool RelocatableStorageBackend::dropLogicalNodeIndexBySchema(std::string_view label, std::string_view propertyKey) {
		requireOpen();
		if (label.empty() || propertyKey.empty()) {
			throw std::invalid_argument("relocatable logical node Index drop requires nonempty label and Property key");
		}
		MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		const std::optional<uint64_t> token =
				system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
		if (!token.has_value()) {
			snapshot.release();
			return false;
		}
		const std::array<std::string, 1U> properties = {std::string(propertyKey)};
		const utils::OwnedStdVector<IndexDefinition> definitions =
				readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		const auto found = std::ranges::find_if(definitions, [&](const IndexDefinition &definition) {
			return sameIndexSchema(definition, IndexDefinitionEntityKind::IDEK_NODE, IndexDefinitionKind::IDK_PROPERTY,
								   *token, properties);
		});
		if (found == definitions.end()) {
			return false;
		}
		const std::string name = found->name;
		snapshot.release();
		return dropLogicalIndexByName(name);
	}

    std::vector<LogicalGraphIndexDefinition>
    RelocatableStorageBackend::listLogicalIndexes(size_t maximumIndexes) const {
        return listOwnedLogicalIndexes(maximumIndexes);
    }

    utils::OwnedStdVector<LogicalGraphIndexDefinition>
    RelocatableStorageBackend::listOwnedLogicalIndexes(size_t maximumIndexes) const {
		requireOpen();
		if (maximumIndexes == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable logical Index list requires a positive result bound");
		}
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		const utils::OwnedStdVector<IndexDefinition> definitions = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
		utils::OwnedStdVector<LogicalGraphIndexDefinition> result;
		result.reserve(definitions.size());
		for (const IndexDefinition &definition: definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind == IndexDefinitionKind::IDK_LABEL) {
				continue;
			}
			if (result.size() == maximumIndexes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable logical Index list exceeded its result bound");
			}
			if (definition.label_token_id == 0U) {
				result.push_back({definition.index_id,
								  definition.name,
								  logicalIndexKind(definition.kind),
								  {},
								  definition.property_names,
								  definition.vector_dimensions,
								  definition.vector_metric});
				continue;
			}
			const auto label = system_catalog_->findOwnedSchemaToken(
					snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, definition.label_token_id);
			if (!label.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable logical Index list found an unknown canonical label Token");
			}
			result.push_back({definition.index_id, definition.name, logicalIndexKind(definition.kind), label->name,
							  definition.property_names, definition.vector_dimensions, definition.vector_metric});
		}
		return result;
	}

	LogicalGraphConstraintDefinition
	RelocatableStorageBackend::createLogicalConstraint(const LogicalGraphConstraintCreateRequest &request) {
		return createLogicalConstraintWithCheckpoint(request, {});
	}

	LogicalGraphConstraintDefinition
	RelocatableStorageBackend::createLogicalConstraintWithCheckpoint(const LogicalGraphConstraintCreateRequest &request,
		const std::function<void()> &checkpoint, const std::function<void(size_t)> &propertyAdmission) {
		if (checkpoint) checkpoint();
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot create a logical constraint");
		}
		requireNoIndeterminateWrite();
        auto writeWorkspace = ddlWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope writeAllocation(writeWorkspace);
        if (request.label.size() > RSS_TOKEN_DEFINITION_MAX_NAME_BYTES)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "logical schema name exceeds its durable byte bound");
		if (request.name.empty() || request.name.size() > RSS_CONSTRAINT_DEFINITION_MAX_NAME_BYTES ||
			request.label.empty() || request.property_keys.empty() ||
			request.property_keys.size() > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTIES ||
			options_.maximum_constraint_definitions == 0U) {
			throw std::invalid_argument("relocatable logical constraint creation requires a bounded complete schema");
		}
		for (const std::string &property: request.property_keys) {
			if (property.empty() || property.size() > RSS_CONSTRAINT_DEFINITION_MAX_PROPERTY_NAME_BYTES) {
				throw std::invalid_argument("relocatable logical constraint has an invalid Property key");
			}
		}
		ConstraintDefinition definition;
		definition.name = request.name;
		definition.entity_kind = constraintDefinitionEntityKind(request.entity_kind);
		definition.kind = constraintDefinitionKind(request.kind);
		definition.property_names = request.property_keys;
		definition.property_type = constraintDefinitionPropertyType(request.property_type);

		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
		const SystemCatalogSchemaTokenKind schemaKind =
				definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
						? SystemCatalogSchemaTokenKind::SCSTK_LABEL
						: SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
		definition.schema_token_id = catalogSession.ensureSchemaToken(schemaKind, request.label);
		utils::OwnedMap<utils::OwnedStdString, uint32_t, std::less<>> requestedPropertyTokens;
		for (const std::string &property: definition.property_names) {
			requestedPropertyTokens.emplace(property, catalogSession.ensurePropertyToken(property));
		}

		const MetadataSnapshotPin &snapshot = transaction.baseSnapshot();
		const utils::OwnedStdVector<ConstraintDefinition> existing =
				readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
		if (existing.size() >= options_.maximum_constraint_definitions) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical constraint catalog reached its configured definition bound");
		}

		utils::OwnedStdVector<ConstraintDefinition> validationDefinitions = existing;
		validationDefinitions.push_back(definition);
		PageBackedConstraintMutationAdapter validationAdapter(
				snapshot, *native_graph_reads_, *btree_pages_, *metadata_,
				[this](const MetadataSnapshotPin &pinned, uint32_t token) {
					const std::optional<std::string> name = system_catalog_->findPropertyName(pinned, token);
					return name.value_or(std::string{});
				},
				[this](const MetadataSnapshotPin &pinned, std::string_view name) {
					const std::optional<uint64_t> token =
							system_catalog_->findSchemaToken(pinned, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
					return token.value_or(0U);
				},
				[this](const MetadataSnapshotPin &pinned, std::string_view name) {
					const std::optional<uint64_t> token = system_catalog_->findSchemaToken(
							pinned, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
					return token.value_or(0U);
				}, selectConstraintProperties(validationDefinitions, [&](std::string_view name) -> std::optional<uint32_t> {
					const auto staged = requestedPropertyTokens.find(name);
					return staged != requestedPropertyTokens.end() ? std::optional<uint32_t>{staged->second}
						: system_catalog_->findPropertyToken(snapshot, name);
				}), checkpoint, propertyAdmission);
		ConstraintMutationValidator validator(
				validationAdapter.makeAdapter(),
				{options_.maximum_online_index_entries, options_.maximum_constraint_support_collisions});
		static_cast<void>(validator.validate(existing, {}, std::span(&definition, 1U)));

		NativeConstraintSupportTreeBuildOptions supportTree;
		if (requiresConstraintSupportTree(definition)) {
			if (options_.maximum_online_index_entries == 0U || options_.maximum_online_index_pages == 0U ||
				options_.online_index_sort_run_bytes == 0U || options_.maximum_online_index_sort_runs < 2U) {
				throw std::invalid_argument(
						"relocatable unique logical constraint creation requires bounded support-tree resources");
			}
			supportTree = {PhysicalSlotId(1U),
						   PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
						   options_.maximum_online_index_entries,
						   options_.maximum_online_index_pages,
						   options_.online_index_sort_run_bytes,
						   options_.maximum_online_index_sort_runs,
						   options_.maximum_constraint_support_collisions};
		}
		supportTree.checkpoint = checkpoint;
		supportTree.property_admission = propertyAdmission;
		NativeConstraintDefinitionWriter::stageCreate(
				transaction, *system_catalog_, catalogSession, *btree_pages_, *metadata_, definition,
				*native_graph_reads_, supportTree,
				[this, &requestedPropertyTokens](const MetadataSnapshotPin &pinned,
												 std::string_view name) -> std::optional<uint32_t> {
					const auto staged = requestedPropertyTokens.find(name);
					if (staged != requestedPropertyTokens.end()) {
						return staged->second;
					}
					return system_catalog_->findPropertyToken(pinned, name);
				});
		if (checkpoint) checkpoint();
		catalogSession.finish();
		if (checkpoint) checkpoint();
        auto result = logicalConstraintDefinition(definition, request.label);
		const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
		if (!committed.committed()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical constraint lost its native publication fence");
		}
		return result;
	}

	bool RelocatableStorageBackend::dropLogicalConstraintByName(std::string_view name) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot drop a logical constraint");
		}
		if (name.empty()) {
			throw std::invalid_argument("relocatable logical constraint drop requires a nonempty name");
		}
		requireNoIndeterminateWrite();
        auto writeWorkspace = ddlWriteWorkspace(resource_domain_);
        utils::OwnedAllocationScope writeAllocation(writeWorkspace);
		std::lock_guard<std::mutex> lock(native_publication_mutex_);
		NativeGraphWriteTransaction transaction = native_graph_write_planner_->beginTransaction();
		const std::optional<ConstraintDefinition> canonical =
				system_catalog_->findConstraintDefinitionByName(transaction.baseSnapshot(), name);
		if (canonical.has_value()) {
			SystemCatalogWriteSession catalogSession = system_catalog_->beginWrite(transaction);
			NativeConstraintDefinitionWriter::stageDrop(transaction, *system_catalog_, catalogSession, *btree_pages_,
														*metadata_, *canonical, options_.maximum_online_index_pages);
			catalogSession.finish();
			const NativeGraphWriteCommitResult committed = commitNativeGraphWriteTransaction(transaction);
			if (!committed.committed()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable canonical constraint drop lost its native publication fence");
			}
			return true;
		}
		transaction.rollback();
		return false;
	}

    std::vector<LogicalGraphConstraintDefinition>
    RelocatableStorageBackend::listLogicalConstraints(size_t maximumConstraints) const {
        return listOwnedLogicalConstraints(maximumConstraints);
    }

    utils::OwnedStdVector<LogicalGraphConstraintDefinition>
    RelocatableStorageBackend::listOwnedLogicalConstraints(size_t maximumConstraints) const {
		requireOpen();
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
        utils::OwnedAllocationScope allocation(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner()
            : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		const utils::OwnedStdVector<ConstraintDefinition> definitions =
				readAllConstraintDefinitions(snapshot, maximumConstraints);
		utils::OwnedStdVector<LogicalGraphConstraintDefinition> result;
		result.reserve(definitions.size());
		for (const ConstraintDefinition &definition: definitions) {
			const SystemCatalogSchemaTokenKind schemaKind =
					definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
							? SystemCatalogSchemaTokenKind::SCSTK_LABEL
							: SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
			const auto schema =
					system_catalog_->findOwnedSchemaToken(snapshot, schemaKind, definition.schema_token_id);
			if (!schema.has_value() || schema->name.empty()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable logical constraint list found an unknown native schema Token");
			}
			result.push_back(logicalConstraintDefinition(definition, schema->name));
		}
		return result;
	}

	ScalarIndexTreeReclaimReport
	RelocatableStorageBackend::reclaimObsoleteScalarIndexTrees(const ScalarIndexTreeReclaimOptions &options,
															   const MetadataPublishOptions &publishOptions) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim obsolete scalar Index trees");
		}
		requireNoIndeterminateWrite();
        const size_t workspaceBytes = options_.maximum_reclaim_workspace_bytes;
        auto account = std::make_shared<query::execution::detail::ResourceAccount>(
            resource_domain_, query::execution::detail::ResourceClass::RCK_MAINTENANCE, workspaceBytes);
        auto workspace = std::make_shared<query::execution::detail::PrepaidMemoryResource>(
            std::move(account), workspaceBytes, std::pmr::new_delete_resource());
        utils::OwnedAllocationScope allocationScope(workspace);
        StorageIO::SequentialReadScope sequentialReads(*data_file_->io());
		ScalarIndexTreeReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const Superblock publicationBase = snapshot.superblock();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable scalar Index-tree reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			ScalarIndexTreeReachabilityReclaimer reclaimer(*btree_pages_, *system_catalog_, *vector_manifests_,
														   *metadata_);
			try {
				report = reclaimer.reclaim(options, checkpointLsn, publishOptions);

			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
		}
		return report;
	}

	VectorIndexStateReclaimReport
	RelocatableStorageBackend::reclaimObsoleteVectorIndexState(const VectorIndexStateReclaimOptions &options,
															   const MetadataPublishOptions &publishOptions) {
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim obsolete Vector Index state");
		}
		requireNoIndeterminateWrite();
        const size_t workspaceBytes = options_.maximum_reclaim_workspace_bytes;
        auto account = std::make_shared<query::execution::detail::ResourceAccount>(
            resource_domain_, query::execution::detail::ResourceClass::RCK_MAINTENANCE, workspaceBytes);
        auto workspace = std::make_shared<query::execution::detail::PrepaidMemoryResource>(
            std::move(account), workspaceBytes, std::pmr::new_delete_resource());
        utils::OwnedAllocationScope allocationScope(workspace);
        StorageIO::SequentialReadScope sequentialReads(*data_file_->io());
		VectorIndexStateReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const Superblock publicationBase = snapshot.superblock();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable Vector Index reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			VectorAdjacencyPageStore adjacency(data_file_->io(), *metadata_,
											   SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
			VectorIndexStateReclaimer reclaimer(*physical_, *slotted_pages_, *btree_pages_, *vector_pages_, adjacency,
												*vector_manifests_, *system_catalog_, *metadata_);
			try {
				report = reclaimer.reclaim(options, checkpointLsn, publishOptions);
			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
		}
		return report;
	}

	LargeValueReachabilityReclaimReport
	RelocatableStorageBackend::reclaimUnreachableLargeValues(const LargeValueReachabilityReclaimOptions &options,
															 const MetadataPublishOptions &publishOptions) {
        debug::PerfTrace::addValue("native_graph.write.maintenance_reclaim_calls", 1);
		requireOpen();
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot reclaim unreachable LargeValues");
		}
		requireNoIndeterminateWrite();
        const size_t workspaceBytes = options_.maximum_reclaim_workspace_bytes;
        auto account = std::make_shared<query::execution::detail::ResourceAccount>(
            resource_domain_, query::execution::detail::ResourceClass::RCK_MAINTENANCE, workspaceBytes);
        auto workspace = std::make_shared<query::execution::detail::PrepaidMemoryResource>(
            std::move(account), workspaceBytes, std::pmr::new_delete_resource());
        utils::OwnedAllocationScope allocationScope(workspace);
        StorageIO::SequentialReadScope sequentialReads(*data_file_->io());
		LargeValueReachabilityReclaimReport report;
		std::lock_guard<std::mutex> publicationLock(native_publication_mutex_);
		{
			MetadataSnapshotPin snapshot = metadata_->openSnapshot();
			const Superblock publicationBase = snapshot.superblock();
			if (snapshot.superblock().checkpoint_lsn.value() == std::numeric_limits<uint64_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable LargeValue reclamation cannot advance the checkpoint LSN");
			}
			const LogSequenceNumber checkpointLsn(snapshot.superblock().checkpoint_lsn.value() + 1U);
			snapshot.release();
			LargeValueReachabilityReclaimer reclaimer(*large_values_, *slotted_pages_, *metadata_);
			try {
				report = reclaimer.reclaim(options, checkpointLsn, publishOptions);
                // An idle pass must retain its zero-write contract. Reclaim
                // metadata pools after this pass actually published pool/value
                // retirement, using the same protected maintenance account.
                if (report.retired_values != 0U || report.empty_pools_retired != 0U ||
                    report.retired_pools_released != 0U) {
                    auto after = metadata_->openSnapshot();
                    const uint64_t maintenanceLsn = after.superblock().checkpoint_lsn.value();
                    after.release();
                    if (maintenanceLsn != std::numeric_limits<uint64_t>::max()) {
                        const auto reclaimed = metadata_->reclaimEmptyMetadataPagePools(
                            LogSequenceNumber(maintenanceLsn + 1U), publishOptions);
                        report.published_generation = reclaimed.superblock.generation;
                    }
                }
			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
		}
		return report;
	}

	void RelocatableStorageBackend::pauseMaintenance() { requireMaintenance().pause(); }

	void RelocatableStorageBackend::resumeMaintenance() { requireMaintenance().resume(); }

	bool RelocatableStorageBackend::nativeMaintenanceEnabled() const {
		if (maintenance_ == nullptr) {
			return false;
		}
		return maintenance_->metrics().loop.state == RelocatableMaintenanceLoopState::VMLS_RUNNING;
	}

	void RelocatableStorageBackend::setNativeMaintenanceEnabled(const bool enabled) {
		if (enabled) {
			requireNoIndeterminateWrite();
		}
		RelocatableMaintenanceService &maintenance = requireMaintenance();
		const RelocatableMaintenanceLoopState state = maintenance.metrics().loop.state;
		if (enabled) {
			if (state == RelocatableMaintenanceLoopState::VMLS_RUNNING) {
				return;
			}
			if (state != RelocatableMaintenanceLoopState::VMLS_PAUSED) {
				throw std::logic_error("relocatable maintenance cannot be enabled outside a clean paused state");
			}
			maintenance.resume();
			return;
		}
		if (state == RelocatableMaintenanceLoopState::VMLS_PAUSED ||
			state == RelocatableMaintenanceLoopState::VMLS_FAULTED) {
			return;
		}
		if (state != RelocatableMaintenanceLoopState::VMLS_RUNNING) {
			throw std::logic_error("relocatable maintenance cannot be disabled outside an active lifecycle state");
		}
		maintenance.pause();
	}

	size_t RelocatableStorageBackend::cancelQueuedMaintenance() { return requireMaintenance().cancelQueuedTasks(); }

	uint64_t RelocatableStorageBackend::enqueuePropertyPageCompaction(SegmentId source, SegmentId destination,
																	  uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueuePropertyPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueBTreePageCompaction(SegmentId source, SegmentId destination,
																   uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueBTreePageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueVectorPageCompaction(SegmentId source, SegmentId destination,
																	uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueVectorPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueVectorManifestCompaction(SegmentId source, SegmentId destination,
																		uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueVectorManifestCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueEntityPageCompaction(SegmentId source, SegmentId destination,
																	uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueEntityPageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueLargeValuePageCompaction(SegmentId source, SegmentId destination, uint32_t pageBudget) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueLargeValuePageCompaction(source, destination, pageBudget);
	}

	uint64_t RelocatableStorageBackend::enqueueLargeValueReachabilityReclamation(
			const LargeValueReachabilityReclaimOptions &options) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueLargeValueReachabilityReclamation(options);
	}

	uint64_t RelocatableStorageBackend::enqueueCodebookExtentRelocation(uint64_t codebookId, ExtentId extentId,
																		PhysicalSlotId firstDestination,
																		PhysicalSlotId lastDestination) {
		requireNoIndeterminateWrite();
		return requireMaintenance().enqueueCodebookExtentRelocation(codebookId, extentId, firstDestination,
																	lastDestination);
	}

	RelocatableMaintenanceMetrics RelocatableStorageBackend::maintenanceMetrics() const {
		return requireMaintenance().metrics();
	}

	std::exception_ptr RelocatableStorageBackend::maintenanceFailure() const {
		return requireMaintenance().lastFailure();
	}

	RelocatableIntegrityReport RelocatableStorageBackend::verifyIntegrity() const {
		requireOpen();
		// Admit outside metadata publication; scanner allocations use local grant
        // accounting and cannot acquire the process coordinator under its gate.
        const size_t workspaceBytes = options_.maximum_integrity_workspace_bytes;
        auto account = std::make_shared<query::execution::detail::ResourceAccount>(
            resource_domain_, query::execution::detail::ResourceClass::RCK_MAINTENANCE, workspaceBytes);
        auto workspace = std::make_shared<query::execution::detail::PrepaidMemoryResource>(
            std::move(account), workspaceBytes, std::pmr::new_delete_resource());
        utils::OwnedAllocationScope allocationScope(workspace);
        StorageIO::SequentialReadScope sequentialReads(*data_file_->io());
        const RelocatableIntegrityReport report = RelocatableIntegrityScanner(data_file_->io(), *metadata_, workspace).scan();
		const MetadataSnapshotPin snapshot = metadata_->openSnapshot();
		verifyConstraintSupportTrees(snapshot);
		return report;
	}

	void RelocatableStorageBackend::verifyConstraintSupportTrees(const MetadataSnapshotPin &snapshot) const {
		if (options_.maximum_constraint_definitions == 0U || options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_entries == std::numeric_limits<size_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native constraint support-tree integrity verification requires finite "
								  "positive bounds");
		}
		const utils::OwnedStdVector<ConstraintDefinition> definitions =
				readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
		for (const ConstraintDefinition &definition: definitions) {
			if (!requiresConstraintSupportTree(definition))
				continue;
			const std::optional<IndexRootLocation> root =
					metadata_->findIndexRootOptional(snapshot, definition.constraint_id);
			if (!root.has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable integrity verification found an active uniqueness constraint "
									  "without a support-tree root");
			}
			const SystemCatalogSchemaTokenKind schemaKind =
					definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE
							? SystemCatalogSchemaTokenKind::SCSTK_LABEL
							: SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
			const std::optional<SystemCatalogSchemaToken> schema =
					system_catalog_->findSchemaToken(snapshot, schemaKind, definition.schema_token_id);
			if (!schema.has_value() || schema->name.empty()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable integrity verification found a constraint with an unknown native schema Token");
			}
			std::vector<uint32_t> propertyTokens;
			propertyTokens.reserve(definition.property_names.size());
			bool propertyKeysAssigned = true;
			for (const std::string &name: definition.property_names) {
				const std::optional<uint32_t> token = system_catalog_->findPropertyToken(snapshot, name);
				if (!token.has_value()) {
					propertyKeysAssigned = false;
					break;
				}
				propertyTokens.push_back(*token);
			}
			std::map<std::vector<std::byte>, uint64_t> expectedEntries;
			std::set<std::vector<std::byte>> semanticKeys;
			bool scopedEntityExists = false;
			const auto add = [&](uint64_t entityId, const std::vector<PropertyCollectionRootEntry> &properties) {
				scopedEntityExists = true;
				if (!propertyKeysAssigned) {
					return;
				}
				std::vector<PropertyValue> values;
				values.reserve(propertyTokens.size());
				for (const uint32_t token: propertyTokens) {
					const auto entry =
							std::ranges::lower_bound(properties, token, {}, &PropertyCollectionRootEntry::key_token);
					const PropertyValue *value =
							entry == properties.end() || entry->key_token != token ? nullptr : &entry->value;
					if (value == nullptr || value->getType() == PropertyType::NULL_TYPE) {
						if (definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY) {
							throw FormatException(
									FormatError::FERR_INVALID_STATE,
									"relocatable integrity verification found a Node KEY entity with a null Property");
						}
						return;
					}
					values.push_back(*value);
				}
				const std::vector<std::byte> canonical = ConstraintSupportTree::canonicalKey(values);
				if (!semanticKeys.insert(canonical).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable integrity verification found duplicate active uniqueness semantics");
				}
				const std::vector<std::byte> entry = ConstraintSupportTree::entryKey(canonical, entityId);
				if (!expectedEntries.emplace(entry, entityId).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable integrity verification found duplicate constraint support-tree "
										  "entry identities");
				}
			};
			size_t scannedEntities = 0U;
			const auto count = [&scannedEntities, this]() {
				if (++scannedEntities > options_.maximum_online_index_entries) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable native constraint support-tree scan exceeded its Entity bound");
				}
			};
			const size_t scanBound = checkedBoundPlusOne(options_.maximum_online_index_entries,
					"relocatable integrity verification scan bound overflowed");
			if (definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE) {
				static_cast<void>(
						native_graph_reads_->visitNodesUntil(snapshot, scanBound, [&](const PageBackedGraphNode &node) {
							count();
							if (std::ranges::find(node.label_token_ids, definition.schema_token_id) !=
								node.label_token_ids.end()) {
								add(node.node_id, node.properties);
							}
							return true;
						}));
			} else {
				static_cast<void>(
						native_graph_reads_->visitEdgesUntil(snapshot, scanBound, [&](const PageBackedGraphEdge &edge) {
							count();
							if (edge.type_id == definition.schema_token_id) {
								add(edge.edge_id, edge.properties);
							}
							return true;
						}));
			}
			if (!propertyKeysAssigned && definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY &&
				scopedEntityExists) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable integrity verification found a Node KEY whose Property-key Token is absent");
			}
			const utils::OwnedStdVector<BTreePageEntry> actual =
					BTreeIndex(definition.constraint_id, *btree_pages_, *metadata_)
							.scan(snapshot, std::nullopt, checkedBoundPlusOne(options_.maximum_online_index_entries,
									"relocatable integrity verification index bound overflowed"));
			if (actual.size() > options_.maximum_online_index_entries || actual.size() != expectedEntries.size()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable integrity verification found a constraint support-tree entry-count mismatch");
			}
			auto expected = expectedEntries.begin();
			for (const BTreePageEntry &entry: actual) {
				if (expected == expectedEntries.end() || entry.key != expected->first ||
					entry.value_or_child != expected->second ||
					ConstraintSupportTree::entryEntityId(entry.key) != entry.value_or_child) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable integrity verification found a constraint support-tree entry mismatch");
				}
				++expected;
			}
		}
	}

	void RelocatableStorageBackend::requireOpen() const {
		if (!isOpen()) {
			throw std::logic_error("relocatable backend is not open");
		}
	}

	RelocatableMaintenanceService &RelocatableStorageBackend::requireMaintenance() {
		requireOpen();
		if (maintenance_ == nullptr) {
			throw std::logic_error("relocatable backend was not opened with background maintenance enabled");
		}
		return *maintenance_;
	}

	const RelocatableMaintenanceService &RelocatableStorageBackend::requireMaintenance() const {
		requireOpen();
		if (maintenance_ == nullptr) {
			throw std::logic_error("relocatable backend was not opened with background maintenance enabled");
		}
		return *maintenance_;
	}

	void RelocatableStorageBackend::requireOwnedSnapshot(const RelocatableReadSnapshot &snapshot) const {
		requireOpen();
		if (snapshot.backend_ != this) {
			throw std::invalid_argument("relocatable backend snapshot does not belong to this open backend");
		}
	}

	std::string RelocatableStorageBackend::resolveNodeLabelToken(const MetadataSnapshotPin &snapshot,
																 uint64_t tokenId) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid Node-label resolver state");
		}
		std::optional<SystemCatalogSchemaToken> token =
				system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, tokenId);
		if (!token.has_value() || token->name.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown Node-label Token");
		}
		return std::move(token->name);
	}

	std::string RelocatableStorageBackend::resolveRelationshipTypeToken(const MetadataSnapshotPin &snapshot,
																		uint64_t tokenId) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid relationship-type resolver state");
		}
		auto token = system_catalog_->findSchemaToken(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId);
		if (!token.has_value() || token->name.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown relationship-type Token");
		}
		return std::move(token->name);
	}
	std::string RelocatableStorageBackend::resolveRelationshipTypeTokenWithNameAdmission(const MetadataSnapshotPin &snapshot,
																		uint64_t tokenId, const std::function<void(size_t)> &admission) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid relationship-type resolver state");
		}
		auto token = system_catalog_->findSchemaToken(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId, admission);
		if (!token.has_value() || token->name.empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown relationship-type Token");
		}
		return std::move(token->name);
	}

	std::string RelocatableStorageBackend::resolvePropertyKey(const MetadataSnapshotPin &snapshot,
															  uint32_t tokenId) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid Property-key resolver state");
		}
		std::optional<std::string> resolved = system_catalog_->findPropertyName(snapshot, tokenId);
		if (!resolved.has_value() || resolved->empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown Property-key Token: " +
										  std::to_string(tokenId));
		}
		return std::move(*resolved);
	}

	void RelocatableStorageBackend::prefetchPropertyKeyNames(const MetadataSnapshotPin &snapshot,
		std::span<const uint32_t> tokens, const std::function<void(uint32_t, std::string_view)> &consume) const {
		const auto names = system_catalog_->findPropertyNamesSorted(snapshot, tokens);
		// Validate the complete batch before publishing any optional cache entry.
		for (size_t i = 0U; i < names.size(); ++i)
			if (!names[i] || names[i]->empty())
				throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable native logical reader found an unknown Property-key Token: " + std::to_string(tokens[i]));
		for (size_t i = 0U; i < names.size(); ++i)
			consume(tokens[i], std::string_view(names[i]->data(), names[i]->size()));
	}

	std::string RelocatableStorageBackend::resolvePropertyKeyWithNameAdmission(const MetadataSnapshotPin &snapshot,
															  uint32_t tokenId, const std::function<void(size_t)> &admission) const {
		if (tokenId == 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader has an invalid Property-key resolver state");
		}
		auto resolved = system_catalog_->findPropertyName(snapshot, tokenId, admission);
		if (!resolved.has_value() || resolved->empty()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native logical reader found an unknown Property-key Token: " +
										  std::to_string(tokenId));
		}
		return std::move(*resolved);
	}

	std::optional<uint32_t> RelocatableStorageBackend::resolvePropertyToken(const MetadataSnapshotPin &snapshot,
																							std::string_view name) const {
		if (name.empty()) {
			return std::nullopt;
		}
		debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
		return system_catalog_->findPropertyToken(snapshot, name);
	}

	std::optional<uint64_t> RelocatableStorageBackend::resolveNodeLabelName(const MetadataSnapshotPin &snapshot,
																						  std::string_view name) const {
		if (name.empty()) {
			return std::nullopt;
		}
		debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
		return system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
	}

	void RelocatableStorageBackend::releaseResources() noexcept {
		if (maintenance_ != nullptr) {
			try {
				maintenance_->stop();
			} catch (...) {
				std::terminate();
			}
		}
		maintenance_.reset();
		native_graph_mutation_executor_.reset();
		native_logical_graph_reads_.reset();
		native_graph_reads_.reset();
		native_graph_write_planner_.reset();
		vector_manifests_.reset();
		vector_pages_.reset();
		properties_.reset();
		large_values_.reset();
		native_labels_.reset();
		native_entities_.reset();
		system_catalog_.reset();
		btree_pages_.reset();
		slotted_pages_.reset();
		vector_codebooks_.reset();
		physical_.reset();
		metadata_.reset();
		data_file_.reset();
        database_file_limit_.reset();
        native_cache_memory_.reset();
		resource_domain_.reset();
	}
