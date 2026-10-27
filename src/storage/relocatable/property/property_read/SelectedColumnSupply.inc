	void PropertyCollectionRootScanCursor::readEntityPropertiesOptionalSorted(
			const MetadataSnapshotPin &snapshot, const std::span<const EntityPropertyKey> keys,
			const std::span<const uint32_t> keyTokens,
			const std::span<std::optional<PropertyCollectionRootEntry>> results,
			const bool columnBatch, const SelectedPropertyReadAdmission *admission,
			const std::function<void(size_t, size_t, std::optional<PropertyCollectionRootEntry>)> *sink, const PropertyScalarReadSink *scalars, const PropertyTextReadSink *texts, const PropertyColumnBatchReadSinks *batches) {
			validateSnapshot(snapshot);
        uint64_t sourceEpoch = data_epoch_;
		if (keyTokens.empty() || keys.size() > std::numeric_limits<size_t>::max() / keyTokens.size() ||
			(sink ? (!*sink || !results.empty()) : results.size() != keys.size() * keyTokens.size())) {
			throwRootStoreError(FormatError::FERR_INVALID_HEADER,
								"Property scan batch requires a rectangular owner/token result shape");
		}
		if (batches && (!batches->scalars || !batches->texts || !sink || !scalars || !texts))
            throw std::invalid_argument("Property runs require both sinks and scalar/owning fallback");
		uint64_t previousDirectoryId = 0U;
		for (const EntityPropertyKey &key : keys) {
			const uint64_t directoryId = key.directoryId();
			if (directoryId <= previousDirectoryId) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property scan batch owners must be strictly ordered");
			}
			previousDirectoryId = directoryId;
		}
		uint32_t previousToken = 0U;
		for (const uint32_t token : keyTokens) {
			if (token == 0U || token <= previousToken) {
				throwRootStoreError(FormatError::FERR_INVALID_HEADER,
									"Property scan batch tokens must be strictly increasing");
			}
			previousToken = token;
		}
		std::fill(results.begin(), results.end(), std::nullopt);
        const auto releaseRow = [&] { current_page_.reset(); current_page_id_ = PageId{}; };
        const std::function<void()> beforeCold = std::ref(releaseRow);
        const auto readHybrid = [&] {
            if constexpr (RSS_HYBRID_PROPERTY_FORMAT_ENABLED) {
                if (snapshot.superblock().hybrid_property_descriptor_root.isValid() &&
                    !(hybrid_context_ && hybrid_context_->active)) {
                    if (!hybrid_context_ || !hybrid_context_->reader.usesSnapshot(snapshot) ||
                        hybrid_context_->epoch != data_epoch_ || data_epoch_ == UINT64_MAX) {
                        hybrid_context_.reset();
                        hybrid_context_ = HybridPropertyScanContext::tryCreate(store_->pages_, store_->metadata_, snapshot, cancellation_);
                    }
                    if (hybrid_context_) {
                        HybridPropertyScanContext::Use use(*hybrid_context_);
                        const auto representation = hybrid_context_->reader.borrowColdRowRepresentation(&beforeCold);
                        return HybridPropertyValueReader::readSelected(hybrid_context_->reader, snapshot,
                            keys, keyTokens, results, {}, store_->large_values_, admission, sink, scalars, texts, batches);
                    }
                }
            }
            return store_->readHybridSelectedBatch(snapshot, keys, keyTokens, results, {},
                cancellation_, admission, sink, scalars, texts, batches, &beforeCold);
        };
        try { if (readHybrid()) return; }
        catch (...) {
            // Private preparation may be retried at a smaller batch. Retained
            // optional cursor state must not consume that retry's headroom.
            if (hybrid_context_ && !hybrid_context_->active) hybrid_context_.reset();
            throw;
        }
        if (hybrid_context_ && !hybrid_context_->active) hybrid_context_.reset();
        std::optional<BorrowedPropertyRuns> runs;
        PropertyScalarReadSink bufferedScalar; PropertyTextReadSink bufferedText;
        const PropertyScalarReadSink *scalarOutput = scalars;
        const PropertyTextReadSink *textOutput = texts;
        if (batches) {
            if (!sink || !scalars || !texts) throw std::invalid_argument("Property runs require scalar/owning fallbacks");
            runs.emplace(*batches);
            bufferedScalar = [&](size_t row, size_t column, PropertyType type, uint64_t bits) { runs->scalar(row, column, type, bits); };
            bufferedText = [&](size_t row, size_t column, std::string_view value) { runs->text(row, column, value); };
            scalarOutput = &bufferedScalar; textOutput = &bufferedText;
        }

		if (sink && admission) {
			constexpr size_t ownerBytes = sizeof(std::optional<EntityPropertyLocation>) + sizeof(size_t);
			if (keys.size() > std::numeric_limits<size_t>::max() / ownerBytes ||
				keyTokens.size() > std::numeric_limits<size_t>::max() / sizeof(std::optional<PropertyCollectionRootEntry>))
				throw std::overflow_error("direct Property cursor staging overflowed");
			(*admission)(keys.size() * ownerBytes);
			(*admission)(keyTokens.size() * sizeof(std::optional<PropertyCollectionRootEntry>));
		}
		std::vector<std::optional<EntityPropertyLocation>> locations(keys.size());
		for (size_t ownerIndex = 0U; ownerIndex < keys.size(); ++ownerIndex) {
			if (cancellation_ != nullptr) {
				cancellation_->check();
			}
			locations[ownerIndex] = findLocationOptional(snapshot, keys[ownerIndex]);
		}
		// Directory results are owner ordered, but COW publication can place those
		// owners on non-monotonic physical pages. Resolve the whole range first,
		// then read each distinct page once and restore owner-major output order.
		std::vector<size_t> present = detail::propertyPageOrder(locations, cancellation_);
		size_t pagesRead = 0U;
		PageId previousPage{};
		std::optional<SlottedPagePinnedRead> pinned;
		SlottedPage *copied = nullptr;
		bool tryPinned = true;
		for (const size_t ownerIndex : present) {
			validateSnapshot(snapshot);
            if (data_epoch_ != sourceEpoch) {
                // A concurrent COW publication does not change this snapshot's
                // values. Drop decoded images and reacquire its authenticated
                // page without replaying already delivered owners.
                if (runs) runs->flush();
                pinned.reset(); copied = nullptr; previousPage = {};
                sourceEpoch = data_epoch_;
            }
			if (cancellation_ != nullptr) cancellation_->check();
			const EntityPropertyLocation &location = *locations[ownerIndex];
			if (previousPage != location.page_id) {
                if (runs) runs->flush(); // Consume text borrows before releasing the old page pin.
				previousPage = location.page_id;
				++pagesRead;
				pinned.reset();
				copied = nullptr;
                if (current_page_.has_value() && current_page_id_ == location.page_id) {
					copied = &*current_page_;
				} else {
                    current_page_.reset(); current_page_id_ = {};
					if (tryPinned) {
                        debug::PerfTrace::addValue("native_graph.read.property_selected_page_acquisition", 1);
						pinned = store_->pages_.readPinned(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL);
                    }
					if (!pinned.has_value()) {
						// Disabled or saturated pools cannot lend an image. Avoid
						// repeating a failed pin route for every remaining page.
						tryPinned = false;
						current_page_.emplace(store_->pages_.read(snapshot, location.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
						current_page_id_ = location.page_id;
						copied = &*current_page_;
					}
				}
			}
			std::vector<std::optional<PropertyCollectionRootEntry>> decoded;
			if (pinned.has_value()) {
				const auto bytes = pinned->page.read({location.page_id, location.slot_id, location.slot_generation});
                if (sink && scalars && store_->emitSelectedEntityProperties(keys[ownerIndex], location,
                        pinned->page.pageId(), pinned->page.ownerKind(), bytes, keyTokens, ownerIndex,
                        *sink, *scalarOutput, textOutput, admission, cancellation_)) {
                    if (cancellation_) cancellation_->check();
                    debug::PerfTrace::addValue("native_graph.read.property_selected_borrowed_owners", 1);
                    continue;
                }
				if (runs) runs->flush();
				decoded = store_->decodeSelectedEntityProperties(snapshot, keys[ownerIndex], location,
						pinned->page.pageId(), pinned->page.ownerKind(), bytes, keyTokens, admission);
				debug::PerfTrace::addValue("native_graph.read.property_selected_borrowed_owners", 1);
			} else {
				const auto bytes = copied->read({location.page_id, location.slot_id, location.slot_generation});
                if (sink && scalars && store_->emitSelectedEntityProperties(keys[ownerIndex], location,
                        copied->pageId(), copied->ownerKind(), bytes, keyTokens, ownerIndex,
                        *sink, *scalarOutput, texts, admission, cancellation_)) {
                    if (cancellation_) cancellation_->check();
                    debug::PerfTrace::addValue("native_graph.read.property_selected_slot_copy_bytes", static_cast<int64_t>(bytes.size()));
                    continue;
                }
				if (runs) runs->flush();
				decoded = store_->decodeSelectedEntityProperties(snapshot, keys[ownerIndex], location,
						copied->pageId(), copied->ownerKind(), bytes, keyTokens, admission);
				debug::PerfTrace::addValue("native_graph.read.property_selected_slot_copy_bytes", static_cast<int64_t>(bytes.size()));
			}
			if (cancellation_ != nullptr) cancellation_->check();
			if (decoded.size() != keyTokens.size()) {
				throwRootStoreError(FormatError::FERR_INVALID_STATE,
									"Property scan batch decoder changed its column cardinality");
			}
			if (sink) {
				for (size_t token = 0U; token < decoded.size(); ++token) {
					if (cancellation_) cancellation_->check();
					(*sink)(ownerIndex, token, std::move(decoded[token]));
				}
			} else {
				std::move(decoded.begin(), decoded.end(), results.begin() +
					static_cast<std::ptrdiff_t>(ownerIndex * keyTokens.size()));
			}
		}
		if (runs) runs->flush();
        // Typed runs have consumed every borrowed cell at flush above. The
        // buffer pool already owns their authenticated frames; copying the last
        // page into a second cursor image provides no remaining borrow lifetime.
        // Keep owned retention only for the non-run cursor path.
        if (!batches && pinned.has_value() && store_->metadata_.dataMutationEpoch() == sourceEpoch &&
                sourceEpoch != std::numeric_limits<uint64_t>::max()) {
            // Results above are already decoded. Retaining an extra image is
            // optional and must not turn a valid bounded read into rejection.
            current_page_.reset(); current_page_id_ = {};
            try {
                auto retained = SlottedPage::open(pinned->contents.owner, pinned->contents.payload(),
                    utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
                        (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
                debug::PerfTrace::addValue("native_graph.read.property_selected_last_page_cache_copy_bytes",
                    static_cast<int64_t>(pinned->contents.payload().size()));
                current_page_.emplace(std::move(retained)); current_page_id_ = previousPage;
            } catch (const query::execution::detail::ResourceAdmissionDenied &) {
                debug::PerfTrace::addValue("native_graph.read.property_selected_last_page_cache_declined", 1);
            } catch (const query::execution::MaterializedByteLimitExceeded &) {
                debug::PerfTrace::addValue("native_graph.read.property_selected_last_page_cache_declined", 1);
            }
		}
		if (columnBatch) {
			debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_pages",
										static_cast<int64_t>(pagesRead));
			debug::PerfTrace::addValue("native_graph.read.property_selected_column_batch_owners",
										static_cast<int64_t>(keys.size()));
		} else {
			debug::PerfTrace::addValue("native_graph.read.property_selected_batch_pages",
										static_cast<int64_t>(pagesRead));
			debug::PerfTrace::addValue("native_graph.read.property_selected_batch_owners",
										static_cast<int64_t>(keys.size()));
		}
	}
