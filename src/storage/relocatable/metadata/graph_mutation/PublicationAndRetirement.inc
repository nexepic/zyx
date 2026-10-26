
		for (size_t ordinal = 0U; ordinal < materializations.size(); ++ordinal) {
			const EntityPageReplacementMaterialization &materialization = materializations[ordinal];
			const EntityPageReplacementMaterialization &planned = plannedMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.expected_location != planned.expected_location) |
					static_cast<uint8_t>(materialization.retire_page != planned.retire_page) |
					static_cast<uint8_t>(materialization.destination_is_new_pool != planned.destination_is_new_pool) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind));
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch replacement callback returned an invalid destination frame");
			}
			if (materialization.retire_page) {
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(materialization.replacement.page_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.segment_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.page_index != 0U) |
						static_cast<uint8_t>(materialization.replacement.generation.isValid()) |
						static_cast<uint8_t>(materialization.replacement.free_bytes != 0U);
				if (invalidRetirement != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable Entity page retirement callback returned a replacement frame");
				}
				retiredPageIds.push_back(materialization.expected_location.page_id.value());
				continue;
			}
			const uint8_t invalidReplacement =
					static_cast<uint8_t>(materialization.replacement.page_id !=
										 materialization.expected_location.page_id) |
					static_cast<uint8_t>(materialization.replacement.segment_id != materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.replacement.generation != materialization.pool.generation) |
					static_cast<uint8_t>(materialization.replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(
							materialization.replacement.segment_id == materialization.expected_location.segment_id &&
							materialization.replacement.page_index == materialization.expected_location.page_index) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.replacement.page_index)
												  .second);
			if (invalidReplacement != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch replacement callback returned an invalid destination frame");
			}
		}
		for (size_t ordinal = 0U; ordinal < appendMaterializations.size(); ++ordinal) {
			const EntityPageAppendBatchMaterialization &materialization = appendMaterializations[ordinal];
			const EntityPageAppendBatchMaterialization &planned = plannedAppendMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.appended_location.page_id !=
										 planned.appended_location.page_id) |
					static_cast<uint8_t>(materialization.appended_location.segment_id !=
										 materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.appended_location.generation !=
										 materialization.pool.generation) |
					static_cast<uint8_t>(materialization.appended_location.page_index <
										 RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.appended_location.page_index >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind)) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.appended_location.page_index)
												  .second);
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page batch append callback returned an invalid destination frame");
			}
		}
		for (size_t ordinal = 0U; ordinal < newPoolMaterializations.size(); ++ordinal) {
			const EntityPageNewPoolBatchMaterialization &materialization = newPoolMaterializations[ordinal];
			const EntityPageNewPoolBatchMaterialization &planned = plannedNewPoolMaterializations[ordinal];
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.owner_kind != planned.owner_kind) |
					static_cast<uint8_t>(materialization.initial_location.page_id != planned.initial_location.page_id) |
					static_cast<uint8_t>(materialization.initial_location.segment_id !=
										 materialization.pool.segment_id) |
					static_cast<uint8_t>(materialization.initial_location.generation !=
										 materialization.pool.generation) |
					static_cast<uint8_t>(materialization.initial_location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.initial_location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(!isEntityPoolPageOwnerKind(materialization.owner_kind)) |
					static_cast<uint8_t>(!destinationFrames
												  .emplace(materialization.pool.segment_id.value(),
														   materialization.initial_location.page_index)
												  .second);
			if (invalid != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Entity page new-pool callback returned an invalid initial page frame");
			}
		}
		// The typed Entity writer owns payload construction, but the metadata
		// coordinator must authenticate every assigned frame before making its
		// Page Directory route reachable. This remains proportional to this
		// publication rather than scanning the complete Page Directory.
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				entityPublicationFrames;
		const auto registerEntityPublication = [&entityPublicationFrames, checkpointLsn](const CatalogEntry &pool,
																						 const PageLocation &location,
																						 PageOwnerKind ownerKind) {
			auto [iterator, inserted] = entityPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Entity materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, pool.owner_id, ownerKind, checkpointLsn});
		};
		for (const EntityPageReplacementMaterialization &materialization: materializations) {
			if (!materialization.retire_page) {
				registerEntityPublication(materialization.pool, materialization.replacement,
										  materialization.owner_kind);
			}
		}
		for (const EntityPageAppendBatchMaterialization &materialization: appendMaterializations) {
			registerEntityPublication(materialization.pool, materialization.appended_location,
									  materialization.owner_kind);
		}
		for (const EntityPageNewPoolBatchMaterialization &materialization: newPoolMaterializations) {
			registerEntityPublication(materialization.pool, materialization.initial_location,
									  materialization.owner_kind);
		}
		for (const auto &[segmentId, publication]: entityPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}
		for (size_t ordinal = 0U; ordinal < propertyMaterializations.size(); ++ordinal) {
			const NativeGraphPropertyPageReplacementMaterialization &materialization =
					propertyMaterializations[ordinal];
			const NativeGraphPropertyPageReplacementMaterialization &planned = plannedPropertyMaterializations[ordinal];
			const bool invalidReplacement =
					materialization.replacement.page_id != materialization.expected_location.page_id ||
					materialization.replacement.segment_id != materialization.pool.segment_id ||
					materialization.replacement.generation != materialization.pool.generation ||
					materialization.replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME ||
					materialization.replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
					(materialization.replacement.segment_id == materialization.expected_location.segment_id &&
					 materialization.replacement.page_index == materialization.expected_location.page_index) ||
					!destinationFrames
							 .emplace(materialization.pool.segment_id.value(), materialization.replacement.page_index)
							 .second;
			const uint8_t invalid =
					static_cast<uint8_t>(materialization.pool.segment_id != planned.pool.segment_id) |
					static_cast<uint8_t>(materialization.pool.physical_slot != planned.pool.physical_slot) |
					static_cast<uint8_t>(materialization.pool.kind != planned.pool.kind) |
					static_cast<uint8_t>(materialization.pool.owner_id != planned.pool.owner_id) |
					static_cast<uint8_t>(materialization.pool.generation != planned.pool.generation) |
					static_cast<uint8_t>(materialization.pool.state != planned.pool.state) |
					static_cast<uint8_t>(materialization.pool.committed_lsn != planned.pool.committed_lsn) |
					static_cast<uint8_t>(materialization.expected_location != planned.expected_location) |
					static_cast<uint8_t>(materialization.page_owner_id != planned.page_owner_id) |
					static_cast<uint8_t>(materialization.expected_owner_lsn != planned.expected_owner_lsn) |
					static_cast<uint8_t>(materialization.retire_page != planned.retire_page) |
					static_cast<uint8_t>(materialization.destination_is_new_pool != planned.destination_is_new_pool) |
					static_cast<uint8_t>(materialization.destination_initializes_new_pool !=
										 planned.destination_initializes_new_pool) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(materialization.page_owner_id == 0U) |
					static_cast<uint8_t>(!materialization.expected_owner_lsn.isValid()) |
					static_cast<uint8_t>(!materialization.retire_page && invalidReplacement);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph page transaction callback returned an invalid Property page frame");
			}
			if (materialization.retire_page) {
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(materialization.replacement.page_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.segment_id.isValid()) |
						static_cast<uint8_t>(materialization.replacement.page_index != 0U) |
						static_cast<uint8_t>(materialization.replacement.generation.isValid()) |
						static_cast<uint8_t>(materialization.replacement.free_bytes != 0U);
				if (invalidRetirement != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph Property page retirement callback returned a replacement frame");
				}
				retiredPageIds.push_back(materialization.expected_location.page_id.value());
			}
		}
		for (size_t ordinal = 0U; ordinal < propertyPageBatchMaterializations.size(); ++ordinal) {
			const NativeGraphPropertyPageBatchMaterialization &materialization =
					propertyPageBatchMaterializations[ordinal];
			const NativeGraphPropertyPageBatchMaterialization &planned =
					plannedPropertyPageBatchMaterializations[ordinal];
			const uint8_t invalidPool =
					static_cast<uint8_t>(materialization.kind != planned.kind) |
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.pool, planned.pool)) |
					static_cast<uint8_t>(materialization.pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL) |
					static_cast<uint8_t>(materialization.pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(materialization.page_owner_id != planned.page_owner_id) |
					static_cast<uint8_t>(materialization.page_owner_id == 0U) |
					static_cast<uint8_t>(materialization.pages.size() != planned.pages.size()) |
					static_cast<uint8_t>(materialization.pages.empty()) |
					static_cast<uint8_t>(materialization.pages.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph page transaction callback changed a Property page-batch identity");
			}
			for (size_t pageOrdinal = 0U; pageOrdinal < materialization.pages.size(); ++pageOrdinal) {
				const NativeGraphPropertyPageInitialMaterialization &page = materialization.pages[pageOrdinal];
				const NativeGraphPropertyPageInitialMaterialization &expectedPage = planned.pages[pageOrdinal];
				const uint8_t invalidPage =
						static_cast<uint8_t>(page.location.page_id != expectedPage.location.page_id) |
						static_cast<uint8_t>(page.location.segment_id != materialization.pool.segment_id) |
						static_cast<uint8_t>(page.location.generation != materialization.pool.generation) |
						static_cast<uint8_t>(page.location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(page.location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(page.location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
						static_cast<uint8_t>(page.upserted_properties.size() !=
											 expectedPage.upserted_properties.size()) |
						static_cast<uint8_t>(
								!destinationFrames
										 .emplace(materialization.pool.segment_id.value(), page.location.page_index)
										 .second);
				if (invalidPage != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph page transaction callback returned an invalid Property page frame");
				}
				for (size_t propertyOrdinal = 0U; propertyOrdinal < page.upserted_properties.size();
					 ++propertyOrdinal) {
					const EntityPropertyLocation &property = page.upserted_properties[propertyOrdinal];
					const EntityPropertyLocation &expected = expectedPage.upserted_properties[propertyOrdinal];
					const uint8_t invalidProperty =
							static_cast<uint8_t>(property.key != expected.key) |
							static_cast<uint8_t>(property.page_id != page.location.page_id) |
							static_cast<uint8_t>(property.slot_id == std::numeric_limits<uint16_t>::max()) |
							static_cast<uint8_t>(property.slot_generation == 0U) |
							static_cast<uint8_t>(
									!publishedPropertySlots.emplace(property.page_id.value(), property.slot_id).second);
					if (invalidProperty != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph Property page callback returned an invalid directory location");
					}
					upsertedEntityProperties.push_back(property);
				}
			}
		}
		// Property pages are owner-keyed slotted payloads. Authenticate their
		// physical headers after the typed preparer has encoded them and before
		// the owner-keyed Property Directory can publish a route to either page.
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				propertyPublicationFrames;
		const auto registerPropertyPublication = [&propertyPublicationFrames,
												  checkpointLsn](const CatalogEntry &pool, const PageLocation &location,
																 uint64_t pageOwnerId) {
			auto [iterator, inserted] = propertyPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Property materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, pageOwnerId, PageOwnerKind::POK_PROPERTY, checkpointLsn});
		};
		for (const NativeGraphPropertyPageReplacementMaterialization &materialization: propertyMaterializations) {
			if (!materialization.retire_page) {
				registerPropertyPublication(materialization.pool, materialization.replacement,
											materialization.page_owner_id);
			}
		}
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: propertyPageBatchMaterializations) {
			for (const NativeGraphPropertyPageInitialMaterialization &page: materialization.pages) {
				registerPropertyPublication(materialization.pool, page.location, materialization.page_owner_id);
			}
		}
		for (const auto &[segmentId, publication]: propertyPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}
		std::vector<PageLocation> publishedLargeValuePages;
		std::vector<LargeValueManifestLocation> publishedLargeValueManifests;
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				largeValuePublicationFrames;
		const auto registerLargeValuePublication = [&largeValuePublicationFrames, checkpointLsn](
														   const CatalogEntry &pool, const PageLocation &location,
														   uint64_t valueId, PageOwnerKind ownerKind) {
			auto [iterator, inserted] = largeValuePublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph LargeValue materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, valueId, ownerKind, checkpointLsn});
		};
		for (size_t ordinal = 0U; ordinal < largeValueMaterializations.size(); ++ordinal) {
			const LargeValueMaterializationResult &materialization = largeValueMaterializations[ordinal];
			const LargeValueMaterializationResult &planned = plannedLargeValueMaterializations[ordinal];
			const NativeGraphLargeValueMutation &mutation = largeValues[ordinal];
			const bool hasManifestPool = !materialization.manifest_pools.empty();
			const uint8_t invalidManifest =
					static_cast<uint8_t>(materialization.value_id != planned.value_id) |
					static_cast<uint8_t>(materialization.value_id != mutation.reference.value_id) |
					static_cast<uint8_t>(materialization.data_pools.size() != planned.data_pools.size()) |
					static_cast<uint8_t>(materialization.manifest_pools.size() != planned.manifest_pools.size()) |
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.manifest_pool, planned.manifest_pool)) |
					static_cast<uint8_t>(materialization.manifest_page_id != planned.manifest_page_id) |
					static_cast<uint8_t>(materialization.manifest_location.page_id != planned.manifest_page_id) |
					static_cast<uint8_t>(materialization.manifest_location.segment_id !=
										 materialization.manifest_pool.segment_id) |
					static_cast<uint8_t>(materialization.manifest_location.generation !=
										 mutation.reference.generation) |
					static_cast<uint8_t>(materialization.manifest_location.page_index <
										 RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(materialization.manifest_location.page_index >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(materialization.manifest_location.free_bytes != 0U) |
					static_cast<uint8_t>(!hasManifestPool);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue preparer changed its manifest identity or frame");
			}
			const LargeValueManifestMaterializationPoolBatch &firstManifestPool =
					materialization.manifest_pools.front();
			const uint8_t invalidManifestRoot =
					static_cast<uint8_t>(!hasSameCatalogEntry(materialization.manifest_pool, firstManifestPool.pool)) |
					static_cast<uint8_t>(firstManifestPool.page_ids.empty()) |
					static_cast<uint8_t>(firstManifestPool.locations.empty()) |
					static_cast<uint8_t>(firstManifestPool.page_ids.front() != materialization.manifest_page_id) |
					static_cast<uint8_t>(firstManifestPool.locations.front() != materialization.manifest_location);
			if (invalidManifestRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph LargeValue preparer changed its manifest root frame");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialization.data_pools.size(); ++poolOrdinal) {
				const LargeValueDataMaterializationPoolBatch &pool = materialization.data_pools[poolOrdinal];
				const LargeValueDataMaterializationPoolBatch &expected = planned.data_pools[poolOrdinal];
				const uint8_t invalidPool =
						static_cast<uint8_t>(!hasSameCatalogEntry(pool.pool, expected.pool)) |
						static_cast<uint8_t>(pool.page_ids != expected.page_ids) |
						static_cast<uint8_t>(pool.is_new_pool != expected.is_new_pool) |
						static_cast<uint8_t>(pool.prepared_prefix_count != expected.prepared_prefix_count) |
						static_cast<uint8_t>(!pool.is_new_pool && pool.prepared_prefix_count != 0U) |
						static_cast<uint8_t>(pool.page_ids.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(
								pool.is_new_pool &&
								(pool.prepared_prefix_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
								 pool.page_ids.size() >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT -
												 std::min(pool.prepared_prefix_count,
														  static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)))) |
						static_cast<uint8_t>(pool.locations.size() != pool.page_ids.size());
				if (invalidPool != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph LargeValue preparer changed a data-pool identity or Page IDs");
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
					const PageLocation &location = pool.locations[pageOrdinal];
					const uint8_t invalidLocation =
							static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
							static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
							static_cast<uint8_t>(location.generation != pool.pool.generation) |
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
							static_cast<uint8_t>(location.free_bytes != 0U) |
							static_cast<uint8_t>(pool.is_new_pool &&
												 location.page_index != pool.prepared_prefix_count + pageOrdinal + 1U) |
							static_cast<uint8_t>(
									!destinationFrames.emplace(location.segment_id.value(), location.page_index)
											 .second);
					if (invalidLocation != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph LargeValue preparer returned an invalid or duplicate data frame");
					}
					registerLargeValuePublication(pool.pool, location, mutation.reference.value_id.value(),
												  PageOwnerKind::POK_LARGE_VALUE_DATA);
					publishedLargeValuePages.push_back(location);
				}
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialization.manifest_pools.size(); ++poolOrdinal) {
				const LargeValueManifestMaterializationPoolBatch &pool = materialization.manifest_pools[poolOrdinal];
				const LargeValueManifestMaterializationPoolBatch &expected = planned.manifest_pools[poolOrdinal];
				const uint8_t invalidPool =
						static_cast<uint8_t>(!hasSameCatalogEntry(pool.pool, expected.pool)) |
						static_cast<uint8_t>(pool.page_ids != expected.page_ids) |
						static_cast<uint8_t>(pool.is_new_pool != expected.is_new_pool) |
						static_cast<uint8_t>(pool.prepared_prefix_count != expected.prepared_prefix_count) |
						static_cast<uint8_t>(!pool.is_new_pool && pool.prepared_prefix_count != 0U) |
						static_cast<uint8_t>(pool.page_ids.size() > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(
								pool.is_new_pool &&
								(pool.prepared_prefix_count > RSS_PAGE_POOL_DATA_FRAME_COUNT ||
								 pool.page_ids.size() >
										 RSS_PAGE_POOL_DATA_FRAME_COUNT -
												 std::min(pool.prepared_prefix_count,
														  static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT)))) |
						static_cast<uint8_t>(pool.locations.size() != pool.page_ids.size());
				if (invalidPool != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph LargeValue preparer changed a manifest-pool identity or Page IDs");
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
					const PageLocation &location = pool.locations[pageOrdinal];
					const uint8_t invalidLocation =
							static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
							static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
							static_cast<uint8_t>(location.generation != pool.pool.generation) |
							static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
							static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
							static_cast<uint8_t>(location.free_bytes != 0U) |
							static_cast<uint8_t>(pool.is_new_pool &&
												 location.page_index != pool.prepared_prefix_count + pageOrdinal + 1U) |
							static_cast<uint8_t>(
									!destinationFrames.emplace(location.segment_id.value(), location.page_index)
											 .second);
					if (invalidLocation != 0U) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"native graph LargeValue preparer returned an invalid or duplicate manifest frame");
					}
					registerLargeValuePublication(pool.pool, location, mutation.reference.value_id.value(),
												  PageOwnerKind::POK_LARGE_VALUE_MANIFEST);
					publishedLargeValuePages.push_back(location);
				}
			}
			publishedLargeValueManifests.push_back({mutation.reference.value_id, materialization.manifest_page_id,
													materialization.manifest_pool.generation});
		}
		for (const auto &[segmentId, publication]: largeValuePublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}

		std::vector<PageLocation> publishedDerivedIndexPages;
		std::vector<IndexRootLocation> publishedDerivedIndexRoots;
		std::vector<uint64_t> erasedDerivedIndexRootIds;
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				derivedIndexPublicationFrames;
		const auto registerDerivedIndexPublication = [&derivedIndexPublicationFrames,
													  checkpointLsn](const CatalogEntry &pool,
																	 const PageLocation &location, uint64_t indexId) {
			auto [iterator, inserted] = derivedIndexPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Index materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, indexId, PageOwnerKind::POK_INDEX, checkpointLsn});
		};
		publishedDerivedIndexRoots.reserve(effectiveIndexMutations.size());
		erasedDerivedIndexRootIds.reserve(effectiveIndexMutations.size());
		for (size_t mutationOrdinal = 0U; mutationOrdinal < effectiveIndexMutations.size(); ++mutationOrdinal) {
			const NativeGraphIndexTreeMutation &mutation = effectiveIndexMutations[mutationOrdinal];
			const std::vector<IndexTreeMutationPoolBatch> &planned = plannedDerivedIndexBatches[mutationOrdinal];
			const std::vector<IndexTreeMutationPoolBatch> &materialized = derivedIndexBatches[mutationOrdinal];
			if (materialized.size() != planned.size()) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph derived-state preparer changed its Index pool batch cardinality");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < materialized.size(); ++poolOrdinal) {
				const IndexTreeMutationPoolBatch &expected = planned[poolOrdinal];
				const IndexTreeMutationPoolBatch &actual = materialized[poolOrdinal];
				const bool invalidShape = // actual and expected pool records are emitted from the same authenticated materialization plan.
						actual.pool.segment_id != expected.pool.segment_id ||
						actual.pool.physical_slot != expected.pool.physical_slot ||
						actual.pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL ||
						actual.pool.kind != expected.pool.kind || actual.pool.owner_id != expected.pool.owner_id ||
						actual.pool.generation != expected.pool.generation ||
						actual.pool.state != SegmentState::SGS_SEALED || actual.pool.state != expected.pool.state ||
						actual.pool.committed_lsn != expected.pool.committed_lsn ||
						actual.replacement_pool.segment_id != expected.replacement_pool.segment_id ||
						actual.replacement_pool.physical_slot != expected.replacement_pool.physical_slot ||
						actual.replacement_pool.kind != expected.replacement_pool.kind ||
						actual.replacement_pool.owner_id != expected.replacement_pool.owner_id ||
						actual.replacement_pool.generation != expected.replacement_pool.generation ||
						actual.replacement_pool.state != expected.replacement_pool.state ||
						actual.replacement_pool.committed_lsn != expected.replacement_pool.committed_lsn ||
						actual.expected_replacements != expected.expected_replacements ||
						actual.appended_page_ids != expected.appended_page_ids ||
						actual.expected_retirements != expected.expected_retirements ||
						actual.newly_reserved != expected.newly_reserved ||
						actual.prepared_new_pool_prefix_count != expected.prepared_new_pool_prefix_count ||
						actual.replacement_locations.size() != expected.replacement_locations.size() ||
						actual.appended_locations.size() != expected.appended_locations.size();
				if (invalidShape) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"native graph derived-state preparer changed an Index pool identity or page shape");
				}
				const auto validatePublishedLocation =
						[&destinationFrames, &publishedDerivedIndexPages, &mutation,
						 &registerDerivedIndexPublication](const PageLocation &location, PageId expectedPageId,
														 const CatalogEntry &expectedPool) {
							const uint8_t invalid =
									static_cast<uint8_t>(location.page_id != expectedPageId) |
									static_cast<uint8_t>(location.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(location.generation != expectedPool.generation) |
									static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
									static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
									static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(
											!destinationFrames.emplace(location.segment_id.value(), location.page_index)
													 .second);
							if (invalid != 0U) {
								throw FormatException(
										FormatError::FERR_INVALID_STATE,
										"native graph derived-state preparer returned an invalid Index page frame");
							}
							publishedDerivedIndexPages.push_back(location);
							registerDerivedIndexPublication(expectedPool, location, mutation.index_id);
						};
				for (size_t pageOrdinal = 0U; pageOrdinal < actual.replacement_locations.size(); ++pageOrdinal) {
					validatePublishedLocation(actual.replacement_locations[pageOrdinal],
											  actual.expected_replacements[pageOrdinal].page_id,
											  actual.replacement_pool.segment_id.isValid() ? actual.replacement_pool : actual.pool);
				}
				for (size_t pageOrdinal = 0U; pageOrdinal < actual.appended_locations.size(); ++pageOrdinal) {
					validatePublishedLocation(actual.appended_locations[pageOrdinal],
											  actual.appended_page_ids[pageOrdinal], actual.pool);
				}
				for (const IndexTreePageRetirement &retirement: actual.expected_retirements) {
					retiredPageIds.push_back(retirement.location.page_id.value());
				}
			}
			if (mutation.retire_root) {
				erasedDerivedIndexRootIds.push_back(mutation.index_id);
				continue;
			}

			IndexRootLocation publishedRoot{mutation.index_id, {}, {}};
			if (mutation.expected_root.has_value()) {
				publishedRoot = *mutation.expected_root;
			}
			if (mutation.replacement_root.has_value()) {
				const NativeGraphIndexTreeAppendedPageReference &reference = *mutation.replacement_root;
				const size_t poolOrdinal = reference.newly_reserved
												   ? mutation.pool_requests.size() + reference.pool_ordinal
												   : reference.pool_ordinal;
				const IndexTreeMutationPoolBatch &pool = materialized[poolOrdinal];
				const PageLocation &location = pool.appended_locations[reference.appended_page_ordinal];
				publishedRoot.root_page_id = location.page_id;
				publishedRoot.root_generation = location.generation;
			} else if (mutation.stable_replacement_root.has_value()) {
				const PageId pageId = *mutation.stable_replacement_root;
				const auto published = std::ranges::find(publishedDerivedIndexPages, pageId, &PageLocation::page_id);
				const PageLocation location = published == publishedDerivedIndexPages.end()
													  ? locations_.findPage(current, pageId)
													  : *published;
				const auto newPool =
						std::ranges::find(newDerivedIndexPoolEntries, location.segment_id, &CatalogEntry::segment_id);
				const CatalogEntry &pool = newPool == newDerivedIndexPoolEntries.end()
												   ? catalog_.find(current, location.segment_id)
												   : *newPool;
				const uint8_t invalidStableRoot =
						static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(pool.owner_id == 0U) |
						static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(pool.generation != location.generation) |
						static_cast<uint8_t>(newPool == newDerivedIndexPoolEntries.end() &&
											 !allocator_.isAllocated(current, pool.physical_slot));
				if (invalidStableRoot != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph derived-state mutation resolved an invalid stable Index root");
				}
				publishedRoot.root_page_id = pageId;
				publishedRoot.root_generation = location.generation;
			}
			publishedDerivedIndexRoots.push_back(publishedRoot);
		}
		for (NativeGraphVectorDeltaMaterialization &materialization: vectorDeltaMaterializations) {
			const auto nodeRoot = std::ranges::find(publishedDerivedIndexRoots, materialization.mutation.index_id,
													&IndexRootLocation::index_id);
			const auto reverseRoot =
					std::ranges::find(publishedDerivedIndexRoots, materialization.mutation.vector_node_mapping_index_id,
									  &IndexRootLocation::index_id);
			const auto deltaRoot = materialization.mutation.delta_mapping_index_id == 0U
										   ? publishedDerivedIndexRoots.end()
										   : std::ranges::find(publishedDerivedIndexRoots,
															   materialization.mutation.delta_mapping_index_id,
															   &IndexRootLocation::index_id);
			if (nodeRoot == publishedDerivedIndexRoots.end() || reverseRoot == publishedDerivedIndexRoots.end() || // vector root identities are authenticated before publication replay.
				(materialization.mutation.delta_mapping_index_id != 0U &&
				 deltaRoot == publishedDerivedIndexRoots.end())) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector delta lost a published semantic or overlay mapping-tree root");
			}
			materialization.node_mapping_root = *nodeRoot;
			materialization.vector_node_mapping_root = *reverseRoot;
			if (deltaRoot != publishedDerivedIndexRoots.end()) {
				materialization.delta_mapping_root = *deltaRoot;
			}
		}
		const std::vector<NativeGraphVectorDeltaMaterialization> plannedVectorDeltaMaterializations =
				vectorDeltaMaterializations;
		if (!vectorDeltaMaterializations.empty()) {
			vectorDeltaPreparer(vectorDeltaMaterializations);
		}

		std::vector<PageLocation> publishedVectorBootstrapPages;
		std::vector<VectorLocation> publishedVectorBootstrapLocations;
		std::vector<VectorManifestLocation> publishedVectorBootstrapManifests;
		std::map<uint64_t, std::pair<CatalogEntry, std::vector<SharedPagePoolPreparedPublication>>>
				vectorPublicationFrames;
		const auto registerVectorPublication = [&vectorPublicationFrames,
												checkpointLsn](const CatalogEntry &pool, const PageLocation &location,
															   uint64_t indexId, PageOwnerKind ownerKind) {
			const bool expectedPoolKind =
					(ownerKind == PageOwnerKind::POK_VECTOR_RAW && pool.kind == SegmentKind::SGK_VECTOR_RAW_POOL) ||
					(ownerKind == PageOwnerKind::POK_VECTOR_MANIFEST &&
					 pool.kind == SegmentKind::SGK_VECTOR_MANIFEST_POOL);
			if (!expectedPoolKind || pool.owner_id != indexId) { // vector materialization supplies a pool kind and owner paired by preflight.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector materialization has an incompatible pool or owner identity");
			}
			auto [iterator, inserted] = vectorPublicationFrames.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.first = pool;
			} else if (!hasSameCatalogEntry(iterator->second.first, pool)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"native graph Vector materialization reused a Segment ID with conflicting pool identity");
			}
			iterator->second.second.push_back({location, indexId, ownerKind, checkpointLsn});
		};
		std::set<std::pair<uint64_t, uint16_t>> publishedVectorBootstrapSlots;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < vectorBootstrapMaterializations.size(); ++mutationOrdinal) {
			const NativeGraphVectorBootstrapMaterialization &actual = vectorBootstrapMaterializations[mutationOrdinal];
			const NativeGraphVectorBootstrapMaterialization &planned =
					plannedVectorBootstrapMaterializations[mutationOrdinal];
			const NativeGraphVectorBootstrapMutation &mutation = planned.mutation;
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(actual.mutation.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.mutation.vector_node_mapping_index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.first_vector_id != mutation.first_vector_id) |
					static_cast<uint8_t>(actual.mutation.vector_count != mutation.vector_count) |
					static_cast<uint8_t>(actual.mutation.dimensions != mutation.dimensions) |
					static_cast<uint8_t>(actual.mutation.metric != mutation.metric) |
					static_cast<uint8_t>(actual.node_mapping_root != planned.node_mapping_root) |
					static_cast<uint8_t>(actual.vector_node_mapping_root != planned.vector_node_mapping_root) |
					static_cast<uint8_t>(actual.raw_pools.size() != planned.raw_pools.size()) |
					static_cast<uint8_t>(actual.raw_pages.size() != planned.raw_pages.size()) |
					static_cast<uint8_t>(actual.vector_locations.size() != mutation.vector_count) |
					static_cast<uint8_t>(actual.manifest_location != planned.manifest_location);
			if (invalidIdentity != 0U || !hasSameCatalogEntry(actual.manifest_pool, planned.manifest_pool)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap preparer changed a metadata-owned identity");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < actual.raw_pools.size(); ++poolOrdinal) {
				if (!hasSameCatalogEntry(actual.raw_pools[poolOrdinal], planned.raw_pools[poolOrdinal])) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer changed a Raw-pool identity");
				}
			}
			std::set<uint64_t> rawPageIds;
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.raw_pages.size(); ++pageOrdinal) {
				const PageLocation &location = actual.raw_pages[pageOrdinal];
				const PageLocation &expected = planned.raw_pages[pageOrdinal];
				const CatalogEntry &pool = actual.raw_pools[pageOrdinal / RSS_PAGE_POOL_DATA_FRAME_COUNT];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != expected.page_id) |
						static_cast<uint8_t>(location.segment_id != pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
						static_cast<uint8_t>(!rawPageIds.insert(location.page_id.value()).second) |
						static_cast<uint8_t>(
								!destinationFrames.emplace(location.segment_id.value(), location.page_index).second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer returned an invalid Raw page frame");
				}
				publishedVectorBootstrapPages.push_back(location);
				registerVectorPublication(pool, location, mutation.index_id, PageOwnerKind::POK_VECTOR_RAW);
			}
			const PageLocation &manifest = actual.manifest_page;
			const uint8_t invalidManifest =
					static_cast<uint8_t>(manifest.page_id != planned.manifest_page.page_id) |
					static_cast<uint8_t>(manifest.segment_id != actual.manifest_pool.segment_id) |
					static_cast<uint8_t>(manifest.generation != actual.manifest_pool.generation) |
					static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(manifest.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
					static_cast<uint8_t>(
							!destinationFrames.emplace(manifest.segment_id.value(), manifest.page_index).second);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector bootstrap preparer returned an invalid Manifest page frame");
			}
			registerVectorPublication(actual.manifest_pool, manifest, mutation.index_id,
									  PageOwnerKind::POK_VECTOR_MANIFEST);
			for (size_t vectorOrdinal = 0U; vectorOrdinal < actual.vector_locations.size(); ++vectorOrdinal) {
				const VectorLocation &location = actual.vector_locations[vectorOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.vector_id != mutation.first_vector_id + vectorOrdinal) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(!rawPageIds.contains(location.page_id.value())) |
						static_cast<uint8_t>(
								!publishedVectorBootstrapSlots.emplace(location.page_id.value(), location.slot_id)
										 .second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector bootstrap preparer returned an invalid Vector location");
				}
				publishedVectorBootstrapLocations.push_back(location);
			}
			publishedVectorBootstrapPages.push_back(manifest);
			publishedVectorBootstrapManifests.push_back(actual.manifest_location);
		}

		std::vector<PageLocation> publishedVectorDeltaPages;
		std::vector<VectorLocation> publishedVectorDeltaLocations;
		std::vector<uint64_t> erasedVectorDeltaIds;
		std::vector<VectorManifestLocation> publishedVectorDeltaManifests;
		std::set<std::pair<uint64_t, uint16_t>> publishedVectorDeltaSlots;
		for (size_t mutationOrdinal = 0U; mutationOrdinal < vectorDeltaMaterializations.size(); ++mutationOrdinal) {
			const NativeGraphVectorDeltaMaterialization &actual = vectorDeltaMaterializations[mutationOrdinal];
			const NativeGraphVectorDeltaMaterialization &planned = plannedVectorDeltaMaterializations[mutationOrdinal];
			const NativeGraphVectorDeltaMutation &mutation = planned.mutation;
			const uint8_t invalidIdentity =
					static_cast<uint8_t>(actual.mutation.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.mutation.vector_node_mapping_index_id !=
										 mutation.vector_node_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.expected_delta_mapping_index_id !=
										 mutation.expected_delta_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.delta_mapping_index_id != mutation.delta_mapping_index_id) |
					static_cast<uint8_t>(actual.mutation.expected_delta_mapping_root !=
										 mutation.expected_delta_mapping_root) |
					static_cast<uint8_t>(actual.mutation.delta_entry_count != mutation.delta_entry_count) |
					static_cast<uint8_t>(actual.mutation.first_vector_id != mutation.first_vector_id) |
					static_cast<uint8_t>(actual.mutation.inserted_vector_count != mutation.inserted_vector_count) |
					static_cast<uint8_t>(actual.mutation.erased_vector_ids != mutation.erased_vector_ids) |
					static_cast<uint8_t>(actual.node_mapping_root != planned.node_mapping_root) |
					static_cast<uint8_t>(actual.vector_node_mapping_root != planned.vector_node_mapping_root) |
					static_cast<uint8_t>(actual.delta_mapping_root != planned.delta_mapping_root) |
					static_cast<uint8_t>(actual.raw_new_pools.size() != planned.raw_new_pools.size()) |
					static_cast<uint8_t>(actual.raw_pages.size() != planned.raw_pages.size()) |
					static_cast<uint8_t>(actual.vector_locations.size() != mutation.inserted_vector_count) |
					static_cast<uint8_t>(actual.manifest_location.index_id != mutation.index_id) |
					static_cast<uint8_t>(actual.manifest_location.manifest_page_id !=
										 actual.manifest_replacement.page_id) |
					static_cast<uint8_t>(actual.manifest_location.generation != actual.manifest_replacement.generation);
			if (invalidIdentity != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer changed a metadata-owned identity");
			}
			for (size_t poolOrdinal = 0U; poolOrdinal < actual.raw_new_pools.size(); ++poolOrdinal) {
				if (!hasSameCatalogEntry(actual.raw_new_pools[poolOrdinal], planned.raw_new_pools[poolOrdinal])) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer changed a fresh Raw-pool identity");
				}
			}
			if (mutation.manifest_in_new_pool &&
				!hasSameCatalogEntry(actual.manifest_new_pool, planned.manifest_new_pool)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer changed a fresh Manifest-pool identity");
			}
			std::set<uint64_t> rawPageIds;
			for (size_t pageOrdinal = 0U; pageOrdinal < actual.raw_pages.size(); ++pageOrdinal) {
				const NativeGraphVectorDeltaRawPageMaterialization &raw = actual.raw_pages[pageOrdinal];
				const NativeGraphVectorDeltaRawPageMaterialization &expected = planned.raw_pages[pageOrdinal];
				const bool cow = expected.mutation.placement == NativeGraphVectorDeltaRawPlacement::NGVDRP_COW_TAIL;
				const uint8_t invalidRaw =
						static_cast<uint8_t>(raw.mutation.placement != expected.mutation.placement) |
						static_cast<uint8_t>(raw.mutation.page_ordinal != expected.mutation.page_ordinal) |
						static_cast<uint8_t>(raw.mutation.inserted_record_count !=
											 expected.mutation.inserted_record_count) |
						static_cast<uint8_t>(!hasSameCatalogEntry(raw.pool, expected.pool)) |
						static_cast<uint8_t>(raw.location.page_id != expected.location.page_id) |
						static_cast<uint8_t>(raw.location.segment_id != raw.pool.segment_id) |
						static_cast<uint8_t>(raw.location.generation != raw.pool.generation) |
						static_cast<uint8_t>(raw.location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(raw.location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(cow && raw.location.page_index == expected.location.page_index) |
						static_cast<uint8_t>(!rawPageIds.insert(raw.location.page_id.value()).second) |
						static_cast<uint8_t>(
								!destinationFrames.emplace(raw.location.segment_id.value(), raw.location.page_index)
										 .second);
				if (invalidRaw != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer returned an invalid Raw page frame");
				}
				publishedVectorDeltaPages.push_back(raw.location);
				registerVectorPublication(raw.pool, raw.location, mutation.index_id, PageOwnerKind::POK_VECTOR_RAW);
			}
			const PageLocation &manifest = actual.manifest_replacement;
			const uint8_t invalidManifest =
					static_cast<uint8_t>(manifest.page_id != planned.manifest_replacement.page_id) |
					static_cast<uint8_t>(manifest.segment_id != planned.manifest_replacement.segment_id) |
					static_cast<uint8_t>(manifest.generation != planned.manifest_replacement.generation) |
					static_cast<uint8_t>(manifest.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(manifest.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(manifest.segment_id == mutation.expected_manifest_location.segment_id &&
										 manifest.page_index == mutation.expected_manifest_location.page_index) |
					static_cast<uint8_t>(
							!destinationFrames.emplace(manifest.segment_id.value(), manifest.page_index).second);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "native graph Vector delta preparer returned an invalid Manifest frame");
			}
			const CatalogEntry &manifestPool =
					mutation.manifest_in_new_pool ? actual.manifest_new_pool : mutation.expected_manifest_pool;
			registerVectorPublication(manifestPool, manifest, mutation.index_id, PageOwnerKind::POK_VECTOR_MANIFEST);
			for (size_t vectorOrdinal = 0U; vectorOrdinal < actual.vector_locations.size(); ++vectorOrdinal) {
				const VectorLocation &location = actual.vector_locations[vectorOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.vector_id != mutation.first_vector_id + vectorOrdinal) |
						static_cast<uint8_t>(location.slot_generation == 0U) |
						static_cast<uint8_t>(!rawPageIds.contains(location.page_id.value())) |
						static_cast<uint8_t>(
								!publishedVectorDeltaSlots.emplace(location.page_id.value(), location.slot_id).second);
				if (invalidLocation != 0U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "native graph Vector delta preparer returned an invalid Vector Location");
				}
				publishedVectorDeltaLocations.push_back(location);
			}
			publishedVectorDeltaPages.push_back(manifest);
			publishedVectorDeltaManifests.push_back(actual.manifest_location);
			erasedVectorDeltaIds.insert(erasedVectorDeltaIds.end(), mutation.erased_vector_ids.begin(),
										mutation.erased_vector_ids.end());
		}

		// Run bounded physical authentication only after every coordinator-owned
		// logical identity and cross-type shape check has completed.
		for (const auto &[segmentId, publication]: derivedIndexPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}
		for (const auto &[segmentId, publication]: vectorPublicationFrames) {
			static_cast<void>(segmentId);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(*io_, current.data_region_offset,
																			publication.first, publication.second);
		}

		std::vector<PageLocation> publishedPages;
		size_t propertyPageCount = 0U;
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: propertyPageBatchMaterializations) {
			propertyPageCount += materialization.pages.size();
		}
		publishedPages.reserve(materializations.size() + appendMaterializations.size() + newPoolMaterializations.size() +
							   propertyMaterializations.size() + propertyPageCount +
							   publishedLargeValuePages.size() + publishedDerivedIndexPages.size() +
							   publishedVectorBootstrapPages.size() + publishedVectorDeltaPages.size());
		for (const EntityPageReplacementMaterialization &materialization: materializations) {
			if (!materialization.retire_page) {
				publishedPages.push_back(materialization.replacement);
			}
		}
		for (const EntityPageAppendBatchMaterialization &materialization: appendMaterializations) {
			publishedPages.push_back(materialization.appended_location);
		}
		for (const EntityPageNewPoolBatchMaterialization &materialization: newPoolMaterializations) {
			publishedPages.push_back(materialization.initial_location);
		}
		for (const NativeGraphPropertyPageReplacementMaterialization &materialization: propertyMaterializations) {
			if (!materialization.retire_page) {
				publishedPages.push_back(materialization.replacement);
			}
		}
		for (const NativeGraphPropertyPageBatchMaterialization &materialization: propertyPageBatchMaterializations) {
			for (const NativeGraphPropertyPageInitialMaterialization &page: materialization.pages) {
				publishedPages.push_back(page.location);
			}
		}
		publishedPages.insert(publishedPages.end(), publishedLargeValuePages.begin(), publishedLargeValuePages.end());
		publishedPages.insert(publishedPages.end(), publishedDerivedIndexPages.begin(),
							  publishedDerivedIndexPages.end());
		publishedPages.insert(publishedPages.end(), publishedVectorBootstrapPages.begin(),
							  publishedVectorBootstrapPages.end());
		publishedPages.insert(publishedPages.end(), publishedVectorDeltaPages.begin(), publishedVectorDeltaPages.end());
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		std::ranges::sort(retiredPageIds);
		const bool duplicatePublishedPage =
				std::ranges::adjacent_find(publishedPages, {}, &PageLocation::page_id) != publishedPages.end();
		const bool duplicateRetiredPage = std::ranges::adjacent_find(retiredPageIds) != retiredPageIds.end();
		const bool publishedAndRetired =
				std::ranges::any_of(publishedPages, [&retiredPageIds](const PageLocation &page) {
					return std::ranges::binary_search(retiredPageIds, page.page_id.value());
				});
		if (duplicatePublishedPage || duplicateRetiredPage || publishedAndRetired) { // publication page sets are canonicalized before this coordinator check.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph publication has duplicate or contradictory Page Directory mutations");
		}
		// A transaction may publish only a durable logical-ID reservation after a
		// provisional graph result is cancelled. That advances the Superblock
		// high-water marks, but it must not manufacture an empty Page Directory
		// mutation merely to satisfy the generic graph publication path.
		if (!publishedPages.empty() || !retiredPageIds.empty()) {
			if (retiredPageIds.empty()) {
				locations_.prepareUpsertPages(batch, publishedPages);
			} else {
				locations_.prepareReplacePages(batch, publishedPages, retiredPageIds);
			}
		}
		if (!insertedNodes.empty() || !erasedNodes.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, insertedNodes, erasedNodes);
		}
		if (!insertedEdges.empty() || !erasedEdges.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, insertedEdges, erasedEdges);
		}
        // A record kind has one published directory root per database generation.
        // Verify that the marker's semantic generation names the actual result,
        // rather than guessing a future physical root address in the planner.
        for (const auto &fence : derivedState.entity_free_summary_fences) {
            const auto root = fence.index_id == RSS_NODE_FREE_SPACE_INDEX_ID ?
                batch.candidate().node_entity_directory_root : batch.candidate().edge_entity_directory_root;
            if ((root.isValid() && root.generation != fence.target_directory_generation) || (!root.isValid() && fence.free_pages)) {
                bool retainedStale = false;
                if (RSS_SCAN_PATH_FORMAT_ENABLED && root.isValid()) {
                    auto source = snapshots_.pinCurrentUnderWrite(writer, current);
                    BTreePageStore sourceTrees(*deferredSchemaPool, *this);
                    const auto kind = fence.index_id == RSS_NODE_FREE_SPACE_INDEX_ID ?
                        EntityPageRecordKind::EPRK_NODE : EntityPageRecordKind::EPRK_EDGE;
                    const auto proof = EntityFreeSpaceIndex::readProof(*this, sourceTrees, source, kind);
                    retainedStale = proof && !proof->fresh &&
                        proof->directory_generation == fence.target_directory_generation.value();
                }
                // A stale source marker may stay stale while changed entries
                // are maintained. It never becomes insertion authority merely
                // because the physical directory was copied elsewhere.
                if (!retainedStale)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "free-space marker does not name the resulting entity directory");
            }
        }
		std::vector<EntityPropertyKey> erasedProperties;
		erasedProperties.reserve(explicitlyErasedEntityProperties.size() + erasedEntityProperties.size());
		erasedProperties.insert(erasedProperties.end(), explicitlyErasedEntityProperties.begin(),
								explicitlyErasedEntityProperties.end());
		erasedProperties.insert(erasedProperties.end(), erasedEntityProperties.begin(), erasedEntityProperties.end());
		std::ranges::sort(upsertedEntityProperties, {},
						  [](const EntityPropertyLocation &location) { return location.key.directoryId(); });
		std::ranges::sort(erasedProperties, {}, &EntityPropertyKey::directoryId);
		// Validate duplicate writes before filtering unchanged routing identities.
		// The replacement page and source fences have already been authenticated;
		// values live behind PageDirectory, not in EntityPropertyDirectory.
		if (std::ranges::adjacent_find(upsertedEntityProperties, {},
				[](const EntityPropertyLocation &location) { return location.key.directoryId(); }) !=
				upsertedEntityProperties.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE, "duplicate Entity Property publication");
		}
		std::erase_if(upsertedEntityProperties, [&](const EntityPropertyLocation &location) {
			const auto existing = findExistingProperty(location.key);
			if (!existing || *existing != location) return false;
			debug::PerfTrace::addValue("native_graph.commit.unchanged_property_locations", 1);
			return true;
		});
		if (!upsertedEntityProperties.empty() || !erasedProperties.empty()) {
			locations_.prepareReplaceEntityProperties(batch, upsertedEntityProperties, erasedProperties);
		}
		// Several independent native derived-state plans can publish in one graph
		// generation. The Location Directory is a canonical ordered map, so make
		// that aggregate deterministic here rather than relying on planner order.
		std::ranges::sort(publishedDerivedIndexRoots, {}, &IndexRootLocation::index_id);
		std::ranges::sort(erasedDerivedIndexRootIds);
		const bool duplicatePublishedDerivedIndexRoot =
				std::ranges::adjacent_find(publishedDerivedIndexRoots, {}, &IndexRootLocation::index_id) !=
				publishedDerivedIndexRoots.end();
		const bool duplicateErasedDerivedIndexRoot =
				std::ranges::adjacent_find(erasedDerivedIndexRootIds) != erasedDerivedIndexRootIds.end();
		const bool publishedAndErasedDerivedIndexRoot = std::ranges::any_of(
				publishedDerivedIndexRoots, [&erasedDerivedIndexRootIds](const IndexRootLocation &root) {
					return std::ranges::binary_search(erasedDerivedIndexRootIds, root.index_id);
				});
		if (duplicatePublishedDerivedIndexRoot || duplicateErasedDerivedIndexRoot ||
			publishedAndErasedDerivedIndexRoot) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"native graph publication has duplicate or contradictory Index Root Directory mutations");
		}
		if (!publishedDerivedIndexRoots.empty() || !erasedDerivedIndexRootIds.empty()) {
			locations_.prepareReplaceIndexRoots(batch, publishedDerivedIndexRoots, erasedDerivedIndexRootIds);
		}
		std::vector<VectorLocation> publishedVectorLocations;
		publishedVectorLocations.reserve(publishedVectorBootstrapLocations.size() +
										 publishedVectorDeltaLocations.size());
		publishedVectorLocations.insert(publishedVectorLocations.end(), publishedVectorBootstrapLocations.begin(),
										publishedVectorBootstrapLocations.end());
		publishedVectorLocations.insert(publishedVectorLocations.end(), publishedVectorDeltaLocations.begin(),
										publishedVectorDeltaLocations.end());
		std::ranges::sort(publishedVectorLocations, {}, &VectorLocation::vector_id);
		std::ranges::sort(erasedVectorDeltaIds);
		if (std::ranges::adjacent_find(publishedVectorLocations, {}, &VectorLocation::vector_id) !=
					publishedVectorLocations.end() ||
			std::ranges::adjacent_find(erasedVectorDeltaIds) != erasedVectorDeltaIds.end() ||
			std::ranges::any_of(publishedVectorLocations, [&erasedVectorDeltaIds](const VectorLocation &location) {
				return std::ranges::binary_search(erasedVectorDeltaIds, location.vector_id);
			})) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"native graph Vector publication has duplicate or contradictory Vector Location mutations");
		}
		if (!publishedVectorLocations.empty() || !erasedVectorDeltaIds.empty()) {
			locations_.prepareReplaceVectorLocations(batch, publishedVectorLocations, erasedVectorDeltaIds);
		}
		std::vector<VectorManifestLocation> publishedVectorManifests;
		publishedVectorManifests.reserve(publishedVectorBootstrapManifests.size() +
										 publishedVectorDeltaManifests.size());
		publishedVectorManifests.insert(publishedVectorManifests.end(), publishedVectorBootstrapManifests.begin(),
										publishedVectorBootstrapManifests.end());
		publishedVectorManifests.insert(publishedVectorManifests.end(), publishedVectorDeltaManifests.begin(),
										publishedVectorDeltaManifests.end());
		std::ranges::sort(publishedVectorManifests, {}, &VectorManifestLocation::index_id);
		if (std::ranges::adjacent_find(publishedVectorManifests, {}, &VectorManifestLocation::index_id) !=
			publishedVectorManifests.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "native graph Vector publication has duplicate Manifest mutations");
		}
		if (!publishedVectorManifests.empty()) {
			locations_.prepareReplaceVectorManifests(batch, publishedVectorManifests, {});
		}
		if (!publishedLargeValueManifests.empty() || !erasedLargeValueIds.empty()) {
			locations_.prepareReplaceLargeValueManifests(batch, publishedLargeValueManifests, erasedLargeValueIds);
		}
        if (preservingNativeFields && !nativeFieldsPrepared) {
            pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
            batch.hybrid_property_publication_ = true;
        }
        if (scanMembership) {
            scanMembership->borrowed_placement_ = hybridPlacement.get();
            materializeScanMembershipMutation(batch, writer, *topologySource, *scanMembership, *conversionIndexes);
        }
        if (hybridPropertyRootMutation) {
            const auto &request = *hybridPropertyRootMutation;
            if (request.mode == NativeHybridPropertyRootMode::NHPRM_CLEAR) {
                batch.setHybridPropertyDescriptorRoot({});
            } else {
                HybridPropertyDescriptor descriptor{current.database_id, batch.generation(), {}};
                static_assert(RSS_HYBRID_PROPERTY_DESCRIPTOR_ROOT_COUNT ==
                    std::tuple_size<decltype(request.replacement_index_ids)>::value);
                for (size_t i = 0; i < descriptor.roots.size(); ++i) {
                    const auto id = request.replacement_index_ids[i];
                    if (!id) continue;
                    if (std::ranges::binary_search(erasedDerivedIndexRootIds, id))
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid descriptor names a retired tree");
                    const auto found = std::ranges::find(publishedDerivedIndexRoots, id, &IndexRootLocation::index_id);
                    const auto root = found != publishedDerivedIndexRoots.end() ? std::optional(*found) :
                        locations_.findIndexRootOptional(current, id);
                    if (!root)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "native hybrid descriptor names an absent tree");
                    descriptor.roots[i] = {root->root_page_id, root->root_generation};
                }
                // Preparation remains fail-closed for nonempty field routing.
                // This late position uses final roots and actual batch identity,
                // while preserving the existing common durability fences.
                pages_.prepareHybridPropertyDescriptor(batch, descriptor);
            }
        }
		{
			debug::ScopedPerfTimer publishTimer("native_graph.mutation.batch_publish");
            if (hybridVectors && hybridVectors->fields_.cancellation) hybridVectors->fields_.cancellation->check();
			publishBatch(batch, writer, options);
		}
        if (hybridPlacement) hybridPlacement->remember();
        if (scanMembership && scanMembership->placement_) scanMembership->placement_->remember();
        if (ordinaryIndexPlacement) ordinaryIndexPlacement->remember();
        if (streamedAdjacencyPlacement) streamedAdjacencyPlacement->remember();
        if (streamedMembershipPlacement) streamedMembershipPlacement->remember();
        for (const auto &plan : deferredPlans) if (plan.images) {
            auto root = plan.images->installed_root_;
            if (!root) {
                const auto found = std::ranges::find(publishedDerivedIndexRoots, plan.images->index_id_,
                    &IndexRootLocation::index_id);
                if (found != publishedDerivedIndexRoots.end()) root = *found;
            }
            if (root) {
                const auto dataRoot = batch.candidate().usesPageDirectoryBootstrap() ?
                    batch.pendingBootstrapDataRoot().value_or(plan.images->source_data_root_) : batch.candidate().page_directory_root;
                std::lock_guard lock(streamed_adjacency_shape_mutex_);
                auto &hint = plan.images->index_id_ == RSS_GLOBAL_ADJACENCY_INDEX_ID ? streamed_adjacency_shape_ : streamed_membership_shape_;
                hint = {batch.generation(), plan.images->entry_count_, plan.images->page_count_, dataRoot, root};
            }
        }
		return batch.candidate();
	}
