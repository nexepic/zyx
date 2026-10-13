#include "graph/storage/LogicalGraphWrite.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <tuple>

namespace graph::storage {

LogicalGraphMutationBatchResult LogicalGraphWriteBackend::executeLogicalGraphPropertyPatches(
	LogicalGraphReadSnapshot &snapshot, std::span<const LogicalGraphPropertyPatchRequest> patches,
	size_t maximumPropertyEntries, const LogicalGraphMutationFence &expectedFence,
	const LogicalGraphMutationReadControl &control) {
	return executeLogicalGraphMutationBatchWithPropertyPatches(snapshot, {}, patches, maximumPropertyEntries, expectedFence, control);
}

LogicalGraphMutationBatchResult LogicalGraphWriteBackend::executeLogicalGraphMutationBatchWithPropertyPatches(
	LogicalGraphReadSnapshot &snapshot, const LogicalGraphMutationBatch &companionBatch,
	std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximumPropertyEntries,
	const LogicalGraphMutationFence &expectedFence, const LogicalGraphMutationReadControl &control) {
	if (maximumPropertyEntries == 0U) throw std::invalid_argument("Property patches require a positive entry bound");
	if (patches.size() > maximumPropertyEntries) throw std::runtime_error("Property patches exceeded their target bound");
	const auto checkpoint = [&] { if (control.checkpoint) control.checkpoint(); };
	checkpoint();
	const auto fence = snapshot.mutationFence();
	if (!fence || !fence->isValid() || !expectedFence.isValid())
		throw std::invalid_argument("Property patches require a snapshot mutation fence");
	if (*fence != expectedFence) throw LogicalGraphMutationConflict("Property patch planning fence does not match its source snapshot");
	std::set<std::tuple<uint8_t, uint8_t, uint64_t>> targets;
	size_t entries = 0U;
	size_t persistedTargets = 0U;
	const auto chargeEntries = [&](size_t count) {
		if (count > maximumPropertyEntries - entries) throw std::runtime_error("Property patches exceeded their entry bound");
		entries += count;
	};
	// Validate every request before reading any entity or staging any mutation.
	for (const auto &patch : patches) {
		checkpoint();
		const auto &entity = patch.entity;
		const bool persisted = entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED && entity.value != 0U;
		const bool createdNode = entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE &&
			entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE && entity.value < companionBatch.node_creates.size();
		const bool createdEdge = entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_EDGE &&
			entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE && entity.value < companionBatch.edge_creates.size();
		if ((entity.entity_kind != LogicalGraphMutationEntityKind::LGMK_NODE && entity.entity_kind != LogicalGraphMutationEntityKind::LGMK_EDGE) ||
			(!persisted && !createdNode && !createdEdge) ||
			!targets.emplace(static_cast<uint8_t>(entity.entity_kind), static_cast<uint8_t>(entity.reference_kind), entity.value).second)
			throw std::invalid_argument("Property patches require distinct valid entity targets");
		chargeEntries(patch.sets.size());
		chargeEntries(patch.removals.size());
		if (persisted) ++persistedTargets;
		// Creation fields are already available in caller-owned input. Admit
		// their work before copying the companion batch or any source payload.
		if (createdNode) chargeEntries(companionBatch.node_creates.at(static_cast<size_t>(entity.value)).properties.size());
		if (createdEdge) chargeEntries(companionBatch.edge_creates.at(static_cast<size_t>(entity.value)).properties.size());
		std::set<std::string_view> keys;
		for (const auto &property : patch.sets) {
			checkpoint();
			if (property.key.empty() || property.value.getType() == PropertyType::NULL_TYPE || !keys.insert(property.key).second)
				throw std::invalid_argument("Property patch sets require unique nonempty keys and non-null values");
		}
		for (const auto &key : patch.removals) {
			checkpoint();
			if (key.empty() || !keys.insert(key).second)
				throw std::invalid_argument("Property patch removal keys must be unique and disjoint from sets");
		}
	}
	for (const auto &deletion : companionBatch.node_deletes) {
		checkpoint();
		if (targets.contains({static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_NODE),
			static_cast<uint8_t>(LogicalGraphMutationReferenceKind::LGRK_PERSISTED), deletion.node_id}))
			throw std::invalid_argument("Property patches cannot target a Node deleted in the same batch");
	}
	for (const auto &deletion : companionBatch.edge_deletes) {
		checkpoint();
		if (targets.contains({static_cast<uint8_t>(LogicalGraphMutationEntityKind::LGMK_EDGE),
			static_cast<uint8_t>(LogicalGraphMutationReferenceKind::LGRK_PERSISTED), deletion.edge_id}))
			throw std::invalid_argument("Property patches cannot target an Edge deleted in the same batch");
	}
	for (const auto &replacement : companionBatch.property_replacements) {
		checkpoint();
		if (targets.contains({static_cast<uint8_t>(replacement.entity.entity_kind),
			static_cast<uint8_t>(replacement.entity.reference_kind), replacement.entity.value}))
			throw std::invalid_argument("Property patches cannot overlap complete replacement targets");
	}
	if (patches.empty()) {
		if (companionBatch.node_creates.empty() && companionBatch.edge_creates.empty() &&
			companionBatch.property_replacements.empty() && companionBatch.node_label_replacements.empty() &&
			companionBatch.node_deletes.empty() && companionBatch.edge_deletes.empty() && !companionBatch.creation_identity_plan)
			return {};
		return executeLogicalGraphMutationBatchWithReadControl(companionBatch, fence, control);
	}
	if (persistedTargets != 0U && control.property_admission && !snapshot.supportsShapedPropertyReadAdmission())
		throw std::logic_error("Property patch source does not support shaped admission");
	LogicalGraphMutationBatch batch = companionBatch;
	if (persistedTargets > (std::numeric_limits<size_t>::max)() - batch.property_replacements.size())
		throw std::overflow_error("Property patch replacement count overflowed");
	batch.property_replacements.reserve(batch.property_replacements.size() + persistedTargets);
	size_t sourceBytes = 0U;
	for (const auto &patch : patches) {
		checkpoint();
		std::vector<LogicalGraphPropertyEntry> source;
		const EntityReadShape shape{.all_properties = true};
		const std::array<uint64_t, 1U> ids{patch.entity.value};
		size_t currentSourceBytes = 0U;
		const auto admit = [&](size_t bytes) {
			checkpoint();
			if (bytes > (std::numeric_limits<size_t>::max)() - sourceBytes)
				throw std::overflow_error("Property patch source bytes overflowed");
			control.property_admission(sourceBytes + bytes);
			currentSourceBytes = std::max(currentSourceBytes, bytes);
		};
		if (patch.entity.reference_kind != LogicalGraphMutationReferenceKind::LGRK_PERSISTED) {
			const auto &properties = patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE
				? companionBatch.node_creates.at(static_cast<size_t>(patch.entity.value)).properties
				: companionBatch.edge_creates.at(static_cast<size_t>(patch.entity.value)).properties;
			source.reserve(properties.size());
			for (const auto &property : properties) { checkpoint(); source.push_back({property.key, property.value}); }
		} else if (patch.entity.entity_kind == LogicalGraphMutationEntityKind::LGMK_NODE) {
			if (control.property_admission) {
				auto nodes = snapshot.readNodesShapedSortedWithAdmission(ids, shape, admit);
				if (nodes.size() != 1U || nodes.front().node_id != ids.front()) throw std::runtime_error("Property patch source changed Node identity");
				source = std::move(nodes.front().properties);
			} else {
				auto node = snapshot.readNodeShaped(ids.front(), shape);
				if (node.node_id != ids.front()) throw std::runtime_error("Property patch source changed Node identity");
				source = std::move(node.properties);
			}
		} else {
			if (control.property_admission) {
				auto edges = snapshot.readEdgesShapedSortedWithAdmission(ids, shape, admit);
				if (edges.size() != 1U || edges.front().edge_id != ids.front()) throw std::runtime_error("Property patch source changed Edge identity");
				source = std::move(edges.front().properties);
			} else {
				auto edge = snapshot.readEdgeShaped(ids.front(), shape);
				if (edge.edge_id != ids.front()) throw std::runtime_error("Property patch source changed Edge identity");
				source = std::move(edge.properties);
			}
		}
		sourceBytes += currentSourceBytes;
		if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_PERSISTED) chargeEntries(source.size());
		std::sort(source.begin(), source.end(), [&](const auto &left, const auto &right) {
			checkpoint();
			return left.key < right.key;
		});
		for (size_t index = 0U; index < source.size(); ++index) {
			checkpoint();
			if (source[index].key.empty() || (index != 0U && source[index - 1U].key == source[index].key))
				throw std::runtime_error("Property patch source contains invalid or duplicate keys");
		}
		std::vector<const LogicalGraphWriteProperty *> sets;
		sets.reserve(patch.sets.size());
		for (const auto &property : patch.sets) { checkpoint(); sets.push_back(&property); }
		std::sort(sets.begin(), sets.end(), [&](const auto *left, const auto *right) {
			checkpoint(); return left->key < right->key;
		});
		std::vector<std::string_view> removals;
		removals.reserve(patch.removals.size());
		for (const auto &key : patch.removals) { checkpoint(); removals.push_back(key); }
		std::sort(removals.begin(), removals.end(), [&](auto left, auto right) {
			checkpoint(); return left < right;
		});
		// Count the final collection before allocating its output buffer. Source
		// and operation entry admission already bounds this count without overflow.
		const auto containsSourceKey = [&](std::string_view key) {
			const auto found = std::lower_bound(source.begin(), source.end(), key,
				[&](const auto &property, std::string_view candidate) {
					checkpoint(); return property.key < candidate;
				});
			return found != source.end() && found->key == key;
		};
		// Preserve the staged revision and avoid rebuilding every downstream
		// index/property root when the patch leaves the visible collection intact.
		bool changesSource = false;
		for (const auto *property : sets) {
			checkpoint();
			const auto found = std::lower_bound(source.begin(), source.end(), std::string_view(property->key),
				[&](const auto &candidate, std::string_view key) {
					checkpoint(); return candidate.key < key;
				});
			if (found == source.end() || found->key != property->key || found->value != property->value) {
				changesSource = true;
				break;
			}
		}
		if (!changesSource) {
			for (const auto key : removals) {
				checkpoint();
				if (containsSourceKey(key)) {
					changesSource = true;
					break;
				}
			}
		}
		if (!changesSource) continue;
		size_t outputEntries = source.size();
		for (const auto key : removals) { checkpoint(); if (containsSourceKey(key)) --outputEntries; }
		for (const auto *property : sets) { checkpoint(); if (!containsSourceKey(property->key)) ++outputEntries; }
		LogicalGraphPropertyMutationRequest replacement{patch.entity, {}};
		// Source payloads move directly into the replacement. Only changed input
		// values are copied; borrowed patch descriptors live for this call only.
		replacement.properties.reserve(outputEntries);
		size_t sourceIndex = 0U, setIndex = 0U;
		while (sourceIndex < source.size() || setIndex < sets.size()) {
			checkpoint();
			if (setIndex < sets.size() && (sourceIndex == source.size() || sets[setIndex]->key <= source[sourceIndex].key)) {
				const auto &property = *sets[setIndex++];
				if (sourceIndex < source.size() && source[sourceIndex].key == property.key) ++sourceIndex;
				replacement.properties.push_back(property);
			} else {
				auto &property = source[sourceIndex++];
				if (!std::binary_search(removals.begin(), removals.end(), std::string_view(property.key)))
					replacement.properties.push_back({std::move(property.key), std::move(property.value)});
			}
		}
		// Created entities must carry their final fields in the creation request.
		// Native executors reject a second replacement over nonempty initial fields.
		if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_NODE) {
			batch.node_creates.at(static_cast<size_t>(patch.entity.value)).properties = std::move(replacement.properties);
		} else if (patch.entity.reference_kind == LogicalGraphMutationReferenceKind::LGRK_CREATED_EDGE) {
			batch.edge_creates.at(static_cast<size_t>(patch.entity.value)).properties = std::move(replacement.properties);
		} else {
			batch.property_replacements.push_back(std::move(replacement));
		}
	}
	checkpoint();
	return executeLogicalGraphMutationBatchWithReadControl(batch, fence, control);
}

} // namespace graph::storage
