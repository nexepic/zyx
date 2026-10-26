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
#if defined(_MSVC_STL_VERSION)
            static constexpr bool MOVE_WITHOUT_HEAP = false;
#else
            static constexpr bool MOVE_WITHOUT_HEAP = std::is_nothrow_move_constructible_v<LogicalPlanReadRow> &&
                std::is_nothrow_move_constructible_v<NodePropertyMaps>;
#endif
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
        using ProcedureRows = std::vector<BufferedProcedureRow, utils::OwnedPmrAllocator<BufferedProcedureRow>>;


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

		class ProcedureByteBoundExceeded final : public std::runtime_error {
		public:
			explicit ProcedureByteBoundExceeded(const std::string &message) : std::runtime_error(message) {}
		};

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

		[[nodiscard]] size_t procedureResultPropertyMapBytes(
				const std::unordered_map<std::string, PropertyValue> &properties) {
			size_t bytes = checkedProcedureSizeMul(properties.bucket_count(), sizeof(void *));
			for (const auto &[key, value]: properties) {
				bytes = checkedProcedureSizeAdd(bytes, sizeof(std::pair<const std::string, PropertyValue>) + 3U * sizeof(void *));
				bytes = checkedProcedureSizeAdd(bytes, key.capacity());
				bytes = checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(value));
			}
			return bytes;
		}

		[[nodiscard]] size_t procedureResultValueBytes(const ResultValue &value) {
			size_t bytes = sizeof(ResultValue);
			const auto &variant = value.getVariant();
			if (const auto *property = std::get_if<PropertyValue>(&variant)) {
				return checkedProcedureSizeAdd(bytes, property_utils::getPropertyValueSize(*property));
			}
			if (const auto *node = std::get_if<LogicalNodeResult>(&variant)) {
				bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(node->labels.capacity(), sizeof(std::string)));
				for (const std::string &label: node->labels) bytes = checkedProcedureSizeAdd(bytes, label.capacity());
				return checkedProcedureSizeAdd(bytes, procedureResultPropertyMapBytes(node->properties));
			}
			if (const auto *edge = std::get_if<LogicalEdgeResult>(&variant)) {
				bytes = checkedProcedureSizeAdd(bytes, edge->type.capacity());
				return checkedProcedureSizeAdd(bytes, procedureResultPropertyMapBytes(edge->properties));
			}
			if (const auto *node = std::get_if<Node>(&variant)) {
				return checkedProcedureSizeAdd(bytes, node->getTotalPropertySize());
			}
			if (const auto *edge = std::get_if<Edge>(&variant)) {
				return checkedProcedureSizeAdd(bytes, edge->getTotalPropertySize());
			}
			return bytes;
		}

		[[nodiscard]] size_t procedureQueryResultBytes(const QueryResult &result) {
			size_t bytes = checkedProcedureSizeMul(result.getRows().capacity(), sizeof(QueryResult::Row));
			bytes = checkedProcedureSizeAdd(bytes,
					checkedProcedureSizeMul(result.getColumns().capacity(), sizeof(std::string)));
			for (const std::string &column: result.getColumns()) {
				bytes = checkedProcedureSizeAdd(bytes, detail::dynamicStringBufferBytes(column));
			}
			for (const QueryResult::Row &row: result.getRows()) {
				bytes = checkedProcedureSizeAdd(bytes, checkedProcedureSizeMul(row.bucket_count(), sizeof(void *)));
				for (const auto &[key, value]: row) {
					bytes = checkedProcedureSizeAdd(bytes, sizeof(std::pair<const std::string, ResultValue>) + 3U * sizeof(void *));
					bytes = checkedProcedureSizeAdd(bytes, key.capacity());
					bytes = checkedProcedureSizeAdd(bytes, procedureResultValueBytes(value));
				}
			}
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
			size_t bytes = checkedProcedureSizeMul(row.bucket_count(), sizeof(void *));
			for (const auto &[key, value]: row) {
				bytes = checkedProcedureSizeAdd(bytes, sizeof(std::pair<const std::string, ResultValue>) + 3U * sizeof(void *));
				bytes = checkedProcedureSizeAdd(bytes, key.capacity());
				bytes = checkedProcedureSizeAdd(bytes, procedureResultValueBytes(value));
			}
			return bytes;
		}

		void appendProcedureResultRow(QueryResult &result, ProcedureRows &rows,
				size_t rowIndex, QueryResult::Row &&output, size_t maximumBytes,
				const size_t retainedSourceBytes = 0U) {
			const size_t pendingRowsBytes = bufferedProcedureRowsBytes(rows, rowIndex);
			const size_t pendingResultBytes = checkedProcedureSizeAdd(procedureQueryResultBytes(result),
					procedureQueryResultRowBytes(output));
			size_t pendingBytes = checkedProcedureSizeAdd(retainedSourceBytes, pendingRowsBytes);
			pendingBytes = checkedProcedureSizeAdd(pendingBytes, pendingResultBytes);
			if (result.getRows().capacity() == result.getRows().size()) {
				const size_t currentCapacity = result.getRows().capacity();
				const size_t minimumCapacity = checkedProcedureSizeAdd(result.getRows().size(), 1U);
				const size_t preferredCapacity = currentCapacity == 0U
						? minimumCapacity
						: std::max(minimumCapacity, checkedProcedureSizeMul(currentCapacity, 2U));
                // Admit old/new backing overlap without rejecting a smaller
                // growth that fits the same materialization ceiling.
                const size_t affordableCapacity = pendingBytes > maximumBytes ? 0U :
                    (maximumBytes - pendingBytes) / sizeof(QueryResult::Row);
                if (affordableCapacity < minimumCapacity)
                    throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
                const size_t requestedCapacity = std::min(preferredCapacity, affordableCapacity);
				const size_t requestedGrowthBytes = checkedProcedureSizeMul(requestedCapacity, sizeof(QueryResult::Row));
				if (checkedProcedureSizeAdd(pendingBytes, requestedGrowthBytes) > maximumBytes) {
					throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
				}
				result.reserveRows(requestedCapacity);
				const size_t liveBytesAfterReserve = checkedProcedureSizeAdd(retainedSourceBytes,
						checkedProcedureSizeAdd(bufferedProcedureRowsBytes(rows, rowIndex),
								checkedProcedureSizeAdd(procedureQueryResultBytes(result), procedureQueryResultRowBytes(output))));
				if (liveBytesAfterReserve > maximumBytes) {
					throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
				}
			}
			if (pendingBytes > maximumBytes) {
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}

			detail::ProcedureResultHandoff::append(result, std::move(output), rows[rowIndex].allocation_credit);
			BufferedProcedureRow &buffered = rows[rowIndex];
            detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
			releaseProcedureContainer(buffered.row.values);
			releaseProcedureContainer(buffered.row.property_cache);
			releaseProcedureContainer(buffered.row.entity_metadata_cache);
			releaseProcedureContainer(buffered.row.path_cache);
			releaseProcedureContainer(buffered.node_properties);
			const size_t liveBytes = checkedProcedureSizeAdd(retainedSourceBytes,
					checkedProcedureSizeAdd(procedureQueryResultBytes(result),
							bufferedProcedureRowsBytes(rows, rowIndex + 1U)));
			if (liveBytes > maximumBytes) {
				throw ProcedureByteBoundExceeded("logical procedure source and retained result exceeded their byte bound");
			}
		}

