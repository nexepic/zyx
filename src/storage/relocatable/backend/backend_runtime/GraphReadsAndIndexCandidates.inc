	LogicalGraphNode RelocatableStorageBackend::readNode(const RelocatableReadSnapshot &snapshot,
														 uint64_t nodeId) const {
		requireOwnedSnapshot(snapshot);
		return native_logical_graph_reads_->readNodeShaped(
				snapshot.pin_, nodeId, {.include_labels = true, .all_properties = true});
	}

	LogicalGraphEdge RelocatableStorageBackend::readEdge(const RelocatableReadSnapshot &snapshot,
														 uint64_t edgeId) const {
		requireOwnedSnapshot(snapshot);
		return native_logical_graph_reads_->readEdgeShaped(
				snapshot.pin_, edgeId, {.include_type = true, .all_properties = true});
	}

	bool RelocatableStorageBackend::containsLogicalNode(const RelocatableReadSnapshot &snapshot,
																																																				 uint64_t nodeId, NativeReadSession *session) const {
		requireOwnedSnapshot(snapshot);
		if (nodeId == 0U) {
			return false;
		}
		if (session != nullptr) {
			session->bind(snapshot.pin_);
			if (const auto cached = session->nodeExists(nodeId); cached.has_value()) {
				return *cached;
			}
		}
		const bool exists = metadata_->findNodePageLocationOptional(snapshot.pin_, nodeId).has_value();
		if (session != nullptr) {
			session->putNodeExists(nodeId, exists);
		}
		return exists;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeEdgeTypeCandidates(
			const MetadataSnapshotPin &snapshot, uint64_t typeToken, size_t maximumRecords,
			const std::function<bool(const EntityRef &)> &visitor, NativeReadSession *session) const {
		if (!visitor || maximumRecords == 0U) {
			throw std::invalid_argument("native type candidate access requires a visitor and positive bound");
		}
		// Historical member trees omit untyped relationships. A zero token must
		// therefore scan the primary records, never produce a false empty result.
		if (typeToken == 0U || typeToken > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
		if (session != nullptr) session->bind(snapshot);
		const auto estimate = system_catalog_->readTokenCardinalityEstimate(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, typeToken);
		if (estimate && !NativeMembershipAccessCost::preferCandidates(
				estimate->first.edge_count, estimate->second, maximumRecords, true)) return std::nullopt;
		const auto *definitions = session == nullptr ? nullptr :
				session->indexDefinitions(options_.maximum_index_definitions);
		utils::OwnedStdVector<IndexDefinition> loaded;
		if (definitions == nullptr) {
			loaded = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) session->putIndexDefinitions(loaded);
			definitions = &loaded;
		}
		for (const auto &definition : *definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_EDGE ||
					definition.kind != IndexDefinitionKind::IDK_LABEL ||
					!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) continue;
			BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			const PropertyValue value(static_cast<int64_t>(typeToken));
			const auto lower = IndexKeyCodec::lowerBound(value);
			const auto upper = IndexKeyCodec::upperBound(value);
			const size_t cursorBound = maximumRecords == std::numeric_limits<size_t>::max() ?
					maximumRecords : maximumRecords + 1U;
			auto cursor = index.openRangeCursor(snapshot, std::span<const std::byte>(lower), cursorBound);
			uint64_t previous = 0U;
			size_t visited = 0U;
			std::vector<uint64_t> ids;
			ids.reserve(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES);
			const auto emit = [&] {
				if (ids.empty()) return true;
				const auto records = native_graph_reads_->readEdgeTopologiesSorted(snapshot, ids, session);
				for (size_t i = 0U; i < ids.size(); ++i) {
					const auto &record = records[i];
					if (record.type_id != typeToken) throw FormatException(FormatError::FERR_INVALID_STATE,
							"native type Index disagrees with the primary Edge type");
					const EntityRef reference{ids[i], true, record.source_node_id, record.target_node_id, record.type_id};
					debug::PerfTrace::addValue("native_graph.read.type_index_candidates", 1);
					if (!visitor(reference)) return false;
				}
				ids.clear(); return true;
			};
			while (auto entry = cursor.next()) {
				if (entry->key > upper) break;
				if (entry->key.size() != lower.size() ||
						!std::equal(lower.begin(), lower.end() - sizeof(uint64_t), entry->key.begin())) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "native type Index has a malformed key");
				}
				const uint64_t id = IndexKeyCodec::decodeEntryEntityId(entry->key);
				if (id <= previous || entry->value_or_child != id) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "native type Index has an invalid Edge ID");
				}
				if (visited == maximumRecords) {
					if (!emit()) return false;
					throw std::runtime_error("native type candidate access exceeded its record bound");
				}
				++visited; previous = id; ids.push_back(id);
				if ((ids.size() == BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES || visited == maximumRecords) && !emit()) return false;
			}
			return emit();
		}
		return std::nullopt;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeLabelCandidates(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			size_t maximumRecords, const std::function<bool(const EntityRef &, bool)> &visitor,
			NativeReadSession *session) const {
		if (!visitor || maximumRecords == 0U) {
			throw std::invalid_argument("native label candidate access requires a visitor and positive bound");
		}
		if (labels.empty()) return std::nullopt;
		if (session != nullptr) session->bind(snapshot);
		std::optional<uint64_t> token;
		std::string_view primaryLabel = labels.front();
		std::optional<uint64_t> smallestCount;
		const bool estimateDriver = labels.size() > 1U && system_catalog_->hasCompleteTokenCardinality(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL);
		for (const auto &label : labels) {
			if (label.empty()) return true;
			std::optional<uint64_t> resolved;
			const auto cached = session == nullptr ? std::nullopt : session->nodeLabelToken(label);
			if (cached.has_value()) resolved = *cached;
			else {
				resolved = system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, label);
				if (session != nullptr) session->putNodeLabelToken(label, resolved);
			}
			if (!resolved.has_value()) return true;
			if (!token.has_value()) token = resolved;
			if (estimateDriver) {
				const auto count = system_catalog_->readTokenCardinality(
						snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *resolved);
				if (count && (!smallestCount || *count < *smallestCount)) {
					smallestCount = count; token = resolved; primaryLabel = label;
				}
			}
		}
		if (*token > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return std::nullopt;
		const auto estimate = system_catalog_->readTokenCardinalityEstimate(
				snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, *token);
        const bool countsPreferScan = estimate && !NativeMembershipAccessCost::preferNodeCandidates(
            estimate->first.node_count, estimate->second, maximumRecords);
        // An all-node label cannot save decoding and adds an index walk. For
        // intermediate selectivity, inspect a bounded prefix before deciding.
        if (countsPreferScan && estimate->first.node_count == estimate->second) {
            debug::PerfTrace::addValue("native_graph.read.label_access.scan", 1);
            return std::nullopt;
        }
        bool samplingPending = countsPreferScan;
        try {
		const auto *definitions = session == nullptr ? nullptr :
				session->indexDefinitions(options_.maximum_index_definitions);
		utils::OwnedStdVector<IndexDefinition> loaded;
		if (definitions == nullptr) {
			loaded = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) session->putIndexDefinitions(loaded);
			definitions = &loaded;
		}
		for (const auto &definition : *definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
					definition.kind != IndexDefinitionKind::IDK_LABEL ||
					!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) continue;
			BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			const PropertyValue value(static_cast<int64_t>(*token));
			const auto lower = IndexKeyCodec::lowerBound(value);
			const auto upper = IndexKeyCodec::upperBound(value);
			const size_t cursorBound = maximumRecords == std::numeric_limits<size_t>::max() ?
					maximumRecords : maximumRecords + 1U;
			auto cursor = index.openRangeCursor(snapshot, std::span<const std::byte>(lower), cursorBound);
            // Multiple-label verification revisits the authenticated records.
            // Keep the whole batch within the existing session bound, avoiding
            // a second Entity decode after the primary-label pass evicts it.
            const size_t membershipBatchRows = labels.size() == 1U ? BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES :
                std::min(BTreeIndex::DEFAULT_CURSOR_BATCH_ENTRIES, NativeReadSession::MAXIMUM_CACHED_ENTITY_RECORDS);
			utils::OwnedStdVector<uint64_t> ids;
			ids.reserve(membershipBatchRows);
			uint64_t previous = 0U;
			size_t visited = 0U;
            const auto nextId = [&]() -> std::optional<uint64_t> {
                auto entry = cursor.next();
                if (!entry || entry->key > upper) return std::nullopt;
                if (entry->key.size() != lower.size() ||
                    !std::equal(lower.begin(), lower.end() - sizeof(uint64_t), entry->key.begin()))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native label Index has a malformed key");
                const auto id = IndexKeyCodec::decodeEntryEntityId(entry->key);
                if (id <= previous || entry->value_or_child != id)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "native label Index has an invalid Node ID");
                previous = id;
                return id;
            };
            bool exhaustedPrefix = false;
            if (countsPreferScan) {
                constexpr size_t localitySampleRows = 64U;
                const auto sampleBound = std::min(maximumRecords, localitySampleRows);
                while (ids.size() < sampleBound) {
                    const auto id = nextId();
                    if (!id) { exhaustedPrefix = true; break; }
                    ++visited; ids.push_back(*id);
                }
                if (!ids.empty()) {
                    try {
                        utils::OwnedStdVector<std::optional<EntityPageLocation>> locations(ids.size());
                        metadata_->findNodePageLocationsSorted(snapshot, ids, locations);
                        utils::OwnedSet<PageId> pages;
                        for (size_t i = 0U; i < locations.size(); ++i) {
                            if (!locations[i] || locations[i]->entity_id != ids[i] || !locations[i]->page_id.isValid())
                                throw FormatException(FormatError::FERR_INVALID_STATE,
                                    "native label Index references an unavailable primary Node");
                            pages.emplace(locations[i]->page_id);
                        }
                        debug::PerfTrace::addValue("native_graph.read.label_access.sample_rows", ids.size());
                        debug::PerfTrace::addValue("native_graph.read.label_access.sample_pages", pages.size());
                        if (!NativeMembershipAccessCost::preferNodeCandidates(estimate->first.node_count,
                                estimate->second, maximumRecords, ids.size(), pages.size(), lower.size(), membershipBatchRows)) {
                            debug::PerfTrace::addValue("native_graph.read.label_access.scan", 1);
                            return std::nullopt;
                        }
                    } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                        // Cost sampling is optional, before any visitor emission.
                        debug::PerfTrace::addValue("native_graph.read.label_access.sample_admission_fallback", 1);
                        return std::nullopt;
                    }
                }
            }
            samplingPending = false; // Never restart a scan after emitting a candidate.
            debug::PerfTrace::addValue("native_graph.read.label_access.index", 1);
			const auto emit = [&]() {
				if (ids.empty()) return true;
				const auto primary = native_logical_graph_reads_->readNodeLabelMatchesSorted(
						snapshot, ids, {std::string(primaryLabel)}, session);
				const auto matches = labels.size() == 1U ? primary :
						native_logical_graph_reads_->readNodeLabelMatchesSorted(snapshot, ids, labels, session);
				for (size_t i = 0U; i < ids.size(); ++i) {
					if (!primary[i]) throw FormatException(FormatError::FERR_INVALID_STATE,
							"native label Index disagrees with the primary Node label set");
					// The label batch has already authenticated the primary Entity record.
					const EntityRef reference{ids[i], false, 0U, 0U, 0U};
					debug::PerfTrace::addValue("native_graph.read.label_index_candidates", 1);
					if (!visitor(reference, matches[i])) return false;
				}
				ids.clear();
				return true;
			};
            while (!exhaustedPrefix) {
                const auto id = nextId();
                if (!id) break;
                if (visited == maximumRecords) {
                    if (!emit()) return false;
                    throw std::runtime_error("native label candidate access exceeded its record bound");
                }
                ++visited; ids.push_back(*id);
                if ((ids.size() == membershipBatchRows || visited == maximumRecords) && !emit()) return false;
            }
			return emit();
		}
        } catch (const query::execution::detail::ResourceAdmissionDenied &) {
            if (!samplingPending) throw;
            debug::PerfTrace::addValue("native_graph.read.label_access.sample_admission_fallback", 1);
        }
		return std::nullopt;
	}

	std::vector<bool> RelocatableStorageBackend::containsLogicalNodesSorted(
			const RelocatableReadSnapshot &snapshot, const std::span<const uint64_t> nodeIds,
			NativeReadSession *session) const {
		requireOwnedSnapshot(snapshot);
		std::vector<bool> result(nodeIds.size(), false);
		if (nodeIds.empty()) {
			return result;
		}
		uint64_t previousNodeId = 0U;
		for (const uint64_t nodeId : nodeIds) {
			if (nodeId == 0U || nodeId <= previousNodeId) {
				throw std::invalid_argument(
						"relocatable sorted Node existence read requires strictly increasing Node IDs");
			}
			previousNodeId = nodeId;
		}
		if (session != nullptr) {
			session->bind(snapshot.pin_);
		}
		std::vector<uint64_t> missingIds;
		std::vector<size_t> missingOrdinals;
		missingIds.reserve(nodeIds.size());
		missingOrdinals.reserve(nodeIds.size());
		for (size_t index = 0U; index < nodeIds.size(); ++index) {
			if (session != nullptr) {
				if (const auto cached = session->nodeExists(nodeIds[index]); cached.has_value()) {
					result[index] = *cached;
					continue;
				}
			}
			missingIds.push_back(nodeIds[index]);
			missingOrdinals.push_back(index);
		}
		if (missingIds.empty()) {
			return result;
		}
		debug::PerfTrace::addValue("native_graph.read.node_existence_batch_requests", 1);
		debug::PerfTrace::addValue("native_graph.read.node_existence_batch_ids",
				static_cast<int64_t>(missingIds.size()));
		std::vector<std::optional<EntityPageLocation>> locations(missingIds.size());
		metadata_->findNodePageLocationsSorted(snapshot.pin_, missingIds, locations);
		for (size_t index = 0U; index < missingIds.size(); ++index) {
			result[missingOrdinals[index]] = locations[index].has_value();
			if (session != nullptr) {
				session->putNodeExists(missingIds[index], result[missingOrdinals[index]]);
			}
		}
		return result;
	}

	std::optional<IndexCandidateBatch> RelocatableStorageBackend::findNativeNodeCandidatesByExactProperties(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
			size_t maximumPropertyEntries, NativeReadSession *session, IndexCandidateBatchVisitor *visitor,
			bool *visitorCompleted, size_t maximumCandidateBytes) const {
		if (visitorCompleted != nullptr) {
			*visitorCompleted = true;
		}
		if (!snapshot.isValid() || maximumNodes == 0U || maximumPropertyEntries == 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable native exact Node lookup requires an active snapshot and positive bounds");
		}
		if (labels.size() != 1U || labels.front().empty() || properties.empty()) {
			return std::nullopt;
		}
		if (session != nullptr) {
			session->bind(snapshot);
		}
		const auto resolveLabelToken = [&]() {
			if (session != nullptr) {
				if (const auto cached = session->nodeLabelToken(labels.front()); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.index_candidate_label_token_resolution", 1);
			const std::optional<uint64_t> resolved =
					system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, labels.front());
			if (session != nullptr) {
				session->putNodeLabelToken(labels.front(), resolved);
			}
			return resolved;
		};
		const auto resolvePropertyToken = [&](const std::string &name) {
			if (session != nullptr) {
				if (const auto cached = session->propertyToken(name); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.index_candidate_property_token_resolution", 1);
			const std::optional<uint32_t> resolved = system_catalog_->findPropertyToken(snapshot, name);
			if (session != nullptr) {
				session->putPropertyToken(name, resolved);
			}
			return resolved;
		};
		std::map<std::string, const PropertyValue *> predicateValues;
		for (const auto &[name, value]: properties) {
			if (name.empty() || value.getType() == PropertyType::NULL_TYPE ||
				!predicateValues.emplace(name, &value).second) {
				return std::nullopt;
			}
		}
		const std::optional<uint64_t> labelToken = resolveLabelToken();
		if (!labelToken.has_value()) {
			IndexCandidateBatch empty;
			empty.snapshot_generation = snapshot.superblock().generation.value();
			empty.exact_match_verified = true;
			empty.label_scope_verified = true;
			return empty;
		}
		if (options_.maximum_constraint_support_collisions == 0U ||
			options_.maximum_constraint_support_collisions == std::numeric_limits<size_t>::max()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable native exact Node lookup requires a finite support-tree collision bound");
		}
		const std::vector<ConstraintDefinition> *cachedConstraints =
				session == nullptr ? nullptr : session->constraintDefinitions(options_.maximum_constraint_definitions);
		utils::OwnedStdVector<ConstraintDefinition> loadedConstraints;
		if (cachedConstraints == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.index_candidate_constraint_definition_read", 1);
			loadedConstraints = readAllConstraintDefinitions(snapshot, options_.maximum_constraint_definitions);
			if (session != nullptr) {
				session->putConstraintDefinitions(loadedConstraints);
				cachedConstraints = session->constraintDefinitions(options_.maximum_constraint_definitions);
			}
		}
		const std::vector<ConstraintDefinition> &definitions =
				cachedConstraints == nullptr ? loadedConstraints : *cachedConstraints;
		for (const ConstraintDefinition &definition: definitions) {
			const bool supportsExactLookup = definition.entity_kind == ConstraintDefinitionEntityKind::CDEK_NODE &&
											 (definition.kind == ConstraintDefinitionKind::CDK_UNIQUE ||
											  definition.kind == ConstraintDefinitionKind::CDK_NODE_KEY) &&
											 definition.schema_token_id == *labelToken;
			if (!supportsExactLookup || definition.property_names.size() != predicateValues.size()) {
				continue;
			}
			std::set<std::string> definitionNames(definition.property_names.begin(), definition.property_names.end());
			bool samePropertyNames = definitionNames.size() == definition.property_names.size();
			auto definitionName = definitionNames.begin();
			auto predicateName = predicateValues.begin();
			while (samePropertyNames && definitionName != definitionNames.end()) {
				if (predicateName == predicateValues.end() || *definitionName != predicateName->first) {
					samePropertyNames = false;
					break;
				}
				++definitionName;
				++predicateName;
			}
			if (!samePropertyNames || predicateName != predicateValues.end()) {
				continue;
			}

			std::vector<PropertyValue> requestedValues;
			std::vector<uint32_t> propertyTokens;
			requestedValues.reserve(definition.property_names.size());
			propertyTokens.reserve(definition.property_names.size());
			for (const std::string &name: definition.property_names) {
				const std::optional<uint32_t> token = resolvePropertyToken(name);
				if (!token.has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup found an unknown constrained Property key");
				}
				requestedValues.push_back(*predicateValues.at(name));
				propertyTokens.push_back(*token);
			}
			const std::vector<std::byte> requestedKey = ConstraintSupportTree::canonicalKey(requestedValues);
			if (!metadata_->findIndexRootOptional(snapshot, definition.constraint_id).has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable native exact Node lookup found a unique constraint without a support tree");
			}
			const BTreeIndex support(definition.constraint_id, *btree_pages_, *metadata_);
			const utils::OwnedStdVector<BTreePageEntry> candidates =
					support.scan(snapshot, ConstraintSupportTree::lowerBound(requestedKey),
								 checkedBoundPlusOne(options_.maximum_constraint_support_collisions,
										"relocatable native exact Node lookup collision bound overflowed"));
			size_t collisionCount = 0U;
			IndexCandidateBatch result;
			result.snapshot_generation = snapshot.superblock().generation.value();
			result.exact_match_verified = true;
			result.predicates_verified = true;
			result.label_scope_verified = true;
			for (const std::string &name: definition.property_names) {
				result.verified_property_columns.push_back({name, {}});
			}
			std::vector<uint64_t> candidateNodeIds;
			candidateNodeIds.reserve(candidates.size());
			for (const BTreePageEntry &candidate: candidates) {
				if (!ConstraintSupportTree::matchesDigest(candidate.key, requestedKey)) {
					break;
				}
				if (++collisionCount > options_.maximum_constraint_support_collisions) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup exceeded its support-tree collision bound");
				}
				const uint64_t nodeId = ConstraintSupportTree::entryEntityId(candidate.key);
				if (candidate.value_or_child != nodeId) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup found a support-tree entry with a mismatched value");
				}
				candidateNodeIds.push_back(nodeId);
			}
			if (candidateNodeIds.size() > maximumNodes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native exact Node lookup exceeded its caller-supplied result bound");
			}
			if (!candidateNodeIds.empty()) {
				std::ranges::sort(candidateNodeIds);
				if (std::ranges::adjacent_find(candidateNodeIds) != candidateNodeIds.end()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable native exact Node lookup found duplicate support-tree Entity IDs");
				}
				// The support tree is ordered by digest then Entity ID. Validate the
				// complete digest collision batch through one sorted Entity/Property
				// pipeline instead of repeating a Directory descent per candidate.
				// The batch API retains owner, label, Property count, token, and value
				// checks; this changes only their granularity.
				prepareExactCandidateColumns(result, candidateNodeIds.size(), requestedValues, maximumCandidateBytes);
				size_t sourceBytes = candidateBatchBytes(result);
				size_t sourceBuffers = 0U;
				const auto admission = exactCandidateSourceAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
				const auto bufferAdmission = exactCandidateBufferAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
				native_graph_reads_->validateNodeExactPropertiesSorted(
						snapshot, candidateNodeIds, definition.schema_token_id, propertyTokens, requestedValues,
						maximumPropertyEntries, session, &admission, &bufferAdmission);
				traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", candidateNodeIds.size());
				for (const uint64_t nodeId: candidateNodeIds) {
					result.candidates.push_back({nodeId, false, 0U, 0U, 0U});
					for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
						result.verified_property_columns[propertyIndex].values.push_back(requestedValues[propertyIndex]);
					}
				}
			}
			if (result.candidates.size() > 1U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable native exact Node lookup found duplicate unique-constraint entries");
			}
			if (visitor != nullptr) {
				// Unique-constraint candidates use the support tree path rather than
				// the ordinary Index cursor, so enforce the same producer-side byte
				// budget before exposing this batch to a storage-neutral consumer.
				enforceCandidateBatchBytes(result, maximumCandidateBytes);
				if (!(*visitor)(result) && visitorCompleted != nullptr) {
					*visitorCompleted = false;
				}
				return IndexCandidateBatch{};
			}
			return result;
		}

		const std::vector<IndexDefinition> *cachedIndexes =
				session == nullptr ? nullptr : session->indexDefinitions(options_.maximum_index_definitions);
		utils::OwnedStdVector<IndexDefinition> loadedIndexes;
		if (cachedIndexes == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.index_candidate_definition_read", 1);
			loadedIndexes = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) {
				session->putIndexDefinitions(loadedIndexes);
				cachedIndexes = session->indexDefinitions(options_.maximum_index_definitions);
			}
		}
		const std::vector<IndexDefinition> &indexDefinitions =
				cachedIndexes == nullptr ? loadedIndexes : *cachedIndexes;
		for (const IndexDefinition &definition: indexDefinitions) {
			const bool supportsExactLookup = definition.entity_kind == IndexDefinitionEntityKind::IDEK_NODE &&
											 (definition.kind == IndexDefinitionKind::IDK_PROPERTY ||
											  definition.kind == IndexDefinitionKind::IDK_COMPOSITE) &&
											 definition.label_token_id == *labelToken &&
											 definition.property_names.size() == predicateValues.size();
			if (!supportsExactLookup) {
				continue;
			}
			std::set<std::string> definitionNames(definition.property_names.begin(), definition.property_names.end());
			bool samePropertyNames = definitionNames.size() == definition.property_names.size();
			auto definitionName = definitionNames.begin();
			auto predicateName = predicateValues.begin();
			while (samePropertyNames && definitionName != definitionNames.end()) {
				if (predicateName == predicateValues.end() || *definitionName != predicateName->first) {
					samePropertyNames = false;
					break;
				}
				++definitionName;
				++predicateName;
			}
			if (!samePropertyNames || predicateName != predicateValues.end()) {
				continue;
			}

			std::vector<PropertyValue> requestedValues;
			std::vector<uint32_t> propertyTokens;
			requestedValues.reserve(definition.property_names.size());
			propertyTokens.reserve(definition.property_names.size());
			bool indexable = true;
			for (const std::string &name: definition.property_names) {
				const PropertyValue &value = *predicateValues.at(name);
				const PropertyType type = value.getType();
				if ((type != PropertyType::BOOLEAN && type != PropertyType::INTEGER && type != PropertyType::DOUBLE &&
					 type != PropertyType::STRING) ||
					(type == PropertyType::DOUBLE && !std::isfinite(std::get<double>(value.getVariant())))) {
					indexable = false;
					break;
				}
				const std::optional<uint32_t> token = resolvePropertyToken(name);
				if (!token.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable native exact Node lookup found an unknown indexed Property key");
				}
				requestedValues.push_back(value);
				propertyTokens.push_back(*token);
			}
			if (!indexable) {
				// The logical predicate is valid, but this bounded ordered index codec
				// deliberately omits non-scalar and non-finite values.
				return std::nullopt;
			}
			const std::optional<std::vector<std::byte>> entryKey =
					definition.kind == IndexDefinitionKind::IDK_PROPERTY
							? IndexKeyCodec::tryEncodeEntryKey(requestedValues.front(), 1U)
							: IndexKeyCodec::tryEncodeCompositeEntryKey(requestedValues, 1U);
			if (!entryKey.has_value()) {
				return std::nullopt;
			}
			std::vector<std::byte> valuePrefix = *entryKey;
			valuePrefix.resize(valuePrefix.size() - sizeof(uint64_t));
			if (!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable native exact Node lookup found an Index definition without a B+Tree root");
			}
			if (maximumNodes == std::numeric_limits<size_t>::max()) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable native exact Node lookup cannot extend its result bound");
			}
			const BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			if (visitor != nullptr) {
				size_t batchCapacity = candidateBatchCapacity(maximumNodes, maximumCandidateBytes);
				size_t keyBytes = 0U;
				for (const auto &name: definition.property_names) {
					if (name.capacity() > std::numeric_limits<size_t>::max() - keyBytes) {
						throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "relocatable native index candidate byte accounting overflowed");
					}
					keyBytes += name.capacity();
				}
				batchCapacity = std::min(batchCapacity, std::max<size_t>(1U,
					(maximumCandidateBytes - std::min(maximumCandidateBytes, keyBytes)) / exactCandidateRowBytes(requestedValues)));
				auto cursor = index.openRangeCursor(snapshot, std::optional(std::span<const std::byte>(*entryKey)),
						options_.maximum_online_index_entries, BufferPoolReadHint::BPRH_SEQUENTIAL);
				debug::PerfTrace::addValue("native_graph.read.index_candidate_root_descent", 1);
				utils::OwnedStdVector<BTreePageEntry> pending;
				pending.reserve(batchCapacity);
				size_t totalCandidates = 0U;
				const auto emit = [&]() -> bool {
					if (pending.empty()) {
						return true;
					}
					IndexCandidateBatch batch;
					batch.snapshot_generation = snapshot.superblock().generation.value();
					batch.exact_match_verified = true;
					batch.predicates_verified = true;
					batch.label_scope_verified = true;
					for (const std::string &name: definition.property_names) {
						batch.verified_property_columns.push_back({name, {}});
					}
					std::vector<uint64_t> nodeIds;
					nodeIds.reserve(pending.size());
					uint64_t previousNodeId = 0U;
					for (const BTreePageEntry &candidate: pending) {
						if (candidate.key.size() != valuePrefix.size() + sizeof(uint64_t) ||
								candidate.key.size() < valuePrefix.size() ||
								!std::equal(valuePrefix.begin(), valuePrefix.end(), candidate.key.begin())) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native exact Node lookup found a malformed scalar Index key");
						}
						const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
						if (nodeId <= previousNodeId || candidate.value_or_child != nodeId) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native exact Node lookup found a noncanonical scalar Index entry");
						}
						previousNodeId = nodeId;
						nodeIds.push_back(nodeId);
					}
					prepareExactCandidateColumns(batch, nodeIds.size(), requestedValues, maximumCandidateBytes);
					size_t sourceBytes = candidateBatchBytes(batch);
					size_t sourceBuffers = 0U;
					const auto admission = exactCandidateSourceAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
					const auto bufferAdmission = exactCandidateBufferAdmission(sourceBytes, sourceBuffers, maximumCandidateBytes);
					native_graph_reads_->validateNodeExactPropertiesSorted(snapshot, nodeIds, definition.label_token_id,
							propertyTokens, requestedValues, maximumPropertyEntries, session, &admission, &bufferAdmission);
					traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", nodeIds.size());
					for (const uint64_t nodeId: nodeIds) {
						batch.candidates.push_back({nodeId, false, 0U, 0U, 0U});
						for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
							batch.verified_property_columns[propertyIndex].values.push_back(requestedValues[propertyIndex]);
						}
					}
					pending.clear();
					traceNativeCandidateMetric("native_graph.read.index_candidate_batches", 1U);
					traceNativeCandidateMetric("native_graph.read.index_candidate_emitted_rows", batch.candidates.size());
					enforceCandidateBatchBytes(batch, maximumCandidateBytes);
					return (*visitor)(batch);
				};
			for (;;) {
				const std::optional<BTreePageEntry> candidate = cursor.next();
				if (!candidate.has_value() || candidate->key.size() < valuePrefix.size() ||
						!std::equal(valuePrefix.begin(), valuePrefix.end(), candidate->key.begin())) {
					break;
				}
				if (++totalCandidates > maximumNodes) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup exceeded its caller-supplied result bound");
				}
				pending.push_back(*candidate);
				if (pending.size() == batchCapacity && !emit()) {
					if (visitorCompleted != nullptr) {
						*visitorCompleted = false;
					}
					return IndexCandidateBatch{};
				}
			}
			if (!emit() && visitorCompleted != nullptr) {
				*visitorCompleted = false;
			}
			// The stream path deliberately performs no unbounded result collection.
			return IndexCandidateBatch{};
		}
			const utils::OwnedStdVector<BTreePageEntry> candidates = index.scan(snapshot, *entryKey,
						checkedBoundPlusOne(maximumNodes, "relocatable native exact Node lookup result bound overflowed"));
			IndexCandidateBatch result;
			result.snapshot_generation = snapshot.superblock().generation.value();
			result.exact_match_verified = true;
			result.predicates_verified = true;
			result.label_scope_verified = true;
			for (const std::string &name: definition.property_names) {
				result.verified_property_columns.push_back({name, {}});
			}
			uint64_t previousNodeId = 0U;
			for (const BTreePageEntry &candidate: candidates) {
				if (candidate.key.size() < valuePrefix.size() ||
					!std::equal(valuePrefix.begin(), valuePrefix.end(), candidate.key.begin())) {
					break;
				}
				if (candidate.key.size() != valuePrefix.size() + sizeof(uint64_t)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable native exact Node lookup found a malformed scalar Index key");
				}
				const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
				if (nodeId <= previousNodeId || candidate.value_or_child != nodeId) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup found a noncanonical scalar Index entry");
				}
				previousNodeId = nodeId;
				if (result.candidates.size() == maximumNodes) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable native exact Node lookup exceeded its caller-supplied result bound");
				}
				result.candidates.push_back({nodeId, false, 0U, 0U, 0U});
				for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
					result.verified_property_columns[propertyIndex].values.push_back(requestedValues[propertyIndex]);
				}
			}
			if (!result.candidates.empty()) {
				std::vector<uint64_t> nodeIds;
				nodeIds.reserve(result.candidates.size());
				for (const EntityRef &candidate: result.candidates) {
					nodeIds.push_back(candidate.entity_id);
				}
				// A B+Tree page proves only its own checksum and key ordering. Verify
				// the authoritative Property collection once for the complete sorted
				// candidate batch so valid-but-stale index keys fail closed.
				native_graph_reads_->validateNodeExactPropertiesSorted(
						snapshot, nodeIds, definition.label_token_id, propertyTokens, requestedValues,
						maximumPropertyEntries, session);
			}
			return result;
		}
		return std::nullopt;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeCandidatesByExactProperties(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, const size_t maximumNodes,
			const size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor,
			NativeReadSession *session, size_t maximumCandidateBytes) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable native exact Node candidate stream requires a visitor");
		}
		bool completed = true;
		IndexCandidateBatchVisitor callback = visitor;
		const auto batch = findNativeNodeCandidatesByExactProperties(snapshot, labels, properties, maximumNodes,
				maximumPropertyEntries, session, &callback, &completed, maximumCandidateBytes);
		if (!batch.has_value()) {
			return std::nullopt;
		}
		return completed;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNativeNodesByExactProperties(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, const size_t maximumNodes,
			const size_t maximumPropertyEntries, NativeReadSession *session) const {
		const std::optional<IndexCandidateBatch> candidates =
				findNativeNodeCandidatesByExactProperties(snapshot, labels, properties, maximumNodes,
																				 maximumPropertyEntries, session);
		if (!candidates.has_value()) {
			return std::nullopt;
		}
		if (!candidates->exact_match_verified || candidates->candidates.size() > maximumNodes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
							  "relocatable native exact Node lookup returned an invalid candidate batch");
		}
		std::vector<LogicalGraphNode> result;
		std::vector<uint64_t> nodeIds;
		nodeIds.reserve(candidates->candidates.size());
		for (const EntityRef &candidate: candidates->candidates) {
			if (candidate.entity_id == 0U || candidate.is_edge) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native exact Node lookup returned an invalid Node reference");
			}
			nodeIds.push_back(candidate.entity_id);
		}
		// Candidate verification already returns a strict logical-ID order. Read the
		// complete records through one page-grouped batch so the public result does
		// not repeat a Directory descent for every indexed Node.
		return native_logical_graph_reads_->readNodesShapedSorted(
				snapshot, nodeIds, {.include_labels = true, .all_properties = true}, session);
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNodesByExactProperties(
			const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
			const std::vector<std::pair<std::string, PropertyValue>> &properties, size_t maximumNodes,
			size_t maximumPropertyEntries) const {
		requireOwnedSnapshot(snapshot);
		return findNativeNodesByExactProperties(snapshot.pin_, labels, properties, maximumNodes,
																				maximumPropertyEntries);
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNativeNodesByPropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries,
			NativeReadSession *session, IndexCandidateBatch *candidateOutput,
			IndexCandidateBatchVisitor *visitor, bool *visitorCompleted, size_t maximumCandidateBytes) const {
		if (visitorCompleted != nullptr) {
			*visitorCompleted = true;
		}
		if (candidateOutput != nullptr) {
			candidateOutput->candidates.clear();
			candidateOutput->verified_property_columns.clear();
			candidateOutput->snapshot_generation = snapshot.isValid() ? snapshot.superblock().generation.value() : 0U;
			candidateOutput->exact_match_verified = false;
			candidateOutput->predicates_verified = false;
			candidateOutput->label_scope_verified = false;
		}
		if (!snapshot.isValid() || maximumNodes == 0U || maximumPropertyEntries == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native range Node lookup requires an active snapshot and positive bounds");
		}
		if (candidateOutput != nullptr) {
			// Once the request shape and resource bounds are valid, an empty result
			// (for example an unknown label/property) is still an authenticated,
			// complete candidate answer rather than a fallback signal.
			candidateOutput->predicates_verified = true;
			candidateOutput->label_scope_verified = true;
		}
		if (session != nullptr) {
			session->bind(snapshot);
		}
		const auto resolveLabelToken = [&]() {
			if (session != nullptr) {
				if (const auto cached = session->nodeLabelToken(labels.front()); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.range_index_label_token_resolution", 1);
			const std::optional<uint64_t> resolved =
					system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, labels.front());
			if (session != nullptr) {
				session->putNodeLabelToken(labels.front(), resolved);
			}
			return resolved;
		};
		const auto resolvePropertyToken = [&](const std::string &name) {
			if (session != nullptr) {
				if (const auto cached = session->propertyToken(name); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.range_index_property_token_resolution", 1);
			const std::optional<uint32_t> resolved = system_catalog_->findPropertyToken(snapshot, name);
			if (session != nullptr) {
				session->putPropertyToken(name, resolved);
			}
			return resolved;
		};
		const auto isOpenBound = [](const PropertyValue &value) {
			return value.getType() != PropertyType::NULL_TYPE;
		};
		if (labels.size() != 1U || labels.front().empty() || range.key.empty() ||
			(!isOpenBound(range.min_value) && !isOpenBound(range.max_value))) {
			return std::nullopt;
		}
		const auto isIndexableBound = [isOpenBound](const PropertyValue &value) {
			if (!isOpenBound(value)) {
				return true;
			}
			const PropertyType type = value.getType();
			return (type == PropertyType::BOOLEAN || type == PropertyType::INTEGER || type == PropertyType::DOUBLE ||
					type == PropertyType::STRING) &&
				   (type != PropertyType::DOUBLE || std::isfinite(std::get<double>(value.getVariant())));
		};
		if (!isIndexableBound(range.min_value) || !isIndexableBound(range.max_value)) {
			return std::nullopt;
		}
		if (isOpenBound(range.min_value) && isOpenBound(range.max_value) &&
			(range.min_value > range.max_value ||
			 (range.min_value == range.max_value && (!range.min_inclusive || !range.max_inclusive)))) {
			return std::vector<LogicalGraphNode>{};
		}
		const std::optional<uint64_t> labelToken = resolveLabelToken();
		if (!labelToken.has_value()) {
			return std::vector<LogicalGraphNode>{};
		}
		const std::optional<uint32_t> propertyToken = resolvePropertyToken(range.key);
		if (!propertyToken.has_value()) {
			return std::vector<LogicalGraphNode>{};
		}
		if (options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_entries == std::numeric_limits<size_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable native range Node lookup requires a finite Index-entry bound");
		}

		const std::vector<IndexDefinition> *cachedIndexes =
				session == nullptr ? nullptr : session->indexDefinitions(options_.maximum_index_definitions);
		utils::OwnedStdVector<IndexDefinition> loadedIndexes;
		if (cachedIndexes == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.range_index_definition_read", 1);
			loadedIndexes = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) {
				session->putIndexDefinitions(loadedIndexes);
				cachedIndexes = session->indexDefinitions(options_.maximum_index_definitions);
			}
		}
		const std::vector<IndexDefinition> &definitions = cachedIndexes == nullptr ? loadedIndexes : *cachedIndexes;
		for (const IndexDefinition &definition: definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind != IndexDefinitionKind::IDK_PROPERTY || definition.label_token_id != *labelToken ||
				definition.property_names.size() != 1U || definition.property_names.front() != range.key) {
				continue;
			}
			if (!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable native range Node lookup found an Index definition without a B+Tree root");
			}
			std::optional<std::vector<std::byte>> lower;
			if (isOpenBound(range.min_value)) {
				// Encode once, including the Entity-ID sentinel for the requested edge.
				lower = IndexKeyCodec::tryEncodeEntryKey(range.min_value,
						range.min_inclusive ? 1U : std::numeric_limits<uint64_t>::max());
				if (!lower.has_value()) return std::nullopt;
			}
			const BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			if (candidateOutput != nullptr) {
				candidateOutput->verified_property_columns.push_back({range.key, {}});
			}
			if (candidateOutput != nullptr && visitor != nullptr) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable native range Node lookup cannot collect and stream candidates simultaneously");
			}
			std::vector<LogicalGraphNode> result;
			std::vector<uint64_t> resultNodeIds;
			CandidateDedupMemoryResource dedupMemory(maximumCandidateBytes);
			std::pmr::unordered_set<uint64_t> seenNodeIds(&dedupMemory);
			// The cursor retains one verified root-to-leaf route. Stream callers use
			// bounded batches; complete callers keep the historical whole-result path
			// so their result-bound semantics remain unchanged.
			auto cursor = index.openRangeCursor(
					snapshot, lower.has_value() ? std::optional(std::span<const std::byte>(*lower)) : std::nullopt,
					options_.maximum_online_index_entries, BufferPoolReadHint::BPRH_SEQUENTIAL);
			debug::PerfTrace::addValue("native_graph.read.index_candidate_root_descent", 1);
			const std::optional<std::vector<std::byte>> upperExclusive = [&]() -> std::optional<std::vector<std::byte>> {
				if (!isOpenBound(range.max_value)) {
					return std::nullopt;
				}
				return range.max_inclusive ? std::optional(IndexKeyCodec::upperBound(range.max_value))
										 : std::optional(IndexKeyCodec::encodeEntryKey(range.max_value, 1U));
			}();
		// The default cursor batch is intentionally small for callers that do not
		// expose a byte budget. Query execution supplies a bounded candidate-byte
		// budget, so use that budget to enlarge the I/O batch and avoid repeating
		// Directory/Property-page work for every few hundred indexed rows. The
		// conservative per-row estimate keeps temporary staging bounded; the final
		// authenticated batch byte check remains authoritative for variable values.
		const size_t batchCapacity = candidateBatchCapacity(maximumNodes, maximumCandidateBytes);
		utils::OwnedStdVector<BTreePageEntry> pending;
		if (visitor != nullptr) {
			pending.reserve(batchCapacity);
		}
		size_t boundaryKeyBytes = 0U;
		const std::function<void(size_t)> admitBoundaryKey = [&](size_t bytes) {
			size_t retained = dedupMemory.retainedBytes();
			const auto add = [&](size_t count, size_t width) {
				if (count > (maximumCandidateBytes - retained) / width) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "range cursor boundary exceeded its byte bound");
				}
				retained += count * width;
			};
			add(pending.capacity(), sizeof(BTreePageEntry));
			for (const auto &entry: pending) add(entry.key.capacity(), 1U);
			add(bytes, 1U);
			boundaryKeyBytes = bytes;
		};
		dedupMemory.setAllocationAdmission([&](size_t bytes) {
			const size_t retainedBoundary = boundaryKeyBytes;
			if (bytes > maximumCandidateBytes - retainedBoundary) {
				throw FormatException(FormatError::FERR_INVALID_STATE, "range dedup allocation exceeded its shared byte bound");
			}
			admitBoundaryKey(retainedBoundary + bytes);
			boundaryKeyBytes = retainedBoundary;
		});
		size_t totalCandidates = 0U;
		struct RangeSourceBytePressure {};
		const auto emitOnce = [&](const std::span<const BTreePageEntry> entries) -> bool {
			if (entries.empty()) {
				return true;
			}
			size_t candidateStagingBytes = dedupMemory.retainedBytes();
			if (boundaryKeyBytes > maximumCandidateBytes - candidateStagingBytes) throw RangeSourceBytePressure{};
			candidateStagingBytes += boundaryKeyBytes;
			// The whole pending batch stays alive during recursive splits.
			// Charge its slots and owned keys, not only the emitted subspan.
			if (pending.capacity() > (maximumCandidateBytes - candidateStagingBytes) / sizeof(BTreePageEntry))
				throw RangeSourceBytePressure{};
			candidateStagingBytes += pending.capacity() * sizeof(BTreePageEntry);
			for (const auto &entry: pending) {
				if (entry.key.capacity() > maximumCandidateBytes - candidateStagingBytes) throw RangeSourceBytePressure{};
				candidateStagingBytes += entry.key.capacity();
			}
			const auto checkSlots = [&](size_t count, size_t width) {
				if (count > (maximumCandidateBytes - candidateStagingBytes) / width)
					throw RangeSourceBytePressure{};
			};
			checkSlots(entries.size(), sizeof(uint64_t));
			std::vector<uint64_t> nodeIds;
			nodeIds.reserve(entries.size());
			checkSlots(nodeIds.capacity(), sizeof(uint64_t));
			candidateStagingBytes += nodeIds.capacity() * sizeof(uint64_t);
			for (const BTreePageEntry &candidate: entries) {
				if (candidate.key.size() <= sizeof(uint64_t)) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup found a malformed scalar Index key");
				}
				const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
				if (candidate.value_or_child != nodeId) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup found a noncanonical scalar Index entry");
				}
				nodeIds.push_back(nodeId);
			}
			checkSlots(nodeIds.size(), sizeof(size_t));
			std::vector<size_t> propertyOrder(nodeIds.size());
			checkSlots(propertyOrder.capacity(), sizeof(size_t));
			candidateStagingBytes += propertyOrder.capacity() * sizeof(size_t);
			std::iota(propertyOrder.begin(), propertyOrder.end(), 0U);
			std::ranges::sort(propertyOrder, [&nodeIds](const size_t left, const size_t right) {
				return nodeIds[left] < nodeIds[right];
			});
			std::vector<uint64_t> sortedNodeIds;
			checkSlots(nodeIds.size(), sizeof(uint64_t));
			sortedNodeIds.reserve(nodeIds.size());
			checkSlots(sortedNodeIds.capacity(), sizeof(uint64_t));
			candidateStagingBytes += sortedNodeIds.capacity() * sizeof(uint64_t);
			for (const size_t index: propertyOrder) {
				sortedNodeIds.push_back(nodeIds[index]);
			}
			const std::array<uint64_t, 1U> labelTokens{definition.label_token_id};
			size_t labelBufferBytes = 0U;
			const SelectedPropertyReadAdmission admitLabelBuffers = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - candidateStagingBytes) throw RangeSourceBytePressure{};
				labelBufferBytes = bytes;
			};
			const std::vector<bool> labelMatches =
					native_graph_reads_->readNodeLabelMatchesSorted(snapshot, sortedNodeIds, labelTokens, session, &admitLabelBuffers);
			if (labelMatches.size() != sortedNodeIds.size() ||
				std::ranges::any_of(labelMatches, [](bool matches) { return !matches; })) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable native range Node lookup found an invalid indexed label");
			}
			size_t selectedSourceBytes = candidateStagingBytes;
			size_t largeValueBufferBytes = labelBufferBytes;
			const SelectedPropertyReadAdmission admission = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - selectedSourceBytes ||
					largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes)
					throw RangeSourceBytePressure{};
				selectedSourceBytes += bytes;
				traceNativeCandidateMetric("native_graph.read.range_candidate_source_bytes", bytes);
			};
			const SelectedPropertyReadAdmission admitLargeValueBuffers = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - selectedSourceBytes ||
					labelBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw RangeSourceBytePressure{};
				largeValueBufferBytes = labelBufferBytes + bytes;
			};
			auto propertyColumnsAndCounts = native_graph_reads_->readNodePropertyColumnsAndCountsSorted(
					snapshot, sortedNodeIds, std::array<uint32_t, 1U>{*propertyToken}, session, &admission, &admitLargeValueBuffers);
			auto &propertyColumns = propertyColumnsAndCounts.first;
			const auto &propertyCounts = propertyColumnsAndCounts.second;
			if (labelMatches.size() != sortedNodeIds.size() || propertyCounts.size() != sortedNodeIds.size() ||
					propertyColumns.size() != 1U || propertyColumns.front().size() != propertyOrder.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup returned an invalid candidate validation batch");
			}
			traceNativeCandidateMetric("native_graph.read.index_candidate_liveness_probes", sortedNodeIds.size());
			for (size_t index = 0U; index < sortedNodeIds.size(); ++index) {
				if (!labelMatches[index] || !propertyCounts[index].has_value() ||
						*propertyCounts[index] > maximumPropertyEntries) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup found an invalid indexed Node");
				}
			}
			// Scatter in place using the existing source-to-destination permutation.
			// No second array of PropertyValue slots needs to coexist with the reader output.
			auto &properties = propertyColumns.front();
			for (size_t index = 0U; index < propertyOrder.size(); ++index) {
				while (propertyOrder[index] != index) {
					const size_t destination = propertyOrder[index];
					std::swap(properties[index], properties[destination]);
					std::swap(propertyOrder[index], propertyOrder[destination]);
				}
			}
			size_t outputBufferBytes = 0U;
			const SelectedPropertyReadAdmission admitOutput = [&](size_t bytes) {
				if (bytes > maximumCandidateBytes - selectedSourceBytes ||
					largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw RangeSourceBytePressure{};
				outputBufferBytes = bytes;
			};
			IndexCandidateBatch streamBatch;
			if (visitor != nullptr) {
				streamBatch.snapshot_generation = snapshot.superblock().generation.value();
				streamBatch.predicates_verified = true;
				streamBatch.label_scope_verified = true;
				reserveCandidateOutput(streamBatch, entries.size(), std::span<const std::string>(&range.key, 1U), admitOutput);
			}
			for (size_t index = 0U; index < entries.size(); ++index) {
				const BTreePageEntry &candidate = entries[index];
				const uint64_t nodeId = nodeIds[index];
				std::optional<PropertyValue> &property = properties[index];
				if (!property.has_value()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a stale scalar Index entry");
				}
				const std::optional<std::vector<std::byte>> expected = IndexKeyCodec::tryEncodeEntryKey(*property, nodeId);
				if (!expected.has_value() || candidate.key != *expected) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a noncanonical scalar Index entry");
				}
				const bool belowLower = isOpenBound(range.min_value) &&
						(range.min_inclusive ? *property < range.min_value : *property <= range.min_value);
				if (belowLower) {
					continue;
				}
					if (candidateOutput != nullptr) {
						candidateOutput->candidates.push_back({nodeId, false, 0U, 0U, 0U});
						candidateOutput->verified_property_columns.front().values.push_back(std::move(*property));
						traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", 1U);
				} else if (visitor != nullptr) {
					streamBatch.candidates.push_back({nodeId, false, 0U, 0U, 0U});
					streamBatch.verified_property_columns.front().values.push_back(std::move(*property));
				} else {
					resultNodeIds.push_back(nodeId);
				}
			}
			if (visitor == nullptr) {
				return true;
			}
			// Range index order is value-major. Sort only this bounded batch so the
			// storage-neutral runtime can validate its monotonic candidate contract.
			// Reuse the already admitted row permutation for output ordering.
			propertyOrder.resize(streamBatch.candidates.size());
			auto &order = propertyOrder;
			std::iota(order.begin(), order.end(), 0U);
			std::ranges::sort(order, [&streamBatch](const size_t left, const size_t right) {
				return streamBatch.candidates[left].entity_id < streamBatch.candidates[right].entity_id;
			});
			reorderCandidateRowsInPlace(streamBatch, order);
			traceNativeCandidateMetric("native_graph.read.index_candidate_batches", streamBatch.candidates.empty() ? 0U : 1U);
			traceNativeCandidateMetric("native_graph.read.index_candidate_emitted_rows", streamBatch.candidates.size());
			enforceCandidateBatchBytes(streamBatch, maximumCandidateBytes);
			return streamBatch.candidates.empty() || (*visitor)(streamBatch);
		};
		std::function<bool(std::span<const BTreePageEntry>)> emit;
		emit = [&](std::span<const BTreePageEntry> entries) -> bool {
			try {
				return emitOnce(entries);
			} catch (const RangeSourceBytePressure &) {
				if (entries.size() <= 1U) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable native index candidate batch exceeded its byte bound");
				}
			}
			traceNativeCandidateMetric("native_graph.read.range_candidate_source_splits", 1U);
			const size_t left = entries.size() / 2U;
			return emit(entries.first(left)) && emit(entries.subspan(left));
		};

		for (;;) {
			std::optional<BTreePageEntry> candidate = cursor.next(admitBoundaryKey);
			if (!candidate.has_value()) {
				break;
			}
			if (upperExclusive.has_value() &&
					(range.max_inclusive ? std::lexicographical_compare(upperExclusive->begin(), upperExclusive->end(),
							candidate->key.begin(), candidate->key.end())
							: !std::lexicographical_compare(candidate->key.begin(), candidate->key.end(),
																 upperExclusive->begin(), upperExclusive->end()))) {
				break;
			}
			if (candidate->key.size() <= sizeof(uint64_t)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a malformed scalar Index key");
			}
			const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate->key);
			if (candidate->value_or_child != nodeId || !seenNodeIds.insert(nodeId).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup found a noncanonical scalar Index entry");
			}
			++totalCandidates;
			if (totalCandidates > maximumNodes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable native range Node lookup exceeded its caller-supplied result bound");
			}
			if (visitor != nullptr) {
				pending.push_back(std::move(*candidate));
				if (pending.size() == batchCapacity) {
					if (!emit(std::span<const BTreePageEntry>(pending))) {
						if (visitorCompleted != nullptr) {
							*visitorCompleted = false;
						}
						return std::vector<LogicalGraphNode>{};
					}
					pending.clear();
				}
			} else {
				// Complete callers retain a bounded vector to preserve their explicit
				// maximumNodes contract and deterministic final Node-ID ordering.
				// Defer Property validation until all candidates are sorted by owner.
				// This path is handled by the same emitter after the cursor is drained.
				pending.push_back(std::move(*candidate));
			}
		}
		if (visitor != nullptr) {
			if (!pending.empty() && !emit(std::span<const BTreePageEntry>(pending)) && visitorCompleted != nullptr) {
				*visitorCompleted = false;
			}
			return std::vector<LogicalGraphNode>{};
		}
		// Complete callers now validate their collected candidates in one owner-
		// sorted batch, then reuse the emitter's normal result/candidate-output path.
		if (!pending.empty()) {
			if (!emit(std::span<const BTreePageEntry>(pending))) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable native range Node lookup unexpectedly stopped a complete validation");
			}
		}
		if (visitor == nullptr && candidateOutput == nullptr && !resultNodeIds.empty()) {
			std::ranges::sort(resultNodeIds);
			result = native_logical_graph_reads_->readNodesShapedSorted(
					snapshot, resultNodeIds, {.include_labels = true, .all_properties = true}, session);
		}
		std::ranges::sort(result, {}, &LogicalGraphNode::node_id);
		if (candidateOutput != nullptr) {
			std::vector<size_t> order(candidateOutput->candidates.size());
			std::iota(order.begin(), order.end(), 0U);
			std::ranges::sort(order, [candidateOutput](size_t left, size_t right) {
				return candidateOutput->candidates[left].entity_id < candidateOutput->candidates[right].entity_id;
			});
			reorderCandidateRowsInPlace(*candidateOutput, order);
		}
		return result;
		}
		return std::nullopt;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNodesByPropertyRange(
			const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries) const {
		requireOwnedSnapshot(snapshot);
		return findNativeNodesByPropertyRange(snapshot.pin_, labels, range, maximumNodes, maximumPropertyEntries);
	}

	std::optional<IndexCandidateBatch> RelocatableStorageBackend::findNativeNodeCandidatesByPropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, size_t maximumNodes, size_t maximumPropertyEntries,
			NativeReadSession *session) const {
		IndexCandidateBatch result;
		const auto complete = findNativeNodesByPropertyRange(snapshot, labels, range, maximumNodes, maximumPropertyEntries,
												 session, &result);
		if (!complete.has_value()) {
			return std::nullopt;
		}
		if (!result.predicates_verified || result.candidates.size() > maximumNodes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native range Node lookup returned an invalid candidate batch");
		}
		for (const auto &column: result.verified_property_columns) {
			if (column.values.size() != result.candidates.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native range Node lookup returned an invalid Property column");
			}
		}
		// The native source owns ordering; validate its contract without another
		// permutation array or a second copy of every verified Property column.
		if (!std::ranges::is_sorted(result.candidates, {}, &EntityRef::entity_id)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable native range Node lookup returned an unsorted candidate batch");
		}
		return result;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeCandidatesByPropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphPropertyRange &range, const size_t maximumNodes, const size_t maximumPropertyEntries,
			const IndexCandidateBatchVisitor &visitor, NativeReadSession *session, size_t maximumCandidateBytes) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable native range Node candidate stream requires a visitor");
		}
		bool completed = true;
		IndexCandidateBatchVisitor callback = visitor;
		const auto result = findNativeNodesByPropertyRange(snapshot, labels, range, maximumNodes,
				maximumPropertyEntries, session, nullptr, &callback, &completed, maximumCandidateBytes);
		if (!result.has_value()) {
			return std::nullopt;
		}
		return completed;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNativeNodesByCompositePropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
			size_t maximumPropertyEntries, NativeReadSession *session, IndexCandidateBatch *candidateOutput,
			IndexCandidateBatchVisitor *visitor, bool *visitorCompleted, size_t maximumCandidateBytes) const {
		if (visitorCompleted != nullptr) {
			*visitorCompleted = true;
		}
		if (candidateOutput != nullptr) {
			candidateOutput->candidates.clear();
			candidateOutput->verified_property_columns.clear();
			candidateOutput->snapshot_generation = snapshot.isValid() ? snapshot.superblock().generation.value() : 0U;
			candidateOutput->exact_match_verified = false;
			candidateOutput->predicates_verified = false;
			candidateOutput->label_scope_verified = false;
		}
		if (!snapshot.isValid() || maximumNodes == 0U || maximumPropertyEntries == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable composite range Node lookup requires an active snapshot and positive bounds");
		}
		if (candidateOutput != nullptr) {
			candidateOutput->predicates_verified = true;
			candidateOutput->label_scope_verified = true;
		}
		if (session != nullptr) {
			session->bind(snapshot);
		}
		const auto resolveLabelToken = [&]() {
			if (session != nullptr) {
				if (const auto cached = session->nodeLabelToken(labels.front()); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.label_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.composite_range_index_label_token_resolution", 1);
			const std::optional<uint64_t> resolved =
					system_catalog_->findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_LABEL, labels.front());
			if (session != nullptr) {
				session->putNodeLabelToken(labels.front(), resolved);
			}
			return resolved;
		};
		const auto resolvePropertyToken = [&](const std::string &name) {
			if (session != nullptr) {
				if (const auto cached = session->propertyToken(name); cached.has_value()) {
					return *cached;
				}
			}
			debug::PerfTrace::addValue("native_graph.read.property_token_resolution", 1);
			debug::PerfTrace::addValue("native_graph.read.composite_range_index_property_token_resolution", 1);
			const std::optional<uint32_t> resolved = system_catalog_->findPropertyToken(snapshot, name);
			if (session != nullptr) {
				session->putPropertyToken(name, resolved);
			}
			return resolved;
		};
		const auto isBound = [](const PropertyValue &value) { return value.getType() != PropertyType::NULL_TYPE; };
		const auto isIndexable = [isBound](const PropertyValue &value) {
			if (!isBound(value)) return true;
			const PropertyType type = value.getType();
			return (type == PropertyType::BOOLEAN || type == PropertyType::INTEGER || type == PropertyType::DOUBLE ||
					type == PropertyType::STRING) &&
					(type != PropertyType::DOUBLE || std::isfinite(std::get<double>(value.getVariant())));
		};
		if (labels.size() != 1U || labels.front().empty() || request.equality_prefix.empty() || request.range.key.empty() ||
			(!isBound(request.range.min_value) && !isBound(request.range.max_value))) {
			return std::nullopt;
		}
		std::map<std::string, const PropertyValue *> prefix;
		for (const auto &[name, value]: request.equality_prefix) {
			if (name.empty() || name == request.range.key || !isIndexable(value) || !prefix.emplace(name, &value).second) {
				return std::nullopt;
			}
		}
		if (!isIndexable(request.range.min_value) || !isIndexable(request.range.max_value)) return std::nullopt;
		if (isBound(request.range.min_value) && isBound(request.range.max_value) &&
			(request.range.min_value > request.range.max_value ||
			 (request.range.min_value == request.range.max_value &&
			  (!request.range.min_inclusive || !request.range.max_inclusive)))) {
			return std::vector<LogicalGraphNode>{};
		}
		const auto labelToken = resolveLabelToken();
		if (!labelToken.has_value()) return std::vector<LogicalGraphNode>{};
		for (const auto &[name, value]: request.equality_prefix) {
			static_cast<void>(value);
			if (!resolvePropertyToken(name).has_value()) {
				return std::vector<LogicalGraphNode>{};
			}
		}
		if (!resolvePropertyToken(request.range.key).has_value()) {
			return std::vector<LogicalGraphNode>{};
		}
		if (options_.maximum_online_index_entries == 0U ||
			options_.maximum_online_index_entries == std::numeric_limits<size_t>::max()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable composite range Node lookup requires a finite Index-entry bound");
		}
		const std::vector<IndexDefinition> *cachedIndexes =
				session == nullptr ? nullptr : session->indexDefinitions(options_.maximum_index_definitions);
		utils::OwnedStdVector<IndexDefinition> loadedIndexes;
		if (cachedIndexes == nullptr) {
			debug::PerfTrace::addValue("native_graph.read.composite_range_index_definition_read", 1);
			loadedIndexes = readAllIndexDefinitions(snapshot, options_.maximum_index_definitions);
			if (session != nullptr) {
				session->putIndexDefinitions(loadedIndexes);
				cachedIndexes = session->indexDefinitions(options_.maximum_index_definitions);
			}
		}
		const std::vector<IndexDefinition> &definitions = cachedIndexes == nullptr ? loadedIndexes : *cachedIndexes;
		for (const IndexDefinition &definition: definitions) {
			if (definition.entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
				definition.kind != IndexDefinitionKind::IDK_COMPOSITE || definition.label_token_id != *labelToken ||
				definition.property_names.size() != prefix.size() + 1U || definition.property_names.back() != request.range.key) continue;
			std::vector<PropertyValue> prefixValues;
			prefixValues.reserve(definition.property_names.size());
			std::vector<uint32_t> propertyTokens;
			bool matches = true;
			for (size_t index = 0U; index + 1U < definition.property_names.size(); ++index) {
				const auto value = prefix.find(definition.property_names[index]);
				const auto token = resolvePropertyToken(definition.property_names[index]);
				if (value == prefix.end() || !token.has_value()) { matches = false; break; }
				prefixValues.push_back(*value->second);
				propertyTokens.push_back(*token);
			}
			const auto rangeToken = resolvePropertyToken(request.range.key);
			if (!matches || !rangeToken.has_value()) continue;
			propertyTokens.push_back(*rangeToken);
			const auto encodedPrefix = IndexKeyCodec::tryEncodeComposite(prefixValues);
			if (!encodedPrefix.has_value()) return std::nullopt;
			// Reuse one prefix workspace for both bounds instead of copying every
			// prefix PropertyValue (including strings) into two additional vectors.
			const auto encodeBound = [&](const PropertyValue &value, uint64_t entityId) {
				prefixValues.push_back(value);
				auto encoded = IndexKeyCodec::tryEncodeCompositeEntryKey(prefixValues, entityId);
				prefixValues.pop_back();
				return encoded;
			};
			std::optional<std::vector<std::byte>> lower;
				if (isBound(request.range.min_value)) {
					lower = encodeBound(request.range.min_value,
							request.range.min_inclusive ? 1U : std::numeric_limits<uint64_t>::max());
					if (!lower.has_value()) return std::nullopt;
				}
			if (!metadata_->findIndexRootOptional(snapshot, definition.index_id).has_value()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable composite range Node lookup found an Index definition without a B+Tree root");
			}
			const BTreeIndex index(definition.index_id, *btree_pages_, *metadata_);
			if (candidateOutput != nullptr && visitor != nullptr) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable composite range Node lookup cannot collect and stream candidates simultaneously");
			}
			if (candidateOutput != nullptr) {
				candidateOutput->predicates_verified = true;
				for (const std::string &name: definition.property_names) {
					candidateOutput->verified_property_columns.push_back({name, {}});
				}
			}
				const std::optional<std::span<const std::byte>> start =
						lower.has_value() ? std::optional(std::span<const std::byte>(*lower))
										: std::optional(std::span<const std::byte>(*encodedPrefix));
				// The lower bound for an exclusive value uses the largest possible
				// Entity-ID suffix. Exclude that sentinel itself so an entry with the
				// maximum ID cannot leak into the exclusive range.
				const bool excludeLowerBound = isBound(request.range.min_value) && !request.range.min_inclusive;
				auto cursor = index.openRangeCursor(snapshot, start, options_.maximum_online_index_entries,
					BufferPoolReadHint::BPRH_SEQUENTIAL, excludeLowerBound);
				debug::PerfTrace::addValue("native_graph.read.index_candidate_root_descent", 1);
				std::optional<std::vector<std::byte>> upperExclusive;
				if (isBound(request.range.max_value)) {
					upperExclusive = encodeBound(request.range.max_value,
							request.range.max_inclusive ? std::numeric_limits<uint64_t>::max() : 1U);
					if (!upperExclusive.has_value()) return std::nullopt;
				}
			std::vector<PropertyValue>().swap(prefixValues);
			CandidateDedupMemoryResource dedupMemory(maximumCandidateBytes);
			std::pmr::unordered_set<uint64_t> seen(&dedupMemory);
			std::vector<LogicalGraphNode> result;
			std::vector<uint64_t> resultNodeIds;
			utils::OwnedStdVector<BTreePageEntry> pending;
			const size_t batchCapacity = candidateBatchCapacity(maximumNodes, maximumCandidateBytes);
			if (visitor != nullptr) {
			pending.reserve(batchCapacity);
			}
			size_t boundaryKeyBytes = 0U;
			const std::function<void(size_t)> admitBoundaryKey = [&](size_t bytes) {
				size_t retained = dedupMemory.retainedBytes();
				const auto add = [&](size_t count, size_t width) {
					if (count > (maximumCandidateBytes - retained) / width) {
						throw FormatException(FormatError::FERR_INVALID_STATE, "range cursor boundary exceeded its byte bound");
					}
					retained += count * width;
				};
				add(pending.capacity(), sizeof(BTreePageEntry));
				for (const auto &entry: pending) add(entry.key.capacity(), 1U);
				add(bytes, 1U);
				boundaryKeyBytes = bytes;
			};
			dedupMemory.setAllocationAdmission([&](size_t bytes) {
				const size_t retainedBoundary = boundaryKeyBytes;
				if (bytes > maximumCandidateBytes - retainedBoundary) {
					throw FormatException(FormatError::FERR_INVALID_STATE, "range dedup allocation exceeded its shared byte bound");
				}
				admitBoundaryKey(retainedBoundary + bytes);
				boundaryKeyBytes = retainedBoundary;
			});
			size_t totalCandidates = 0U;
			bool rangeExhausted = false;
			struct CompositeSourceBytePressure {};
			const auto emitOnce = [&](const std::span<const BTreePageEntry> entries) -> bool {
				if (entries.empty()) return true;
				size_t candidateStagingBytes = dedupMemory.retainedBytes();
				if (boundaryKeyBytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
				candidateStagingBytes += boundaryKeyBytes;
				// The whole pending batch stays alive during recursive splits.
				// Charge its slots and owned keys, not only the emitted subspan.
				if (pending.capacity() > (maximumCandidateBytes - candidateStagingBytes) / sizeof(BTreePageEntry))
					throw CompositeSourceBytePressure{};
				candidateStagingBytes += pending.capacity() * sizeof(BTreePageEntry);
				for (const auto &entry: pending) {
					if (entry.key.capacity() > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
					candidateStagingBytes += entry.key.capacity();
				}
				size_t labelBufferBytes = 0U;
				const auto checkSlots = [&](size_t count, size_t width) {
					if (count > (maximumCandidateBytes - candidateStagingBytes - labelBufferBytes) / width)
						throw CompositeSourceBytePressure{};
				};
				checkSlots(entries.size(), sizeof(uint64_t));
				std::vector<uint64_t> nodeIds;
				nodeIds.reserve(entries.size());
				checkSlots(nodeIds.capacity(), sizeof(uint64_t));
				candidateStagingBytes += nodeIds.capacity() * sizeof(uint64_t);
				for (const BTreePageEntry &candidate: entries) {
					if (candidate.key.size() <= encodedPrefix->size() + sizeof(uint64_t) ||
							!std::equal(encodedPrefix->begin(), encodedPrefix->end(), candidate.key.begin())) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found a malformed Index key");
					}
					const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate.key);
					if (candidate.value_or_child != nodeId) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found a noncanonical Index entry");
					}
					nodeIds.push_back(nodeId);
				}
				checkSlots(nodeIds.size(), sizeof(size_t));
				std::vector<size_t> propertyOrder(nodeIds.size());
				checkSlots(propertyOrder.capacity(), sizeof(size_t));
				candidateStagingBytes += propertyOrder.capacity() * sizeof(size_t);
				std::iota(propertyOrder.begin(), propertyOrder.end(), 0U);
				std::ranges::sort(propertyOrder, [&nodeIds](size_t left, size_t right) {
					return nodeIds[left] < nodeIds[right];
				});
				std::vector<uint64_t> sortedNodeIds;
				checkSlots(propertyOrder.size(), sizeof(uint64_t));
				sortedNodeIds.reserve(propertyOrder.size());
				checkSlots(sortedNodeIds.capacity(), sizeof(uint64_t));
				candidateStagingBytes += sortedNodeIds.capacity() * sizeof(uint64_t);
				for (const size_t index: propertyOrder) sortedNodeIds.push_back(nodeIds[index]);
				const std::array<uint64_t, 1U> labelTokens{definition.label_token_id};
				const SelectedPropertyReadAdmission admitLabelBuffers = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
					labelBufferBytes = bytes;
				};
				const auto labelMatches = native_graph_reads_->readNodeLabelMatchesSorted(
						snapshot, sortedNodeIds, labelTokens, session, &admitLabelBuffers);
				if (labelMatches.size() != sortedNodeIds.size() ||
					std::ranges::any_of(labelMatches, [](bool matches) { return !matches; })) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
						"relocatable composite range Node lookup found an invalid indexed label");
				}

				std::vector<std::pair<uint32_t, size_t>> tokenOrder;
				checkSlots(propertyTokens.size(), sizeof(std::pair<uint32_t, size_t>));
				tokenOrder.reserve(propertyTokens.size());
				checkSlots(tokenOrder.capacity(), sizeof(std::pair<uint32_t, size_t>));
				candidateStagingBytes += tokenOrder.capacity() * sizeof(std::pair<uint32_t, size_t>);
				if (labelBufferBytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
				for (size_t index = 0U; index < propertyTokens.size(); ++index) {
					tokenOrder.emplace_back(propertyTokens[index], index);
				}
				std::ranges::sort(tokenOrder, {}, &std::pair<uint32_t, size_t>::first);
				for (size_t index = 1U; index < tokenOrder.size(); ++index) {
					if (tokenOrder[index - 1U].first == tokenOrder[index].first) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup resolved duplicate Property tokens");
					}
				}
				std::vector<uint32_t> sortedPropertyTokens;
				checkSlots(tokenOrder.size(), sizeof(uint32_t));
				sortedPropertyTokens.reserve(tokenOrder.size());
				checkSlots(sortedPropertyTokens.capacity(), sizeof(uint32_t));
				candidateStagingBytes += sortedPropertyTokens.capacity() * sizeof(uint32_t);
				if (labelBufferBytes > maximumCandidateBytes - candidateStagingBytes) throw CompositeSourceBytePressure{};
				for (const auto &[token, originalColumn]: tokenOrder) {
					static_cast<void>(originalColumn);
					sortedPropertyTokens.push_back(token);
				}
				size_t selectedSourceBytes = candidateStagingBytes;
				size_t largeValueBufferBytes = labelBufferBytes;
				const SelectedPropertyReadAdmission admission = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - selectedSourceBytes ||
						largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes)
						throw CompositeSourceBytePressure{};
					selectedSourceBytes += bytes;
					traceNativeCandidateMetric("native_graph.read.composite_range_candidate_source_bytes", bytes);
				};
				const SelectedPropertyReadAdmission admitLargeValueBuffers = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - selectedSourceBytes ||
						labelBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw CompositeSourceBytePressure{};
					largeValueBufferBytes = labelBufferBytes + bytes;
				};
				auto columnsAndCounts = native_graph_reads_->readNodePropertyColumnsAndCountsSorted(
						snapshot, sortedNodeIds, sortedPropertyTokens, session, &admission, &admitLargeValueBuffers);
				auto &sortedPropertiesByToken = columnsAndCounts.first;
				const auto &propertyCounts = columnsAndCounts.second;
				if (labelMatches.size() != sortedNodeIds.size() || propertyCounts.size() != sortedNodeIds.size()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup returned an invalid candidate validation batch");
				}
				traceNativeCandidateMetric("native_graph.read.index_candidate_liveness_probes", sortedNodeIds.size());
				for (size_t index = 0U; index < sortedNodeIds.size(); ++index) {
					if (!labelMatches[index] || !propertyCounts[index].has_value() ||
							*propertyCounts[index] > maximumPropertyEntries) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found an invalid indexed Node");
					}
				}
				if (sortedPropertiesByToken.size() != propertyTokens.size()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup returned an invalid Property column batch");
				}
				for (const auto &column: sortedPropertiesByToken) {
					if (column.size() != propertyOrder.size()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup returned an invalid Property batch");
					}
				}
				// Reuse both permutations to scatter rows and columns without allocating
				// another matrix of optional PropertyValue slots.
				auto &propertiesByToken = sortedPropertiesByToken;
				for (size_t index = 0U; index < propertyOrder.size(); ++index) {
					while (propertyOrder[index] != index) {
						const size_t destination = propertyOrder[index];
						for (auto &column: propertiesByToken) std::swap(column[index], column[destination]);
						std::swap(propertyOrder[index], propertyOrder[destination]);
					}
				}
				for (size_t index = 0U; index < tokenOrder.size(); ++index) {
					while (tokenOrder[index].second != index) {
						const size_t destination = tokenOrder[index].second;
						std::swap(propertiesByToken[index], propertiesByToken[destination]);
						std::swap(tokenOrder[index], tokenOrder[destination]);
					}
				}
				size_t outputBufferBytes = 0U;
				const SelectedPropertyReadAdmission admitOutput = [&](size_t bytes) {
					if (bytes > maximumCandidateBytes - selectedSourceBytes ||
						largeValueBufferBytes > maximumCandidateBytes - selectedSourceBytes - bytes) throw CompositeSourceBytePressure{};
					outputBufferBytes = bytes;
				};
				IndexCandidateBatch streamBatch;
				if (visitor != nullptr) {
					streamBatch.snapshot_generation = snapshot.superblock().generation.value();
					streamBatch.predicates_verified = true;
					streamBatch.label_scope_verified = true;
					reserveCandidateOutput(streamBatch, entries.size(), definition.property_names, admitOutput);
				}
				// One row workspace is reused; its slots coexist with reader/output arrays.
				std::vector<PropertyValue> actual;
				const size_t outputSlots = outputBufferBytes;
				if (propertyTokens.size() > (maximumCandidateBytes - selectedSourceBytes - largeValueBufferBytes - outputSlots) / sizeof(PropertyValue))
					throw CompositeSourceBytePressure{};
				admitOutput(outputSlots + propertyTokens.size() * sizeof(PropertyValue));
				actual.reserve(propertyTokens.size());
				admitOutput(outputSlots + actual.capacity() * sizeof(PropertyValue));
				for (size_t index = 0U; index < entries.size(); ++index) {
					const uint64_t nodeId = nodeIds[index];
					actual.clear();
					for (auto &properties: propertiesByToken) {
						if (!properties[index].has_value()) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
									"relocatable composite range Node lookup found a stale Index entry");
						}
						actual.push_back(std::move(*properties[index]));
					}
					const auto expected = IndexKeyCodec::tryEncodeCompositeEntryKey(actual, nodeId);
					if (!expected.has_value() || entries[index].key != *expected) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
								"relocatable composite range Node lookup found a noncanonical Index entry");
					}
					const PropertyValue &value = actual.back();
					if (isBound(request.range.max_value) &&
							(request.range.max_inclusive ? value > request.range.max_value : value >= request.range.max_value)) {
						rangeExhausted = true;
						continue;
					}
					const bool belowLower = isBound(request.range.min_value) &&
							(request.range.min_inclusive ? value < request.range.min_value : value <= request.range.min_value);
					if (belowLower) continue;
					if (candidateOutput != nullptr) {
						for (size_t propertyIndex = 0U; propertyIndex < definition.property_names.size(); ++propertyIndex) {
							candidateOutput->verified_property_columns[propertyIndex].values.push_back(std::move(actual[propertyIndex]));
						}
						candidateOutput->candidates.push_back({nodeId, false, 0U, 0U, 0U});
						traceNativeCandidateMetric("native_graph.read.index_candidate_verified_rows", 1U);
					} else if (visitor != nullptr) {
						streamBatch.candidates.push_back({nodeId, false, 0U, 0U, 0U});
						for (size_t propertyIndex = 0U; propertyIndex < actual.size(); ++propertyIndex) {
							streamBatch.verified_property_columns[propertyIndex].values.push_back(std::move(actual[propertyIndex]));
						}
					} else {
						resultNodeIds.push_back(nodeId);
					}
				}
				if (visitor == nullptr) return true;
				// Reuse the already admitted row permutation for output ordering.
				propertyOrder.resize(streamBatch.candidates.size());
				auto &order = propertyOrder;
				std::iota(order.begin(), order.end(), 0U);
				std::ranges::sort(order, [&streamBatch](size_t left, size_t right) {
					return streamBatch.candidates[left].entity_id < streamBatch.candidates[right].entity_id;
				});
				reorderCandidateRowsInPlace(streamBatch, order);
				traceNativeCandidateMetric("native_graph.read.index_candidate_batches", streamBatch.candidates.empty() ? 0U : 1U);
				traceNativeCandidateMetric("native_graph.read.index_candidate_emitted_rows", streamBatch.candidates.size());
				enforceCandidateBatchBytes(streamBatch, maximumCandidateBytes);
				return streamBatch.candidates.empty() || (*visitor)(streamBatch);
			};
			std::function<bool(std::span<const BTreePageEntry>)> emit;
			emit = [&](std::span<const BTreePageEntry> entries) -> bool {
				try {
					return emitOnce(entries);
				} catch (const CompositeSourceBytePressure &) {
					if (entries.size() <= 1U) {
						throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable native index candidate batch exceeded its byte bound");
					}
				}
				traceNativeCandidateMetric("native_graph.read.composite_range_candidate_source_splits", 1U);
				const size_t left = entries.size() / 2U;
				return emit(entries.first(left)) && emit(entries.subspan(left));
			};

				for (;;) {
					if (rangeExhausted) break;
					auto candidate = cursor.next(admitBoundaryKey);
					if (!candidate.has_value() || candidate->key.size() <= encodedPrefix->size() + sizeof(uint64_t) ||
							!std::equal(encodedPrefix->begin(), encodedPrefix->end(), candidate->key.begin())) break;
					if (upperExclusive.has_value()) {
						const bool outsideUpper = request.range.max_inclusive
								? std::lexicographical_compare(upperExclusive->begin(), upperExclusive->end(),
										candidate->key.begin(), candidate->key.end())
								: !std::lexicographical_compare(candidate->key.begin(), candidate->key.end(),
										upperExclusive->begin(), upperExclusive->end());
						if (outsideUpper) break;
					}
					const uint64_t nodeId = IndexKeyCodec::decodeEntryEntityId(candidate->key);
				if (candidate->value_or_child != nodeId || !seen.insert(nodeId).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup found a noncanonical Index entry");
				}
				if (++totalCandidates > maximumNodes) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
							"relocatable composite range Node lookup exceeded its caller-supplied result bound");
				}
				pending.push_back(std::move(*candidate));
				if (pending.size() == batchCapacity) {
					if (!emit(std::span<const BTreePageEntry>(pending))) {
						if (visitorCompleted != nullptr) *visitorCompleted = false;
						return std::vector<LogicalGraphNode>{};
					}
					pending.clear();
				}
			}
			if (!pending.empty()) {
				if (!emit(std::span<const BTreePageEntry>(pending)) && visitorCompleted != nullptr) *visitorCompleted = false;
			}
			if (visitor != nullptr) return std::vector<LogicalGraphNode>{};
			if (visitor == nullptr && candidateOutput == nullptr && !resultNodeIds.empty()) {
				std::ranges::sort(resultNodeIds);
				result = native_logical_graph_reads_->readNodesShapedSorted(
						snapshot, resultNodeIds, {.include_labels = true, .all_properties = true}, session);
			}
			std::ranges::sort(result, {}, &LogicalGraphNode::node_id);
			if (candidateOutput != nullptr) {
				std::vector<size_t> order(candidateOutput->candidates.size());
				std::iota(order.begin(), order.end(), 0U);
				std::ranges::sort(order, [candidateOutput](size_t left, size_t right) {
					return candidateOutput->candidates[left].entity_id < candidateOutput->candidates[right].entity_id;
				});
				reorderCandidateRowsInPlace(*candidateOutput, order);
			}
			return result;
		}
		return std::nullopt;
	}

	std::optional<std::vector<LogicalGraphNode>> RelocatableStorageBackend::findNodesByCompositePropertyRange(
			const RelocatableReadSnapshot &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, size_t maximumNodes, size_t maximumPropertyEntries) const {
		requireOwnedSnapshot(snapshot);
		return findNativeNodesByCompositePropertyRange(snapshot.pin_, labels, request, maximumNodes, maximumPropertyEntries);
	}

	std::optional<IndexCandidateBatch> RelocatableStorageBackend::findNativeNodeCandidatesByCompositePropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, size_t maximumNodes,
			size_t maximumPropertyEntries, NativeReadSession *session) const {
		IndexCandidateBatch result;
		const auto complete = findNativeNodesByCompositePropertyRange(snapshot, labels, request, maximumNodes,
														 maximumPropertyEntries, session, &result);
		if (!complete.has_value()) {
			return std::nullopt;
		}
		if (!result.predicates_verified || result.candidates.size() > maximumNodes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native composite range Node lookup returned an invalid candidate batch");
		}
		for (const auto &column: result.verified_property_columns) {
			if (column.values.size() != result.candidates.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
										"relocatable native composite range Node lookup returned an invalid Property column");
			}
		}
		// The composite source already orders the complete batch and its columns.
		if (!std::ranges::is_sorted(result.candidates, {}, &EntityRef::entity_id)) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
					"relocatable native composite range Node lookup returned an unsorted candidate batch");
		}
		return result;
	}

	std::optional<bool> RelocatableStorageBackend::visitNativeNodeCandidatesByCompositePropertyRange(
			const MetadataSnapshotPin &snapshot, const std::vector<std::string> &labels,
			const LogicalGraphCompositePropertyRange &request, const size_t maximumNodes,
			const size_t maximumPropertyEntries, const IndexCandidateBatchVisitor &visitor,
			NativeReadSession *session, size_t maximumCandidateBytes) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable native composite range Node candidate stream requires a visitor");
		}
		bool completed = true;
		IndexCandidateBatchVisitor callback = visitor;
		const auto result = findNativeNodesByCompositePropertyRange(snapshot, labels, request, maximumNodes,
				maximumPropertyEntries, session, nullptr, &callback, &completed, maximumCandidateBytes);
		if (!result.has_value()) {
			return std::nullopt;
		}
		return completed;
	}

	void RelocatableStorageBackend::visitNodes(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<void(const LogicalGraphNode &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable Node scan requires a visitor");
		}
		static_cast<void>(visitNodesUntil(snapshot, maximumRecords, [&visitor](const LogicalGraphNode &node) {
			visitor(node);
			return true;
		}));
	}

	void RelocatableStorageBackend::visitEdges(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<void(const LogicalGraphEdge &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable Edge scan requires a visitor");
		}
		static_cast<void>(visitEdgesUntil(snapshot, maximumRecords, [&visitor](const LogicalGraphEdge &edge) {
			visitor(edge);
			return true;
		}));
	}

	bool
	RelocatableStorageBackend::visitNodesUntil(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<bool(const LogicalGraphNode &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Node scan requires a positive bound and visitor");
		}
		return native_logical_graph_reads_->visitNodesUntil(snapshot.pin_, maximumRecords, visitor);
	}

	bool
	RelocatableStorageBackend::visitEdgesUntil(const RelocatableReadSnapshot &snapshot, size_t maximumRecords,
											   const std::function<bool(const LogicalGraphEdge &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Edge scan requires a positive bound and visitor");
		}
		return native_logical_graph_reads_->visitEdgesUntil(snapshot.pin_, maximumRecords, visitor);
	}

	LogicalGraphNodePage RelocatableStorageBackend::scanNodesAfter(const RelocatableReadSnapshot &snapshot,
																   uint64_t afterNodeId, size_t maximumRecords,
																   size_t maximumMetadataPages) const {
		requireOwnedSnapshot(snapshot);
		if (maximumRecords == 0U || maximumMetadataPages == 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable resumable Node scan requires positive bounds");
		}
		return native_logical_graph_reads_->scanNodesAfter(snapshot.pin_, afterNodeId, maximumRecords,
														   maximumMetadataPages);
	}

	LogicalGraphAdjacencyPage RelocatableStorageBackend::scanAdjacentEdges(
			const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			std::optional<LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords) const {
		requireOwnedSnapshot(snapshot);
		return native_logical_graph_reads_->scanAdjacentEdges(snapshot.pin_, nodeId, direction, std::move(continuation),
															  maximumRecords);
	}

	void
	RelocatableStorageBackend::visitAdjacentEdges(const RelocatableReadSnapshot &snapshot, uint64_t nodeId,
												  LogicalGraphReadDirection direction, size_t maximumEdges,
												  const std::function<void(const LogicalGraphEdge &)> &visitor) const {
		if (!visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable adjacency traversal requires a visitor");
		}
		static_cast<void>(visitAdjacentEdgesUntil(snapshot, nodeId, direction, maximumEdges,
												  [&visitor](const LogicalGraphEdge &edge) {
													  visitor(edge);
													  return true;
												  }));
	}

	bool RelocatableStorageBackend::visitAdjacentEdgesUntil(
			const RelocatableReadSnapshot &snapshot, uint64_t nodeId, LogicalGraphReadDirection direction,
			size_t maximumEdges, const std::function<bool(const LogicalGraphEdge &)> &visitor) const {
		requireOwnedSnapshot(snapshot);
		if (maximumEdges == 0U || !visitor) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable adjacency traversal requires a positive bound and visitor");
		}
		return native_logical_graph_reads_->visitAdjacentEdgesUntil(snapshot.pin_, nodeId, direction, maximumEdges,
																	visitor);
	}

	std::optional<uint32_t> RelocatableStorageBackend::findPropertyKeyToken(const RelocatableReadSnapshot &snapshot,
																			std::string_view key) const {
		requireOwnedSnapshot(snapshot);
		return system_catalog_->findPropertyToken(snapshot.pin_, key);
	}

	std::optional<std::string> RelocatableStorageBackend::findPropertyKey(const RelocatableReadSnapshot &snapshot,
																		  uint32_t token) const {
		requireOwnedSnapshot(snapshot);
		return system_catalog_->findPropertyName(snapshot.pin_, token);
	}

	std::optional<TokenDefinition> RelocatableStorageBackend::findSchemaToken(const RelocatableReadSnapshot &snapshot,
																			  uint64_t tokenId) const {
		requireOwnedSnapshot(snapshot);
		const std::optional<SystemCatalogSchemaToken> label =
				system_catalog_->findSchemaToken(snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, tokenId);
		const std::optional<SystemCatalogSchemaToken> relationshipType = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, tokenId);
		if (label.has_value() && relationshipType.has_value()) { // SystemCatalog allocates both schema domains from one monotonic token sequence, so a valid snapshot cannot bind one ID twice.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable schema Token ID is ambiguous across native schema domains");
		}
		const std::optional<SystemCatalogSchemaToken> token = label.has_value() ? label : relationshipType;
		if (!token.has_value()) {
			return std::nullopt;
		}
		return TokenDefinition{token->token_id, token->name};
	}

	std::optional<uint64_t> RelocatableStorageBackend::findSchemaTokenId(const RelocatableReadSnapshot &snapshot,
																					 std::string_view name) const {
		requireOwnedSnapshot(snapshot);
		const std::optional<uint64_t> label =
				system_catalog_->findSchemaToken(snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, name);
		const std::optional<uint64_t> relationshipType = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
		if (label.has_value() && relationshipType.has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable schema Token name is ambiguous across native schema domains");
		}
		return label.has_value() ? label : relationshipType;
	}

	std::optional<uint64_t>
	RelocatableStorageBackend::findRelationshipTypeTokenId(const RelocatableReadSnapshot &snapshot,
																								std::string_view name) const {
		requireOwnedSnapshot(snapshot);
		if (name.empty()) {
			return std::nullopt;
		}
		debug::PerfTrace::addValue("native_graph.read.relationship_type_token_resolution", 1);
		return system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, name);
	}

	utils::OwnedStdVector<IndexDefinition>
	RelocatableStorageBackend::readIndexDefinitions(const RelocatableReadSnapshot &snapshot,
													size_t maximumDefinitions) const {
		requireOwnedSnapshot(snapshot);
		return readAllIndexDefinitions(snapshot.pin_, maximumDefinitions);
	}

