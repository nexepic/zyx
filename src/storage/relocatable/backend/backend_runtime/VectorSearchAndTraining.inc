	LogicalGraphVectorSearchResult
	RelocatableStorageBackend::searchLogicalVector(const RelocatableReadSnapshot &snapshot,
												   const LogicalGraphVectorSearchRequest &request) const {
		requireOwnedSnapshot(snapshot);
		if (request.index_name.empty() || request.query_vector.empty() || request.top_k == 0U ||
			request.maximum_mapping_entries == 0U || request.maximum_raw_vector_reads == 0U ||
			request.maximum_mapping_entries == std::numeric_limits<size_t>::max() ||
			request.maximum_raw_vector_reads == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument(
					"relocatable logical Vector search requires a name, query, result count, and finite bounds");
		}
		if (!std::ranges::all_of(request.query_vector, [](float value) { return std::isfinite(value); })) {
			throw std::invalid_argument("relocatable logical Vector search query contains a non-finite coordinate");
		}
		if (request.mode != LogicalGraphVectorSearchMode::LGVSM_AUTO &&
			request.mode != LogicalGraphVectorSearchMode::LGVSM_EXACT) {
			throw std::invalid_argument("relocatable logical Vector search received an unknown search mode");
		}

		const utils::OwnedStdVector<IndexDefinition> definitions =
				readAllIndexDefinitions(snapshot.pin_, options_.maximum_index_definitions);
		const auto definition = std::ranges::find_if(
				definitions, [&](const IndexDefinition &candidate) { return candidate.name == request.index_name; });
		if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
			definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE) {
			throw std::runtime_error("Vector index not found: " + request.index_name);
		}
		if (request.query_vector.size() != definition->vector_dimensions) {
			throw std::invalid_argument("relocatable logical Vector search query dimension does not match its Index");
		}
		if (definition->vector_metric == "cosine" &&
			!std::ranges::any_of(request.query_vector, [](float value) { return value != 0.0F; })) {
			throw std::invalid_argument("relocatable cosine Vector search query must have a nonzero L2 norm");
		}
		// An index may validly precede its first matching Property. In that state no
		// physical mapping or Manifest exists yet, so the exact answer is empty.
		if (!metadata_->findVectorManifestOptional(snapshot.pin_, definition->index_id).has_value()) {
			return {};
		}
		const VectorManifest manifest = vector_manifests_->read(snapshot.pin_, definition->index_id).manifest();
		VectorAdjacencyPageStore adjacencies(data_file_->io(), *metadata_,
											 isReadOnly() ? SharedPagePoolOpenMode::SPPOM_READ_ONLY
														  : SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
		return RelocatableVectorSearch::search(snapshot.pin_, *definition, manifest, request, *btree_pages_,
											   *vector_pages_, adjacencies, *vector_codebooks_, *metadata_);
	}

	std::optional<LogicalGraphVectorIndexDescriptor>
	RelocatableStorageBackend::describeLogicalVectorIndex(const RelocatableReadSnapshot &snapshot,
														  const LogicalGraphVectorSearchRequest &request) const {
		requireOwnedSnapshot(snapshot);
		if (request.index_name.empty() || request.query_vector.empty() || request.top_k == 0U ||
			request.maximum_mapping_entries == 0U || request.maximum_raw_vector_reads == 0U ||
			request.maximum_mapping_entries == std::numeric_limits<size_t>::max() ||
			request.maximum_raw_vector_reads == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument(
					"relocatable logical Vector search requires a name, query, result count, and finite bounds");
		}
		if (!std::ranges::all_of(request.query_vector, [](float value) { return std::isfinite(value); })) {
			throw std::invalid_argument("relocatable logical Vector search query contains a non-finite coordinate");
		}
		if (request.mode != LogicalGraphVectorSearchMode::LGVSM_AUTO &&
			request.mode != LogicalGraphVectorSearchMode::LGVSM_EXACT) {
			throw std::invalid_argument("relocatable logical Vector search received an unknown search mode");
		}
		const utils::OwnedStdVector<IndexDefinition> definitions =
				readAllIndexDefinitions(snapshot.pin_, options_.maximum_index_definitions);
		const auto definition = std::ranges::find_if(
				definitions, [&](const IndexDefinition &candidate) { return candidate.name == request.index_name; });
		if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
			definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE ||
			definition->property_names.size() != 1U || definition->label_token_id == 0U ||
			definition->vector_dimensions == 0U) {
			throw std::runtime_error("Vector index not found: " + request.index_name);
		}
		if (request.query_vector.size() != definition->vector_dimensions) {
			throw std::invalid_argument("relocatable logical Vector search query dimension does not match its Index");
		}
		if (definition->vector_metric == "cosine" &&
			!std::ranges::any_of(request.query_vector, [](float value) { return value != 0.0F; })) {
			throw std::invalid_argument("relocatable cosine Vector search query must have a nonzero L2 norm");
		}
		const std::optional<SystemCatalogSchemaToken> label = system_catalog_->findSchemaToken(
				snapshot.pin_, SystemCatalogSchemaTokenKind::SCSTK_LABEL, definition->label_token_id);
		if (!label.has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable logical Vector search found an unknown canonical label Token");
		}
		return LogicalGraphVectorIndexDescriptor{definition->index_id, label->name, definition->property_names.front(),
												 definition->vector_dimensions, definition->vector_metric};
	}

	std::optional<LogicalGraphVectorSearchHit> RelocatableStorageBackend::scoreLogicalVectorExactOverlayCandidate(
			const RelocatableReadSnapshot &snapshot, const LogicalGraphVectorSearchRequest &request,
			const LogicalGraphVectorIndexDescriptor &index, const LogicalGraphNode &node) const {
		requireOwnedSnapshot(snapshot);
		// The descriptor is obtained once from this immutable snapshot before the
		// bounded scan starts. Re-reading the Catalog for every candidate would
		// turn the exact fallback into O(N * catalog-size) metadata work.
		if (index.index_id == 0U || index.label.empty() || index.property_key.empty() || index.dimensions == 0U ||
			request.query_vector.size() != index.dimensions ||
			(index.metric != "l2" && index.metric != "ip" && index.metric != "cosine")) {
			throw std::logic_error("logical Vector exact overlay candidate received an invalid Index descriptor");
		}
		if (!std::ranges::binary_search(node.labels, index.label)) {
			return std::nullopt;
		}
		const auto property = std::ranges::find_if(node.properties, [&](const LogicalGraphPropertyEntry &entry) {
			return entry.key == index.property_key;
		});
		if (property == node.properties.end()) {
			return std::nullopt;
		}
		const std::vector<std::byte> raw = VectorSemanticCodec::encodeBFloat16(property->value, index.dimensions);
		double score = 0.0;
		double querySquaredNorm = 0.0;
		double candidateSquaredNorm = 0.0;
		for (uint32_t dimension = 0U; dimension < index.dimensions; ++dimension) {
			common::BFloat16 encoded;
			encoded.data = LittleEndianCodec::readU16(raw, static_cast<size_t>(dimension) * sizeof(uint16_t));
			const float candidate = encoded.toFloat();
			if (!std::isfinite(candidate)) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "logical Vector exact overlay candidate decoded a non-finite coordinate");
			}
			if (index.metric == "l2") {
				const double difference = static_cast<double>(request.query_vector[dimension]) - candidate;
				score += difference * difference;
			} else {
				const double query = static_cast<double>(request.query_vector[dimension]);
				score += query * candidate;
				querySquaredNorm += query * query;
				candidateSquaredNorm += static_cast<double>(candidate) * candidate;
			}
		}
		if (index.metric == "ip") {
			score = -score;
		} else if (index.metric == "cosine") {
			if (candidateSquaredNorm == 0.0) {
				throw std::invalid_argument("logical Vector exact overlay candidate has zero cosine norm");
			}
			score = 1.0 - std::clamp(score / std::sqrt(querySquaredNorm * candidateSquaredNorm), -1.0, 1.0);
		} else if (index.metric != "l2") {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "logical Vector exact overlay candidate has an unsupported metric");
		}
		if (!std::isfinite(score) || score > static_cast<double>(std::numeric_limits<float>::max()) ||
			score < -static_cast<double>(std::numeric_limits<float>::max())) {
			throw std::overflow_error("logical Vector exact overlay candidate produced a non-finite distance");
		}
		return LogicalGraphVectorSearchHit{node.node_id, static_cast<float>(score)};
	}

	std::unique_ptr<PreparedLogicalGraphVectorTraining>
	RelocatableStorageBackend::prepareLogicalVectorTraining(const LogicalGraphVectorTrainingRequest &request) {
		requireOpen();
		std::unique_lock<std::mutex> publicationLock(native_publication_mutex_);
		checkVectorTrainingCancellation(request);
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot train a logical Vector Index");
		}
		requireNoIndeterminateWrite();
		if (request.index_name.empty() || request.maximum_records == 0U || request.maximum_pages == 0U ||
			request.maximum_records == std::numeric_limits<size_t>::max() ||
			request.maximum_pages == std::numeric_limits<size_t>::max() ||
			options_.maximum_vector_training_records == 0U || options_.maximum_vector_training_pages == 0U ||
			options_.maximum_vector_training_samples == 0U || options_.vector_training_iterations == 0U) {
			throw std::invalid_argument("relocatable logical Vector training requires a nonempty index name and finite "
										"positive resource bounds");
		}
		const size_t maximumRecords = std::min(request.maximum_records, options_.maximum_vector_training_records);
		const size_t maximumPages = std::min(request.maximum_pages, options_.maximum_vector_training_pages);
		if (maximumRecords == 0U || maximumPages == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("relocatable logical Vector training has an invalid effective resource bound");
		}

		// Keep the writer fence until commit so the status inspected by the result
		// pipeline cannot diverge from the generation ultimately published.
		MetadataSnapshotPin sourceSnapshot = metadata_->openSnapshot();
		const utils::OwnedStdVector<IndexDefinition> definitions =
				readAllIndexDefinitions(sourceSnapshot, options_.maximum_index_definitions);
		const auto definition = std::ranges::find_if(
				definitions, [&](const IndexDefinition &candidate) { return candidate.name == request.index_name; });
		if (definition == definitions.end() || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
			definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE) {
			throw std::runtime_error("Vector index not found: " + request.index_name);
		}
		if (definition->vector_dimensions == 0U || definition->vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS) {
			failVectorTraining(FormatError::FERR_INVALID_HEADER,
							   "relocatable logical Vector training requires one valid Vector Index definition");
		}
		bool willTrain = false;
		if (metadata_->findVectorManifestOptional(sourceSnapshot, definition->index_id).has_value()) {
			const utils::OwnedStdVector<BTreePageEntry> firstRecord =
					BTreeIndex(definition->index_id, *btree_pages_, *metadata_)
							.scan(sourceSnapshot, std::nullopt, 1U, BufferPoolReadHint::BPRH_SEQUENTIAL,
								  request.cancellation);
			willTrain = !firstRecord.empty();
		}
		sourceSnapshot.release();

		const LogicalGraphVectorTrainingPreview preview{willTrain};
		return std::make_unique<PreparedRelocatableVectorTraining>(
				std::move(publicationLock), preview, [this, request, preview] {
					const LogicalGraphVectorTrainingResult result = trainLogicalVectorUnderPublicationLock(request);
					if (result.trained != preview.will_train) {
						throw std::logic_error("relocatable Vector training preview diverged from its fenced source");
					}
					return result;
				});
	}

	LogicalGraphVectorTrainingResult RelocatableStorageBackend::trainLogicalVectorUnderPublicationLock(
			const LogicalGraphVectorTrainingRequest &request) {
		requireOpen();
		checkVectorTrainingCancellation(request);
		if (isReadOnly()) {
			throw std::runtime_error("relocatable read-only backend cannot train a logical Vector Index");
		}
		requireNoIndeterminateWrite();
		if (request.index_name.empty() || request.maximum_records == 0U || request.maximum_pages == 0U ||
			request.maximum_records == std::numeric_limits<size_t>::max() ||
			request.maximum_pages == std::numeric_limits<size_t>::max() ||
			options_.maximum_vector_training_records == 0U || options_.maximum_vector_training_pages == 0U ||
			options_.maximum_vector_training_samples == 0U || options_.vector_training_iterations == 0U) {
			throw std::invalid_argument("relocatable logical Vector training requires a nonempty index name and finite "
										"positive resource bounds");
		}

		const size_t maximumRecords = std::min(request.maximum_records, options_.maximum_vector_training_records);
		const size_t maximumPages = std::min(request.maximum_pages, options_.maximum_vector_training_pages);
		if (maximumRecords == 0U || maximumPages == 0U || maximumRecords == std::numeric_limits<size_t>::max()) {
			throw std::invalid_argument("relocatable logical Vector training has an invalid effective resource bound");
		}

		LogicalGraphVectorTrainingResult result;
		{
			MetadataSnapshotPin sourceSnapshot = metadata_->openSnapshot();
			checkVectorTrainingCancellation(request);
            const auto definition = system_catalog_->findIndexDefinitionByName(sourceSnapshot, request.index_name);
            if (!definition || definition->kind != IndexDefinitionKind::IDK_VECTOR ||
				definition->entity_kind != IndexDefinitionEntityKind::IDEK_NODE) {
				throw std::runtime_error("Vector index not found: " + request.index_name);
			}
			if (definition->vector_dimensions == 0U ||
				definition->vector_dimensions > RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS) {
				failVectorTraining(FormatError::FERR_INVALID_HEADER,
								   "relocatable logical Vector training requires one valid Vector Index definition");
			}

			// A schema may exist before its first matching Property is inserted. There
			// is no physical semantic mapping to train in that valid empty state.
			if (!metadata_->findVectorManifestOptional(sourceSnapshot, definition->index_id).has_value()) {
				sourceSnapshot.release();
				return result;
			}
			const VectorManifest manifest = vector_manifests_->read(sourceSnapshot, definition->index_id).manifest();
			const std::optional<IndexRootLocation> forwardRoot =
					metadata_->findIndexRootOptional(sourceSnapshot, definition->index_id);
			const std::optional<IndexRootLocation> reverseRoot =
					metadata_->findIndexRootOptional(sourceSnapshot, manifest.vector_node_mapping_index_id);
			const uint8_t invalidSemanticManifest =
					static_cast<uint8_t>(manifest.index_id != definition->index_id) |
					static_cast<uint8_t>(manifest.dimensions != definition->vector_dimensions) |
					static_cast<uint8_t>(manifest.metric != VectorSemanticCodec::metricFor(*definition)) |
					static_cast<uint8_t>(!manifest.node_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(!manifest.node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(!VectorNodeMapping::isMappingIndexId(manifest.vector_node_mapping_index_id)) |
					static_cast<uint8_t>(!manifest.vector_node_mapping_root_page_id.isValid()) |
					static_cast<uint8_t>(!manifest.vector_node_mapping_root_generation.isValid()) |
					static_cast<uint8_t>(!forwardRoot.has_value()) | static_cast<uint8_t>(!reverseRoot.has_value()) |
					static_cast<uint8_t>(forwardRoot.has_value() &&
										 (forwardRoot->root_page_id != manifest.node_mapping_root_page_id ||
										  forwardRoot->root_generation != manifest.node_mapping_root_generation)) |
					static_cast<uint8_t>(
							reverseRoot.has_value() &&
							(reverseRoot->root_page_id != manifest.vector_node_mapping_root_page_id ||
							 reverseRoot->root_generation != manifest.vector_node_mapping_root_generation));
			if (invalidSemanticManifest != 0U) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found an invalid semantic Manifest generation");
			}
			if (hasAnyPqIdentity(manifest) && !hasCompletePqIdentity(manifest)) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found a partial PQ Manifest generation");
			}
			if (hasCompletePqIdentity(manifest)) {
				const std::optional<IndexRootLocation> pqRoot =
						metadata_->findIndexRootOptional(sourceSnapshot, manifest.pq_mapping_index_id);
				if (!pqRoot.has_value() || pqRoot->root_page_id != manifest.pq_mapping_root_page_id ||
					pqRoot->root_generation != manifest.pq_mapping_root_generation) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found a stale PQ mapping root");
				}
			}
			const size_t nonPqManifestEntries =
					static_cast<size_t>(std::ranges::count_if(manifest.entries, [](const VectorManifestEntry &entry) {
						return entry.storage_kind != VectorStorageKind::VSK_PQ;
					}));
			if (nonPqManifestEntries >= RSS_VECTOR_MANIFEST_MAX_ENTRIES) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training has no Manifest capacity for a PQ generation");
			}
			const size_t maximumPqManifestPages =
					std::min(maximumPages, static_cast<size_t>(RSS_VECTOR_MANIFEST_MAX_ENTRIES) - nonPqManifestEntries);

			const utils::OwnedStdVector<BTreePageEntry> forward =
					BTreeIndex(definition->index_id, *btree_pages_, *metadata_)
							.scan(sourceSnapshot, std::nullopt, checkedBoundPlusOne(maximumRecords,
									"relocatable logical Vector training record bound overflowed"),
								  BufferPoolReadHint::BPRH_SEQUENTIAL, request.cancellation);
			if (forward.size() > maximumRecords) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training exceeded its Vector-record bound");
			}
			std::vector<std::pair<uint64_t, uint64_t>> semanticPairs;
			semanticPairs.reserve(forward.size());
			std::vector<DeterministicPqTrainingRecord> records;
			records.reserve(forward.size());
			const size_t rawRecordBytes =
					sizeof(uint64_t) + static_cast<size_t>(definition->vector_dimensions) * sizeof(uint16_t);
			for (const BTreePageEntry &entry: forward) {
				checkVectorTrainingCancellation(request);
				const uint64_t nodeId = decodeCanonicalNodeMappingKey(entry.key);
				const uint64_t vectorId = entry.value_or_child;
				if (vectorId == 0U) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found a zero semantic Vector ID");
				}
				const std::optional<uint64_t> reverseNode = VectorNodeMapping::findNode(
						sourceSnapshot, *btree_pages_, *metadata_, manifest.vector_node_mapping_index_id, vectorId);
				if (!reverseNode.has_value() || *reverseNode != nodeId) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found non-bijective semantic mappings");
				}
				const VectorLocation location = metadata_->findVectorLocation(sourceSnapshot, vectorId);
				const VectorPage page = vector_pages_->read(sourceSnapshot, location.page_id);
				const uint8_t invalidRawPage = static_cast<uint8_t>(page.header().index_id != definition->index_id) |
											   static_cast<uint8_t>(page.header().kind != VectorStorageKind::VSK_RAW) |
											   static_cast<uint8_t>(page.header().page_id != location.page_id) |
											   static_cast<uint8_t>(page.header().record_bytes != rawRecordBytes);
				if (invalidRawPage != 0U) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found an invalid raw Vector page");
				}
				const std::vector<std::byte> raw = page.read(location.slot_id, location.slot_generation);
				if (raw.size() != rawRecordBytes || LittleEndianCodec::readU64(raw, 0U) != vectorId) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
									   "relocatable logical Vector training found a stale raw Vector record");
				}
				semanticPairs.emplace_back(vectorId, nodeId);
				records.push_back({vectorId, std::vector<std::byte>(raw.begin() + sizeof(uint64_t), raw.end())});
			}
			std::ranges::sort(semanticPairs);
			std::ranges::sort(records, {}, &DeterministicPqTrainingRecord::vector_id);
			if (std::ranges::adjacent_find(semanticPairs, {}, &std::pair<uint64_t, uint64_t>::first) !=
				semanticPairs.end()) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found duplicate semantic Vector IDs");
			}
			const utils::OwnedStdVector<BTreePageEntry> reverse =
					BTreeIndex(manifest.vector_node_mapping_index_id, *btree_pages_, *metadata_)
							.scan(sourceSnapshot, std::nullopt, checkedBoundPlusOne(maximumRecords,
									"relocatable logical Vector training mapping bound overflowed"),
								  BufferPoolReadHint::BPRH_SEQUENTIAL, request.cancellation);
			if (reverse.size() != semanticPairs.size()) {
				failVectorTraining(
						FormatError::FERR_INVALID_STATE,
						"relocatable logical Vector training found mismatched semantic mapping cardinalities");
			}
			for (size_t ordinal = 0U; ordinal < reverse.size(); ++ordinal) {
				checkVectorTrainingCancellation(request);
				const uint64_t vectorId = decodeCanonicalVectorMappingKey(reverse[ordinal].key);
				if (reverse[ordinal].value_or_child == 0U ||
					semanticPairs[ordinal] !=
							std::pair<uint64_t, uint64_t>{vectorId, reverse[ordinal].value_or_child}) {
					failVectorTraining(
							FormatError::FERR_INVALID_STATE,
							"relocatable logical Vector training found inconsistent semantic mapping directions");
				}
			}
			if (records.empty()) {
				sourceSnapshot.release();
				return result;
			}

			const uint32_t subspaces = selectPqSubspaceCount(definition->vector_dimensions);
			const uint32_t centroids = static_cast<uint32_t>(std::min<size_t>(UINT8_MAX + 1U, records.size()));
			const DeterministicPqOptions training{definition->vector_dimensions,
												  subspaces,
												  centroids,
												  options_.vector_training_iterations,
												  maximumRecords,
												  std::min(options_.maximum_vector_training_samples, records.size())};
			OfflineVectorBaseRebuildOptions rebuild;
			rebuild.maximum_delta_mapping_pages = options_.maximum_vector_delta_pages;
			rebuild.maximum_delta_overlay_entries = options_.maximum_vector_delta_overlay_entries;
			rebuild.pq.training = training;
			rebuild.pq.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
			rebuild.pq.codebook.dimensions = training.dimensions;
			rebuild.pq.codebook.subspace_count = training.subspace_count;
			rebuild.pq.codebook.centroid_count = training.centroid_count;
			rebuild.pq.codebook_first_slot = PhysicalSlotId(1U);
			rebuild.pq.codebook_last_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
			rebuild.pq.pages = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
								Generation(1U), maximumRecords, maximumPqManifestPages};
			rebuild.pq.mapping.maximum_entries = maximumRecords;
			rebuild.pq.mapping.tree = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()), 0U,
									   maximumRecords, maximumPages};

			const bool hasAnyAdjacencyIdentity = manifest.adjacency_mapping_index_id != 0U ||
												 manifest.adjacency_mapping_root_page_id.isValid() ||
												 manifest.adjacency_mapping_root_generation.isValid();
			const bool hasCompleteAdjacencyIdentity =
					VectorAdjacencyMapping::isMappingIndexId(manifest.adjacency_mapping_index_id) &&
					manifest.adjacency_mapping_root_page_id.isValid() &&
					manifest.adjacency_mapping_root_generation.isValid() && manifest.adjacency_generation.isValid() &&
					manifest.entry_point_vector_id != 0U;
			if ((hasAnyPqIdentity(manifest) && !hasCompletePqIdentity(manifest)) ||
				(hasAnyAdjacencyIdentity && !hasCompleteAdjacencyIdentity)) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found a partial derived Vector generation");
			}

			const bool hasDelta = manifest.delta_mapping_index_id != 0U ||
								  manifest.delta_mapping_root_page_id.isValid() ||
								  manifest.delta_mapping_root_generation.isValid() || manifest.delta_entry_count != 0U;
			const bool hasDeltaIdentity = manifest.delta_mapping_index_id != 0U;
			const bool hasDeltaRootPage = manifest.delta_mapping_root_page_id.isValid();
			const bool hasDeltaRootGeneration = manifest.delta_mapping_root_generation.isValid();
			if (hasDeltaIdentity != hasDeltaRootPage || hasDeltaIdentity != hasDeltaRootGeneration ||
				(hasDeltaIdentity && manifest.delta_entry_count == 0U) ||
				(!hasDeltaIdentity && manifest.delta_entry_count != 0U) ||
				(hasDeltaIdentity && !VectorDeltaMapping::isMappingIndexId(manifest.delta_mapping_index_id))) {
				failVectorTraining(FormatError::FERR_INVALID_STATE,
								   "relocatable logical Vector training found a partial delta generation");
			}
			if (hasDeltaIdentity) {
				const std::optional<IndexRootLocation> deltaRoot =
						metadata_->findIndexRootOptional(sourceSnapshot, manifest.delta_mapping_index_id);
				if (!deltaRoot.has_value() || deltaRoot->root_page_id != manifest.delta_mapping_root_page_id ||
					deltaRoot->root_generation != manifest.delta_mapping_root_generation) {
					failVectorTraining(FormatError::FERR_INVALID_STATE,
										   "relocatable logical Vector training found a stale delta mapping root");
				}
			}
			bool reuseVisibleCodebook = false;
			if (!hasDelta && hasCompletePqIdentity(manifest) &&
				VectorCodebookExtentIdCodec::canEncodeCodebookId(manifest.codebook_id)) {
				const VectorCodebookManifest existingCodebook =
						vector_codebooks_->manifest(sourceSnapshot, manifest.codebook_id);
				const VectorCodebookHeader &header = existingCodebook.header;
				if (header.generation == manifest.codebook_generation &&
					header.element_type == rebuild.pq.codebook.element_type &&
					header.dimensions == training.dimensions && header.subspace_count == training.subspace_count &&
					header.centroid_count == training.centroid_count) {
					const ExtentLocation rootExtent = metadata_->findExtentLocation(
							sourceSnapshot, VectorCodebookExtentIdCodec::manifestExtentId(manifest.codebook_id));
					rebuild.pq.codebook.codebook_id = manifest.codebook_id;
					rebuild.pq.codebook.manifest_segment_id = rootExtent.segment_id;
					rebuild.pq.codebook.generation = manifest.codebook_generation;
					rebuild.pq.codebook.data_extents.reserve(existingCodebook.entries.size());
					for (const VectorCodebookManifestEntry &entry: existingCodebook.entries) {
						const ExtentLocation location = metadata_->findExtentLocation(sourceSnapshot, entry.extent_id);
						rebuild.pq.codebook.data_extents.push_back({entry.extent_id, location.segment_id});
					}
					rebuild.pq.pages.codebook_generation = manifest.codebook_generation;
					reuseVisibleCodebook = true;
				}
			}

			if (hasCompletePqIdentity(manifest)) {
				rebuild.pq.mapping_index_id = manifest.pq_mapping_index_id;
				rebuild.pq.mapping.tree.pool_owner_id = manifest.pq_mapping_index_id;
			}
			if (hasCompleteAdjacencyIdentity) {
				if (hasDelta && manifest.adjacency_generation.value() == std::numeric_limits<uint64_t>::max()) {
					failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
									   "relocatable logical Vector training exhausted adjacency generation space");
				}
				rebuild.adjacency.mapping_index_id = manifest.adjacency_mapping_index_id;
				rebuild.adjacency.mapping.tree.pool_owner_id = manifest.adjacency_mapping_index_id;
				rebuild.adjacency.adjacency_generation =
						hasDelta ? Generation(manifest.adjacency_generation.value() + 1U)
								 : manifest.adjacency_generation;
			} else {
				rebuild.adjacency.adjacency_generation = Generation(1U);
			}

			const bool needsPqMappingId = rebuild.pq.mapping_index_id == 0U;
			const bool needsAdjacencyMappingId = rebuild.adjacency.mapping_index_id == 0U;
			if (!reuseVisibleCodebook) {
				checkVectorTrainingCancellation(request);
				const uint64_t codebookBytes =
						static_cast<uint64_t>(training.dimensions) * training.centroid_count * sizeof(float);
				if (codebookBytes > std::numeric_limits<size_t>::max()) {
					failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
									   "relocatable logical Vector training Codebook exceeds host-addressable memory");
				}
				const size_t dataExtentCount =
						VectorCodebookStore::requiredDataExtentCount(static_cast<size_t>(codebookBytes));
				if (dataExtentCount == std::numeric_limits<size_t>::max()) {
					failVectorTraining(FormatError::FERR_ARITHMETIC_OVERFLOW,
									   "relocatable logical Vector training derived identity count overflowed");
				}
				const Superblock &sourceSuperblock = sourceSnapshot.superblock();
				const uint64_t maximumCodebookPayload = RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK;
				const uint8_t exhaustedCodebookIdentity =
						static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(
								sourceSuperblock.next_logical_page_id)) |
						static_cast<uint8_t>(dataExtentCount > maximumCodebookPayload) |
						static_cast<uint8_t>(
								dataExtentCount != 0U &&
								(dataExtentCount > maximumCodebookPayload ||
								 sourceSuperblock.next_logical_segment_id == 0U ||
								 sourceSuperblock.next_logical_segment_id > maximumCodebookPayload - dataExtentCount));
				if (exhaustedCodebookIdentity != 0U) {
					failVectorTraining(
							FormatError::FERR_ARITHMETIC_OVERFLOW,
							"relocatable logical Vector training exhausted immutable Codebook identity space");
				}
				const std::vector<SegmentId> segmentIds = metadata_->allocateLogicalSegmentIds(
						dataExtentCount + 1U, nextVectorTrainingCheckpoint(*metadata_));
				const size_t mappingIdentityCount =
						static_cast<size_t>(needsPqMappingId) + static_cast<size_t>(needsAdjacencyMappingId);
				const std::vector<PageId> pageIds = metadata_->allocateLogicalPageIds(
						mappingIdentityCount + 1U, nextVectorTrainingCheckpoint(*metadata_));
				const PageId codebookAllocatorId = pageIds.front();
				rebuild.pq.codebook.codebook_id = codebookAllocatorId.value();
				rebuild.pq.codebook.manifest_segment_id = segmentIds.front();
				rebuild.pq.codebook.generation = Generation(1U);
				rebuild.pq.codebook.data_extents.reserve(dataExtentCount);
				for (size_t ordinal = 0U; ordinal < dataExtentCount; ++ordinal) {
					checkVectorTrainingCancellation(request);
					rebuild.pq.codebook.data_extents.push_back(
							{VectorCodebookExtentIdCodec::dataExtentId(segmentIds.at(ordinal + 1U)),
							 segmentIds.at(ordinal + 1U)});
				}
				// The trailing logical IDs are independently typed so a damaged
				// Manifest can never bind PQ data as adjacency data. Codebook extent
				// IDs derive from their dedicated Segment IDs and need no Page-ID
				// reservation, avoiding unused logical identity gaps for large books.
				size_t mappingOrdinal = 1U;
				if (needsPqMappingId) {
					rebuild.pq.mapping_index_id =
							derivedVectorIdentity(RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal++));
					rebuild.pq.mapping.tree.pool_owner_id = rebuild.pq.mapping_index_id;
				}
				if (needsAdjacencyMappingId) {
					rebuild.adjacency.mapping_index_id = derivedVectorIdentity(
							RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal));
					rebuild.adjacency.mapping.tree.pool_owner_id = rebuild.adjacency.mapping_index_id;
				}
			} else if (needsPqMappingId || needsAdjacencyMappingId) {
				checkVectorTrainingCancellation(request);
				const size_t mappingCount =
						static_cast<size_t>(needsPqMappingId) + static_cast<size_t>(needsAdjacencyMappingId);
				const std::vector<PageId> pageIds =
						metadata_->allocateLogicalPageIds(mappingCount, nextVectorTrainingCheckpoint(*metadata_));
				size_t mappingOrdinal = 0U;
				if (needsPqMappingId) {
					rebuild.pq.mapping_index_id =
							derivedVectorIdentity(RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal++));
					rebuild.pq.mapping.tree.pool_owner_id = rebuild.pq.mapping_index_id;
				}
				if (needsAdjacencyMappingId) {
					rebuild.adjacency.mapping_index_id = derivedVectorIdentity(
							RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX, pageIds.at(mappingOrdinal));
					rebuild.adjacency.mapping.tree.pool_owner_id = rebuild.adjacency.mapping_index_id;
				}
			}

			rebuild.adjacency.maximum_exact_records =
					std::min(options_.maximum_vector_adjacency_exact_records, maximumRecords);
			rebuild.adjacency.build_mode = records.size() <= rebuild.adjacency.maximum_exact_records
												   ? VectorAdjacencyBuildMode::VABM_EXACT
												   : VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
			rebuild.adjacency.maximum_candidate_records = maximumRecords;
			rebuild.adjacency.candidate_search_budget =
					std::min(options_.vector_adjacency_candidate_search_budget, maximumRecords);
			rebuild.adjacency.pruning_alpha = options_.vector_adjacency_pruning_alpha;
			rebuild.adjacency.maximum_degree = options_.maximum_vector_adjacency_degree;
			rebuild.adjacency.pages = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									   maximumRecords, maximumPages};
			rebuild.adjacency.mapping.maximum_entries = maximumRecords;
			rebuild.adjacency.mapping.tree = {PhysicalSlotId(1U), PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
											  rebuild.adjacency.mapping_index_id, maximumRecords, maximumPages};
			sourceSnapshot.release();
			checkVectorTrainingCancellation(request);

			VectorAdjacencyPageStore adjacencyPages(data_file_->io(), *metadata_,
													SharedPagePoolOpenMode::SPPOM_DEFER_RECONCILIATION);
			// Logical ID leases above may have their own durable metadata generation.
			// Compare only the state immediately preceding the derived-generation
			// publisher so a pre-publication rebuild failure stays retryable.
			MetadataSnapshotPin publicationSnapshot = metadata_->openSnapshot();
			const Superblock publicationBase = publicationSnapshot.superblock();
			publicationSnapshot.release();
			OfflineVectorBaseRebuildResult published;
			try {
				published = OfflineVectorBaseRebuild::run(*btree_pages_, *vector_pages_, adjacencyPages,
														  *vector_codebooks_, *vector_manifests_, *metadata_,
														  definition->index_id, records, rebuild, request.cancellation);
			} catch (...) {
				markReopenRequiredIfPublicationAdvanced(publicationBase);
				throw;
			}
			result.vector_count = published.vector_count;
			result.trained = true;
			result.reused_existing_generation = published.already_published;
		}
		return result;
	}

