		struct BufferedProcedureRow {
            detail::ProcedurePayloadCredit allocation_credit;
			LogicalPlanReadRow row;
			NodePropertyMaps node_properties;
            static size_t emptyHeapUpper() {
                using detail::procedureEmptyMapHeapUpper;
                return procedureEmptyMapHeapUpper<decltype(row.values)>() +
                    procedureEmptyMapHeapUpper<decltype(row.property_cache)>() +
                    procedureEmptyMapHeapUpper<decltype(row.entity_metadata_cache)>() +
                    procedureEmptyMapHeapUpper<decltype(row.complete_entity_property_cache)>() +
                    procedureEmptyMapHeapUpper<decltype(row.path_cache)>() +
                    procedureEmptyMapHeapUpper<NodePropertyMaps>();
            }
            BufferedProcedureRow() : allocation_credit(emptyHeapUpper()) {}
            BufferedProcedureRow(const BufferedProcedureRow &source)
                : allocation_credit(source.allocation_credit.resource(), utils::detail::addStandardHeapBytes(
                    detail::procedurePayloadHeapUpper<true>(source.row),
                    detail::procedurePayloadHeapUpper<true>(source.node_properties))),
                  row(source.row), node_properties(source.node_properties) {}
            // All private row indexes now have embedded tree headers. Use
            // their actual move guarantees instead of forcing MSVC vectors
            // to deep-copy every buffered payload during reallocation.
            static constexpr bool MOVE_WITHOUT_HEAP = std::is_nothrow_move_constructible_v<LogicalPlanReadRow> &&
                std::is_nothrow_move_constructible_v<NodePropertyMaps>;
            BufferedProcedureRow(BufferedProcedureRow &&source) noexcept(MOVE_WITHOUT_HEAP)
                : allocation_credit(source.allocation_credit.resource(), emptyHeapUpper()),
                  row(std::move(source.row)), node_properties(std::move(source.node_properties)) {
                allocation_credit.swap(source.allocation_credit);
            }
            void reconcilePayloadCredit() {
                allocation_credit.set(utils::detail::addStandardHeapBytes(
                    detail::procedurePayloadHeapUpper<false>(row),
                    detail::procedurePayloadHeapUpper<false>(node_properties)));
            }
            void swap(BufferedProcedureRow &other) noexcept {
                row.values.swap(other.row.values); row.property_cache.swap(other.row.property_cache);
                row.entity_metadata_cache.swap(other.row.entity_metadata_cache);
                row.complete_entity_property_cache.swap(other.row.complete_entity_property_cache);
                row.path_cache.swap(other.row.path_cache); node_properties.swap(other.node_properties);
                allocation_credit.swap(other.allocation_credit);
            }
            friend void swap(BufferedProcedureRow &a, BufferedProcedureRow &b) noexcept { a.swap(b); }
            BufferedProcedureRow &operator=(const BufferedProcedureRow &source) {
                if (this != &source) { BufferedProcedureRow copy(source); swap(copy); } return *this;
            }
            BufferedProcedureRow &operator=(BufferedProcedureRow &&source) noexcept(MOVE_WITHOUT_HEAP) {
                if (this != &source) { BufferedProcedureRow moved(std::move(source)); swap(moved); } return *this;
            }
		};
        // Declared before raw output maps: refund unused row grants only after
        // their moved-from MSVC bucket/proxy storage has actually been freed.
        struct ProcedurePayloadReconcileGuard final {
            BufferedProcedureRow &row;
            ~ProcedurePayloadReconcileGuard() noexcept {
                try {
                    const size_t retained = utils::detail::addStandardHeapBytes(
                        detail::procedurePayloadHeapUpper<false>(row.row),
                        detail::procedurePayloadHeapUpper<false>(row.node_properties));
                    if (retained < row.allocation_credit.bytes()) row.allocation_credit.set(retained);
                } catch (...) { /* Keeping a conservative grant is safe during unwinding. */ }
            }
        };
        // Only the vector backing is admitted here. Nested row/cache payloads
        // retain their separate materialization admission and lifetime rules.
        using ProcedureRows = utils::OwnedVector<BufferedProcedureRow>;


		template<typename Container>
		void releaseProcedureContainer(Container &container) {
			Container empty;
			container.swap(empty);
		}

		constexpr size_t MAXIMUM_PROCEDURE_NODE_BATCH_ROWS = 256U;

		[[nodiscard]] size_t procedureNodeBatchCapacity(const LogicalPlanReadRuntimeBounds &bounds) {
			if (bounds.maximum_rows == 0U || bounds.maximum_materialized_bytes == 0U) {
				throw std::invalid_argument("logical procedure runtime requires positive Node batch bounds");
			}
			// Reserve only a conservative minimum per complete Node.  The actual
			// payload is checked after the read, but this prevents a small byte budget
			// from staging the default 256 full records before that check can run.
			constexpr size_t minimumNodeBytes = sizeof(BufferedProcedureRow) + sizeof(LogicalPlanReadRow) +
					 sizeof(storage::LogicalGraphNode) + sizeof(PropertyValue);
			const size_t byteCapacity = std::max<size_t>(
				1U, bounds.maximum_materialized_bytes / std::max<size_t>(1U, minimumNodeBytes));
			return std::min({MAXIMUM_PROCEDURE_NODE_BATCH_ROWS, bounds.maximum_rows, byteCapacity});
		}

		[[nodiscard]] size_t reserveBoundedProcedureRows(ProcedureRows &rows,
											  const size_t requestedRows,
											  const LogicalPlanReadRuntimeBounds &bounds,
											  const size_t retainedSourceBytes = 0U) {
			if (retainedSourceBytes > bounds.maximum_materialized_bytes) {
				throw std::runtime_error("logical procedure source and output exceeded their byte bound");
			}
		const size_t availableBytes = bounds.maximum_materialized_bytes - retainedSourceBytes;
		const size_t byteCapacity = availableBytes / sizeof(BufferedProcedureRow);
			rows.reserve(std::min({requestedRows, bounds.maximum_rows, byteCapacity}));
			return availableBytes;
		}

		[[nodiscard]] std::optional<size_t> vectorProcedurePrefixThroughLimit(
				const std::vector<const logical::LogicalOperator *> &operations, const size_t totalRows) {
			if (operations.empty() || !std::ranges::all_of(operations, [](const logical::LogicalOperator *operation) {
					const auto type = operation->getType();
					return type == logical::LogicalOpType::LOP_SKIP || type == logical::LogicalOpType::LOP_LIMIT ||
							(type == logical::LogicalOpType::LOP_PROJECT &&
									!dynamic_cast<const logical::LogicalProject &>(*operation).isDistinct());
				})) {
				return std::nullopt;
			}

			size_t remaining = totalRows;
			size_t requiredPrefix = 0U;
			bool reachedLimit = false;
			for (const logical::LogicalOperator *operation: operations) {
				if (operation->getType() == logical::LogicalOpType::LOP_SKIP) {
					const int64_t offset = dynamic_cast<const logical::LogicalSkip &>(*operation).getOffset();
					if (offset < 0) {
						throw std::invalid_argument("logical Vector procedure SKIP cannot be negative");
					}
					const size_t skipped = std::min(remaining, static_cast<size_t>(offset));
					if (!reachedLimit) {
						requiredPrefix += skipped;
					}
					remaining -= skipped;
					continue;
				}
				if (operation->getType() == logical::LogicalOpType::LOP_LIMIT) {
					const int64_t limit = dynamic_cast<const logical::LogicalLimit &>(*operation).getLimit();
					if (limit < 0) {
						throw std::invalid_argument("logical Vector procedure LIMIT cannot be negative");
					}
					remaining = std::min(remaining, static_cast<size_t>(limit));
					if (!reachedLimit) {
						requiredPrefix += remaining;
						reachedLimit = true;
					}
				}
			}
			return reachedLimit ? std::optional<size_t>(requiredPrefix) : std::nullopt;
		}

		[[nodiscard]] size_t checkedProcedureSizeAdd(const size_t left, const size_t right) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				throw std::overflow_error("logical procedure materialization size overflowed");
			}
			return left + right;
		}

		[[nodiscard]] size_t checkedProcedureSizeMul(const size_t left, const size_t right) {
			if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
				throw std::overflow_error("logical procedure materialization size overflowed");
			}
			return left * right;
		}

		[[nodiscard]] size_t procedureValueBytes(const LogicalPlanReadValue &value) {
			size_t bytes = sizeof(value);
			if (const auto *scalar = std::get_if<PropertyValue>(&value)) {
				return checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(*scalar));
			}
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(node->labels.size(), sizeof(std::string)));
				for (const std::string &label: node->labels) {
					bytes = checkedProcedureSizeAdd(bytes, label.size());
				}
				bytes = checkedProcedureSizeAdd(
						bytes, checkedProcedureSizeMul(node->properties.size(), sizeof(storage::LogicalGraphPropertyEntry)));
				for (const storage::LogicalGraphPropertyEntry &entry: node->properties) {
					bytes = checkedProcedureSizeAdd(bytes, entry.key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value));
				}
				return bytes;
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				bytes = checkedProcedureSizeAdd(bytes, edge->type.size());
				bytes = checkedProcedureSizeAdd(
						bytes, checkedProcedureSizeMul(edge->properties.size(), sizeof(storage::LogicalGraphPropertyEntry)));
				for (const storage::LogicalGraphPropertyEntry &entry: edge->properties) {
					bytes = checkedProcedureSizeAdd(bytes, entry.key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value));
				}
				return bytes;
			}
			return checkedProcedureSizeAdd(bytes, sizeof(storage::EntityRef));
		}

		// Account for a complete Node result without constructing a temporary
		// LogicalPlanReadRow. Procedure hit materialization keeps the Node in the
		// sorted-read buffer until it is moved into its final score-order slot;
		// copying it just to measure the byte bound would recreate the payload
		// duplication this path is designed to avoid.
		[[nodiscard]] size_t bufferedProcedureNodeRowBytes(const storage::LogicalGraphNode &node,
																	 const bool includesScore) {
			size_t bytes = sizeof(BufferedProcedureRow);
			bytes = checkedProcedureSizeAdd(bytes, std::string_view("node").size());
			bytes = checkedProcedureSizeAdd(bytes, sizeof(LogicalPlanReadValue));
			bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(node.labels.size(), sizeof(std::string)));
			for (const std::string &label: node.labels) {
				bytes = checkedProcedureSizeAdd(bytes, label.size());
			}
			bytes = checkedProcedureSizeAdd(
					bytes, checkedProcedureSizeMul(node.properties.size(), sizeof(storage::LogicalGraphPropertyEntry)));
			for (const storage::LogicalGraphPropertyEntry &entry: node.properties) {
				bytes = checkedProcedureSizeAdd(bytes, entry.key.size());
				bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(entry.value));
			}
			if (includesScore) {
				bytes = checkedProcedureSizeAdd(bytes, std::string_view("score").size());
				bytes = checkedProcedureSizeAdd(bytes, sizeof(LogicalPlanReadValue));
				bytes = checkedProcedureSizeAdd(bytes, sizeof(double));
			}
			return bytes;
		}

		[[nodiscard]] size_t bufferedProcedureRowBytes(const BufferedProcedureRow &buffered) {
			size_t bytes = sizeof(buffered);
			for (const auto &[name, value]: buffered.row.values) {
				bytes = checkedProcedureSizeAdd(bytes, name.size());
				bytes = checkedProcedureSizeAdd(bytes, procedureValueBytes(value));
			}
			for (const auto &[alias, properties]: buffered.row.property_cache) {
				bytes = checkedProcedureSizeAdd(bytes, alias.size());
				for (const auto &[key, value]: properties) {
					bytes = checkedProcedureSizeAdd(bytes, key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
				}
			}
			for (const auto &[alias, metadata]: buffered.row.entity_metadata_cache) {
				bytes = checkedProcedureSizeAdd(bytes, alias.size());
				for (const auto &[key, value]: metadata) {
					bytes = checkedProcedureSizeAdd(bytes, key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
				}
			}
			for (const auto &[alias, properties]: buffered.node_properties) {
				bytes = checkedProcedureSizeAdd(bytes, sizeof(alias));
				for (const auto &[key, value]: properties.values) {
					bytes = checkedProcedureSizeAdd(bytes, key.size());
					bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
				}
			}
			return bytes;
		}

        using detail::ProcedureByteBoundExceeded;

		void admitProcedureBytes(MaterializedByteReservation &reservation, size_t bytes) {
			if (reservation.wouldExceed(bytes)) throw ProcedureByteBoundExceeded("logical procedure shared materialization exceeded its byte bound");
			reservation.set(bytes);
		}

		void validateBufferedProcedureBytes(const ProcedureRows &rows, const size_t maximumBytes) {
			if (maximumBytes == 0U) {
				throw std::invalid_argument("logical procedure runtime requires a positive materialization byte bound");
			}
			size_t total = 0U;
			for (const BufferedProcedureRow &row: rows) {
				const size_t rowBytes = bufferedProcedureRowBytes(row);
				if (rowBytes > maximumBytes - std::min(total, maximumBytes)) {
					throw ProcedureByteBoundExceeded("logical procedure materialization exceeded its byte bound");
				}
				total = checkedProcedureSizeAdd(total, rowBytes);
			}
		}

		[[nodiscard]] size_t bufferedProcedureRowsBytes(const ProcedureRows &rows,
				size_t firstIndex = 0U) {
			size_t bytes = 0U;
			for (size_t index = firstIndex; index < rows.size(); ++index) {
				bytes = checkedProcedureSizeAdd(bytes, bufferedProcedureRowBytes(rows[index]));
			}
			return bytes;
		}

        // ResultValue is embedded in each hash node. Charge the structural
        // node once and only its separately owned heap through the shared model.
        [[nodiscard]] size_t procedureQueryResultBytes(const QueryResult &result) {
            size_t bytes = detail::standardVectorHeapBufferBytes(result.getRows());
            bytes = checkedProcedureSizeAdd(bytes, graph::query::detail::resultStringsHeapBytes(result.getColumns()));
            bytes = checkedProcedureSizeAdd(bytes, graph::query::detail::resultStringsHeapBytes(result.getNotifications()));
            for (const auto &row : result.getRows())
                bytes = checkedProcedureSizeAdd(bytes, graph::query::detail::resultRowHeapBytes(row));
            return bytes;
        }

		void validateProcedureResultBudget(const QueryResult &result, const size_t retainedSourceBytes,
				const size_t maximumBytes) {
			const size_t resultBytes = procedureQueryResultBytes(result);
			if (retainedSourceBytes > maximumBytes || resultBytes > maximumBytes - retainedSourceBytes) {
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}
		}

        [[nodiscard]] size_t procedureQueryResultRowBytes(const QueryResult::Row &row) {
            return graph::query::detail::resultRowHeapBytes(row);
        }
        // Consumed row slots and moved-from STL buckets remain alive until the
        // complete vector is released. Never refund their capacity by advancing
        // firstIndex. The logical floor still covers non-heap materialized data.
        [[nodiscard]] size_t publicationSourceBytes(const ProcedureRows &rows, size_t firstIndex) {
            // Owning vectors have proxy-free storage, including MSVC debug.
            // Their allocator observes exactly capacity*sizeof(row).
            size_t retained = checkedProcedureSizeMul(rows.capacity(), sizeof(BufferedProcedureRow));
            for (const auto &row : rows) {
                retained = checkedProcedureSizeAdd(retained, detail::procedurePayloadHeapUpper<false>(row.row));
                retained = checkedProcedureSizeAdd(retained, detail::procedurePayloadHeapUpper<false>(row.node_properties));
            }
            return std::max(retained, bufferedProcedureRowsBytes(rows, firstIndex));
        }

        void reportProcedurePublicationPressure(const QueryResult &result, const ProcedureRows &rows,
                const char *phase, size_t externalSourceBytes, size_t rowSourceBytes,
                size_t resultAndOutputBytes, size_t requestedBackingBytes, size_t maximumBytes) noexcept {
#ifdef ZYX_ENABLE_STORAGE_TEST_HOOKS
            debug::CiBudgetTrace trace(&result, "procedure_publication");
            if (trace.allow()) {
                const auto owner = utils::OwnedAllocationScope::owner();
                const auto *budget = dynamic_cast<const MaterializedByteBudget *>(owner.get());
                size_t payloadCredits = 0U, ownerHeadroom = 0U;
                bool saturated = false, headroomValid = false, relocationValid = false;
                size_t relocationBytes = 0U;
                try {
                    relocationBytes = graph::query::detail::resultRowsRelocationHeapUpper(result.getRows());
                    relocationValid = true;
                } catch (...) { /* Keep the original error even if a diagnostic bound overflows. */ }
                for (const auto &row : rows) {
                    const size_t amount = row.allocation_credit.bytes();
                    if (amount > (std::numeric_limits<size_t>::max)() - payloadCredits) {
                        saturated = true; payloadCredits = (std::numeric_limits<size_t>::max)();
                    } else payloadCredits += amount;
                }
                try {
                    if (budget) { ownerHeadroom = budget->availableWithParent(); headroomValid = true; }
                } catch (...) { /* Diagnostics must preserve the original failure. */ }
                std::fprintf(stderr,
                    "CI_PROCEDURE_PRESSURE owner=%zu phase=%s source_rows=%zu source_capacity=%zu source_row_width=%zu source_backing=%zu external_source=%zu source_retained=%zu result_rows=%zu result_capacity=%zu result_row_width=%zu result_and_output=%zu requested_backing=%zu maximum=%zu row_payload_credits=%zu credits_saturated=%d result_resource_credit=%zu allocation_owner=%zu owner_used=%zu owner_maximum=%zu parent_headroom=%zu headroom_valid=%d result_relocation_upper=%zu relocation_valid=%d\n",
                    trace.id(), phase, rows.size(), rows.capacity(), sizeof(BufferedProcedureRow),
                    rows.capacity() * sizeof(BufferedProcedureRow), externalSourceBytes, rowSourceBytes,
                    result.getRows().size(), result.getRows().capacity(), sizeof(QueryResult::Row),
                    resultAndOutputBytes, requestedBackingBytes, maximumBytes, payloadCredits, int(saturated),
                    result.retainedResourceBytes(), budget ? debug::CiBudgetTrace(budget, "materialized").id() : 0U,
                    budget ? budget->used() : 0U, budget ? budget->used() + budget->available() : 0U,
                    ownerHeadroom, int(headroomValid), relocationBytes, int(relocationValid));
                std::fflush(stderr);
            }
            // One-shot pressure snapshots retain the process-wide output cap,
            // without leaving result addresses in the diagnostic registry.
            trace.discard();
#endif
        }

        // The batch cardinality is already known. Reserve its final backing
        // before publishing the first row, including for entity outputs whose
        // MSVC map moves can throw. This avoids copying prior output payloads
        // during vector growth. Only storage actually allocated here is admitted:
        // every later source/output overlap remains checked by append below.
        void reserveInitialProcedureResult(QueryResult &result, const ProcedureRows &rows,
                size_t maximumBytes, size_t externalSourceBytes = 0U) {
            if (rows.empty() || !result.isEmpty() || result.getRows().capacity() != 0U) return;
            const size_t source = checkedProcedureSizeAdd(externalSourceBytes,
                publicationSourceBytes(rows, 0U));
            const size_t current = checkedProcedureSizeAdd(source, procedureQueryResultBytes(result));
            const size_t backing = utils::detail::standardAllocatorRequestBytes(
                checkedProcedureSizeMul(rows.size(), sizeof(QueryResult::Row)), alignof(QueryResult::Row));
            if (current > maximumBytes || backing > maximumBytes - current) return;
            try { result.reserveRows(rows.size()); }
            catch (const MaterializedByteLimitExceeded &) { return; }
            catch (const detail::ResourceAdmissionDenied &) { return; }
            validateProcedureResultBudget(result, source, maximumBytes);
        }

        void compactConsumedProcedureRows(const QueryResult &result, ProcedureRows &rows,
                size_t &rowIndex, size_t maximumBytes, size_t externalSourceBytes = 0U) {
            if (rowIndex == 0U || rowIndex >= rows.size()) return;
            const size_t remaining = rows.size() - rowIndex;
            if (rowIndex < remaining / 3U + (remaining % 3U != 0U) || rows.capacity() <= remaining ||
                    (rowIndex & (rowIndex - 1U)) != 0U) return;
            // A move must not allocate empty-container payloads under a credit
            // constructor: all payload grants must transfer without new charges.
            if (BufferedProcedureRow::emptyHeapUpper() != 0U) return;
            const size_t retained = checkedProcedureSizeAdd(externalSourceBytes,
                checkedProcedureSizeAdd(publicationSourceBytes(rows, rowIndex),
                    procedureQueryResultBytes(result)));
            if (retained > maximumBytes) return;
            detail::compactConsumedPrefix(rows, rowIndex, maximumBytes - retained);
        }

		void appendProcedureResultRow(QueryResult &result, ProcedureRows &rows,
				size_t rowIndex, QueryResult::Row &&output, size_t maximumBytes,
				const size_t retainedSourceBytes = 0U) {
			size_t pendingRowsBytes = publicationSourceBytes(rows, rowIndex);
			const size_t pendingResultBytes = checkedProcedureSizeAdd(procedureQueryResultBytes(result),
					procedureQueryResultRowBytes(output));
			size_t pendingBytes = checkedProcedureSizeAdd(retainedSourceBytes, pendingRowsBytes);
			pendingBytes = checkedProcedureSizeAdd(pendingBytes, pendingResultBytes);
            // The owning output is complete, but its construction overlapped
            // the current source. Validate that real peak before releasing it.
            if (pendingBytes > maximumBytes) {
                reportProcedurePublicationPressure(result, rows, "source_result_overlap", retainedSourceBytes,
                    pendingRowsBytes, pendingResultBytes, 0U, maximumBytes);
                throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
            }
			BufferedProcedureRow &buffered = rows[rowIndex];
            detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
			releaseProcedureContainer(buffered.row.values);
			releaseProcedureContainer(buffered.row.property_cache);
			releaseProcedureContainer(buffered.row.entity_metadata_cache);
			releaseProcedureContainer(buffered.row.complete_entity_property_cache);
			releaseProcedureContainer(buffered.row.path_cache);
			releaseProcedureContainer(buffered.node_properties);
            // The output owns its payload independently. After destroying the
            // consumed source, retain the output and moved-from-map handoff
            // fees until transfer or unwind. Row slots remain charged too.
            if (buffered.allocation_credit.resource()) {
                const size_t retained = checkedProcedureSizeAdd(
                    detail::procedurePayloadHeapUpper<false>(buffered.row),
                    detail::procedurePayloadHeapUpper<false>(buffered.node_properties));
                const size_t handoff = checkedProcedureSizeMul(2U,
                    detail::procedureEmptyMapHeapUpper<QueryResult::Row>());
                const size_t liveCredit = checkedProcedureSizeAdd(retained,
                    checkedProcedureSizeAdd(procedureQueryResultRowBytes(output), handoff));
                buffered.allocation_credit.set(liveCredit);
            }
            pendingRowsBytes = publicationSourceBytes(rows, rowIndex + 1U);
            pendingBytes = checkedProcedureSizeAdd(retainedSourceBytes,
                checkedProcedureSizeAdd(pendingRowsBytes, pendingResultBytes));
			if (result.getRows().capacity() == result.getRows().size()) {
				const size_t currentCapacity = result.getRows().capacity();
				const size_t minimumCapacity = checkedProcedureSizeAdd(result.getRows().size(), 1U);
				const size_t preferredCapacity = currentCapacity == 0U
						? minimumCapacity
						: std::max(minimumCapacity, checkedProcedureSizeMul(currentCapacity, 2U));
                // A throwing row move makes vector::reserve copy its maps.
                // Both their payloads and both backing arrays coexist. Admit
                // those copies even when this direct call has no query lease.
                const size_t relocationBytes = graph::query::detail::resultRowsRelocationHeapUpper(result.getRows());
                const size_t available = pendingBytes > maximumBytes || relocationBytes > maximumBytes - pendingBytes
                    ? 0U : maximumBytes - pendingBytes - relocationBytes;
                const auto backingRequest = [](size_t count) {
                    return utils::detail::standardAllocatorRequestBytes(
                        checkedProcedureSizeMul(count, sizeof(QueryResult::Row)), alignof(QueryResult::Row));
                };
                const size_t affordableCapacity = std::min(preferredCapacity, available / sizeof(QueryResult::Row));
                if (affordableCapacity < minimumCapacity || backingRequest(minimumCapacity) > available) {
                    reportProcedurePublicationPressure(result, rows, "minimum_result_growth", retainedSourceBytes,
                        pendingRowsBytes, pendingResultBytes, backingRequest(minimumCapacity), maximumBytes);
                    throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
                }
                // MSVC's aligned-allocation prefix changes at 4 KiB. Search
                // the monotone actual request rather than rounding it away.
                size_t requestedCapacity = minimumCapacity, upperCapacity = affordableCapacity;
                while (requestedCapacity < upperCapacity) {
                    const auto middle = requestedCapacity + (upperCapacity - requestedCapacity + 1U) / 2U;
                    if (backingRequest(middle) <= available) requestedCapacity = middle;
                    else upperCapacity = middle - 1U;
                }
                const size_t requestedGrowthBytes = backingRequest(requestedCapacity);
				try { result.reserveRows(requestedCapacity); }
                catch (...) {
                    reportProcedurePublicationPressure(result, rows, "result_growth_exception", retainedSourceBytes,
                        pendingRowsBytes, pendingResultBytes, requestedGrowthBytes, maximumBytes);
                    throw;
                }
				const size_t liveBytesAfterReserve = checkedProcedureSizeAdd(retainedSourceBytes,
						checkedProcedureSizeAdd(pendingRowsBytes,
								checkedProcedureSizeAdd(procedureQueryResultBytes(result), procedureQueryResultRowBytes(output))));
				if (liveBytesAfterReserve > maximumBytes) {
                    reportProcedurePublicationPressure(result, rows, "after_result_growth", retainedSourceBytes,
                        pendingRowsBytes, liveBytesAfterReserve - retainedSourceBytes - pendingRowsBytes,
                        requestedGrowthBytes, maximumBytes);
					throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
				}
			}
			if (pendingBytes > maximumBytes) {
                reportProcedurePublicationPressure(result, rows, "source_result_overlap", retainedSourceBytes,
                    pendingRowsBytes, pendingResultBytes, 0U, maximumBytes);
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}

			detail::ProcedureResultHandoff::append(result, std::move(output), rows[rowIndex].allocation_credit);

			const size_t liveBytes = checkedProcedureSizeAdd(retainedSourceBytes,
					checkedProcedureSizeAdd(procedureQueryResultBytes(result),
							pendingRowsBytes));
			if (liveBytes > maximumBytes) {
                const size_t remaining = pendingRowsBytes;
                reportProcedurePublicationPressure(result, rows, "after_source_release", retainedSourceBytes,
                    remaining, liveBytes - retainedSourceBytes - remaining, 0U, maximumBytes);
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}
		}

