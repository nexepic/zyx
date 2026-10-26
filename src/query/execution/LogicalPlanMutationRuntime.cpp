/**
 * @file LogicalPlanMutationRuntime.cpp
 * @brief Implementation of the relocatable logical-plan mutation compiler.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/query/execution/LogicalPlanMutationRuntime.hpp"
#include "BudgetMemoryResource.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"
#include "DeferredReferenceRowVisitor.hpp"
#include "graph/query/execution/BoundedBatch.hpp"
#include "graph/query/execution/GuardedRowCopy.hpp"
#include "graph/query/execution/StringBufferBytes.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph/core/Types.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryPlan.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionUtils.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalAggregate.hpp"
#include "graph/query/logical/operators/LogicalCallSubquery.hpp"
#include "graph/query/logical/operators/LogicalCreateEdge.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalForeach.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalMergeEdge.hpp"
#include "graph/query/logical/operators/LogicalMergeNode.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalRemove.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalSingleRow.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/logical/operators/LogicalSort.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"

namespace graph::query::execution {

	namespace {

#include "mutation_runtime/PlanningAndBindings.inc"
#include "mutation_runtime/CreateAndTransactionalCall.inc"
#include "mutation_runtime/ResultMaterialization.inc"
#include "mutation_runtime/MergeExecution.inc"
	} // namespace

	QueryResult LogicalPlanMutationRuntime::execute(const logical::LogicalOperator &root,
													const LogicalPlanReadRuntimeBounds &bounds,
													const QueryContext &context) const {
        // Match the read runtime: private snapshot/planning allocations belong
        // to this QUERY, while storage publication selects WRITE/CACHE itself.
        std::optional<utils::OwnedAllocationScope> queryAllocation;
        if (context.resource_account)
            queryAllocation.emplace(std::make_shared<MaterializedByteBudget>(
                bounds.maximum_materialized_bytes, context.resource_account, std::pmr::new_delete_resource()));

		if (bounds.maximum_rows == 0U) {
			throw std::invalid_argument("logical mutation runtime requires a positive mutation bound");
		}
		if (root.getType() == logical::LogicalOpType::LOP_AGGREGATE) {
			const auto *aggregate = dynamic_cast<const logical::LogicalAggregate *>(&root);
			if (aggregate == nullptr) {
				throw std::logic_error("logical mutation runtime received an invalid Aggregate operator");
			}
			const std::vector<logical::LogicalOperator *> children = aggregate->getChildren();
			const auto &items = aggregate->getAggregations();
			if (children.size() != 1U || children.front() == nullptr || !aggregate->getGroupByExprs().empty() ||
				items.size() != 1U || items.front().functionName != "count" || items.front().argument ||
				items.front().distinct || items.front().extraArg || items.front().alias.empty()) {
				throw std::logic_error(
						"logical mutation runtime currently requires an ungrouped count(*) mutation aggregate");
			}

			const logical::LogicalOperator &mutationRoot = *children.front();
			size_t inputRows = 0U;
			const logical::LogicalOperator *chainRoot = &mutationRoot;
			if (chainRoot->getType() == logical::LogicalOpType::LOP_PROJECT) {
				if (dynamic_cast<const logical::LogicalProject *>(chainRoot) == nullptr) {
					throw std::logic_error("logical mutation aggregate received an invalid Project operator");
				}
				chainRoot = requireOptionalOnlyChild(*chainRoot);
				if (chainRoot == nullptr) {
					throw std::logic_error("logical mutation aggregate Project has no mutation child");
				}
			}
			if (const std::optional<CreateSetMutationChain> chain = collectCreateSetMutationChain(*chainRoot);
				chain.has_value()) {
				QueryResult mutationResult = executeCreateSetMutationChain(*chain, read_backend_, write_backend_,
																		   bounds, context, nullptr, {});
				inputRows = mutationResult.getPipelineRowCount();
			} else {
				QueryResult mutationResult = execute(mutationRoot, bounds, context);
				inputRows = mutationResult.getPipelineRowCount();
			}
			if (inputRows > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
				throw std::overflow_error("logical mutation count(*) exceeds the Property integer range");
			}
			QueryResult result;
			result.setColumns({items.front().alias});
			QueryResult::Row row;
			row.emplace(items.front().alias, PropertyValue(static_cast<int64_t>(inputRows)));
			result.addRow(std::move(row));
			return result;
		}
		if (const std::optional<InTransactionsWriteOnlyCall> batchedCall = collectInTransactionsWriteOnlyCall(root);
			batchedCall.has_value()) {
			return executeInTransactionsWriteOnlyCall(*batchedCall, read_backend_, write_backend_,
													  permits_independent_batch_commits_, bounds, context);
		}
		if (std::optional<ReboundWriteOnlyCall> reboundCall = rebindWriteOnlyCallSubquery(root);
			reboundCall.has_value()) {
			if (reboundCall->exported_projection == nullptr) {
				const std::optional<CreateSetMutationChain> chain =
						collectCreateSetMutationChain(*reboundCall->mutation_root);
				if (chain.has_value()) {
					return executeCreateSetMutationChain(
							*chain, read_backend_, write_backend_, bounds, context, reboundCall->outer_projection.get(),
							visibleMutationOutputColumns(*reboundCall->mutation_root), nullptr, nullptr, nullptr,
							reboundCall->sort, reboundCall->skip, reboundCall->limit);
				}
				if (reboundCall->sort != nullptr || reboundCall->skip != nullptr || reboundCall->limit != nullptr) {
					throw std::runtime_error(
							"logical mutation runtime requires a CREATE-based write-only CALL result pipeline");
				}
				if (reboundCall->outer_projection != nullptr) {
					reboundCall->outer_projection->setChild(0U, std::move(reboundCall->mutation_root));
					return execute(*reboundCall->outer_projection, bounds, context);
				}
				return execute(*reboundCall->mutation_root, bounds, context);
			}
			const std::optional<CreateSetMutationChain> chain = collectCreateSetMutationChain(
					*reboundCall->mutation_root, false);
			if (!chain.has_value()) {
				throw std::runtime_error("logical mutation runtime received an invalid exported write-only CALL body");
			}
			const logical::LogicalProject *finalProjection = reboundCall->outer_projection != nullptr
																	 ? reboundCall->outer_projection.get()
																	 : reboundCall->exported_projection.get();
			return executeCreateSetMutationChain(
					*chain, read_backend_, write_backend_, bounds, context, finalProjection,
					visibleMutationOutputColumns(*reboundCall->mutation_root), nullptr, nullptr,
					reboundCall->exported_projection.get(), reboundCall->sort, reboundCall->skip, reboundCall->limit);
		}
		if (root.getType() == logical::LogicalOpType::LOP_FOREACH) {
			const auto *foreach = dynamic_cast<const logical::LogicalForeach *>(&root);
			if (foreach == nullptr) {
				throw std::logic_error("logical mutation runtime received an invalid Foreach operator");
			}
			return executeCreateSetMutationChain(collectForeachMutationChain(*foreach), read_backend_, write_backend_,
												 bounds, context, nullptr, visibleMutationOutputColumns(*foreach),
												 foreach);
		}
		const logical::LogicalProject *createSetProjection = nullptr;
		const logical::LogicalSort *createSetSort = nullptr;
		const logical::LogicalSkip *createSetSkip = nullptr;
		const logical::LogicalLimit *createSetLimit = nullptr;
		const logical::LogicalOperator *createSetRoot = &root;
		// Result operators must remain outside the mutation chain: they shape the
		// rows returned to the caller, never the rows staged for atomic publication.
		if (createSetRoot->getType() == logical::LogicalOpType::LOP_LIMIT) {
			createSetLimit = dynamic_cast<const logical::LogicalLimit *>(createSetRoot);
			if (createSetLimit == nullptr) {
				throw std::logic_error("logical CREATE/SET result has an invalid Limit operator");
			}
			createSetRoot = requireOptionalOnlyChild(*createSetRoot);
			if (createSetRoot == nullptr) {
				throw std::logic_error("logical CREATE/SET result Limit requires a child");
			}
		}
		if (createSetRoot->getType() == logical::LogicalOpType::LOP_SKIP) {
			createSetSkip = dynamic_cast<const logical::LogicalSkip *>(createSetRoot);
			if (createSetSkip == nullptr) {
				throw std::logic_error("logical CREATE/SET result has an invalid Skip operator");
			}
			createSetRoot = requireOptionalOnlyChild(*createSetRoot);
			if (createSetRoot == nullptr) {
				throw std::logic_error("logical CREATE/SET result Skip requires a child");
			}
		}
		if (createSetRoot->getType() == logical::LogicalOpType::LOP_PROJECT) {
			createSetProjection = dynamic_cast<const logical::LogicalProject *>(createSetRoot);
			if (createSetProjection == nullptr) {
				throw std::logic_error("logical CREATE/SET projection has an invalid Project operator");
			}
			createSetRoot = requireOptionalOnlyChild(*createSetRoot);
			if (createSetRoot == nullptr) {
				throw std::logic_error("logical CREATE/SET projection requires a mutation child");
			}
		}
		if (createSetRoot->getType() == logical::LogicalOpType::LOP_SORT) {
			createSetSort = dynamic_cast<const logical::LogicalSort *>(createSetRoot);
			if (createSetSort == nullptr) {
				throw std::logic_error("logical CREATE/SET result has an invalid Sort operator");
			}
			createSetRoot = requireOptionalOnlyChild(*createSetRoot);
			if (createSetRoot == nullptr) {
				throw std::logic_error("logical CREATE/SET result Sort requires a child");
			}
		}
		if (const std::optional<CreateSetMutationChain> chain = collectCreateSetMutationChain(*createSetRoot);
			chain.has_value()) {
			return executeCreateSetMutationChain(*chain, read_backend_, write_backend_, bounds, context,
													 createSetProjection, visibleMutationOutputColumns(*createSetRoot), nullptr,
													 nullptr, nullptr, createSetSort, createSetSkip, createSetLimit);
		}
		if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
			const logical::LogicalOperator *candidate = requireOptionalOnlyChild(root);
			if (candidate != nullptr && candidate->getType() == logical::LogicalOpType::LOP_FOREACH) {
				const auto *foreach = dynamic_cast<const logical::LogicalForeach *>(candidate);
				if (foreach == nullptr) {
					throw std::logic_error("logical mutation runtime received an invalid Foreach operator");
				}
				return executeCreateSetMutationChain(collectForeachMutationChain(*foreach), read_backend_,
													 write_backend_, bounds, context, createSetProjection,
													 visibleMutationOutputColumns(*foreach), foreach);
			}
		}
		const logical::LogicalProject *readMutationProjection = nullptr;
		const logical::LogicalOperator *readMutationRoot = &root;
		if (root.getType() == logical::LogicalOpType::LOP_PROJECT) {
			const logical::LogicalOperator *candidate = requireOptionalOnlyChild(root);
			if (candidate != nullptr && (candidate->getType() == logical::LogicalOpType::LOP_DELETE ||
										 candidate->getType() == logical::LogicalOpType::LOP_SET ||
										 candidate->getType() == logical::LogicalOpType::LOP_REMOVE)) {
				readMutationProjection = dynamic_cast<const logical::LogicalProject *>(&root);
				if (readMutationProjection == nullptr) {
					throw std::logic_error("logical mutation runtime received a malformed Project operator");
				}
				readMutationRoot = candidate;
			}
		}
		if (const std::optional<ReadPropertyMutationChain> chain = collectReadPropertyMutationChain(*readMutationRoot);
			chain.has_value()) {
			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						read_backend_.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical SET/REMOVE chain could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error(
							"logical SET/REMOVE chain requires a backend that exposes an optimistic read fence");
				}

				MutableGraphEntities entities;
				std::vector<LogicalPlanReadRow> projectionRows;
				LogicalPlanReadRuntime reader(read_backend_);
				const LogicalPlanReadRuntimeStats stats = reader.executeWithSnapshot(
						*snapshot, *chain->read_root, bounds,
						[&](const LogicalPlanReadRow &row) {
							context.checkGuard();
							std::unordered_map<std::string, PropertyMap> propertyCache = row.property_cache;
							for (const logical::LogicalOperator *operation: chain->operations) {
								context.checkGuard();
								if (operation->getType() == logical::LogicalOpType::LOP_SET) {
									const auto *set = dynamic_cast<const logical::LogicalSet *>(operation);
									if (set == nullptr) { // collectReadPropertyMutationChain verifies the concrete SET type before retaining the operation pointer.
										throw std::logic_error("logical SET/REMOVE chain has an invalid SET operator");
									}
									applySetOperation(*set, row, *snapshot, bounds, context, entities, propertyCache);
								} else {
									const auto *remove = dynamic_cast<const logical::LogicalRemove *>(operation);
									if (remove == nullptr) { // collectReadPropertyMutationChain verifies the concrete REMOVE type before retaining the operation pointer.
										throw std::logic_error(
												"logical SET/REMOVE chain has an invalid REMOVE operator");
									}
									applyRemoveOperation(*remove, row, *snapshot, bounds, entities, propertyCache);
								}
							}
							LogicalPlanReadRow projected = row;
							projected.property_cache = std::move(propertyCache);
							projectionRows.push_back(std::move(projected));
							return true;
						},
						context);
				requireCompleteReadForMutation(stats, bounds, "logical SET/REMOVE chain");

				storage::LogicalGraphMutationBatch batch;
				std::vector<storage::LogicalGraphPropertyPatchRequest> patches;
				const bool usePatches = snapshot->supportsShapedPropertyReadAdmission();
				size_t patchEntryLimit = 0U;
				for (const auto &[_, entity]: entities) {
					const LogicalGraphMutationEntityReference reference{
							entity.kind, LogicalGraphMutationReferenceKind::LGRK_PERSISTED, entity.id};
					if (entity.properties_changed) {
						if (!entity.properties.has_value()) {
							throw std::logic_error(
									"logical SET/REMOVE chain lost its staged complete Property collection");
						}
						if (usePatches) {
							auto patch = finalPropertyPatch(entity, context, bounds.maximum_materialized_bytes);
							if (!patch.sets.empty() || !patch.removals.empty()) {
								for (size_t entries : {entity.direct_properties.size(), patch.sets.size(), patch.removals.size()}) {
									if (entries > (std::numeric_limits<size_t>::max)() - patchEntryLimit)
										throw std::overflow_error("logical Property patch entry count overflowed");
									patchEntryLimit += entries;
								}
								debug::PerfTrace::addValue("logical_plan.mutation.patch_sets", static_cast<int64_t>(patch.sets.size()));
								debug::PerfTrace::addValue("logical_plan.mutation.patch_removals", static_cast<int64_t>(patch.removals.size()));
								patches.push_back(std::move(patch));
							}
						} else batch.property_replacements.push_back({reference, sortedProperties(*entity.properties)});
					}
					if (entity.labels_changed) {
						batch.node_label_replacements.push_back({reference, entity.labels});
					}
				}
				const auto resolveBinding = [&](const LogicalPlanReadRow &row, const std::string &variable) {
					return materializeMutableBinding(*snapshot, row, variable, entities, bounds);
				};
				const auto buildResult = [&]() {
					if (readMutationProjection != nullptr) {
						return buildMutationProjection(*readMutationProjection, projectionRows, *snapshot, bounds,
													   context, resolveBinding);
					}
					return buildImplicitMutationResult(visibleMutationOutputColumns(*readMutationRoot), projectionRows,
													   context, resolveBinding);
				};
				// All bindings are snapshot-backed, so the result can be constructed
				// before publication and reused after it. This keeps deterministic
				// result failures outside the mutation's atomicity boundary.
				QueryResult preparedResult = buildResult();
				if (patches.empty() && batch.property_replacements.empty() && batch.node_label_replacements.empty()) {
					return preparedResult;
				}
				try {
					context.checkGuardNow();
					if (!patches.empty()) {
						storage::LogicalGraphMutationReadControl control;
						control.checkpoint = [&context] { context.checkGuardNow(); };
						control.property_admission = [&context, &bounds](size_t bytes) {
							context.checkGuardNow();
							if (bytes > bounds.maximum_materialized_bytes)
								throw std::runtime_error("logical Property patch source exceeded its byte bound");
						};
						static_cast<void>(write_backend_.executeLogicalGraphMutationBatchWithPropertyPatches(
							*snapshot, batch, patches, patchEntryLimit, *fence, control));
					} else static_cast<void>(executeMutationWithReadControl(write_backend_, batch, fence, bounds, context));
					return preparedResult;
				} catch (const storage::LogicalGraphMutationConflict &) {
					// Re-read and reapply the ordered chain against the new graph generation.
				}
			}
			throw std::runtime_error("logical SET/REMOVE chain exceeded its optimistic conflict retry limit");
		}
		if (readMutationRoot->getType() == logical::LogicalOpType::LOP_DELETE) {
			const auto *deleteOperator = dynamic_cast<const logical::LogicalDelete *>(readMutationRoot);
			if (deleteOperator == nullptr) {
				throw std::logic_error("logical mutation runtime received an invalid Delete operator");
			}
			const logical::LogicalOperator *child = requireOptionalOnlyChild(*deleteOperator);
			if (child == nullptr || deleteOperator->getVariables().empty()) {
				throw std::logic_error("logical DELETE requires one read child and at least one entity variable");
			}

			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						read_backend_.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical DELETE could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error("logical DELETE requires a backend that exposes an optimistic read fence");
				}

				// Literal RETURN needs only row multiplicity and delete identities.
				// Keep full before-delete values for every other projection/implicit
				// result, and charge both retained input rows and output results.
				const bool literalProjection = readMutationProjection != nullptr &&
					std::ranges::all_of(readMutationProjection->getItems(), [](const auto &item) {
						return dynamic_cast<const expressions::LiteralExpression *>(item.expression.get()) != nullptr;
					});
				MaterializedByteBudget planningBudget = context.resource_account
					? MaterializedByteBudget(bounds.maximum_materialized_bytes, context.resource_account)
					: MaterializedByteBudget(bounds.maximum_materialized_bytes);
				detail::BudgetMemoryResource planningMemory(planningBudget);
				std::pmr::set<uint64_t> nodeIds(&planningMemory), edgeIds(&planningMemory);
				BoundedBatch<LogicalPlanReadRow> projectionRows(bounds.maximum_rows,
					bounds.maximum_materialized_bytes,
					[&](const auto &row) { return detail::retainedLogicalReadRowBytes(row, context); },
					&planningBudget, true, sizeof(LogicalPlanReadRow));
				LogicalPlanReadRuntime reader(read_backend_);
				const auto consumeRow = [&](const LogicalPlanReadRow &row) {
					context.checkGuard();
					if (literalProjection) projectionRows.append({});
					else {
						MaterializedByteReservation copyBytes(&planningBudget);
						copyBytes.set(detail::retainedLogicalReadRowBytes(row, context));
						auto copy = detail::copyLogicalReadRowGuarded(row, [&] { context.checkGuardNow(); });
						projectionRows.appendCharged(std::move(copy), std::move(copyBytes));
					}
					for (const std::string &variable: deleteOperator->getVariables()) {
						const auto found = row.values.find(variable);
						if (found == row.values.end())
							throw std::logic_error("logical DELETE could not resolve a requested entity variable");
						if (const auto *node = std::get_if<storage::LogicalGraphNode>(&found->second))
							nodeIds.insert(node->node_id);
						else if (const auto *edge = std::get_if<storage::LogicalGraphEdge>(&found->second))
							edgeIds.insert(edge->edge_id);
						else if (const auto *reference = std::get_if<storage::EntityRef>(&found->second)) {
							if (!reference->entity_id) throw std::logic_error("logical DELETE received a zero identity");
							if (reference->is_edge) edgeIds.insert(reference->entity_id);
							else nodeIds.insert(reference->entity_id);
						} else throw std::logic_error("logical DELETE requires Node or Edge bindings");
					}
					return true;
				};
				const auto stats = literalProjection
					? detail::visitDeferredReferenceRows(*snapshot, *child, bounds, consumeRow, context)
					: reader.executeWithSnapshot(*snapshot, *child, bounds, consumeRow, context);
				// A limited read cannot safely become a write: without a proof that
				// the producer was exhausted, deleting the prefix would be silent data
				// loss relative to the declarative Cypher statement.
				requireCompleteReadForMutation(stats, bounds, "logical DELETE");
				const auto buildDeleteResult = [&, snapshot = snapshot.get()]() -> QueryResult {
					const auto resolve = [&, snapshot](const LogicalPlanReadRow &row, const std::string &variable) {
						return materializeReadBinding(*snapshot, row, variable, bounds);
					};
					if (readMutationProjection != nullptr) {
						return buildMutationProjection(*readMutationProjection, projectionRows.values(), *snapshot, bounds,
													   context, resolve);
					}
					return buildImplicitMutationResult(visibleMutationOutputColumns(*readMutationRoot), projectionRows.values(),
													   context, resolve);
				};
				if (nodeIds.empty() && edgeIds.empty()) {
					return buildDeleteResult();
				}
				// DELETE makes the matched bindings invisible to the transaction overlay.
				// Materialize the statement result from the pre-delete snapshot before
				// publishing that visibility change.
				QueryResult deleteResult = buildDeleteResult();

				MaterializedByteReservation requestBytes(&planningBudget);
                requestBytes.set(detail::addPropertyBufferBytes(
                    detail::multiplyPropertyBufferBytes(edgeIds.size(), sizeof(storage::LogicalGraphEdgeDeleteRequest)),
                    detail::multiplyPropertyBufferBytes(nodeIds.size(), sizeof(storage::LogicalGraphNodeDeleteRequest))));
                storage::LogicalGraphMutationBatch batch;
				batch.edge_deletes.reserve(edgeIds.size());
				for (const uint64_t edgeId: edgeIds) {
					batch.edge_deletes.push_back({edgeId});
				}
				batch.node_deletes.reserve(nodeIds.size());
				for (const uint64_t nodeId: nodeIds) {
					batch.node_deletes.push_back({nodeId, deleteOperator->isDetach(),
												  deleteOperator->isDetach() ? bounds.maximum_edges_per_node : 0U});
				}
				try {
					context.checkGuardNow();
					static_cast<void>(executeMutationWithReadControl(write_backend_, batch, fence, bounds, context));
					return deleteResult;
				} catch (const storage::LogicalGraphMutationConflict &) {
					// Re-open the snapshot and re-evaluate the full read plan. Never reuse
					// a stale entity set after a concurrent graph publication.
				}
			}
			throw std::runtime_error("logical DELETE exceeded its optimistic conflict retry limit");
		}
		if (const std::optional<MergeResultPipeline> mergePipeline = collectMergeResultPipeline(root);
			mergePipeline.has_value()) {
			constexpr size_t MAX_CONFLICT_RETRIES = 4U;
			for (size_t attempt = 0U; attempt < MAX_CONFLICT_RETRIES; ++attempt) {
				std::unique_ptr<storage::LogicalGraphReadSnapshot> snapshot =
						read_backend_.openLogicalGraphReadSnapshot();
				if (!snapshot) {
					throw std::runtime_error("logical MERGE could not open a read snapshot");
				}
				const std::optional<storage::LogicalGraphMutationFence> fence = snapshot->mutationFence();
				if (!fence.has_value() || !fence->isValid()) {
					throw std::logic_error("logical MERGE requires a backend that exposes an optimistic read fence");
				}
				LogicalPlanReadRuntime reader(read_backend_);
				MergeExecutionState state{*snapshot, bounds, context, reader};
				std::vector<MergeRuntimeRow> rows;
				visitMergeOperator(*mergePipeline->mutation_chain.merge_root, state, [&](MergeRuntimeRow &&row) {
					applyMergePostMutations(mergePipeline->mutation_chain.operations, row, state);
					rows.push_back(std::move(row));
				});
				appendModifiedMergeEntities(state);
				synchronizeCreatedMergeEntities(state);
				// Publication must not turn a deterministic RETURN/ORDER BY expression
				// failure into a partially visible mutation. Validate the expression surface
				// against the staged graph before crossing the backend write boundary.
				const MergePrepublicationResultCache prepublicationCache =
						preflightMergeResultExpressions(*mergePipeline, rows, state);
				try {
					context.checkGuardNow();
					storage::LogicalGraphMutationBatchResult result;
					if (!state.property_patches.empty()) {
						storage::LogicalGraphMutationReadControl control;
						control.checkpoint = [&context] { context.checkGuardNow(); };
						control.property_admission = [&context, &bounds](size_t bytes) {
							context.checkGuardNow();
							if (bytes > bounds.maximum_materialized_bytes)
								throw std::runtime_error("logical MERGE Property patch source exceeded its byte bound");
						};
						result = write_backend_.executeLogicalGraphMutationBatchWithPropertyPatches(
							*snapshot, state.batch, state.property_patches, state.patch_entry_limit, *fence, control);
					} else if (mergeBatchHasMutations(state.batch)) {
						result = executeMutationWithReadControl(write_backend_, state.batch, fence, bounds, context);
					}
					return buildMergeResult(*mergePipeline,
											visibleMutationOutputColumns(*mergePipeline->mutation_chain.merge_root),
											rows, state, result, prepublicationCache);
				} catch (const storage::LogicalGraphMutationConflict &) {
					// A MERGE decision is snapshot-dependent. Re-evaluate all match and
					// creation choices rather than replaying a stale batch after a writer.
				}
			}
			throw std::runtime_error("logical MERGE exceeded its optimistic conflict retry limit");
		}

		throw std::logic_error("logical mutation runtime received an unsupported mutation plan");
	}
} // namespace graph::query::execution
