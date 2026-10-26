		void prefetchProcedureNodeMetadata(ProcedureRows &rows,
				const std::vector<const expressions::Expression *> &expressions,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context) {
            detail::ProcedurePayloadCredit sourcePayloadCredit;
            detail::ProcedurePayloadAdmissionScope sourcePayloadScope(sourcePayloadCredit);

			for (const auto &request: LogicalPlanReadRuntime::collectBatchedMetadataRequests(expressions)) {
				if (request.function != "labels") continue;
				MaterializedByteBudget budget(bounds.maximum_materialized_bytes);
				MaterializedByteReservation rowBytes(&budget);
				for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
				MaterializedByteReservation idBytes(&budget);
				const auto uncachedReference = [&](const BufferedProcedureRow &buffered) -> const storage::EntityRef * {
					const auto found = buffered.row.values.find(request.variable);
					if (found == buffered.row.values.end()) return nullptr;
					const auto *reference = std::get_if<storage::EntityRef>(&found->second);
					if (reference == nullptr || reference->is_edge) return nullptr;
					if (reference->entity_id == 0U) throw std::runtime_error("logical procedure metadata batch received a zero Node ID");
					const auto cached = buffered.row.entity_metadata_cache.find(request.variable);
					return cached != buffered.row.entity_metadata_cache.end() && cached->second.contains("labels") ? nullptr : reference;
				};
				size_t referenceCount = 0U;
				for (const auto &row: rows) {
					context.checkGuard();
					const auto *reference = uncachedReference(row);
					if (reference != nullptr) referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
				}
				idBytes.set(checkedProcedureSizeMul(referenceCount, sizeof(uint64_t)));
				utils::OwnedStdVector<uint64_t> ids;
				ids.reserve(referenceCount);
				idBytes.set(checkedProcedureSizeMul(ids.capacity(), sizeof(uint64_t)));
				for (const auto &row: rows) {
					context.checkGuard();
					const auto *reference = uncachedReference(row);
					if (reference == nullptr) continue;
					ids.push_back(reference->entity_id);
				}
				std::ranges::sort(ids);
				ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
				if (ids.size() > bounds.maximum_node_records) throw std::runtime_error("logical procedure metadata batch exceeded its Node bound");
				struct MetadataColumnPressure {};
				const auto processOnce = [&](size_t offset, size_t count) {
					context.checkGuard();
					const auto batchIds = std::span<const uint64_t>(ids).subspan(offset, count);
					MaterializedByteReservation consumerBytes(&budget);
					const size_t countBytes = checkedProcedureSizeMul(count, sizeof(size_t));
					if (consumerBytes.wouldExceed(countBytes)) throw MetadataColumnPressure{};
					consumerBytes.set(countBytes);
					utils::OwnedStdVector<size_t> consumers(count, 0U);
					const auto rowIndex = [&](const BufferedProcedureRow &row) -> std::optional<size_t> {
						const auto *reference = uncachedReference(row);
						if (reference == nullptr) return std::nullopt;
						const auto found = std::ranges::lower_bound(batchIds, reference->entity_id);
						return found == batchIds.end() || *found != reference->entity_id ? std::nullopt
							: std::optional<size_t>(static_cast<size_t>(found - batchIds.begin()));
					};
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					MaterializedByteReservation sourceBytes(&budget);
					auto nodes = snapshot.readNodesShapedSorted(batchIds, {.include_labels = true});
					context.checkGuardNow();
					if (nodes.size() != count) throw std::runtime_error("logical procedure backend changed metadata batch cardinality");
					size_t bytes = 0U;
					for (size_t index = 0U; index < count; ++index) {
						context.checkGuard();
						validateNode(nodes[index]);
						if (nodes[index].node_id != batchIds[index]) throw std::runtime_error("logical procedure metadata batch changed Node order");
						bytes = checkedProcedureSizeAdd(bytes, bufferedProcedureNodeRowBytes(nodes[index], false) -
							sizeof(BufferedProcedureRow) - 4U - sizeof(LogicalPlanReadValue) + sizeof(storage::LogicalGraphNode));
					}
					if (sourceBytes.wouldExceed(bytes)) throw MetadataColumnPressure{};
					sourceBytes.set(bytes);
					const auto labelPayload = [](const storage::LogicalGraphNode &node) {
						size_t payload = 0U;
						for (const auto &label: node.labels) payload = checkedProcedureSizeAdd(payload, label.size());
						return payload;
					};
					const auto cacheBytes = [&](const BufferedProcedureRow &row, const storage::LogicalGraphNode &node, size_t payload) {
						const auto metadata = row.row.entity_metadata_cache.find(request.variable);
						const size_t alias = metadata == row.row.entity_metadata_cache.end() ? request.variable.size() : 0U;
						return checkedProcedureSizeAdd(alias + 6U + sizeof(uint32_t),
							checkedProcedureSizeAdd(payload, checkedProcedureSizeMul(node.labels.size(), sizeof(size_t))));
					};
					// Preflight every publication peak before adding any cache entry.
					size_t plannedRows = rowBytes.bytes();
					size_t plannedSource = bytes;
					const size_t otherBytes = budget.used() - plannedRows - plannedSource;
					for (const auto &row: rows) {
						context.checkGuard();
						const auto index = rowIndex(row);
						if (!index) continue;
						const auto &node = nodes[*index];
						const size_t payload = labelPayload(node);
						if (--consumers[*index] == 0U) plannedSource -= payload;
						plannedRows = checkedProcedureSizeAdd(plannedRows, cacheBytes(row, node, payload));
						if (checkedProcedureSizeAdd(plannedRows, plannedSource) > bounds.maximum_materialized_bytes - otherBytes) throw MetadataColumnPressure{};
					}
					std::fill(consumers.begin(), consumers.end(), 0U);
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					for (auto &row: rows) {
                detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
						context.checkGuard();
						const auto index = rowIndex(row);
						if (!index) continue;
						auto &node = nodes[*index];
						const bool last = --consumers[*index] == 0U;
						const size_t payload = labelPayload(node);
						if (last) sourceBytes.set(sourceBytes.bytes() - payload);
						admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), cacheBytes(row, node, payload)));
						std::vector<PropertyValue> labels;
						detail::procedureVectorReserve(labels, node.labels.size());
						for (auto &label: node.labels) {
							detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(label.size()));
                            if (last) labels.emplace_back(std::move(label));
                            else labels.emplace_back(label);
						}
						row.row.entity_metadata_cache[request.variable].insert_or_assign("labels", makeProcedurePropertyValue(std::move(labels)));
					}
					validateBufferedProcedureBytes(rows, bounds.maximum_materialized_bytes);
				};
				std::function<void(size_t, size_t)> processRange;
				processRange = [&](size_t offset, size_t count) {
					try { processOnce(offset, count); }
					catch (const MetadataColumnPressure &) {
						if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure metadata batch exceeded its joint byte bound");
						const size_t first = count / 2U;
						processRange(offset, first);
						processRange(offset + first, count - first);
					}
				};
				const size_t capacity = procedureNodeBatchCapacity(bounds);
				for (size_t offset = 0U; offset < ids.size(); offset += capacity) processRange(offset, std::min(capacity, ids.size() - offset));
			}
		}

		void prefetchProcedureCompleteNodes(ProcedureRows &rows,
				const std::vector<const expressions::Expression *> &expressions,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context) {
            detail::ProcedurePayloadCredit sourcePayloadCredit;
            detail::ProcedurePayloadAdmissionScope sourcePayloadScope(sourcePayloadCredit);

			const auto variables = LogicalPlanReadRuntime::collectBatchedCompleteEntityVariables(expressions);
			for (const std::string &variable: variables) {
				MaterializedByteBudget budget(bounds.maximum_materialized_bytes);
				MaterializedByteReservation rowBytes(&budget);
				for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
				MaterializedByteReservation idBytes(&budget);
				size_t referenceCount = 0U;
				for (const BufferedProcedureRow &buffered: rows) {
					context.checkGuard();
					const auto found = buffered.row.values.find(variable);
					if (found == buffered.row.values.end() || buffered.row.complete_entity_property_cache.contains(variable)) continue;
					const auto *reference = std::get_if<storage::EntityRef>(&found->second);
					if (reference == nullptr || reference->is_edge) continue;
					if (reference->entity_id == 0U) throw std::runtime_error("logical procedure complete batch received a zero Node ID");
					referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
				}
				idBytes.set(checkedProcedureSizeMul(referenceCount, sizeof(uint64_t)));
				utils::OwnedStdVector<uint64_t> ids;
				ids.reserve(referenceCount);
				idBytes.set(checkedProcedureSizeMul(ids.capacity(), sizeof(uint64_t)));
				for (const BufferedProcedureRow &buffered: rows) {
					context.checkGuard();
					const auto found = buffered.row.values.find(variable);
					if (found == buffered.row.values.end()) continue;
					const auto *reference = std::get_if<storage::EntityRef>(&found->second);
					if (reference == nullptr || reference->is_edge || buffered.row.complete_entity_property_cache.contains(variable)) continue;
					if (reference->entity_id == 0U) throw std::runtime_error("logical procedure complete batch received a zero Node ID");
					ids.push_back(reference->entity_id);
				}
				std::ranges::sort(ids);
				ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
				if (ids.size() > bounds.maximum_node_records) {
					throw std::runtime_error("logical procedure complete batch exceeded its Node bound");
				}
				struct CompleteColumnPressure {};
				const auto processOnce = [&](size_t offset, size_t count) {
					context.checkGuard();
					const auto batchIds = std::span<const uint64_t>(ids).subspan(offset, count);
					MaterializedByteReservation consumerBytes(&budget);
					const size_t countBytes = checkedProcedureSizeMul(count, sizeof(size_t));
					if (consumerBytes.wouldExceed(countBytes)) throw CompleteColumnPressure{};
					consumerBytes.set(countBytes);
					utils::OwnedStdVector<size_t> consumers(count, 0U);
					const auto rowIndex = [&](const BufferedProcedureRow &row) -> std::optional<size_t> {
						const auto found = row.row.values.find(variable);
						if (found == row.row.values.end() || row.row.complete_entity_property_cache.contains(variable)) return std::nullopt;
						const auto *reference = std::get_if<storage::EntityRef>(&found->second);
						if (reference == nullptr || reference->is_edge) return std::nullopt;
						const auto position = std::ranges::lower_bound(batchIds, reference->entity_id);
						return position == batchIds.end() || *position != reference->entity_id ? std::nullopt
							: std::optional<size_t>(static_cast<size_t>(position - batchIds.begin()));
					};
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					MaterializedByteReservation sourceBytes(&budget);
					const bool admitSource = snapshot.supportsShapedPropertyReadAdmission();
					if (admitSource) {
						const size_t nodeFrames = checkedProcedureSizeMul(count, sizeof(storage::LogicalGraphNode));
						if (sourceBytes.wouldExceed(nodeFrames)) throw CompleteColumnPressure{};
						sourceBytes.set(nodeFrames);
					}
					const std::function<void(size_t)> admitProperty = [&](size_t encodedBytes) {
						context.checkGuard();
						const size_t next = checkedProcedureSizeAdd(sourceBytes.bytes(), encodedBytes);
						if (sourceBytes.wouldExceed(next)) throw CompleteColumnPressure{};
						sourceBytes.set(next);
					};
					auto nodes = admitSource
						? snapshot.readNodesShapedSortedWithAdmission(batchIds, completeNodeReadShape(), admitProperty)
						: snapshot.readNodesShapedSorted(batchIds, completeNodeReadShape());
					context.checkGuardNow();
					if (nodes.size() != count) throw std::runtime_error("logical procedure backend changed complete batch cardinality");
					size_t bytes = 0U;
					for (size_t index = 0U; index < count; ++index) {
						context.checkGuard();
						validateNode(nodes[index]);
						if (nodes[index].node_id != batchIds[index]) throw std::runtime_error("logical procedure complete batch changed Node order");
						if (nodes[index].properties.size() > bounds.maximum_property_entries) throw std::runtime_error("logical procedure complete batch exceeded its Property bound");
						std::unordered_set<std::string_view> keys;
						for (const auto &entry: nodes[index].properties) {
							if (entry.key.empty() || !keys.insert(entry.key).second) throw std::runtime_error("logical procedure complete batch has invalid Property keys");
						}
						// Count the temporary Node itself, not an imaginary output row.
						bytes = checkedProcedureSizeAdd(bytes, bufferedProcedureNodeRowBytes(nodes[index], false) -
							sizeof(BufferedProcedureRow) - 4U - sizeof(LogicalPlanReadValue) + sizeof(storage::LogicalGraphNode));
					}
					// Reconcile canonical entry scratch to the returned Node/label/key
					// retention after every returned-record semantic check.
					if (sourceBytes.wouldExceed(bytes)) throw CompleteColumnPressure{};
					sourceBytes.set(bytes);
					// Simulate every publication peak before mutating rows. Reuse the
					// consumer counters; no second payload or count array is needed.
					size_t plannedRows = rowBytes.bytes();
					size_t plannedSource = bytes;
					const size_t otherBytes = budget.used() - plannedRows - plannedSource;
					for (const auto &buffered: rows) {
						context.checkGuard();
						const auto index = rowIndex(buffered);
						if (!index) continue;
						const auto &node = nodes[*index];
						size_t propertyBytes = 0U;
						for (const auto &entry: node.properties) propertyBytes = checkedProcedureSizeAdd(propertyBytes,
							checkedProcedureSizeAdd(entry.key.size(), property_utils::getPropertyValueSize(entry.value)));
						size_t labelPayload = 0U;
						for (const auto &label: node.labels) labelPayload = checkedProcedureSizeAdd(labelPayload, label.size());
						if (--consumers[*index] == 0U) plannedSource -= propertyBytes + labelPayload;
						if (const auto old = buffered.row.property_cache.find(variable); old != buffered.row.property_cache.end()) {
							plannedRows -= variable.size();
							for (const auto &[key, value]: old->second) plannedRows -= key.size() + property_utils::getPropertyValueSize(value);
						}
						plannedRows = checkedProcedureSizeAdd(plannedRows, checkedProcedureSizeAdd(variable.size(), propertyBytes));
						const auto metadata = buffered.row.entity_metadata_cache.find(variable);
						if (metadata == buffered.row.entity_metadata_cache.end()) plannedRows = checkedProcedureSizeAdd(plannedRows, variable.size() + 6U);
						else if (const auto old = metadata->second.find("labels"); old != metadata->second.end()) plannedRows -= property_utils::getPropertyValueSize(old->second);
						else plannedRows = checkedProcedureSizeAdd(plannedRows, 6U);
						plannedRows = checkedProcedureSizeAdd(plannedRows, checkedProcedureSizeAdd(sizeof(uint32_t),
							checkedProcedureSizeAdd(labelPayload, checkedProcedureSizeMul(node.labels.size(), sizeof(size_t)))));
						if (checkedProcedureSizeAdd(plannedRows, plannedSource) > bounds.maximum_materialized_bytes - otherBytes) throw CompleteColumnPressure{};
					}
					std::fill(consumers.begin(), consumers.end(), 0U);
					for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
					for (auto &buffered: rows) {
                detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
						context.checkGuard();
						const auto index = rowIndex(buffered);
						if (!index) continue;
						auto &node = nodes[*index];
						const bool last = --consumers[*index] == 0U;
						// Drop selected/NULL sentinels before replacing with the complete
						// map; account copied bindings before publishing their payload.
						const size_t previous = bufferedProcedureRowBytes(buffered);
						buffered.row.property_cache.erase(variable);
						rowBytes.set(rowBytes.bytes() - previous + bufferedProcedureRowBytes(buffered));
						size_t propertyBytes = 0U;
						for (const auto &entry: node.properties) propertyBytes = checkedProcedureSizeAdd(propertyBytes,
							checkedProcedureSizeAdd(entry.key.size(), property_utils::getPropertyValueSize(entry.value)));
						size_t labelPayload = 0U;
						for (const auto &label: node.labels) labelPayload = checkedProcedureSizeAdd(labelPayload, label.size());
						if (last) sourceBytes.set(sourceBytes.bytes() - propertyBytes - labelPayload);
						size_t next = checkedProcedureSizeAdd(rowBytes.bytes(), checkedProcedureSizeAdd(variable.size(), propertyBytes));
						const auto metadata = buffered.row.entity_metadata_cache.find(variable);
						if (metadata == buffered.row.entity_metadata_cache.end()) next = checkedProcedureSizeAdd(next, variable.size() + 6U);
						else if (const auto existing = metadata->second.find("labels"); existing != metadata->second.end()) next -= property_utils::getPropertyValueSize(existing->second);
						else next = checkedProcedureSizeAdd(next, 6U);
						next = checkedProcedureSizeAdd(next, checkedProcedureSizeAdd(sizeof(uint32_t),
							checkedProcedureSizeAdd(labelPayload, checkedProcedureSizeMul(node.labels.size(), sizeof(size_t)))));
						admitProcedureBytes(rowBytes, next);
                        detail::ProcedurePayloadAdmissionScope::admit(detail::procedureEmptyMapHeapUpper<PropertyMap>());
						PropertyMap complete;
						for (auto &entry: node.properties) {
							if (last) detail::procedureMapEmplace(complete, std::move(entry.key), std::move(entry.value));
							else detail::procedureMapEmplace(complete, entry.key, entry.value);
						}
						detail::procedureMapInsertOrAssign(buffered.row.property_cache, variable, std::move(complete));
						detail::procedureSetInsert(buffered.row.complete_entity_property_cache, variable);
						std::vector<PropertyValue> labels;
						detail::procedureVectorReserve(labels, node.labels.size());
						for (auto &label: node.labels) {
							detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(label.size()));
                            if (last) labels.emplace_back(std::move(label));
                            else labels.emplace_back(label);
						}
						auto [metadataOwner, metadataInserted] = detail::procedureMapTryEmplace(buffered.row.entity_metadata_cache, variable);
                        static_cast<void>(metadataInserted);
                        detail::procedureMapInsertOrAssign(metadataOwner->second, "labels", makeProcedurePropertyValue(std::move(labels)));
					}
					validateBufferedProcedureBytes(rows, bounds.maximum_materialized_bytes);
				};
				std::function<void(size_t, size_t)> processRange;
				processRange = [&](size_t offset, size_t count) {
					try { processOnce(offset, count); }
					catch (const CompleteColumnPressure &) {
						if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure complete batch exceeded its joint byte bound");
						const size_t first = count / 2U;
						processRange(offset, first);
						processRange(offset + first, count - first);
					}
				};
				const size_t capacity = procedureNodeBatchCapacity(bounds);
				for (size_t offset = 0U; offset < ids.size(); offset += capacity) processRange(offset, std::min(capacity, ids.size() - offset));
			}
		}

		// Buffered procedures already allow read-ahead (notably ORDER BY). Use
		// the logical runtime's expression collector and selected-column contract
		// before evaluating each layer; unsupported expressions keep their fallback.
		void prefetchProcedureProperties(ProcedureRows &rows,
				const std::vector<const expressions::Expression *> &expressions,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context,
				const std::vector<LogicalPlanBatchedPropertyRequest> &additionalRequests = {}) {
            detail::ProcedurePayloadCredit sourcePayloadCredit;
            detail::ProcedurePayloadAdmissionScope sourcePayloadScope(sourcePayloadCredit);
			prefetchProcedureCompleteNodes(rows, expressions, snapshot, bounds, context);
			prefetchProcedureNodeMetadata(rows, expressions, snapshot, bounds, context);
			std::vector<LogicalPlanBatchedPropertyRequest> requests;
			static_cast<void>(LogicalPlanReadRuntime::collectBatchedPropertyRequests(expressions, requests));
			for (const auto &request: additionalRequests) {
				if (std::ranges::find(requests, request) == requests.end()) requests.push_back(request);
			}
			if (requests.empty()) return;
			struct Group {
				std::string variable;
				std::vector<std::string_view> keys;
			};
			std::vector<Group> groups;
			for (const auto &request: requests) {
				auto group = std::ranges::find(groups, request.variable, &Group::variable);
				if (group == groups.end()) {
					groups.push_back({request.variable, {request.key}});
				} else if (std::ranges::find(group->keys, request.key) == group->keys.end()) {
					group->keys.push_back(request.key);
				}
			}
			for (const Group &group: groups) {
				if (group.keys.size() > bounds.maximum_property_entries) {
					throw std::runtime_error("logical procedure Property batch exceeded its key bound");
				}
				for (const bool isEdge: {false, true}) {
					std::vector<std::string_view> keys;
					detail::procedureVectorReserve(keys, group.keys.size());
					for (const std::string_view key: group.keys) {
						bool foundConsumer = false;
						bool cachedByEveryConsumer = true;
						for (const BufferedProcedureRow &buffered: rows) {
							context.checkGuard();
							const auto found = buffered.row.values.find(group.variable);
							if (found == buffered.row.values.end() ||
									buffered.row.complete_entity_property_cache.contains(group.variable)) continue;
							const auto *reference = std::get_if<storage::EntityRef>(&found->second);
							if (reference == nullptr || reference->is_edge != isEdge) continue;
							if (reference->entity_id == 0U) {
								throw std::runtime_error("logical procedure Property batch received a zero Entity ID");
							}
							foundConsumer = true;
							const auto cache = buffered.row.property_cache.find(group.variable);
							if (cache == buffered.row.property_cache.end() || !cache->second.contains(std::string(key))) {
								cachedByEveryConsumer = false;
								break;
							}
						}
						if (!foundConsumer || !cachedByEveryConsumer) keys.push_back(key);
					}
					if (keys.empty()) continue;
					MaterializedByteBudget budget(bounds.maximum_materialized_bytes);
					MaterializedByteReservation rowBytes(&budget);
					for (const auto &row: rows) admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(rowBytes.bytes(), bufferedProcedureRowBytes(row)));
					MaterializedByteReservation keyBytes(&budget);
					admitProcedureBytes(keyBytes, checkedProcedureSizeMul(keys.capacity(), sizeof(std::string_view)));
					MaterializedByteReservation idBytes(&budget);
					const auto uncachedId = [&](const BufferedProcedureRow &buffered) -> std::optional<uint64_t> {
						context.checkGuard();
						const auto found = buffered.row.values.find(group.variable);
						if (found == buffered.row.values.end()) return std::nullopt;
						const auto *reference = std::get_if<storage::EntityRef>(&found->second);
						if (reference == nullptr || reference->is_edge != isEdge || buffered.row.complete_entity_property_cache.contains(group.variable)) return std::nullopt;
						if (reference->entity_id == 0U) {
							throw std::runtime_error("logical procedure Property batch received a zero Entity ID");
						}
						const auto cached = buffered.row.property_cache.find(group.variable);
						if (cached != buffered.row.property_cache.end() &&
							std::ranges::all_of(keys, [&](const std::string_view key) {
								return cached->second.contains(std::string(key));
							})) return std::nullopt;
						return reference->entity_id;
					};
					size_t referenceCount = 0U;
					for (const auto &buffered: rows) {
						if (uncachedId(buffered).has_value()) referenceCount = checkedProcedureSizeAdd(referenceCount, 1U);
					}
					admitProcedureBytes(idBytes, checkedProcedureSizeMul(referenceCount, sizeof(uint64_t)));
					utils::OwnedStdVector<uint64_t> ids;
					ids.reserve(referenceCount);
					admitProcedureBytes(idBytes, checkedProcedureSizeMul(ids.capacity(), sizeof(uint64_t)));
					for (const auto &buffered: rows) {
						if (const auto id = uncachedId(buffered)) ids.push_back(*id);
					}
					std::ranges::sort(ids);
					ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
					// Deduplication keeps the allocation alive at its original capacity.
					const size_t maximumIds = isEdge ? bounds.maximum_edge_records : bounds.maximum_node_records;
					if (ids.size() > maximumIds) {
						throw std::runtime_error("logical procedure Property batch exceeded its Entity bound");
					}
					struct ProcedureColumnBytePressure {};
					const auto processOnce = [&](size_t offset, size_t count) {
						context.checkGuard();
						const auto batch = std::span<const uint64_t>(ids).subspan(offset, count);
						MaterializedByteReservation consumerBytes(&budget);
						const size_t consumerSize = checkedProcedureSizeMul(count, sizeof(size_t));
						if (consumerBytes.wouldExceed(consumerSize)) throw ProcedureColumnBytePressure{};
						consumerBytes.set(consumerSize);
						utils::OwnedStdVector<size_t> consumers(count, 0U);
						const auto rowIndex = [&](const BufferedProcedureRow &buffered) -> std::optional<size_t> {
							const auto found = buffered.row.values.find(group.variable);
							if (found == buffered.row.values.end() || buffered.row.complete_entity_property_cache.contains(group.variable)) return std::nullopt;
							const auto *reference = std::get_if<storage::EntityRef>(&found->second);
							if (reference == nullptr || reference->is_edge != isEdge) return std::nullopt;
							const auto position = std::ranges::lower_bound(batch, reference->entity_id);
							return position == batch.end() || *position != reference->entity_id ? std::nullopt
								: std::optional<size_t>(static_cast<size_t>(position - batch.begin()));
						};
						for (const auto &row: rows) if (const auto index = rowIndex(row)) ++consumers[*index];
						MaterializedByteReservation columnBytes(&budget);
						const size_t structureBytes = checkedProcedureSizeAdd(
							checkedProcedureSizeMul(keys.size(), sizeof(std::vector<PropertyValue>)),
							checkedProcedureSizeMul(checkedProcedureSizeMul(keys.size(), count), sizeof(PropertyValue)));
						if (columnBytes.wouldExceed(structureBytes)) throw ProcedureColumnBytePressure{};
						columnBytes.set(structureBytes);
						const std::function<void(size_t)> admission = [&](size_t bytes) {
							context.checkGuard();
							const size_t next = checkedProcedureSizeAdd(columnBytes.bytes(), bytes);
							if (columnBytes.wouldExceed(next)) throw ProcedureColumnBytePressure{};
							columnBytes.set(next);
						};
						auto columns = snapshot.supportsPropertyReadAdmission()
							? (isEdge ? snapshot.readEdgePropertyColumnsSortedWithAdmission(batch, keys, admission)
								: snapshot.readNodePropertyColumnsSortedWithAdmission(batch, keys, admission))
							: (isEdge ? snapshot.readEdgePropertyColumnsSorted(batch, keys)
								: snapshot.readNodePropertyColumnsSorted(batch, keys));
						if (columns.size() != keys.size() || std::ranges::any_of(columns, [&](const auto &column) {
							return column.size() != count;
						})) throw std::runtime_error("logical procedure backend changed Property batch cardinality");
						size_t retainedBytes = structureBytes;
						for (const auto &column: columns) for (const auto &value: column) {
							retainedBytes = checkedProcedureSizeAdd(retainedBytes, property_utils::getPropertyValueSize(value));
						}
						if (columnBytes.wouldExceed(retainedBytes)) throw ProcedureColumnBytePressure{};
						columnBytes.set(retainedBytes);
						for (BufferedProcedureRow &buffered: rows) {
                detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
							context.checkGuard();
							const auto index = rowIndex(buffered);
							if (!index) continue;
							const bool lastConsumer = --consumers[*index] == 0U;
							for (size_t column = 0U; column < keys.size(); ++column) {
								auto &value = columns[column][*index];
								const size_t payload = property_utils::getPropertyValueSize(value);
								const auto cache = buffered.row.property_cache.find(group.variable);
								size_t next = rowBytes.bytes();
								if (cache == buffered.row.property_cache.end()) {
									next = checkedProcedureSizeAdd(next, group.variable.size());
									next = checkedProcedureSizeAdd(next, keys[column].size());
								} else {
									const auto previous = cache->second.find(std::string(keys[column]));
									if (previous == cache->second.end()) next = checkedProcedureSizeAdd(next, keys[column].size());
									else next -= property_utils::getPropertyValueSize(previous->second);
								}
								if (lastConsumer) columnBytes.set(columnBytes.bytes() - payload);
								admitProcedureBytes(rowBytes, checkedProcedureSizeAdd(next, payload));
								auto [cacheOwner, cacheInserted] = detail::procedureMapTryEmplace(buffered.row.property_cache, group.variable);
                                static_cast<void>(cacheInserted);
                                detail::ProcedurePayloadAdmissionScope::admit(utils::detail::standardFreshStringHeapUpper(keys[column].size()));
                                const std::string cacheKey(keys[column]);
                                if (lastConsumer) detail::procedureMapInsertOrAssign(cacheOwner->second, cacheKey, std::move(value));
                                else detail::procedureMapInsertOrAssign(cacheOwner->second, cacheKey, value);

							}
						}
						validateBufferedProcedureBytes(rows, bounds.maximum_materialized_bytes);
					};
					std::function<void(size_t, size_t)> processRange;
					processRange = [&](size_t offset, size_t count) {
						try { processOnce(offset, count); }
						catch (const ProcedureColumnBytePressure &) {
							if (count == 1U) throw ProcedureByteBoundExceeded("logical procedure Property columns exceeded their byte bound (retained rows=" +
								std::to_string(rowBytes.bytes()) + ", IDs=" + std::to_string(idBytes.bytes()) +
								", limit=" + std::to_string(bounds.maximum_materialized_bytes) + ")");
							const size_t first = count / 2U;
							processRange(offset, first);
							processRange(offset + first, count - first);
						}
					};
					const size_t capacity = procedureNodeBatchCapacity(bounds);
					for (size_t offset = 0U; offset < ids.size(); offset += capacity) {
						processRange(offset, std::min(capacity, ids.size() - offset));
					}
					}
				}

		}

		void appendBoundedProcedureRow(ProcedureRows &rows, BufferedProcedureRow row,
									  const size_t maximumRows, const std::string_view procedureName,
									  size_t *retainedBytes = nullptr,
									  const size_t maximumBytes = std::numeric_limits<size_t>::max()) {
			if (maximumRows == 0U) {
				throw std::invalid_argument("logical procedure runtime requires a positive result-row bound");
			}
			if (rows.size() == maximumRows) {
				throw std::runtime_error(std::string(procedureName) + " exceeded its configured result-row bound");
			}
			size_t nextRetainedBytes = 0U;
			if (retainedBytes != nullptr) {
				nextRetainedBytes = checkedProcedureSizeAdd(*retainedBytes, bufferedProcedureRowBytes(row));
				if (nextRetainedBytes > maximumBytes) {
					throw ProcedureByteBoundExceeded("logical procedure materialization exceeded its byte bound");
				}
			}
			rows.push_back(std::move(row));
			if (retainedBytes != nullptr) *retainedBytes = nextRetainedBytes;
		}

		[[nodiscard]] PropertyValue toStringListValue(const std::vector<std::string> &values) {
			std::vector<PropertyValue> result;
			detail::procedureVectorReserve(result, values.size());
			for (const std::string &value: values) {
				result.emplace_back(makeProcedurePropertyValue(value));
			}
			return makeProcedurePropertyValue(std::move(result));
		}

		[[nodiscard]] PropertyValue
		toRelationshipListValue(const std::vector<gds::LogicalRelationshipProjection> &relationships) {
			std::vector<PropertyValue> result;
			detail::procedureVectorReserve(result, relationships.size());
			for (const gds::LogicalRelationshipProjection &relationship: relationships) {
				detail::ProcedurePayloadAdmissionScope::admit(detail::procedureEmptyMapHeapUpper<PropertyValue::MapType>());
                PropertyValue::MapType entry;
				detail::procedureMapEmplace(entry, "type", makeProcedurePropertyValue(relationship.type));
				switch (relationship.orientation) {
					case gds::LogicalProjectionOrientation::LPO_NATURAL:
						detail::procedureMapEmplace(entry, "orientation", makeProcedurePropertyValue("NATURAL"));
						break;
					case gds::LogicalProjectionOrientation::LPO_REVERSE:
						detail::procedureMapEmplace(entry, "orientation", makeProcedurePropertyValue("REVERSE"));
						break;
					case gds::LogicalProjectionOrientation::LPO_UNDIRECTED:
						detail::procedureMapEmplace(entry, "orientation", makeProcedurePropertyValue("UNDIRECTED"));
						break;
				}
				// Relationship descriptors are public catalog data. Keep one canonical
				// map shape for every weight mode so bindings never have to infer a
				// field's existence from weightKind.
				switch (relationship.weight.kind) {
					case gds::LogicalProjectionWeightKind::LPWK_NONE:
						detail::procedureMapEmplace(entry, "weightKind", makeProcedurePropertyValue("NONE"));
						break;
					case gds::LogicalProjectionWeightKind::LPWK_CONSTANT:
						detail::procedureMapEmplace(entry, "weightKind", makeProcedurePropertyValue("CONSTANT"));
						break;
					case gds::LogicalProjectionWeightKind::LPWK_PROPERTY:
						detail::procedureMapEmplace(entry, "weightKind", makeProcedurePropertyValue("PROPERTY"));
						break;
				}
				detail::procedureMapEmplace(entry, "weightProperty", makeProcedurePropertyValue(relationship.weight.property_name));
				detail::procedureMapEmplace(entry, "constantWeight", makeProcedurePropertyValue(relationship.weight.constant_weight));
				detail::procedureMapEmplace(entry, "defaultWeight", makeProcedurePropertyValue(relationship.weight.default_weight));
				result.emplace_back(makeProcedurePropertyValue(std::move(entry)));
			}
			return makeProcedurePropertyValue(std::move(result));
		}

		[[nodiscard]] BufferedProcedureRow
		makeProjectionDescriptorRow(const gds::LogicalProjectionDescriptor &descriptor) {
			if (descriptor.node_count > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.edge_count > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.estimated_memory_bytes > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.source_revision > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
				descriptor.current_revision > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical projection descriptor cannot represent an output count");
			}
			BufferedProcedureRow result;
            detail::ProcedurePayloadAdmissionScope resultPayloadScope(result.allocation_credit);
			detail::procedureMapEmplace(result.row.values, "name", makeProcedurePropertyValue(descriptor.name));
			detail::procedureMapEmplace(result.row.values, "nodeCount", makeProcedurePropertyValue(static_cast<int64_t>(descriptor.node_count)));
			detail::procedureMapEmplace(result.row.values, "edgeCount", makeProcedurePropertyValue(static_cast<int64_t>(descriptor.edge_count)));
			detail::procedureMapEmplace(result.row.values, "isWeighted", makeProcedurePropertyValue(descriptor.weighted));
			detail::procedureMapEmplace(result.row.values, "createdAtMillis", makeProcedurePropertyValue(descriptor.created_at_epoch_millis));
			detail::procedureMapEmplace(result.row.values, "buildMillis", makeProcedurePropertyValue(descriptor.build_millis));
			detail::procedureMapEmplace(result.row.values, "memoryBytes",
									  makeProcedurePropertyValue(static_cast<int64_t>(descriptor.estimated_memory_bytes)));
			detail::procedureMapEmplace(result.row.values, "hasCsr", makeProcedurePropertyValue(descriptor.has_csr));
			detail::procedureMapEmplace(result.row.values, "csrMemoryBytes",
									  makeProcedurePropertyValue(static_cast<int64_t>(descriptor.csr_memory_bytes)));
			detail::procedureMapEmplace(result.row.values, "stale", makeProcedurePropertyValue(descriptor.stale));
			detail::procedureMapEmplace(result.row.values, "sourceRevision",
									  makeProcedurePropertyValue(static_cast<int64_t>(descriptor.source_revision)));
			detail::procedureMapEmplace(result.row.values, "currentRevision",
									  makeProcedurePropertyValue(static_cast<int64_t>(descriptor.current_revision)));
			detail::procedureMapEmplace(result.row.values, "nodeLabels", toStringListValue(descriptor.node_labels));
			detail::procedureMapEmplace(result.row.values, "relationships", toRelationshipListValue(descriptor.relationships));
			return result;
		}

		[[nodiscard]] QueryResult makeProcedureResult(ProcedureRows rows,
															  const std::vector<std::string> &columns,
															  const LogicalPlanReadRuntimeBounds &readBounds, const QueryContext &context) {
			if (rows.size() > readBounds.maximum_rows) {
				throw std::runtime_error("logical procedure exceeded its configured result-row bound");
			}
			validateBufferedProcedureBytes(rows, readBounds.maximum_materialized_bytes);
			QueryResult result = makeAdmittedProcedureResult(context);
			result.setColumns(columns);
			for (size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
				BufferedProcedureRow &buffered = rows[rowIndex];
            detail::ProcedurePayloadAdmissionScope bufferedPayloadScope(buffered.allocation_credit);
				context.checkGuard();
				const ProcedurePayloadReconcileGuard reconcile{buffered};
                detail::ProcedurePayloadAdmissionScope::admit(2U * detail::procedureEmptyMapHeapUpper<QueryResult::Row>());
                QueryResult::Row row;
                detail::procedureMapReserve(row, columns.size());
				for (const std::string &column: columns) {
					const auto found = buffered.row.values.find(column);
					if (found == buffered.row.values.end()) {
						throw std::logic_error("logical projection procedure did not bind its declared output column");
					}
					auto *scalar = std::get_if<PropertyValue>(&found->second);
					if (scalar == nullptr) {
						throw std::logic_error("logical projection procedure emitted a non-scalar catalog value");
					}
					detail::procedureAdmitMapNode(row, column);
                    row.emplace(column, std::move(*scalar));
				}
				appendProcedureResultRow(result, rows, rowIndex, std::move(row),
						readBounds.maximum_materialized_bytes);
			}
			ProcedureRows().swap(rows);
			validateProcedureResultBudget(result, 0U, readBounds.maximum_materialized_bytes);
			return result;
		}

		[[nodiscard]] ProcedureRows
		makeBufferedShortestPathRows(const std::vector<uint64_t> &path,
									 const LogicalPlanReadRuntimeBounds &bounds) {
			if (path.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical shortest-path procedure result cannot represent its row count");
			}
			ProcedureRows rows;
		const size_t retainedPathBytes = checkedProcedureSizeAdd(sizeof(path),
				checkedProcedureSizeMul(path.capacity(), sizeof(uint64_t)));
		const size_t outputByteAllowance = reserveBoundedProcedureRows(rows, path.size(), bounds, retainedPathBytes);
			size_t retainedBytes = 0U;
			for (size_t step = 0U; step < path.size(); ++step) {
				BufferedProcedureRow row;
            detail::ProcedurePayloadAdmissionScope rowPayloadScope(row.allocation_credit);
				detail::procedureMapReserve(row.node_properties, 1U);
				detail::procedureMapEmplace(row.row.values, "step", makeProcedurePropertyValue(static_cast<int64_t>(step)));
				detail::procedureMapEmplace(row.row.values, "node", storage::EntityRef{path[step], false, 0U, 0U, 0U});
				detail::procedureMapEmplace(row.row.values, "path_length", makeProcedurePropertyValue(static_cast<int64_t>(path.size())));
				appendBoundedProcedureRow(rows, std::move(row), bounds.maximum_rows, "algo.shortestPath",
						&retainedBytes, outputByteAllowance);
			}
			return rows;
		}

		void validateVectorSearchHits(const std::span<storage::LogicalGraphVectorSearchHit> hits,
										 const LogicalPlanReadRuntimeBounds &bounds, const QueryContext &context) {
			uint64_t previousNodeId = 0U;
			float previousScore = -std::numeric_limits<float>::infinity();
			for (const storage::LogicalGraphVectorSearchHit &hit: hits) {
				context.checkGuard();
				if (hit.node_id == 0U || !std::isfinite(hit.score) ||
					(hit.score < previousScore || (hit.score == previousScore && hit.node_id <= previousNodeId))) {
					throw std::runtime_error("logical Vector procedure backend returned unordered or invalid search hits");
				}
				previousNodeId = hit.node_id;
				previousScore = hit.score;
			}
			const auto idOrder = [&context](const auto &left, const auto &right) {
				context.checkGuard();
				return left.node_id < right.node_id;
			};
			std::ranges::sort(hits, idOrder);
			for (size_t index = 1U; index < hits.size(); ++index) {
				context.checkGuard();
				if (hits[index - 1U].node_id == hits[index].node_id) {
					throw std::runtime_error("logical Vector procedure backend returned unordered or invalid search hits");
				}
			}
			std::ranges::sort(hits, [&context](const auto &left, const auto &right) {
				context.checkGuard();
				return left.score < right.score || (left.score == right.score && left.node_id < right.node_id);
			});
		}

