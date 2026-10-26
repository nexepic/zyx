		[[nodiscard]] storage::LogicalGraphMutationBatchResult executeMutationWithReadControl(
			storage::LogicalGraphWriteBackend &backend, const storage::LogicalGraphMutationBatch &batch,
			std::optional<storage::LogicalGraphMutationFence> fence, const LogicalPlanReadRuntimeBounds &bounds,
			const QueryContext &context) {
			storage::LogicalGraphMutationReadControl control;
			if (context.guard) control.checkpoint = [&context] { context.checkGuardNow(); };
			control.property_admission = [&context, maximumBytes = bounds.maximum_materialized_bytes](size_t bytes) {
				context.checkGuardNow();
				if (bytes > maximumBytes) throw std::runtime_error("logical mutation constraint Property source exceeded its byte bound");
			};
			return backend.executeLogicalGraphMutationBatchWithReadControl(batch, fence, control);
		}

		using storage::LogicalGraphBatchEdgeCreateRequest;
		using storage::LogicalGraphMutationBatch;
		using storage::LogicalGraphMutationEntityKind;
		using storage::LogicalGraphMutationEntityReference;
		using storage::LogicalGraphMutationReferenceKind;
		using storage::LogicalGraphNodeCreateRequest;
		using storage::LogicalGraphPropertyMutationRequest;
		using storage::LogicalGraphWriteProperty;
		using PropertyMap = std::unordered_map<std::string, PropertyValue>;

		void assignBoundedProperty(PropertyMap &properties, const std::string &key, const PropertyValue &value, size_t maximumEntries) {
			if (!properties.contains(key) && properties.size() >= maximumEntries)
				throw std::runtime_error("logical SET exceeded its Property bound");
			properties.insert_or_assign(key, value);
		}

		bool mergePropertyMap(PropertyMap &properties, const PropertyValue &value, const QueryContext &context, size_t maximumEntries) {
			if (value.getType() != PropertyType::MAP) throw std::logic_error("logical SET map merge requires a map value");
			size_t finalEntries = properties.size();
			for (const auto &[key, field] : value.getMap()) {
				context.checkGuard();
				if (key.empty()) throw std::logic_error("logical SET map merge has an empty Property key");
				if (field.getType() == PropertyType::NULL_TYPE && properties.contains(key)) --finalEntries;
			}
			for (const auto &[key, field] : value.getMap()) {
				context.checkGuard();
				if (field.getType() != PropertyType::NULL_TYPE && !properties.contains(key)) {
					if (finalEntries >= maximumEntries) throw std::runtime_error("logical SET map merge exceeded its Property bound");
					++finalEntries;
				}
			}
			if (finalEntries > maximumEntries) throw std::runtime_error("logical SET map merge exceeded its Property bound");
			bool changed = false;
			// Remove first so a within-bound map never needs extra Property slots.
			for (const auto &[key, field] : value.getMap()) {
				context.checkGuard();
				if (field.getType() == PropertyType::NULL_TYPE) changed = properties.erase(key) != 0U || changed;
			}
			for (const auto &[key, field] : value.getMap()) {
				context.checkGuard();
				if (field.getType() == PropertyType::NULL_TYPE) continue;
				const auto found = properties.find(key);
				if (found == properties.end() || found->second != field) {
					assignBoundedProperty(properties, key, field, maximumEntries); changed = true;
				}
			}
			return changed;
		}

		bool replacePropertyMap(PropertyMap &properties, const PropertyValue &value, const QueryContext &context, size_t maximumEntries) {
			if (value.getType() != PropertyType::MAP)
				throw std::logic_error("logical SET map replacement requires a map value");
			size_t entries = 0U;
			for (const auto &[key, field] : value.getMap()) {
				context.checkGuard();
				if (key.empty()) throw std::logic_error("logical SET map replacement has an empty Property key");
				if (field.getType() != PropertyType::NULL_TYPE) {
					if (entries == maximumEntries) throw std::runtime_error("logical SET map replacement exceeded its Property bound");
					++entries;
				}
			}
			PropertyMap replacement;
			replacement.reserve(entries);
			for (const auto &[key, field] : value.getMap()) {
				context.checkGuard();
				if (key.empty()) throw std::logic_error("logical SET map replacement has an empty Property key");
				if (field.getType() != PropertyType::NULL_TYPE) replacement.emplace(key, field);
			}
			if (replacement == properties) return false;
			properties = std::move(replacement);
			return true;
		}

		[[nodiscard]] storage::EntityReadShape completeNodeReadShape() {
			return {.include_labels = true, .include_type = false, .all_properties = true};
		}

		[[nodiscard]] storage::EntityReadShape completeEdgeReadShape() {
			return {.include_labels = false, .include_type = true, .all_properties = true};
		}

		[[nodiscard]] LogicalGraphMutationEntityReference createdNodeReference(size_t ordinal) {
			return {LogicalGraphMutationEntityKind::LGMK_NODE, LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE,
					static_cast<uint64_t>(ordinal)};
		}

		[[nodiscard]] std::vector<LogicalGraphWriteProperty>
		toLogicalProperties(const std::unordered_map<std::string, PropertyValue> &properties, size_t maximumEntries) {
			if (properties.size() > maximumEntries)
				throw std::runtime_error("logical CREATE literal Properties exceeded their entry bound");
			std::vector<std::pair<std::string, PropertyValue>> ordered;
			ordered.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				ordered.emplace_back(key, value);
			}
			std::ranges::sort(ordered, {}, &std::pair<std::string, PropertyValue>::first);
			std::vector<LogicalGraphWriteProperty> result;
			result.reserve(ordered.size());
			for (const auto &[key, value]: ordered) {
				result.push_back({key, value});
			}
			return result;
		}

		[[nodiscard]] const logical::LogicalOperator *requireOptionalOnlyChild(const logical::LogicalOperator &op) {
			const std::vector<logical::LogicalOperator *> children = op.getChildren();
			if (children.empty()) {
				return nullptr;
			}
			if (children.size() != 1U || children.front() == nullptr) {
				throw std::runtime_error("logical mutation runtime requires unary CREATE operators");
			}
			return children.front();
		}

		struct CreateSetMutationChain {
			std::vector<const logical::LogicalOperator *> operations;
			const logical::LogicalOperator *read_root = nullptr;
		};

		struct ReadPropertyMutationChain {
			std::vector<const logical::LogicalOperator *> operations;
			const logical::LogicalOperator *read_root = nullptr;
		};

		void validateSetOperationShape(const logical::LogicalSet &set, const char *context) {
			if (set.getItems().empty()) {
				throw std::logic_error(std::string(context) + " has an empty SET operator");
			}
			for (const logical::LogicalSetItem &item: set.getItems()) {
				switch (item.type) {
					case logical::SetActionType::LSET_PROPERTY:
						if (item.variable.empty() || item.key.empty() || !item.expression) {
							throw std::logic_error(std::string(context) + " has an invalid SET Property assignment");
						}
						break;
					case logical::SetActionType::LSET_LABEL:
						if (item.variable.empty() || item.key.empty() || item.expression) {
							throw std::logic_error(std::string(context) + " has an invalid SET label assignment");
						}
						break;
					case logical::SetActionType::LSET_MAP_REPLACE:
					case logical::SetActionType::LSET_MAP_MERGE:
						if (item.variable.empty() || !item.expression) {
							throw std::logic_error(std::string(context) + " has an invalid SET map-merge assignment");
						}
						break;
					default:
						throw std::logic_error(std::string(context) + " has an unsupported SET assignment type");
				}
			}
		}

		void validateRemoveOperationShape(const logical::LogicalRemove &remove, const char *context) {
			if (remove.getItems().empty()) {
				throw std::logic_error(std::string(context) + " has an empty REMOVE operator");
			}
			for (const logical::LogicalRemoveItem &item: remove.getItems()) {
				if (item.variable.empty() || item.key.empty()) {
					throw std::logic_error(std::string(context) + " has an invalid REMOVE item");
				}
				switch (item.type) {
					case logical::LogicalRemoveActionType::LREM_PROPERTY:
					case logical::LogicalRemoveActionType::LREM_LABEL:
						break;
					default:
						throw std::logic_error(std::string(context) + " has an unsupported REMOVE item type");
				}
			}
		}

		[[nodiscard]] CreateSetMutationChain collectForeachMutationChain(const logical::LogicalForeach &foreach) {
			if (foreach.getIterVar().empty() || !foreach.getListExpr() || foreach.getBody() == nullptr) {
				throw std::logic_error(
						"logical FOREACH requires an iteration variable, list expression, and update body");
			}
			CreateSetMutationChain chain;
			const logical::LogicalOperator *tail = foreach.getBody();
			for (;;) {
				const logical::LogicalOpType type = tail->getType();
				if (type != logical::LogicalOpType::LOP_CREATE_NODE &&
					type != logical::LogicalOpType::LOP_CREATE_EDGE && type != logical::LogicalOpType::LOP_SET &&
					type != logical::LogicalOpType::LOP_REMOVE && type != logical::LogicalOpType::LOP_DELETE) {
					throw std::logic_error("logical FOREACH supports only CREATE Node/relationship, SET, REMOVE, and "
										   "DELETE update chains");
				}
				if (type == logical::LogicalOpType::LOP_SET) {
					const auto *set = dynamic_cast<const logical::LogicalSet *>(tail);
					if (set == nullptr) {
						throw std::logic_error("logical FOREACH SET body is malformed");
					}
					validateSetOperationShape(*set, "logical FOREACH SET body");
				}
				if (type == logical::LogicalOpType::LOP_REMOVE) {
					const auto *remove = dynamic_cast<const logical::LogicalRemove *>(tail);
					if (remove == nullptr) {
						throw std::logic_error("logical FOREACH REMOVE body is malformed");
					}
					validateRemoveOperationShape(*remove, "logical FOREACH REMOVE body");
				}
				chain.operations.push_back(tail);
				const std::vector<logical::LogicalOperator *> children = tail->getChildren();
				if (children.empty()) {
					break;
				}
				if (children.size() != 1U || children.front() == nullptr) {
					throw std::logic_error("logical FOREACH update body must be a unary operator chain");
				}
				if (children.front()->getType() == logical::LogicalOpType::LOP_SINGLE_ROW) {
					if (!children.front()->getChildren().empty()) {
						throw std::logic_error("logical FOREACH update body has a malformed SingleRow seed");
					}
					break;
				}
				tail = children.front();
			}
			if (chain.operations.empty()) { // a non-null FOREACH body reaches the loop only after validation and is appended before every possible break.
				throw std::logic_error("logical FOREACH update body cannot be empty");
			}
			std::ranges::reverse(chain.operations);
			chain.read_root = foreach.getInput();
			return chain;
		}

		[[nodiscard]] std::optional<CreateSetMutationChain>
		collectCreateSetMutationChain(const logical::LogicalOperator &root, bool requireCreate = true) {
			CreateSetMutationChain chain;
			const logical::LogicalOperator *current = &root;
			bool sawCreate = false;
			while (current != nullptr && (current->getType() == logical::LogicalOpType::LOP_CREATE_NODE ||
										  current->getType() == logical::LogicalOpType::LOP_CREATE_EDGE ||
										  current->getType() == logical::LogicalOpType::LOP_SET ||
										  current->getType() == logical::LogicalOpType::LOP_REMOVE ||
										  current->getType() == logical::LogicalOpType::LOP_DELETE)) {
				switch (current->getType()) {
					case logical::LogicalOpType::LOP_CREATE_NODE:
						// Preserve the public unary-plan error before checking the
						// implementation-specific concrete type.
						static_cast<void>(requireOptionalOnlyChild(*current));
						if (dynamic_cast<const logical::LogicalCreateNode *>(current) == nullptr) {
							throw std::logic_error("logical CREATE/SET chain has an invalid CreateNode operator");
						}
						break;
					case logical::LogicalOpType::LOP_CREATE_EDGE:
						static_cast<void>(requireOptionalOnlyChild(*current));
						if (dynamic_cast<const logical::LogicalCreateEdge *>(current) == nullptr) {
							throw std::logic_error("logical CREATE/SET chain has an invalid CreateEdge operator");
						}
						break;
					case logical::LogicalOpType::LOP_SET: {
						const auto *set = dynamic_cast<const logical::LogicalSet *>(current);
						if (set == nullptr) {
							throw std::logic_error("logical CREATE/SET chain has an invalid SET operator");
						}
						break;
					}
					case logical::LogicalOpType::LOP_REMOVE: {
						const auto *remove = dynamic_cast<const logical::LogicalRemove *>(current);
						if (remove == nullptr) {
							throw std::logic_error("logical CREATE/SET chain has an invalid REMOVE operator");
						}
						break;
					}
					case logical::LogicalOpType::LOP_DELETE: {
						const auto *deleteOperator = dynamic_cast<const logical::LogicalDelete *>(current);
						if (deleteOperator == nullptr || deleteOperator->getVariables().empty()) {
							throw std::logic_error("logical CREATE/SET chain has an invalid Delete operator");
						}
						break;
					}
					default:
						throw std::logic_error("logical CREATE/SET chain has an unsupported mutation operator");
				}
				chain.operations.push_back(current);
				sawCreate = sawCreate || current->getType() == logical::LogicalOpType::LOP_CREATE_NODE ||
							current->getType() == logical::LogicalOpType::LOP_CREATE_EDGE;
				current = requireOptionalOnlyChild(*current);
			}
			if ((requireCreate && !sawCreate) || chain.operations.empty()) {
				return std::nullopt;
			}
			for (const logical::LogicalOperator *operation: chain.operations) {
				if (operation->getType() == logical::LogicalOpType::LOP_SET) {
					validateSetOperationShape(*dynamic_cast<const logical::LogicalSet *>(operation),
											  "logical CREATE/SET chain");
				} else if (operation->getType() == logical::LogicalOpType::LOP_REMOVE) {
					validateRemoveOperationShape(*dynamic_cast<const logical::LogicalRemove *>(operation),
												 "logical CREATE/SET chain");
				}
			}
			std::ranges::reverse(chain.operations);
			chain.read_root = current;
			return chain;
		}

		[[nodiscard]] std::optional<ReadPropertyMutationChain>
		collectReadPropertyMutationChain(const logical::LogicalOperator &root) {
			ReadPropertyMutationChain chain;
			const logical::LogicalOperator *current = &root;
			while (current != nullptr && (current->getType() == logical::LogicalOpType::LOP_SET ||
										  current->getType() == logical::LogicalOpType::LOP_REMOVE)) {
				if (current->getType() == logical::LogicalOpType::LOP_SET) {
					const auto *set = dynamic_cast<const logical::LogicalSet *>(current);
					if (set == nullptr) {
						throw std::logic_error("logical SET/REMOVE chain has an invalid SET operator");
					}
					validateSetOperationShape(*set, "logical SET/REMOVE chain");
				} else {
					const auto *remove = dynamic_cast<const logical::LogicalRemove *>(current);
					if (remove == nullptr) {
						throw std::logic_error("logical SET/REMOVE chain has an invalid REMOVE operator");
					}
					validateRemoveOperationShape(*remove, "logical SET/REMOVE chain");
				}
				chain.operations.push_back(current);
				current = requireOptionalOnlyChild(*current);
			}
			// SET and REMOVE have the same snapshot, staging, retry, and result
			// semantics whether a statement contains one update or a chain. Keeping
			// them on one path prevents the two implementations from drifting.
			if (chain.operations.empty() || current == nullptr) {
				return std::nullopt;
			}
			// MERGE owns its post-match SET/REMOVE chain so it can stage a single
			// atomic create-or-update batch. It is not a read-only chain input.
			if (current->getType() == logical::LogicalOpType::LOP_MERGE_NODE ||
				current->getType() == logical::LogicalOpType::LOP_MERGE_EDGE) {
				return std::nullopt;
			}
			std::ranges::reverse(chain.operations);
			chain.read_root = current;
			return chain;
		}

		// A write-only CALL body is planned with its own SingleRow seed. Rebind that
		// seed to the outer pipeline so the existing bounded mutation runtime can stage
		// every outer row and publish one atomic batch. General CALL semantics stay
		// outside this deliberately narrow mutation path.
		struct ReboundWriteOnlyCall {
			std::unique_ptr<logical::LogicalOperator> mutation_root;
			std::unique_ptr<logical::LogicalProject> outer_projection;
			std::unique_ptr<logical::LogicalProject> exported_projection;
			const logical::LogicalSort *sort = nullptr;
			const logical::LogicalSkip *skip = nullptr;
			const logical::LogicalLimit *limit = nullptr;
		};

		[[nodiscard]] std::optional<ReboundWriteOnlyCall>
		rebindWriteOnlyCallSubquery(const logical::LogicalOperator &root) {
			const logical::LogicalProject *outerProjection = nullptr;
			const logical::LogicalOperator *candidate = &root;
			const logical::LogicalSort *sort = nullptr;
			const logical::LogicalSkip *skip = nullptr;
			const logical::LogicalLimit *limit = nullptr;
			// The planner places result operators outside a mutation as
			// LIMIT -> SKIP -> PROJECT -> SORT -> CALL. They must be kept out of
			// the write chain so pagination cannot reduce the rows being mutated.
			if (candidate->getType() == logical::LogicalOpType::LOP_LIMIT) {
				limit = dynamic_cast<const logical::LogicalLimit *>(candidate);
				if (limit == nullptr) {
					throw std::logic_error("logical write-only CALL has an invalid Limit operator");
				}
				candidate = requireOptionalOnlyChild(*candidate);
				if (candidate == nullptr) {
					throw std::logic_error("logical write-only CALL Limit requires a child");
				}
			}
			if (candidate->getType() == logical::LogicalOpType::LOP_SKIP) {
				skip = dynamic_cast<const logical::LogicalSkip *>(candidate);
				if (skip == nullptr) {
					throw std::logic_error("logical write-only CALL has an invalid Skip operator");
				}
				candidate = requireOptionalOnlyChild(*candidate);
				if (candidate == nullptr) {
					throw std::logic_error("logical write-only CALL Skip requires a child");
				}
			}
			if (candidate->getType() == logical::LogicalOpType::LOP_PROJECT) {
				outerProjection = dynamic_cast<const logical::LogicalProject *>(candidate);
				if (outerProjection == nullptr) {
					throw std::logic_error("logical write-only CALL has an invalid Project operator");
				}
				candidate = requireOptionalOnlyChild(*candidate);
				if (candidate == nullptr) {
					throw std::logic_error("logical write-only CALL projection requires a child");
				}
			}
			if (candidate->getType() == logical::LogicalOpType::LOP_SORT) {
				sort = dynamic_cast<const logical::LogicalSort *>(candidate);
				if (sort == nullptr) {
					throw std::logic_error("logical write-only CALL has an invalid Sort operator");
				}
				candidate = requireOptionalOnlyChild(*candidate);
				if (candidate == nullptr) {
					throw std::logic_error("logical write-only CALL Sort requires a child");
				}
			}
			if (candidate->getType() != logical::LogicalOpType::LOP_CALL_SUBQUERY) {
				return std::nullopt;
			}

			const auto *call = dynamic_cast<const logical::LogicalCallSubquery *>(candidate);
			if (call == nullptr) {
				throw std::logic_error("logical write-only CALL has an invalid CallSubquery operator");
			}
			// Batched CALL has its own execution path below; leave it available for
			// collectInTransactionsWriteOnlyCall instead of rejecting it here.
			if (call->isInTransactions()) {
				return std::nullopt;
			}
			if (call->getBatchSize() != 0) {
				throw std::runtime_error(
						"logical mutation runtime does not support CALL subqueries with transaction batching");
			}
			if (call->getSubquery() == nullptr) {
				throw std::logic_error("logical write-only CALL requires an update body");
			}

			std::unique_ptr<logical::LogicalOperator> reboundBody = call->getSubquery()->clone();
			std::unique_ptr<logical::LogicalProject> exportedProjection;
			if (!call->getReturnedVars().empty()) {
				if (dynamic_cast<logical::LogicalProject *>(reboundBody.get()) == nullptr) {
					throw std::runtime_error("logical write-only CALL has an invalid exported-variable projection");
				}
				exportedProjection = std::unique_ptr<logical::LogicalProject>(
						static_cast<logical::LogicalProject *>(reboundBody.release()));
				if (exportedProjection->getOutputVariables() != call->getReturnedVars()) {
					throw std::runtime_error(
							"logical write-only CALL exported-variable projection has invalid columns");
				}
				reboundBody = exportedProjection->detachChild(0U);
				if (reboundBody == nullptr) {
					throw std::runtime_error(
							"logical write-only CALL exported-variable projection requires a mutation child");
				}
			}
			logical::LogicalOperator *current = reboundBody.get();
			bool sawImportProjection = false;
			for (;;) {
				if (current == nullptr) {
					throw std::logic_error("logical write-only CALL update body contains a null operator");
				}
				switch (current->getType()) {
					case logical::LogicalOpType::LOP_CREATE_NODE:
					case logical::LogicalOpType::LOP_CREATE_EDGE:
					case logical::LogicalOpType::LOP_DELETE:
						break;
					case logical::LogicalOpType::LOP_SET:
					case logical::LogicalOpType::LOP_REMOVE:
						break;
					case logical::LogicalOpType::LOP_PROJECT:
						// WITH preserves the subquery's explicit import boundary while the
						// replaced seed supplies one outer row at a time.
						sawImportProjection = true;
						break;
					default:
						throw std::logic_error("logical write-only CALL supports only CREATE, SET, REMOVE, and DELETE "
											   "update chains");
				}

				const std::vector<logical::LogicalOperator *> children = current->getChildren();
				if (children.size() != 1U || children.front() == nullptr) {
					throw std::logic_error(
							"logical write-only CALL update body must be a unary chain ending in SingleRow");
				}
				if (children.front()->getType() == logical::LogicalOpType::LOP_SINGLE_ROW) {
					if (!children.front()->getChildren().empty()) {
						throw std::logic_error("logical write-only CALL update body has a malformed SingleRow seed");
					}
					current->setChild(0U, call->getInput() != nullptr ? call->getInput()->clone()
																	  : std::make_unique<logical::LogicalSingleRow>());
					break;
				}
				current = children.front();
			}
			if (!call->getImportedVars().empty() && !sawImportProjection) {
				throw std::runtime_error("logical write-only CALL imports require a WITH projection");
			}
			if (outerProjection == nullptr) {
				return ReboundWriteOnlyCall{
						std::move(reboundBody), nullptr, std::move(exportedProjection), sort, skip, limit};
			}
			std::unique_ptr<logical::LogicalProject> reboundOuter(
					dynamic_cast<logical::LogicalProject *>(outerProjection->clone().release()));
			if (reboundOuter == nullptr) {
				throw std::logic_error("logical write-only CALL has an invalid outer projection");
			}
			if (exportedProjection != nullptr) {
				return ReboundWriteOnlyCall{std::move(reboundBody),
											std::move(reboundOuter),
											std::move(exportedProjection),
											sort,
											skip,
											limit};
			}
			return ReboundWriteOnlyCall{std::move(reboundBody), std::move(reboundOuter), nullptr, sort, skip, limit};
		}

		struct InTransactionsWriteOnlyCall {
			const logical::LogicalCallSubquery *call = nullptr;
			const logical::LogicalProject *outer_projection = nullptr;
			const logical::LogicalProject *exported_projection = nullptr;
			CreateSetMutationChain mutation_chain;
		};

		[[nodiscard]] std::optional<InTransactionsWriteOnlyCall>
		collectInTransactionsWriteOnlyCall(const logical::LogicalOperator &root) {
			const logical::LogicalProject *outerProjection = nullptr;
			const logical::LogicalOperator *candidate = &root;
			if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
				outerProjection = dynamic_cast<const logical::LogicalProject *>(&root);
				if (outerProjection == nullptr) {
					throw std::logic_error("logical IN TRANSACTIONS CALL has an invalid Project operator");
				}
				candidate = requireOptionalOnlyChild(root);
				if (candidate == nullptr) {
					throw std::logic_error("logical IN TRANSACTIONS CALL projection requires a child");
				}
			}
			if (candidate->getType() != logical::LogicalOpType::LOP_CALL_SUBQUERY) {
				return std::nullopt;
			}
			const auto *call = dynamic_cast<const logical::LogicalCallSubquery *>(candidate);
			if (call == nullptr) {
				throw std::logic_error("logical IN TRANSACTIONS CALL has an invalid CallSubquery operator");
			}
			if (!call->isInTransactions()) {
				return std::nullopt;
			}
			if (call->getSubquery() == nullptr) {
				throw std::runtime_error("logical IN TRANSACTIONS CALL requires a write-only subquery body");
			}
			const logical::LogicalOperator *body = call->getSubquery();
			const logical::LogicalProject *exportedProjection = nullptr;
			if (!call->getReturnedVars().empty()) {
				exportedProjection = dynamic_cast<const logical::LogicalProject *>(body);
				if (exportedProjection == nullptr || exportedProjection->isDistinct() ||
					exportedProjection->getOutputVariables() != call->getReturnedVars()) {
					throw std::runtime_error(
							"logical IN TRANSACTIONS CALL has an invalid exported-variable projection");
				}
				const std::vector<logical::LogicalOperator *> children = exportedProjection->getChildren();
				if (children.size() != 1U || children.front() == nullptr) {
					throw std::runtime_error(
							"logical IN TRANSACTIONS CALL exported-variable projection requires a mutation child");
				}
				body = children.front();
				if (body == nullptr) {
					throw std::runtime_error(
							"logical IN TRANSACTIONS CALL exported-variable projection requires a mutation child");
				}
			}
			const std::optional<CreateSetMutationChain> chain = collectCreateSetMutationChain(*body, false);
			if (!chain.has_value()) {
				throw std::runtime_error(
						"logical IN TRANSACTIONS CALL requires a supported unary mutation chain");
			}
			return InTransactionsWriteOnlyCall{call, outerProjection, exportedProjection, *chain};
		}

		[[nodiscard]] PropertyMap
		propertyMapFromLogicalProperties(const std::vector<LogicalGraphWriteProperty> &properties,
										 const char *operation) {
			PropertyMap result;
			result.reserve(properties.size());
			for (const LogicalGraphWriteProperty &property: properties) {
				if (property.key.empty() || !result.emplace(property.key, property.value).second) {
					throw std::logic_error(std::string(operation) + " has an invalid complete Property collection");
				}
			}
			return result;
		}

		[[nodiscard]] std::vector<LogicalGraphWriteProperty> sortedProperties(const PropertyMap &properties) {
			std::vector<LogicalGraphWriteProperty> result;
			result.reserve(properties.size());
			for (const auto &[key, value]: properties) {
				if (key.empty()) {
					throw std::logic_error("logical mutation has an empty Property key");
				}
				result.push_back({key, value});
			}
			std::ranges::sort(result, {}, &LogicalGraphWriteProperty::key);
			return result;
		}

		[[nodiscard]] PropertyMap readInlineProperties(const std::vector<storage::LogicalGraphPropertyEntry> &entries,
													   size_t maximumEntries) {
			if (entries.size() > maximumEntries) {
				throw std::runtime_error("logical mutation projection read exceeded its direct Property bound");
			}
			PropertyMap result;
			result.reserve(entries.size());
			for (const storage::LogicalGraphPropertyEntry &entry: entries) {
				if (entry.key.empty() || !result.emplace(entry.key, entry.value).second) {
					throw std::runtime_error("logical mutation projection read an invalid direct Property entry set");
				}
			}
			return result;
		}

		[[nodiscard]] PropertyMap readNodeProperties(const storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphNode &node, size_t maximumEntries) {
			static_cast<void>(snapshot);
			return readInlineProperties(node.properties, maximumEntries);
		}

		[[nodiscard]] PropertyMap readEdgeProperties(const storage::LogicalGraphReadSnapshot &snapshot,
													 const storage::LogicalGraphEdge &edge, size_t maximumEntries) {
			static_cast<void>(snapshot);
			return readInlineProperties(edge.properties, maximumEntries);
		}

		[[nodiscard]] PropertyValue
		evaluateCreatePropertyExpression(const expressions::Expression &expression, const QueryContext &context,
										 const LogicalPlanReadRow &row, storage::LogicalGraphReadSnapshot &snapshot,
										 const LogicalPlanReadRuntimeBounds &bounds,
										 std::unordered_map<std::string, PropertyMap> &propertyCache) {
			LogicalPlanReadRow evaluationRow = row;
			evaluationRow.property_cache = propertyCache;
			const LogicalPlanReadValue value = LogicalPlanReadRuntime::evaluateExpressionWithSnapshot(
					snapshot, expression, evaluationRow, bounds, context);
			propertyCache = std::move(evaluationRow.property_cache);
			if (const auto *property = std::get_if<PropertyValue>(&value)) {
				return *property;
			}
			throw std::logic_error("logical mutation Property assignment must evaluate to a Property value");
		}

		void requireCompleteReadForMutation(const LogicalPlanReadRuntimeStats &stats,
											const LogicalPlanReadRuntimeBounds &bounds, const char *operation) {
			if (stats.rows_emitted == bounds.maximum_rows || stats.nodes_scanned == bounds.maximum_node_records ||
				stats.edges_scanned == bounds.maximum_edges_per_node) {
				throw std::runtime_error(std::string(operation) +
										 " reached a read bound before proving its input was exhausted");
			}
		}

		struct MutableGraphEntity {
			LogicalGraphMutationEntityKind kind = LogicalGraphMutationEntityKind::LGMK_NODE;
			uint64_t id = 0U;
			std::vector<storage::LogicalGraphPropertyEntry> direct_properties;
			std::optional<PropertyMap> properties;
			std::vector<std::string> labels;
			bool properties_changed = false;
			bool labels_changed = false;
		};

		using MutableGraphEntities = std::map<std::pair<uint8_t, uint64_t>, MutableGraphEntity>;

		[[nodiscard]] storage::LogicalGraphPropertyPatchRequest finalPropertyPatch(
			const MutableGraphEntity &entity, const QueryContext &context, size_t maximumScratchBytes);

		[[nodiscard]] MutableGraphEntity &mutableEntityForValue(MutableGraphEntities &entities,
																const LogicalPlanReadValue &value) {
			if (const auto *node = std::get_if<storage::LogicalGraphNode>(&value)) {
				const auto key =
						std::pair{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_NODE), node->node_id};
				auto [iterator, inserted] = entities.try_emplace(key);
				if (inserted) {
					iterator->second.kind = LogicalGraphMutationEntityKind::LGMK_NODE;
					iterator->second.id = node->node_id;
					iterator->second.direct_properties = node->properties;
					iterator->second.labels = node->labels;
				}
				return iterator->second;
			}
			if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value)) {
				const auto key =
						std::pair{static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_EDGE), edge->edge_id};
				auto [iterator, inserted] = entities.try_emplace(key);
				if (inserted) {
					iterator->second.kind = LogicalGraphMutationEntityKind::LGMK_EDGE;
					iterator->second.id = edge->edge_id;
					iterator->second.direct_properties = edge->properties;
				}
				return iterator->second;
			}
			throw std::logic_error("logical mutation requires Node or Edge bindings");
		}

		[[nodiscard]] PropertyMap &mutableEntityProperties(MutableGraphEntity &entity,
														   const storage::LogicalGraphReadSnapshot &snapshot,
														   const LogicalPlanReadRuntimeBounds &bounds) {
			if (!entity.properties.has_value()) {
				static_cast<void>(snapshot);
				entity.properties = readInlineProperties(entity.direct_properties, bounds.maximum_property_entries);
			}
			return *entity.properties;
		}

		void synchronizeEntityPropertyCache(const LogicalPlanReadRow &row, const MutableGraphEntity &entity,
											const PropertyMap &properties,
											std::unordered_map<std::string, PropertyMap> &propertyCache) {
			for (const auto &[variable, value]: row.values) {
				const auto *node = std::get_if<storage::LogicalGraphNode>(&value);
				const auto *edge = std::get_if<storage::LogicalGraphEdge>(&value);
				const bool matchesNode = node != nullptr && entity.kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
										 node->node_id == entity.id;
				const bool matchesEdge = edge != nullptr && entity.kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
										 edge->edge_id == entity.id;
				if (matchesNode || matchesEdge) {
					propertyCache.insert_or_assign(variable, properties);
				}
			}
		}

		void applySetOperation(const logical::LogicalSet &setOperator, const LogicalPlanReadRow &row,
								   storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
								   const QueryContext &context, MutableGraphEntities &entities,
								   std::unordered_map<std::string, PropertyMap> &propertyCache) {
			// Keep direct callers on the same fail-closed shape contract as every
			// collected mutation chain before resolving a runtime binding.
			validateSetOperationShape(setOperator, "logical SET");
			for (const logical::LogicalSetItem &item: setOperator.getItems()) {
				const auto binding = row.values.find(item.variable);
				if (binding == row.values.end()) {
					throw std::logic_error("logical SET could not resolve a requested entity variable");
				}
				MutableGraphEntity &entity = mutableEntityForValue(entities, binding->second);
				switch (item.type) {
					case logical::SetActionType::LSET_PROPERTY: {
						PropertyMap &properties = mutableEntityProperties(entity, snapshot, bounds);
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						const PropertyValue value = evaluateCreatePropertyExpression(*item.expression, context, row,
																					 snapshot, bounds, propertyCache);
						if (value.getType() == PropertyType::NULL_TYPE) {
							if (properties.erase(item.key) != 0U) {
								entity.properties_changed = true;
							}
						} else {
							const auto existing = properties.find(item.key);
							if (existing == properties.end() || existing->second != value) {
								assignBoundedProperty(properties, item.key, value, bounds.maximum_property_entries);
								entity.properties_changed = true;
							}
						}
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						break;
					}
					case logical::SetActionType::LSET_LABEL:
						if (entity.kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
							throw std::logic_error("logical SET label assignment requires a Node and nonempty label");
						}
						if (std::ranges::find(entity.labels, item.key) == entity.labels.end()) {
							entity.labels.push_back(item.key);
							entity.labels_changed = true;
						}
						break;
					case logical::SetActionType::LSET_MAP_REPLACE: {
						PropertyMap &properties = mutableEntityProperties(entity, snapshot, bounds);
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						const PropertyValue value = evaluateCreatePropertyExpression(*item.expression, context, row, snapshot, bounds, propertyCache);
						entity.properties_changed = replacePropertyMap(properties, value, context, bounds.maximum_property_entries) || entity.properties_changed;
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						break;
					}
					case logical::SetActionType::LSET_MAP_MERGE: {
						PropertyMap &properties = mutableEntityProperties(entity, snapshot, bounds);
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						const PropertyValue value = evaluateCreatePropertyExpression(*item.expression, context, row,
																					 snapshot, bounds, propertyCache);
						entity.properties_changed = mergePropertyMap(properties, value, context, bounds.maximum_property_entries) || entity.properties_changed;
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						break;
					}
				}
			}
		}

		void applyRemoveOperation(const logical::LogicalRemove &removeOperator, const LogicalPlanReadRow &row,
									  storage::LogicalGraphReadSnapshot &snapshot,
									  const LogicalPlanReadRuntimeBounds &bounds, MutableGraphEntities &entities,
									  std::unordered_map<std::string, PropertyMap> &propertyCache) {
			validateRemoveOperationShape(removeOperator, "logical REMOVE");
			for (const logical::LogicalRemoveItem &item: removeOperator.getItems()) {
				const auto binding = row.values.find(item.variable);
				if (binding == row.values.end()) {
					throw std::logic_error("logical REMOVE could not resolve a valid entity item");
				}
				MutableGraphEntity &entity = mutableEntityForValue(entities, binding->second);
				switch (item.type) {
					case logical::LogicalRemoveActionType::LREM_PROPERTY: {
						PropertyMap &properties = mutableEntityProperties(entity, snapshot, bounds);
						if (properties.erase(item.key) != 0U) {
							entity.properties_changed = true;
						}
						synchronizeEntityPropertyCache(row, entity, properties, propertyCache);
						break;
					}
					case logical::LogicalRemoveActionType::LREM_LABEL: {
						if (entity.kind != LogicalGraphMutationEntityKind::LGMK_NODE) {
							throw std::logic_error("logical REMOVE label assignment requires a Node");
						}
						const auto label = std::ranges::find(entity.labels, item.key);
						if (label != entity.labels.end()) {
							entity.labels.erase(label);
							entity.labels_changed = true;
						}
						break;
					}
				}
			}
		}

		struct CreateSetRuntimeRow {
			LogicalPlanReadRow source;
			std::unordered_map<std::string, LogicalGraphMutationEntityReference> entities;
			// A CREATE followed by DELETE cancels the provisional entity before
			// publication, so an implicit result cannot expose a durable binding.
			std::set<std::string> unavailable_implicit_entities;
		};

		// Scalar result expressions that cannot observe a newly assigned entity ID
		// are evaluated once before publication and reused afterwards. Expressions
		// that reference a created entity still validate before publication, then
		// run after publication to observe the real backend ID.
		struct CreateSetPrepublicationResultCache {
			std::vector<std::vector<std::optional<PropertyValue>>> sort_keys;
			std::vector<std::vector<std::optional<ResultValue>>> projection_values;
		};

		using MutationProjectionResolver = std::function<ResultValue(const LogicalPlanReadRow &, const std::string &)>;

		[[nodiscard]] std::vector<std::string> visibleMutationOutputColumns(const logical::LogicalOperator &root) {
			std::vector<std::string> columns;
			std::set<std::string> seen;
			for (const std::string &column: root.getOutputVariables()) {
				// Match the physical executor: anonymous pattern bindings are never
				// exposed to callers, but named pass-through bindings are result rows.
				if (!column.empty() && !isAnonymousVariable(column) && seen.insert(column).second) {
					columns.push_back(column);
				}
			}
			return columns;
		}

		template<typename Rows, typename Resolver>
		[[nodiscard]] QueryResult buildImplicitMutationResult(const std::vector<std::string> &columns,
															  const Rows &rows, const QueryContext &context,
															  Resolver &&resolveBinding) {
			QueryResult result = context.resource_account ? QueryResult(detail::ResourceLease(context.resource_account)) : QueryResult{};
			result.setColumns(columns);
			// Mutation aggregates consume pipeline rows even when this statement has
			// no client-visible columns (for example, an anonymous CREATE).
			result.setPipelineRowCount(rows.size());
			if (columns.empty()) {
				return result;
			}
			for (const auto &source: rows) {
				QueryResult::Row output;
				output.reserve(columns.size());
				for (const std::string &column: columns) {
					context.checkGuard();
					if (!output.emplace(column, resolveBinding(source, column)).second) {
						throw std::logic_error("logical mutation produced duplicate output columns");
					}
				}
				result.addRow(std::move(output));
			}
			return result;
		}

		[[nodiscard]] ResultValue evaluateMutationProjectionExpression(
				const expressions::Expression &expression, const LogicalPlanReadRow &source,
				storage::LogicalGraphReadSnapshot &snapshot, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context, const MutationProjectionResolver &resolveBinding);

		[[nodiscard]] QueryResult buildCreateSetResult(
				const logical::LogicalProject *projection, const std::vector<std::string> &implicitColumns,
				const std::vector<CreateSetRuntimeRow> &rows, storage::LogicalGraphReadSnapshot &snapshot,
				const LogicalGraphMutationBatch &batch, const storage::LogicalGraphMutationBatchResult &result,
				const MutableGraphEntities &persistedEntities, const LogicalPlanReadRuntimeBounds &bounds,
				const QueryContext &context, const logical::LogicalProject *exportedProjection = nullptr,
				CreateSetPrepublicationResultCache *prepublicationCache = nullptr,
				bool cachePrepublicationValues = false, const logical::LogicalSort *resultSort = nullptr,
				const logical::LogicalSkip *resultSkip = nullptr, const logical::LogicalLimit *resultLimit = nullptr);

