		[[nodiscard]] int hexDigit(char value) {
			if (value >= '0' && value <= '9') {
				return value - '0';
			}
			if (value >= 'a' && value <= 'f') {
				return value - 'a' + 10;
			}
			if (value >= 'A' && value <= 'F') {
				return value - 'A' + 10;
			}
			return -1;
		}

		[[nodiscard]] std::string decodeCsvFileUriPath(std::string_view uri) {
			constexpr std::string_view FILE_URI_PREFIX = "file://";
			if (!uri.starts_with(FILE_URI_PREFIX)) {
				throw std::invalid_argument("logical LOAD CSV accepts only file: URIs");
			}
			const std::string_view encodedPath = uri.substr(FILE_URI_PREFIX.size());
			if (encodedPath.empty() || encodedPath.front() != '/' ||
				encodedPath.find_first_of("?#") != std::string_view::npos) {
				throw std::invalid_argument("logical LOAD CSV has an invalid local-file URI");
			}
			std::string decoded;
			decoded.reserve(encodedPath.size());
			for (size_t index = 0U; index < encodedPath.size(); ++index) {
				const char current = encodedPath[index];
				if (current != '%') {
					if (current == '\0') {
						throw std::invalid_argument("logical LOAD CSV URI contains a NUL byte");
					}
					decoded += current;
					continue;
				}
				if (index + 2U >= encodedPath.size()) {
					throw std::invalid_argument("logical LOAD CSV URI has invalid percent encoding");
				}
				const int high = hexDigit(encodedPath[index + 1U]);
				const int low = hexDigit(encodedPath[index + 2U]);
				if (high < 0 || low < 0) {
					throw std::invalid_argument("logical LOAD CSV URI has invalid percent encoding");
				}
				const char decodedByte = static_cast<char>((high << 4) | low);
				if (decodedByte == '\0') {
					throw std::invalid_argument("logical LOAD CSV URI contains a NUL byte");
				}
				decoded += decodedByte;
				index += 2U;
			}
#ifdef _WIN32
			// A local Windows file URI uses file:///C:/..., never a UNC authority.
			if (decoded.size() >= 3U && decoded[0] == '/' && std::isalpha(static_cast<unsigned char>(decoded[1])) &&
				decoded[2] == ':') {
				decoded.erase(0U, 1U);
			} else if (decoded.starts_with("//")) {
				throw std::invalid_argument("logical LOAD CSV does not permit UNC file paths");
			}
#endif
			return decoded;
		}

		[[nodiscard]] bool isWithinImportRoot(const std::filesystem::path &candidate,
											  const std::filesystem::path &root) {
			auto candidatePart = candidate.begin();
			for (auto rootPart = root.begin(); rootPart != root.end(); ++rootPart, ++candidatePart) {
				if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::filesystem::path resolveCsvImportFile(const PropertyValue &value,
																 const QueryContext &context) {
			if (!context.csv_import_policy.import_root.has_value()) {
				throw std::runtime_error("logical LOAD CSV is disabled because no import root is configured");
			}
			if (context.csv_import_policy.maximum_file_bytes == 0U || context.csv_import_policy.maximum_rows == 0U ||
				context.csv_import_policy.maximum_record_bytes == 0U ||
				context.csv_import_policy.maximum_record_bytes > context.csv_import_policy.maximum_file_bytes ||
				context.csv_import_policy.maximum_columns == 0U ||
				context.csv_import_policy.maximum_field_bytes == 0U ||
				context.csv_import_policy.maximum_field_bytes > context.csv_import_policy.maximum_file_bytes) {
				throw std::invalid_argument("logical LOAD CSV has invalid import resource limits");
			}
			const auto *uri = std::get_if<std::string>(&value.getVariant());
			if (uri == nullptr) {
				throw std::invalid_argument("logical LOAD CSV URI expression must evaluate to a string");
			}
			const std::filesystem::path requested(decodeCsvFileUriPath(*uri));
			if (!requested.is_absolute()) {
				throw std::invalid_argument("logical LOAD CSV requires an absolute file URI path");
			}

			std::error_code error;
			const std::filesystem::path root =
					std::filesystem::weakly_canonical(*context.csv_import_policy.import_root, error);
			if (error || !std::filesystem::is_directory(root, error) || error) {
				throw std::runtime_error("logical LOAD CSV import root is inaccessible");
			}
			const std::filesystem::path canonical = std::filesystem::weakly_canonical(requested, error);
			if (error || !std::filesystem::exists(canonical, error) || error ||
				!std::filesystem::is_regular_file(canonical, error) || error) {
				throw std::runtime_error("logical LOAD CSV requires an accessible regular file");
			}
			if (!isWithinImportRoot(canonical, root)) {
				throw std::runtime_error("logical LOAD CSV file is outside the configured import root");
			}
			const uintmax_t fileBytes = std::filesystem::file_size(canonical, error);
			if (error || fileBytes > static_cast<uintmax_t>(context.csv_import_policy.maximum_file_bytes)) {
				throw std::runtime_error("logical LOAD CSV file exceeds its byte bound");
			}
			return canonical;
		}

		[[nodiscard]] const logical::LogicalOperator &requireOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.size() != 1U ||
				children.front() == nullptr) { // ZYX_COV_EXCL_LINE: planner-owned unary operators expose one child or
											   // throw before execution.
				throw std::runtime_error("logical-plan read runtime received an operator without one valid child");
			}
			return *children.front();
		}

		[[nodiscard]] bool isNull(const PropertyValue &value) { return value.getType() == PropertyType::NULL_TYPE; }

		[[nodiscard]] std::optional<bool> booleanOrNull(const PropertyValue &value) {
			if (isNull(value)) {
				return std::nullopt;
			}
			if (const bool *boolean = std::get_if<bool>(&value.getVariant())) {
				return *boolean;
			}
			throw std::runtime_error("logical-plan read runtime requires a Boolean predicate value");
		}

		[[nodiscard]] PropertyValue makeBoolean(std::optional<bool> value) {
			return value.has_value() ? PropertyValue(*value) : PropertyValue();
		}

		[[nodiscard]] int64_t checkedAdd(int64_t left, int64_t right) {
			if ((right > 0 && left > std::numeric_limits<int64_t>::max() - right) ||
				(right < 0 && left < std::numeric_limits<int64_t>::min() - right)) {
				throw std::overflow_error("logical-plan integer addition overflowed");
			}
			return left + right;
		}

		[[nodiscard]] int64_t checkedSubtract(int64_t left, int64_t right) {
			if ((right > 0 && left < std::numeric_limits<int64_t>::min() + right) ||
				(right < 0 && left > std::numeric_limits<int64_t>::max() + right)) {
				throw std::overflow_error("logical-plan integer subtraction overflowed");
			}
			return left - right;
		}

		[[nodiscard]] int64_t checkedMultiply(int64_t left, int64_t right) {
			if (left == 0 || right == 0) {
				return 0;
			}
			if ((left == -1 && right == std::numeric_limits<int64_t>::min()) ||
				(right == -1 && left == std::numeric_limits<int64_t>::min())) {
				throw std::overflow_error("logical-plan integer multiplication overflowed");
			}
			if ((left > 0 && right > 0 && left > std::numeric_limits<int64_t>::max() / right) ||
				(left > 0 && right < 0 && right < std::numeric_limits<int64_t>::min() / left) ||
				(left < 0 && right > 0 && left < std::numeric_limits<int64_t>::min() / right) ||
				(left < 0 && right < 0 && left < std::numeric_limits<int64_t>::max() / right)) {
				throw std::overflow_error("logical-plan integer multiplication overflowed");
			}
			return left * right;
		}

		[[nodiscard]] std::pair<int, int> addMonthsToYearMonth(int year, int month, int64_t months) {
			const int64_t zeroBasedMonth = checkedAdd(checkedMultiply(static_cast<int64_t>(year), 12LL),
													  checkedAdd(static_cast<int64_t>(month) - 1LL, months));
			int64_t resultYear = zeroBasedMonth / 12LL;
			int64_t resultMonth = zeroBasedMonth % 12LL;
			if (resultMonth < 0LL) {
				resultMonth += 12LL;
				--resultYear;
			}
			if (resultYear < static_cast<int64_t>(std::numeric_limits<int>::min()) ||
				resultYear > static_cast<int64_t>(std::numeric_limits<int>::max())) {
				throw std::overflow_error("logical-plan temporal year overflowed");
			}
			return {static_cast<int>(resultYear), static_cast<int>(resultMonth + 1LL)};
		}

		[[nodiscard]] int64_t durationWholeDays(const TemporalDuration &duration) {
			constexpr int64_t NANOS_PER_DAY = 86'400LL * 1'000'000'000LL;
			return checkedAdd(duration.days, duration.nanos / NANOS_PER_DAY);
		}

		[[nodiscard]] PropertyValue addDurationToDate(const TemporalDate &date, const TemporalDuration &duration) {
			const auto [year, month] = addMonthsToYearMonth(date.year(), date.month(), duration.months);
			const TemporalDate monthAdjusted = TemporalDate::fromYMD(year, month, date.day());
			const int64_t epochDays =
					checkedAdd(static_cast<int64_t>(monthAdjusted.epochDays), durationWholeDays(duration));
			if (epochDays < static_cast<int64_t>(std::numeric_limits<int32_t>::min()) ||
				epochDays > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
				throw std::overflow_error("logical-plan temporal date overflowed");
			}
			return PropertyValue(TemporalDate{static_cast<int32_t>(epochDays)});
		}

		[[nodiscard]] PropertyValue addDurationToDateTime(const TemporalDateTime &dateTime,
														  const TemporalDuration &duration) {
			const auto [year, month] = addMonthsToYearMonth(dateTime.year(), dateTime.month(), duration.months);
			TemporalDateTime result = TemporalDateTime::fromComponents(year, month, dateTime.day(), dateTime.hour(),
																	   dateTime.minute(), dateTime.second());
			constexpr int64_t MILLIS_PER_DAY = 86'400LL * 1'000LL;
			const int64_t dayMillis = checkedMultiply(duration.days, MILLIS_PER_DAY);
			const int64_t nanosMillis = duration.nanos / 1'000'000LL;
			result.epochMillis = checkedAdd(result.epochMillis, checkedAdd(dayMillis, nanosMillis));
			return PropertyValue(result);
		}

		[[nodiscard]] PropertyValue temporalComponent(const PropertyValue &value, std::string_view component) {
			if (value.getType() == PropertyType::DATE) {
				const TemporalDate &date = std::get<TemporalDate>(value.getVariant());
				if (component == "year")
					return PropertyValue(static_cast<int64_t>(date.year()));
				if (component == "month")
					return PropertyValue(static_cast<int64_t>(date.month()));
				if (component == "day")
					return PropertyValue(static_cast<int64_t>(date.day()));
				if (component == "epochDays")
					return PropertyValue(static_cast<int64_t>(date.epochDays));
			}
			if (value.getType() == PropertyType::DATETIME) {
				const TemporalDateTime &dateTime = std::get<TemporalDateTime>(value.getVariant());
				if (component == "year")
					return PropertyValue(static_cast<int64_t>(dateTime.year()));
				if (component == "month")
					return PropertyValue(static_cast<int64_t>(dateTime.month()));
				if (component == "day")
					return PropertyValue(static_cast<int64_t>(dateTime.day()));
				if (component == "hour")
					return PropertyValue(static_cast<int64_t>(dateTime.hour()));
				if (component == "minute")
					return PropertyValue(static_cast<int64_t>(dateTime.minute()));
				if (component == "second")
					return PropertyValue(static_cast<int64_t>(dateTime.second()));
				if (component == "epochMillis")
					return PropertyValue(dateTime.epochMillis);
			}
			if (value.getType() == PropertyType::DURATION) {
				const TemporalDuration &duration = std::get<TemporalDuration>(value.getVariant());
				if (component == "months")
					return PropertyValue(duration.months);
				if (component == "days")
					return PropertyValue(duration.days);
				if (component == "seconds")
					return PropertyValue(duration.nanos / 1'000'000'000LL);
				if (component == "nanoseconds")
					return PropertyValue(duration.nanos);
			}
			return PropertyValue();
		}

		[[nodiscard]] PropertyValue scalarValue(const LogicalPlanReadValue &value) {
			if (const PropertyValue *property = std::get_if<PropertyValue>(&value)) {
				return *property;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				return PropertyValue(node->node_id);
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				return PropertyValue(edge->edge_id);
			}
			return PropertyValue(std::get<storage::EntityRef>(value).entity_id);
		}

		void validateNode(const storage::LogicalGraphNode &node, ExecutionState &state) {
			if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			if (node.node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Node");
			}
			detail::BudgetMemoryResource labelMemory(state.shared.materialized_budget);
			std::pmr::unordered_set<std::string_view> labels(&labelMemory);
			detail::procedureVectorReserve(labels, node.labels.size());
			size_t labelsUntilGuard = 256U;
			for (const std::string &label: node.labels) {
				if (--labelsUntilGuard == 0U) {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
					labelsUntilGuard = 256U;
				}
				if (label.empty() || !labels.insert(label).second) {
					throw std::runtime_error("logical graph backend returned an invalid Node label set");
				}
			}
		}

		void validateNodeIdentity(ExecutionState &state, uint64_t nodeId) {
			checkGuard(state);
			const auto node = state.shared.snapshot.readNodeShaped(nodeId, {});
			validateNode(node, state);
			if (node.node_id != nodeId) {
				throw std::runtime_error("logical graph endpoint identity changed during validation");
			}
		}

		void validateEdge(const storage::LogicalGraphEdge &edge) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Edge");
			}
		}

		void validateEdge(const storage::LogicalGraphEdgeTopology &edge) {
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U) {
				throw std::runtime_error("logical graph backend returned an invalid Edge topology");
			}
		}

		void validateEntityRef(const storage::EntityRef &reference) {
			if (reference.entity_id == 0U ||
				(reference.is_edge && (reference.source_node_id == 0U || reference.target_node_id == 0U))) {
				throw std::runtime_error("logical graph backend returned an invalid Entity reference");
			}
		}

		void validateNodeRef(const storage::EntityRef &reference, const char *message) {
			if (reference.entity_id == 0U || reference.is_edge) {
				throw std::runtime_error(message);
			}
		}

		void validateEdgeRef(const storage::EntityRef &reference, const char *message) {
			if (reference.entity_id == 0U || !reference.is_edge || reference.source_node_id == 0U ||
				reference.target_node_id == 0U) {
				throw std::runtime_error(message);
			}
		}

		void validateEdgeReferenceRecord(const storage::EntityRef &reference, const storage::LogicalGraphEdge &edge,
										 ExecutionState &state, const char *context) {
			validateEdge(edge);
			if (edge.edge_id != reference.entity_id || edge.source_node_id != reference.source_node_id ||
				edge.target_node_id != reference.target_node_id) {
				throw std::runtime_error(std::string(context) +
										 " topology disagrees with its materialized relationship");
			}
			if (reference.type_token != 0U && state.shared.snapshot.supportsNativeRelationshipTypeTokens(edge.type)) {
				const std::optional<uint64_t> resolved = state.shared.snapshot.findRelationshipTypeToken(edge.type);
				if (!resolved.has_value() || *resolved != reference.type_token) {
					throw std::runtime_error(std::string(context) +
											 " relationship type disagrees with its materialized relationship");
				}
			}
		}

		void appendMissingReferenceId(std::vector<uint64_t> &ids, uint64_t id, size_t maximumCount,
									  MaterializedByteReservation &slots) {
			if (ids.size() == ids.capacity()) {
				const size_t previousBytes = slots.bytes();
				const size_t capacity = ids.capacity();
				const size_t nextCapacity =
						capacity == 0U
								? 1U
								: std::min(maximumCount, capacity > maximumCount / 2U ? maximumCount : capacity * 2U);
				slots.set(checkedSizeMul(nextCapacity, sizeof(uint64_t),
										 "logical-plan reference ID buffer size overflowed"));
				try {
					ids.reserve(nextCapacity);
					slots.set(checkedSizeMul(ids.capacity(), sizeof(uint64_t),
											 "logical-plan reference ID buffer size overflowed"));
				} catch (...) {
					slots.set(previousBytes);
					throw;
				}
			}
			ids.push_back(id);
		}

		template<typename Record>
		void appendReferenceFallback(std::vector<Record> &records, Record &&record, size_t recordBytes,
									 size_t maximumCount, size_t &heapBytes, MaterializedByteReservation &reservation) {
			const size_t nextHeapBytes = checkedSizeAdd(heapBytes, recordBytes - sizeof(Record),
														"logical-plan reference fallback size overflowed");
			const size_t previousBytes = reservation.bytes();
			const size_t capacity = records.capacity();
			const size_t nextCapacity =
					records.size() == capacity
							? (capacity == 0U ? 1U
											  : std::min(maximumCount,
														 capacity > maximumCount / 2U ? maximumCount : capacity * 2U))
							: capacity;
			reservation.set(checkedSizeAdd(
					nextHeapBytes,
					checkedSizeMul(nextCapacity, sizeof(Record), "logical-plan reference fallback size overflowed"),
					"logical-plan reference fallback size overflowed"));
			try {
				if (nextCapacity != capacity)
					records.reserve(nextCapacity);
				reservation.set(checkedSizeAdd(nextHeapBytes,
											   checkedSizeMul(records.capacity(), sizeof(Record),
															  "logical-plan reference fallback size overflowed"),
											   "logical-plan reference fallback size overflowed"));
				records.push_back(std::move(record));
			} catch (...) {
				reservation.set(previousBytes);
				throw;
			}
			heapBytes = nextHeapBytes;
		}

		[[nodiscard]] std::vector<storage::LogicalGraphNode>
		readNodesShapedSortedBudgeted(ExecutionState &state, std::span<const uint64_t> ids,
									  const storage::EntityReadShape &shape,
									  MaterializedByteReservation *retainedSource = nullptr) {
			MaterializedByteReservation temporarySource(&state.shared.materialized_budget);
			MaterializedByteReservation &sourceBytes = retainedSource == nullptr ? temporarySource : *retainedSource;
			if (retainedSource != nullptr) {
				sourceBytes.set(checkedSizeMul(ids.size(), sizeof(storage::LogicalGraphNode),
											   "logical-plan shaped Node source frame size overflowed"));
			}
			if (!state.shared.snapshot.supportsShapedPropertyReadAdmission()) {
				return state.shared.snapshot.readNodesShapedSorted(ids, shape);
			}
			return state.shared.snapshot.readNodesShapedSortedWithAdmission(ids, shape, [&](size_t bytes) {
				checkGuard(state);
				sourceBytes.set(
						checkedSizeAdd(sourceBytes.bytes(), bytes, "logical-plan shaped Node source size overflowed"));
			});
		}

		[[nodiscard]] std::vector<storage::LogicalGraphEdge>
		readEdgesShapedSortedBudgeted(ExecutionState &state, std::span<const uint64_t> ids,
									  const storage::EntityReadShape &shape,
									  MaterializedByteReservation *retainedSource = nullptr) {
			MaterializedByteReservation temporarySource(&state.shared.materialized_budget);
			MaterializedByteReservation &sourceBytes = retainedSource == nullptr ? temporarySource : *retainedSource;
			if (retainedSource != nullptr) {
				sourceBytes.set(checkedSizeMul(ids.size(), sizeof(storage::LogicalGraphEdge),
											   "logical-plan shaped Edge source frame size overflowed"));
			}
			if (!state.shared.snapshot.supportsShapedPropertyReadAdmission()) {
				return state.shared.snapshot.readEdgesShapedSorted(ids, shape);
			}
			return state.shared.snapshot.readEdgesShapedSortedWithAdmission(ids, shape, [&](size_t bytes) {
				checkGuard(state);
				sourceBytes.set(
						checkedSizeAdd(sourceBytes.bytes(), bytes, "logical-plan shaped Edge source size overflowed"));
			});
		}

		[[nodiscard]] size_t packedBooleanBytes(size_t capacity) {
			constexpr size_t bitsPerByte = std::numeric_limits<unsigned char>::digits;
			return capacity / bitsPerByte + (capacity % bitsPerByte != 0U ? 1U : 0U);
		}

		void retainShapedBatchBytes(const std::vector<storage::LogicalGraphNode> &records,
									MaterializedByteReservation &reservation, const QueryContext *context) {
			size_t bytes = checkedSizeMul(records.capacity(), sizeof(storage::LogicalGraphNode),
										  "logical-plan shaped Node batch capacity overflowed");
			for (const auto &record: records) {
				bytes = checkedSizeAdd(bytes, logicalNodeCacheBytes(record, context) - sizeof(record),
									   "logical-plan shaped Node batch size overflowed");
			}
			reservation.set(bytes);
		}

		void retainShapedBatchBytes(const std::vector<storage::LogicalGraphEdge> &records,
									MaterializedByteReservation &reservation, const QueryContext *context) {
			size_t bytes = checkedSizeMul(records.capacity(), sizeof(storage::LogicalGraphEdge),
										  "logical-plan shaped Edge batch capacity overflowed");
			for (const auto &record: records) {
				bytes = checkedSizeAdd(bytes, logicalEdgeCacheBytes(record, context) - sizeof(record),
									   "logical-plan shaped Edge batch size overflowed");
			}
			reservation.set(bytes);
		}

		void admitReferenceResultBytes(MaterializedByteReservation &reservation, size_t bytes, const char *message) {
			if (reservation.wouldExceed(bytes))
				throw std::runtime_error(message);
			reservation.set(bytes);
		}

		[[nodiscard]] std::vector<storage::LogicalGraphNode>
		materializeNodeReferences(std::span<const storage::EntityRef> references, ExecutionState &state,
								  const bool collectResults = true,
								  MaterializedByteReservation *resultBytes = nullptr) {
			std::vector<uint64_t> missingIds;
			MaterializedByteReservation missingIdSlots(&state.shared.materialized_budget);
			for (const storage::EntityRef &reference: references) {
				validateNodeRef(
						reference,
						"logical-plan materialization received an invalid or Edge reference where a Node was required");
				if (!state.shared.node_cache.contains(reference.entity_id)) {
					appendMissingReferenceId(missingIds, reference.entity_id, references.size(), missingIdSlots);
				}
			}
			std::ranges::sort(missingIds);
			missingIds.erase(std::unique(missingIds.begin(), missingIds.end()), missingIds.end());
			if (missingIds.size() > state.shared.bounds.maximum_node_records) {
				throw std::runtime_error("logical-plan Node materialization exceeded its record bound");
			}

			MaterializedByteReservation fallbackBytes(&state.shared.materialized_budget);
			std::vector<storage::LogicalGraphNode> loaded;
			size_t fallbackHeapBytes = 0U;
			// A record enters this fallback only when the shared cache declines it.
			for (size_t offset = 0U; offset < missingIds.size();) {
				checkGuard(state);
				size_t end = std::min(missingIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
				MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
				std::vector<storage::LogicalGraphNode> batch;
				while (true) {
					const auto ids = std::span<const uint64_t>(missingIds).subspan(offset, end - offset);
					try {
						batch = readNodesShapedSortedBudgeted(state, ids, completeNodeReadShape(), &sourceBytes);
						if (batch.size() != end - offset) {
							throw std::runtime_error("logical-plan shaped Node batch changed result cardinality");
						}
						for (size_t index = 0U; index < batch.size(); ++index) {
							validateNode(batch[index], state);
							if (batch[index].node_id != missingIds[offset + index]) {
								throw std::runtime_error("logical-plan shaped Node batch changed logical ID order");
							}
						}
						retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
						break;
					} catch (const std::runtime_error &error) {
						if (std::string_view(error.what()) != "shared materialization exceeded its byte bound" ||
							end - offset == 1U) {
							throw;
						}
						std::vector<storage::LogicalGraphNode>().swap(batch);
						sourceBytes.reset();
						end = offset + std::max<size_t>(1U, (end - offset) / 2U);
					}
				}
				for (size_t index = 0U; index < batch.size(); ++index) {
					const uint64_t id = batch[index].node_id;
					const size_t recordBytes = logicalNodeCacheBytes(batch[index], state.shared.context);
					const size_t previousSourceBytes = sourceBytes.bytes();
					// The vector frame remains live, but its heap payload is transferred
					// to either the cache or the fallback buffer.
					sourceBytes.set(previousSourceBytes - (recordBytes - sizeof(storage::LogicalGraphNode)));
					cacheNodeRecord(state, std::move(batch[index]));
					if (!state.shared.node_cache.contains(id) && collectResults) {
						appendReferenceFallback(loaded, std::move(batch[index]), recordBytes, missingIds.size(),
												fallbackHeapBytes, fallbackBytes);
					} else if (!state.shared.node_cache.contains(id)) {
						sourceBytes.set(previousSourceBytes);
					}
				}
				offset = end;
			}

			if (!collectResults)
				return {};
			std::vector<storage::LogicalGraphNode> result;
			if (resultBytes != nullptr) {
				const size_t slotBytes = checkedSizeMul(references.size(), sizeof(storage::LogicalGraphNode),
														"logical-plan Node reference result capacity overflowed");
				admitReferenceResultBytes(*resultBytes, slotBytes,
										  "logical-plan Node reference result exceeded its shared byte bound");
				try {
					result.reserve(references.size());
				} catch (...) {
					resultBytes->reset();
					throw;
				}
				admitReferenceResultBytes(*resultBytes,
										  checkedSizeMul(result.capacity(), sizeof(storage::LogicalGraphNode),
														 "logical-plan Node reference result capacity overflowed"),
										  "logical-plan Node reference result exceeded its shared byte bound");
			} else {
				result.reserve(references.size());
			}
			const auto appendResult = [&](const storage::LogicalGraphNode &node) {
				const size_t previousBytes = resultBytes == nullptr ? 0U : resultBytes->bytes();
				if (resultBytes != nullptr) {
					admitReferenceResultBytes(*resultBytes,
											  checkedSizeAdd(previousBytes, logicalNodeCacheBytes(node, state.shared.context) - sizeof(node),
															 "logical-plan Node reference result size overflowed"),
											  "logical-plan Node reference result exceeded its shared byte bound");
				}
				try {
					result.push_back(node);
				} catch (...) {
					if (resultBytes != nullptr)
						resultBytes->set(previousBytes);
					throw;
				}
			};
			for (const storage::EntityRef &reference: references) {
				if (const auto cached = state.shared.node_cache.find(reference.entity_id); cached != state.shared.node_cache.end()) {
					validateNode(cached->second, state);
					appendResult(cached->second);
					continue;
				}
				const auto position =
						std::ranges::lower_bound(loaded, reference.entity_id, {}, &storage::LogicalGraphNode::node_id);
				if (position == loaded.end() || position->node_id != reference.entity_id) {
					throw std::runtime_error("logical-plan shaped Node batch lost a requested Node");
				}
				appendResult(*position);
			}
			return result;
		}

		[[nodiscard]] std::vector<storage::LogicalGraphEdge>
		materializeEdgeReferences(std::span<const storage::EntityRef> references, ExecutionState &state,
								  const bool collectResults = true,
								  MaterializedByteReservation *resultBytes = nullptr) {
			std::vector<uint64_t> missingIds;
			MaterializedByteReservation missingIdSlots(&state.shared.materialized_budget);
			for (const storage::EntityRef &reference: references) {
				validateEdgeRef(reference, "logical-plan materialization received an invalid or Node reference where "
										   "an Edge was required");
				if (!state.shared.edge_cache.contains(reference.entity_id)) {
					appendMissingReferenceId(missingIds, reference.entity_id, references.size(), missingIdSlots);
				}
			}
			std::ranges::sort(missingIds);
			missingIds.erase(std::unique(missingIds.begin(), missingIds.end()), missingIds.end());
			if (missingIds.size() > state.shared.bounds.maximum_edge_records) {
				throw std::runtime_error("logical-plan Edge materialization exceeded its record bound");
			}

			MaterializedByteReservation fallbackBytes(&state.shared.materialized_budget);
			std::vector<storage::LogicalGraphEdge> loaded;
			size_t fallbackHeapBytes = 0U;
			for (size_t offset = 0U; offset < missingIds.size();) {
				checkGuard(state);
				size_t end = std::min(missingIds.size(), offset + DEFAULT_PROPERTY_BATCH_ROWS);
				MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
				std::vector<storage::LogicalGraphEdge> batch;
				while (true) {
					const auto ids = std::span<const uint64_t>(missingIds).subspan(offset, end - offset);
					try {
						batch = readEdgesShapedSortedBudgeted(state, ids, completeEdgeReadShape(), &sourceBytes);
						if (batch.size() != end - offset) {
							throw std::runtime_error("logical-plan shaped Edge batch changed result cardinality");
						}
						for (size_t index = 0U; index < batch.size(); ++index) {
							validateEdge(batch[index]);
							if (batch[index].edge_id != missingIds[offset + index]) {
								throw std::runtime_error("logical-plan shaped Edge batch changed logical ID order");
							}
						}
						retainShapedBatchBytes(batch, sourceBytes, state.shared.context);
						break;
					} catch (const std::runtime_error &error) {
						if (std::string_view(error.what()) != "shared materialization exceeded its byte bound" ||
							end - offset == 1U) {
							throw;
						}
						std::vector<storage::LogicalGraphEdge>().swap(batch);
						sourceBytes.reset();
						end = offset + std::max<size_t>(1U, (end - offset) / 2U);
					}
				}
				for (size_t index = 0U; index < batch.size(); ++index) {
					const uint64_t id = batch[index].edge_id;
					const size_t recordBytes = logicalEdgeCacheBytes(batch[index], state.shared.context);
					const size_t previousSourceBytes = sourceBytes.bytes();
					sourceBytes.set(previousSourceBytes - (recordBytes - sizeof(storage::LogicalGraphEdge)));
					cacheEdgeRecord(state, std::move(batch[index]));
					if (!state.shared.edge_cache.contains(id)) {
						appendReferenceFallback(loaded, std::move(batch[index]), recordBytes, missingIds.size(),
												fallbackHeapBytes, fallbackBytes);
					}
				}
				offset = end;
			}

			std::vector<storage::LogicalGraphEdge> result;
			if (collectResults) {
				if (resultBytes != nullptr) {
					const size_t slotBytes = checkedSizeMul(references.size(), sizeof(storage::LogicalGraphEdge),
															"logical-plan Edge reference result capacity overflowed");
					admitReferenceResultBytes(*resultBytes, slotBytes,
											  "logical-plan Edge reference result exceeded its shared byte bound");
					try {
						result.reserve(references.size());
					} catch (...) {
						resultBytes->reset();
						throw;
					}
					admitReferenceResultBytes(*resultBytes,
											  checkedSizeMul(result.capacity(), sizeof(storage::LogicalGraphEdge),
															 "logical-plan Edge reference result capacity overflowed"),
											  "logical-plan Edge reference result exceeded its shared byte bound");
				} else {
					result.reserve(references.size());
				}
			}
			for (const storage::EntityRef &reference: references) {
				const storage::LogicalGraphEdge *edge = nullptr;
				if (const auto cached = state.shared.edge_cache.find(reference.entity_id); cached != state.shared.edge_cache.end()) {
					edge = &cached->second;
				} else {
					const auto position = std::ranges::lower_bound(loaded, reference.entity_id, {},
																   &storage::LogicalGraphEdge::edge_id);
					if (position == loaded.end() || position->edge_id != reference.entity_id) {
						throw std::runtime_error("logical-plan shaped Edge batch lost a requested relationship");
					}
					edge = &*position;
				}
				validateEdgeReferenceRecord(reference, *edge, state, "logical graph Entity reference");
				if (collectResults) {
					const size_t previousBytes = resultBytes == nullptr ? 0U : resultBytes->bytes();
					if (resultBytes != nullptr) {
						admitReferenceResultBytes(*resultBytes,
												  checkedSizeAdd(previousBytes,
																 logicalEdgeCacheBytes(*edge, state.shared.context) - sizeof(*edge),
																 "logical-plan Edge reference result size overflowed"),
												  "logical-plan Edge reference result exceeded its shared byte bound");
					}
					try {
						result.push_back(*edge);
					} catch (...) {
						if (resultBytes != nullptr)
							resultBytes->set(previousBytes);
						throw;
					}
				}
			}
			return result;
		}

		[[nodiscard]] storage::LogicalGraphNode materializeNodeValue(ExecutionState &state,
																	 const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node, state);
				return detail::copyLogicalEntityGuarded(*node, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateNodeRef(
						*reference,
						"logical-plan expected a Node reference but received an invalid or relationship reference");
				return readNodeCached(state, reference->entity_id);
			}
			throw std::runtime_error("logical-plan expected a Node variable");
		}

		[[nodiscard]] uint64_t nodeBindingId(const LogicalPlanReadValue &value, ExecutionState &state) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				validateNode(*node, state);
				return node->node_id;
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateNodeRef(
						*reference,
						"logical-plan expected a Node reference but received an invalid or relationship reference");
				return reference->entity_id;
			}
			throw std::runtime_error("logical-plan expected a Node variable");
		}

		[[nodiscard]] storage::LogicalGraphNode patternSourceIdentity(ExecutionState &state,
																	  const LogicalPlanReadValue &value) {
			const uint64_t id = nodeBindingId(value, state);
			if (std::holds_alternative<storage::EntityRef>(value)) {
				if (!state.shared.snapshot.supportsNodeExistenceProbes()) {
					validateNodeIdentity(state, id);
					return {id, {}, {}};
				}
				checkGuard(state);
				const std::array<uint64_t, 1U> ids{id};
				const auto present = state.shared.snapshot.containsNodesSorted(ids);
				if (present.size() != 1U || !present.front()) {
					throw std::runtime_error("logical-plan pattern source is missing from its pinned snapshot");
				}
			}
			return {id, {}, {}};
		}

		[[nodiscard]] storage::LogicalGraphEdge materializeEdgeValue(ExecutionState &state,
																	 const LogicalPlanReadValue &value) {
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				validateEdge(*edge);
				return detail::copyLogicalEntityGuarded(*edge, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				});
			}
			if (const auto *reference = std::get_if<storage::EntityRef>(&value)) {
				validateEdgeRef(
						*reference,
						"logical-plan expected a relationship reference but received an invalid or Node reference");
				const storage::LogicalGraphEdge edge = readEdgeCached(state, reference->entity_id);
				validateEdgeReferenceRecord(*reference, edge, state, "logical graph Entity reference");
				return edge;
			}
			throw std::runtime_error("logical-plan expected a relationship variable");
		}

		[[nodiscard]] PropertyMap readInlineProperties(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													   ExecutionState &state) {
			if (entries.size() > state.shared.bounds.maximum_property_entries) {
				throw std::runtime_error("logical graph backend returned too many direct Property entries");
			}
			incrementBounded(state.task.stats.property_sets_materialized,
							 "logical-plan Property materialization counter overflowed");
			PropertyMap properties;
			properties.reserve(entries.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				checkGuard(state);
				if (entry.key.empty() || !properties.emplace(entry.key, detail::copyPropertyValueGuarded(entry.value, [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				})).second) {
					throw std::runtime_error("logical graph backend returned an invalid direct Property entry set");
				}
			}
			return properties;
		}

		[[nodiscard]] PropertyMap readNodeProperties(storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphNode &node, ExecutionState &state) {
			static_cast<void>(snapshot);
			return readInlineProperties(node.properties, state);
		}

		[[nodiscard]] PropertyMap readEdgeProperties(storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphEdge &edge, ExecutionState &state) {
			static_cast<void>(snapshot);
			return readInlineProperties(edge.properties, state);
		}

		[[nodiscard]] bool propertiesMatch(const PropertyMap &properties,
										   const std::vector<std::pair<std::string, PropertyValue>> &predicates) {
			for (const auto &[key, expected]: predicates) {
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Property predicate has an empty key");
				}
				const auto found = properties.find(key);
				if (found == properties.end() || found->second != expected) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool nodeMatches(storage::LogicalGraphReadSnapshot &snapshot,
									   const storage::LogicalGraphNode &node, const std::vector<std::string> &labels,
									   const std::vector<std::pair<std::string, PropertyValue>> &predicates,
									   ExecutionState &state) {
			validateNode(node, state);
			std::unordered_set<std::string_view> actualLabels(node.labels.begin(), node.labels.end());
			std::unordered_set<std::string_view> requiredLabels;
			requiredLabels.reserve(labels.size());
			for (const std::string &label: labels) {
				checkGuard(state);
				if (label.empty() || !requiredLabels.insert(label).second) {
					throw std::invalid_argument("logical-plan Node scan has an empty or duplicate label");
				}
				if (!actualLabels.contains(label)) {
					return false;
				}
			}
			if (predicates.empty()) {
				return true;
			}
			return propertiesMatch(readNodeProperties(snapshot, node, state), predicates);
		}

		[[nodiscard]] bool
		nodeScanMatches(storage::LogicalGraphReadSnapshot &snapshot, const storage::LogicalGraphNode &node,
						const logical::LogicalNodeScan &scan,
						const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
						ExecutionState &state) {
			validateNode(node, state);
			std::unordered_set<std::string_view> actualLabels(node.labels.begin(), node.labels.end());
			std::unordered_set<std::string_view> requiredLabels;
			requiredLabels.reserve(scan.getLabels().size());
			for (const std::string &label: scan.getLabels()) {
				checkGuard(state);
				if (label.empty() || !requiredLabels.insert(label).second) {
					throw std::invalid_argument("logical-plan Node scan has an empty or duplicate label");
				}
				if (!actualLabels.contains(label)) {
					return false;
				}
			}
			const bool needsProperties = !scan.getPropertyPredicates().empty() || !parameterPredicates.empty() ||
										 !scan.getRangePredicates().empty() || scan.getCompositeEquality().has_value();
			if (!needsProperties) {
				return true;
			}
			const PropertyMap properties = readNodeProperties(snapshot, node, state);
			if (!propertiesMatch(properties, scan.getPropertyPredicates())) {
				return false;
			}
			if (!propertiesMatch(properties, parameterPredicates)) {
				return false;
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				checkGuard(state);
				if (range.key.empty()) {
					throw std::invalid_argument("logical-plan Node range predicate has an empty key");
				}
				const auto found = properties.find(range.key);
				if (found == properties.end()) {
					return false;
				}
				if (!isNull(range.minValue) &&
					(range.minInclusive ? found->second < range.minValue : found->second <= range.minValue)) {
					return false;
				}
				if (!isNull(range.maxValue) &&
					(range.maxInclusive ? found->second > range.maxValue : found->second >= range.maxValue)) {
					return false;
				}
			}
			if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					checkGuard(state);
					if (composite->keys[index].empty()) {
						throw std::invalid_argument("logical-plan Node composite predicate has an empty key");
					}
					const auto found = properties.find(composite->keys[index]);
					if (found == properties.end() || found->second != composite->values[index]) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] std::optional<std::vector<std::pair<std::string, PropertyValue>>>
		resolveParameterPropertyPredicates(const logical::LogicalNodeScan &scan, ExecutionState &state) {
			std::unordered_set<std::string_view> keys;
			keys.reserve(scan.getPropertyPredicates().size() + scan.getParameterPropertyPredicates().size());
			for (const auto &[key, value]: scan.getPropertyPredicates()) {
				static_cast<void>(value);
				if (key.empty() || !keys.insert(key).second) {
					throw std::invalid_argument(
							"logical-plan Node scan has an empty or duplicate Property predicate key");
				}
			}
			std::vector<std::pair<std::string, PropertyValue>> result;
			result.reserve(scan.getParameterPropertyPredicates().size());
			for (const logical::ParameterPropertyPredicate &predicate: scan.getParameterPropertyPredicates()) {
				checkGuard(state);
				if (predicate.key.empty() || predicate.parameter_name.empty() || !keys.insert(predicate.key).second) {
					throw std::invalid_argument("logical-plan Node scan has an empty, duplicate, or conflicting "
												"parameter Property predicate key");
				}
				if (state.shared.context == nullptr) {
					throw std::runtime_error("logical-plan parameter equality predicate requires a QueryContext");
				}
				const auto value = state.shared.context->parameters.find(predicate.parameter_name);
				if (value == state.shared.context->parameters.end()) {
					throw std::runtime_error("logical-plan read runtime could not resolve a query parameter");
				}
				// Cypher equality with null does not select a Node, so it need not
				// materialize a Property collection only to discard every candidate.
				if (isNull(value->second)) {
					return std::nullopt;
				}
				result.emplace_back(predicate.key, value->second);
			}
			return result;
		}

		[[nodiscard]] bool edgeMatches(storage::LogicalGraphReadSnapshot &snapshot,
									   const storage::LogicalGraphEdge &edge, const std::string &type,
									   const std::unordered_map<std::string, PropertyValue> &predicates,
									   ExecutionState &state) {
			validateEdge(edge);
			if (!type.empty() && edge.type != type) {
				return false;
			}
			if (predicates.empty()) {
				return true;
			}
			std::vector<std::pair<std::string, PropertyValue>> ordered;
			ordered.reserve(predicates.size());
			for (const auto &[key, value]: predicates) {
				ordered.emplace_back(key, value);
			}
			return propertiesMatch(readEdgeProperties(snapshot, edge, state), ordered);
		}

		[[nodiscard]] const LogicalPlanReadValue &requireBinding(const LogicalPlanReadRow &row,
																 const std::string &variable) {
			const auto found = row.values.find(variable);
			if (found == row.values.end()) {
				throw std::runtime_error("logical-plan read runtime could not resolve a variable");
			}
			return found->second;
		}

		[[nodiscard]] storage::LogicalGraphAdjacencyTopologyPage readTopologyPageBudgeted(
			ExecutionState &state, uint64_t nodeId, storage::LogicalGraphReadDirection direction,
			std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			MaterializedByteReservation &sourceBytes) {
			struct SourcePressure {};
			const auto admit = [&](size_t bytes) {
				checkGuard(state);
				if (sourceBytes.wouldExceed(bytes)) throw SourcePressure{};
				sourceBytes.set(bytes);
			};
			size_t requested = maximumRecords;
			for (;;) {
				checkGuard(state);
				try {
					auto page = state.shared.snapshot.supportsAdjacencyTopologyBufferAdmission()
						? state.shared.snapshot.scanAdjacentEdgeTopologiesWithBufferAdmission(nodeId, direction, continuation, requested, admit)
						: state.shared.snapshot.scanAdjacentEdgeTopologies(nodeId, direction, continuation, requested);
					if (page.edges.size() > requested || (page.edges.empty() && page.continuation))
						throw std::runtime_error("logical graph topology source returned an invalid page shape");
					uint64_t previous = continuation ? continuation->previous_edge_id : 0U;
					if (continuation && (page.edges.empty() || page.edges.front().edge_id != continuation->next_edge_id))
						throw std::runtime_error("logical graph topology source returned a discontinuous page");
					size_t bytes = checkedSizeMul(page.edges.capacity(), sizeof(storage::LogicalGraphEdgeTopology),
						"logical-plan topology capacity overflowed");
					for (const auto &edge: page.edges) {
						validateEdge(edge);
						if (edge.edge_id <= previous ||
							(direction == storage::LogicalGraphReadDirection::LGRD_OUTGOING ? edge.source_node_id : edge.target_node_id) != nodeId)
							throw std::runtime_error("logical graph topology source returned invalid order or endpoint");
						previous = edge.edge_id;
						bytes = checkedSizeAdd(bytes, edge.type.capacity(), "logical-plan topology names overflowed");
					}
					if (page.continuation && (page.continuation->previous_edge_id != previous || page.continuation->next_edge_id <= previous))
						throw std::runtime_error("logical graph topology source returned an invalid continuation");
					admit(bytes);
					return page;
				} catch (const SourcePressure &) {
					sourceBytes.reset();
					if (requested <= 1U) throw std::runtime_error("shared materialization exceeded its byte bound");
					requested = std::max<size_t>(1U, requested / 2U);
				}
			}
		}

		[[nodiscard]] storage::LogicalGraphAdjacencyReferencePage readReferencePageBudgeted(
			ExecutionState &state, uint64_t nodeId, storage::LogicalGraphReadDirection direction,
			std::optional<storage::LogicalGraphAdjacencyContinuation> continuation, size_t maximumRecords,
			MaterializedByteReservation &sourceBytes) {
			struct SourcePressure {};
			const auto admit = [&](size_t bytes) {
				checkGuard(state);
				if (sourceBytes.wouldExceed(bytes)) throw SourcePressure{};
				sourceBytes.set(bytes);
			};
			size_t requested = maximumRecords;
			for (;;) {
				checkGuard(state);
				try {
					auto page = state.shared.snapshot.supportsAdjacencyReferenceBufferAdmission()
						? state.shared.snapshot.scanAdjacentEdgeRefsWithBufferAdmission(nodeId, direction, continuation, requested, admit)
						: state.shared.snapshot.scanAdjacentEdgeRefs(nodeId, direction, continuation, requested);
					if (page.edges.size() > requested || (page.edges.empty() && page.continuation))
						throw std::runtime_error("logical graph reference source returned an invalid page shape");
					admit(checkedSizeMul(page.edges.capacity(), sizeof(storage::EntityRef),
						"logical-plan reference capacity overflowed"));
					return page;
				} catch (const SourcePressure &) {
					sourceBytes.reset();
					if (requested <= 1U) throw std::runtime_error("shared materialization exceeded its byte bound");
					requested = std::max<size_t>(1U, requested / 2U);
				}
			}
		}

		[[nodiscard]] PropertyValue readSelectedPropertyBudgeted(storage::LogicalGraphReadSnapshot &snapshot,
			uint64_t id, bool edge, std::string_view key, ExecutionState &state) {
			checkGuard(state);
			if (!snapshot.supportsPropertyReadAdmission())
				return edge ? snapshot.readEdgeProperty(id, key) : snapshot.readNodeProperty(id, key);
			MaterializedByteReservation sourceBytes(&state.shared.materialized_budget);
			sourceBytes.set(sizeof(std::vector<PropertyValue>) + sizeof(PropertyValue));
			const std::function<void(size_t)> admission = [&](size_t bytes) {
				checkGuard(state);
				sourceBytes.set(checkedSizeAdd(sourceBytes.bytes(), bytes,
					"logical-plan selected Property source size overflowed"));
			};
			const std::span<const uint64_t> ids(&id, 1U);
			const std::span<const std::string_view> keys(&key, 1U);
			auto columns = edge ? snapshot.readEdgePropertyColumnsSortedWithAdmission(ids, keys, admission)
				: snapshot.readNodePropertyColumnsSortedWithAdmission(ids, keys, admission);
			if (columns.size() != 1U || columns.front().size() != 1U)
				throw std::runtime_error("logical graph selected Property columns changed their shape");
			const size_t frames = checkedSizeAdd(
				checkedSizeMul(columns.capacity(), sizeof(std::vector<PropertyValue>),
					"logical-plan selected Property column size overflowed"),
				checkedSizeMul(columns.front().capacity(), sizeof(PropertyValue),
					"logical-plan selected Property row size overflowed"),
				"logical-plan selected Property frame size overflowed");
			sourceBytes.set(checkedSizeAdd(frames, property_utils::getPropertyValueSize(columns.front().front()),
				"logical-plan selected Property payload size overflowed"));
			checkGuard(state);
			return std::move(columns.front().front());
		}

		[[nodiscard]] PropertyValue readProperty(storage::LogicalGraphReadSnapshot &snapshot, LogicalPlanReadRow &row,
												 const std::string &variable, const std::string &key,
												 ExecutionState &state, const LogicalPlanReadValue *boundBinding = nullptr) {
			if (key.empty()) {
				throw std::invalid_argument("logical-plan Property access has an empty key");
			}
			const auto copyResult = [&](const PropertyValue &source, bool borrowed = false) {
				// Fixed scalar alternatives cannot allocate payload buffers. Their
				// owning output slots remain charged by the receiving row/container.
				switch (source.getType()) {
					case PropertyType::NULL_TYPE:
					case PropertyType::BOOLEAN:
					case PropertyType::INTEGER:
					case PropertyType::DOUBLE:
					case PropertyType::DATE:
					case PropertyType::DATETIME:
					case PropertyType::DURATION:
						return source;
					default: break;
				}
				MaterializedByteReservation copyBytes(&state.shared.materialized_budget);
				PropertyValue copied = copyReservedPropertyValue(source, state, copyBytes);
				debug::PerfTrace::addValue("logical_plan.property.owned_result_copy_bytes",
					static_cast<int64_t>(copyBytes.bytes()));
				if (borrowed) debug::PerfTrace::addValue("logical_plan.prefetch.borrowed_value_copy_bytes",
					static_cast<int64_t>(copyBytes.bytes()));
				return copied;
			};
			if (const auto cached = row.property_cache.find(variable); cached != row.property_cache.end()) {
				const auto found = cached->second.find(key);
				if (found != cached->second.end()) {
					return copyResult(found->second);
				}
				if (row.complete_entity_property_cache.contains(variable)) {
					return PropertyValue();
				}
				// An EntityRef cache is a selected-field cache, not proof that the
				// requested field is absent. Continue to the token-aware native read
				// for a field not seen on this row yet.
				const auto *binding = boundBinding;
				if (binding == nullptr) {
					const auto found = row.values.find(variable);
					if (found != row.values.end()) binding = &found->second;
				}
				if (binding == nullptr || !std::holds_alternative<storage::EntityRef>(*binding)) {
					return PropertyValue();
				}
			}
			const LogicalPlanReadValue &binding = boundBinding == nullptr ? requireBinding(row, variable) : *boundBinding;
			if (const auto *reference = std::get_if<storage::EntityRef>(&binding);
				reference != nullptr && !reference->is_edge && state.task.property_columns != nullptr &&
				*state.task.property_columns->variable == variable) {
				validateNodeRef(*reference, "logical-plan borrowed Property received an invalid Node reference");
				const auto &view = *state.task.property_columns;
				const auto id = std::ranges::lower_bound(view.ids, reference->entity_id);
				const auto keyPosition = std::ranges::find(*view.keys, key);
				if (id != view.ids.end() && *id == reference->entity_id && keyPosition != view.keys->end()) {
					debug::PerfTrace::addValue("logical_plan.prefetch.borrowed_values", 1);
					return copyResult(view.columns[keyPosition - view.keys->begin()][id - view.ids.begin()], true);
				}
			}
			if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
				if (value->getType() == PropertyType::DATE || value->getType() == PropertyType::DATETIME ||
					value->getType() == PropertyType::DURATION) {
					return temporalComponent(*value, key);
				}
				if (value->getType() != PropertyType::MAP) {
					return PropertyValue();
				}
				const auto &properties = value->getMap();
				const auto found = properties.find(key);
				return found == properties.end() ? PropertyValue() : copyResult(found->second);
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
				auto [cached, inserted] =
						detail::procedureMapEmplace(row.property_cache, variable, readNodeProperties(snapshot, *node, state));
				static_cast<void>(inserted);
				const auto found = cached->second.find(key);
				return found == cached->second.end() ? PropertyValue() : copyResult(found->second);
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
				auto [cached, inserted] =
						detail::procedureMapEmplace(row.property_cache, variable, readEdgeProperties(snapshot, *edge, state));
				static_cast<void>(inserted);
				const auto found = cached->second.find(key);
				return found == cached->second.end() ? PropertyValue() : copyResult(found->second);
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(binding);
			validateEntityRef(reference);
			// Native snapshots resolve the property name to a token and decode only
			// the requested value. The virtual default retains complete-read
			// compatibility for non-native logical backends.
			PropertyValue value = readSelectedPropertyBudgeted(
				snapshot, reference.entity_id, reference.is_edge, key, state);
			// The selected value is already owned. Transfer it into the row cache;
			// only the independent expression result needs a payload copy.
			auto [cacheOwner, cacheInserted] = detail::procedureMapTryEmplace(row.property_cache, variable);
            static_cast<void>(cacheInserted);
            const auto [cached, inserted] = detail::procedureMapInsertOrAssign(cacheOwner->second, key, std::move(value));
			static_cast<void>(inserted);
			return copyResult(cached->second);
		}

		[[nodiscard]] bool
		nodeRefScanMatches(storage::LogicalGraphReadSnapshot &snapshot, LogicalPlanReadRow &row,
						   const std::string &variable, const logical::LogicalNodeScan &scan,
						   const std::vector<std::pair<std::string, PropertyValue>> &parameterPredicates,
						   ExecutionState &state, const bool labelsAlreadyMatched = false) {
			const storage::EntityRef &reference = std::get<storage::EntityRef>(requireBinding(row, variable));
			validateNodeRef(reference, "logical-plan Node predicate received an invalid or Edge reference");
			if (!labelsAlreadyMatched && !snapshot.nodeHasLabels(reference.entity_id, scan.getLabels())) {
				return false;
			}
			const auto inspectProperty = [&](const std::string &key, const auto &inspect) {
				if (const auto cached = row.property_cache.find(variable); cached != row.property_cache.end()) {
					if (const auto found = cached->second.find(key); found != cached->second.end()) {
						return inspect(found->second);
					}
				}
				return inspect(readProperty(snapshot, row, variable, key, state));
			};
			const auto equals = [&](const std::string &key, const PropertyValue &expected) {
				checkGuard(state);
				if (key.empty()) {
					throw std::invalid_argument("logical-plan Property predicate has an empty key");
				}
				if (isNull(expected)) {
					return false;
				}
				return inspectProperty(
						key, [&](const PropertyValue &actual) { return !isNull(actual) && actual == expected; });
			};
			for (const auto &predicate: scan.getPropertyPredicates()) {
				if (!equals(predicate.first, predicate.second)) {
					return false;
				}
			}
			for (const auto &predicate: parameterPredicates) {
				if (!equals(predicate.first, predicate.second)) {
					return false;
				}
			}
			for (const logical::RangePredicate &range: scan.getRangePredicates()) {
				checkGuard(state);
				if (range.key.empty()) {
					throw std::invalid_argument("logical-plan Node range predicate has an empty key");
				}
				const bool matches = inspectProperty(range.key, [&](const PropertyValue &actual) {
					return !isNull(actual) &&
						   (isNull(range.minValue) ||
							!(range.minInclusive ? actual < range.minValue : actual <= range.minValue)) &&
						   (isNull(range.maxValue) ||
							!(range.maxInclusive ? actual > range.maxValue : actual >= range.maxValue));
				});
				if (!matches)
					return false;
			}
			if (const auto &composite = scan.getCompositeEquality(); composite.has_value()) {
				if (composite->keys.empty() || composite->keys.size() != composite->values.size()) {
					throw std::invalid_argument("logical-plan Node composite predicate has an invalid shape");
				}
				for (size_t index = 0U; index < composite->keys.size(); ++index) {
					if (!equals(composite->keys[index], composite->values[index])) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] const PropertyMap &readBindingProperties(storage::LogicalGraphReadSnapshot &snapshot,
															   LogicalPlanReadRow &row, const std::string &variable,
															   ExecutionState &state) {
			const LogicalPlanReadValue &binding = requireBinding(row, variable);
			const bool isEntityReference = std::holds_alternative<storage::EntityRef>(binding);
			if (const auto cached = row.property_cache.find(variable);
				cached != row.property_cache.end() &&
				(!isEntityReference || row.complete_entity_property_cache.contains(variable))) {
				return cached->second;
			}
			if (const PropertyValue *value = std::get_if<PropertyValue>(&binding)) {
				if (value->getType() != PropertyType::MAP) {
					throw std::runtime_error(
							"logical-plan entity Property function requires a node, relationship, or Map");
				}
				return detail::procedureMapEmplace(row.property_cache, variable, detail::copyPropertyMapGuarded(value->getMap(), [&] {
					if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
				})).first->second;
			}
			if (const storage::LogicalGraphNode *node = std::get_if<storage::LogicalGraphNode>(&binding)) {
				return detail::procedureMapEmplace(row.property_cache, variable, readNodeProperties(snapshot, *node, state)).first->second;
			}
			if (const storage::LogicalGraphEdge *edge = std::get_if<storage::LogicalGraphEdge>(&binding)) {
				return detail::procedureMapEmplace(row.property_cache, variable, readEdgeProperties(snapshot, *edge, state)).first->second;
			}
			const storage::EntityRef &reference = std::get<storage::EntityRef>(binding);
			validateEntityRef(reference);
			PropertyMap complete = reference.is_edge
										   ? readEdgeProperties(snapshot, materializeEdgeValue(state, binding), state)
										   : readNodeProperties(snapshot, materializeNodeValue(state, binding), state);
			auto [cached, inserted] = detail::procedureMapInsertOrAssign(row.property_cache, variable, std::move(complete));
			static_cast<void>(inserted);
			detail::procedureSetInsert(row.complete_entity_property_cache, variable);
			return cached->second;
		}

		[[nodiscard]] size_t retainedExpressionValueBytes(const PropertyValue &value, const ExecutionState &state) {
			return detail::retainedPropertyValueBytes(value, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
		}

		// List comprehensions, reducers, and quantifiers bind short-lived scalar
		// values. Restoring both the value and its Property cache prevents a nested
		// expression from corrupting an outer row binding with the same name.
		class ScopedRowBinding final {
		public:
			template<typename Value>
			ScopedRowBinding(LogicalPlanReadRow &row, std::string_view variable, const Value &value,
							 ExecutionState &state) : row_(row), binding_bytes_(&state.shared.materialized_budget) {
				if (variable.empty()) {
					throw std::invalid_argument("logical-plan temporary variable name cannot be empty");
				}
				size_t bytes = checkedSizeAdd(variable.size(), sizeof(LogicalPlanReadValue),
											  "logical-plan scoped binding size overflowed");
				bytes = checkedSizeAdd(bytes, bindingPayloadBytes(value, state),
									   "logical-plan scoped binding size overflowed");
				binding_bytes_.set(bytes);
				variable_.assign(variable);
				if (const auto previous = row_.values.find(variable_); previous != row_.values.end()) {
					bytes = checkedSizeAdd(bytes, logicalReadValueBytes(previous->second, state.shared.context),
										   "logical-plan scoped binding size overflowed");
					bytes = checkedSizeAdd(bytes, sizeof(LogicalPlanReadValue),
										   "logical-plan scoped binding size overflowed");
				}
				if (const auto previous = row_.property_cache.find(variable_); previous != row_.property_cache.end()) {
					bytes = checkedSizeAdd(
							bytes, propertyMapBytes(previous->second, "logical-plan scoped binding size overflowed", state.shared.context),
							"logical-plan scoped binding size overflowed");
				}
				if (const auto previous = row_.entity_metadata_cache.find(variable_);
					previous != row_.entity_metadata_cache.end()) {
					bytes = checkedSizeAdd(
							bytes, propertyMapBytes(previous->second, "logical-plan scoped binding size overflowed", state.shared.context),
							"logical-plan scoped binding size overflowed");
				}
				binding_bytes_.set(bytes);
				if (const auto previous = row_.values.find(variable_); previous != row_.values.end()) {
					previous_value_ = previous->second;
				}
				if (const auto previous = row_.property_cache.find(variable_); previous != row_.property_cache.end()) {
					previous_properties_ = previous->second;
					previous_properties_complete_ = row_.complete_entity_property_cache.contains(variable_);
				}
				if (const auto previous = row_.entity_metadata_cache.find(variable_);
					previous != row_.entity_metadata_cache.end()) {
					previous_metadata_ = previous->second;
				}
				row_.values.insert_or_assign(variable_, LogicalPlanReadValue(value));
				row_.property_cache.erase(variable_);
				row_.complete_entity_property_cache.erase(variable_);
				row_.entity_metadata_cache.erase(variable_);
			}

			~ScopedRowBinding() {
				if (previous_value_.has_value()) {
					row_.values.insert_or_assign(variable_, std::move(*previous_value_));
				} else {
					row_.values.erase(variable_);
				}
				if (previous_properties_.has_value()) {
					row_.property_cache.insert_or_assign(variable_, std::move(*previous_properties_));
					if (previous_properties_complete_.value_or(false)) {
						row_.complete_entity_property_cache.insert(variable_);
					} else {
						row_.complete_entity_property_cache.erase(variable_);
					}
				} else {
					row_.property_cache.erase(variable_);
					row_.complete_entity_property_cache.erase(variable_);
				}
				if (previous_metadata_.has_value()) {
					row_.entity_metadata_cache.insert_or_assign(variable_, std::move(*previous_metadata_));
				} else {
					row_.entity_metadata_cache.erase(variable_);
				}
			}

			ScopedRowBinding(const ScopedRowBinding &) = delete;
			ScopedRowBinding &operator=(const ScopedRowBinding &) = delete;

		private:
			[[nodiscard]] static size_t bindingPayloadBytes(const PropertyValue &value, const ExecutionState &state) {
				return retainedExpressionValueBytes(value, state);
			}
			[[nodiscard]] static size_t bindingPayloadBytes(const storage::LogicalGraphNode &value, const ExecutionState &state) {
				return logicalNodeCacheBytes(value, state.shared.context);
			}
			[[nodiscard]] static size_t bindingPayloadBytes(const storage::LogicalGraphEdge &value, const ExecutionState &state) {
				return logicalEdgeCacheBytes(value, state.shared.context);
			}

			LogicalPlanReadRow &row_;
			MaterializedByteReservation binding_bytes_;
			std::string variable_;
			std::optional<LogicalPlanReadValue> previous_value_;
			std::optional<PropertyMap> previous_properties_;
			std::optional<bool> previous_properties_complete_;
			std::optional<std::unordered_map<std::string, PropertyValue>> previous_metadata_;
		};

		[[nodiscard]] const std::vector<PropertyValue> &
		requireBoundedList(const PropertyValue &value, const ExecutionState &state, const char *operation) {
			if (value.getType() != PropertyType::LIST) {
				throw std::runtime_error(std::string("logical-plan ") + operation + " requires a List value");
			}
			const std::vector<PropertyValue> &list = value.getList();
			if (list.size() > state.shared.bounds.maximum_rows) {
				throw std::runtime_error(std::string("logical-plan ") + operation +
										 " exceeded the configured List bound");
			}
			return list;
		}

		[[nodiscard]] size_t materializedListStorageBytes(const PropertyValue &value, const ExecutionState &state) {
			const auto estimates = detail::propertyBufferEstimates(value, [&] {
				if (state.shared.context != nullptr) state.shared.context->checkGuardNow();
			});
			const size_t logicalBytes = checkedSizeAdd(estimates.logicalBytes,
								  checkedSizeMul(value.getList().capacity(), sizeof(PropertyValue),
												 "logical-plan List capacity overflowed"),
								  "logical-plan List storage size overflowed");
			return std::max(logicalBytes, estimates.heapBytes);
		}

		[[nodiscard]] PropertyValue
		evaluateScalarExpression(const expressions::Expression &expression, LogicalPlanReadRow &row,
								 ExecutionState &state,
								 ShortestPathResultMode shortestMode = ShortestPathResultMode::SPR_FULL);

		using PatternMatchVisitor =
				std::function<bool(const storage::LogicalGraphEdge &, const storage::LogicalGraphNode &)>;

		// Evaluate the common topology-only EXISTS shape without materializing an
		// Edge or target Node.  The native reference page is already authenticated
		// against the pinned snapshot; endpoint probes retain the fail-closed
		// invariant while keeping the hot path independent of Property pages.
