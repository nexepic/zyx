/**
 * @file NativeBulkEdgeCatalogTokenMaterializer.cpp
 * @brief Bounded Catalog-token overlay planning for streamed native Edges.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include "graph/storage/relocatable/schema/NativeBulkEdgeCatalogTokenMaterializer.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool lessKey(std::span<const std::byte> left, std::span<const std::byte> right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] std::optional<BTreePageEntry> entry(utils::OwnedStdVector<std::byte> key, uint64_t value) {
			return BTreePageEntry{std::move(key), value};
		}

		struct DecodedTokenEntry {
			NativeBulkEdgeTokenKind kind = NativeBulkEdgeTokenKind::NBETK_PROPERTY;
			std::string name;
		};

		[[nodiscard]] DecodedTokenEntry decodeTokenEntry(const BTreePageEntry &entry) {
			if (entry.value_or_child != 0U || entry.key.size() < 2U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native streamed Edge token source has an invalid entry value or key");
			}
			const uint8_t rawKind = std::to_integer<uint8_t>(entry.key.front());
			NativeBulkEdgeTokenKind kind;
			switch (rawKind) {
				case static_cast<uint8_t>(NativeBulkEdgeTokenKind::NBETK_PROPERTY):
					kind = NativeBulkEdgeTokenKind::NBETK_PROPERTY;
					break;
				case static_cast<uint8_t>(NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE):
					kind = NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE;
					break;
				default:
					fail(FormatError::FERR_INVALID_ENUM, "native streamed Edge token source has an unknown token kind");
			}
			const std::string name(reinterpret_cast<const char *>(entry.key.data() + 1U), entry.key.size() - 1U);
			SystemCatalogTokenKeyCodec::validateName(name);
			return {kind, name};
		}

		[[nodiscard]] std::optional<uint64_t>
		findToken(const SystemCatalog &catalog, const MetadataSnapshotPin &snapshot, const DecodedTokenEntry &entry) {
			if (entry.kind == NativeBulkEdgeTokenKind::NBETK_PROPERTY) {
				return catalog.findPropertyToken(snapshot, entry.name);
			}
			return catalog.findSchemaToken(snapshot, SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, entry.name);
		}

		[[nodiscard]] uint64_t nextSequence(const SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
											const std::vector<std::byte> &key, uint64_t fallback) {
			if (!catalog.metadataStore().findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value()) {
				return fallback;
			}
			return BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, catalog.pageStore(), catalog.metadataStore())
					.find(snapshot, key)
					.value_or(fallback);
		}

		struct AllocationState {
			uint64_t next_property = 1U;
			uint64_t next_schema = 1U;
			uint64_t new_property = 0U;
			uint64_t new_schema = 0U;
		};

		[[nodiscard]] NativeBulkEdgeCatalogTokenResolution resolveOne(const SystemCatalog &catalog,
																	  const MetadataSnapshotPin &snapshot,
																	  const DecodedTokenEntry &entry,
																	  AllocationState &state) {
			if (const std::optional<uint64_t> existing = findToken(catalog, snapshot, entry); existing.has_value()) {
				if (*existing == 0U || (entry.kind == NativeBulkEdgeTokenKind::NBETK_PROPERTY &&
										*existing > std::numeric_limits<uint32_t>::max())) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge token source resolved to an invalid Catalog Token ID");
				}
				return {entry.kind, entry.name, *existing, false};
			}
			if (entry.kind == NativeBulkEdgeTokenKind::NBETK_PROPERTY) {
				if (state.next_property == 0U || state.next_property > std::numeric_limits<uint32_t>::max() ||
					state.new_property == std::numeric_limits<uint64_t>::max()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native streamed Edge publication exhausted Property Token IDs");
				}
				const uint64_t token = state.next_property++;
				++state.new_property;
				return {entry.kind, entry.name, token, true};
			}
			if (state.next_schema == 0U || state.new_schema == std::numeric_limits<uint64_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native streamed Edge publication exhausted relationship-type Token IDs");
			}
			const uint64_t token = state.next_schema;
			state.next_schema = token == std::numeric_limits<uint64_t>::max() ? 0U : token + 1U;
			++state.new_schema;
			return {entry.kind, entry.name, token, true};
		}

		[[nodiscard]] NativeBulkEdgeCatalogTokenResolutionSource
		makeResolutionSource(SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
							 const OrderedBTreeEntrySource &tokens,
							 NativeBulkEdgeCatalogTokenMaterializationOptions options) {
			return [&catalog, &snapshot, tokens, options] {
				if (!snapshot.isValid() || !tokens || options.maximum_token_entries == 0U) {
					fail(FormatError::FERR_INVALID_STATE,
						 "native streamed Edge token resolution source escaped its planning scope");
				}
				struct Cursor final {
					SystemCatalog *catalog = nullptr;
					const MetadataSnapshotPin *snapshot = nullptr;
					OrderedBTreeEntryCursor tokens;
					size_t maximum = 0U;
					size_t emitted = 0U;
					std::optional<std::vector<std::byte>> previous_key;
					AllocationState allocation;

					[[nodiscard]] std::optional<NativeBulkEdgeCatalogTokenResolution> next() {
						const std::optional<BTreePageEntry> nextEntry = tokens();
						if (!nextEntry.has_value()) {
							return std::nullopt;
						}
						if (++emitted > maximum ||
							(previous_key.has_value() && !lessKey(*previous_key, nextEntry->key))) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge token source is unordered or exceeds its admission bound");
						}
						previous_key = nextEntry->key;
						return resolveOne(*catalog, *snapshot, decodeTokenEntry(*nextEntry), allocation);
					}
				};
				AllocationState allocation;
				allocation.next_property =
						nextSequence(catalog, snapshot, SystemCatalogTokenKeyCodec::propertyTokenSequenceKeyOwned(), 1U);
				allocation.next_schema =
						nextSequence(catalog, snapshot, SystemCatalogTokenKeyCodec::schemaTokenSequenceKeyOwned(), 1U);
				auto cursor = std::make_shared<Cursor>(Cursor{
						&catalog, &snapshot, tokens(), options.maximum_token_entries, 0U, std::nullopt, allocation});
				if (!cursor->tokens) {
					fail(FormatError::FERR_INVALID_STATE, "native streamed Edge token source did not open a cursor");
				}
				return [cursor] { return cursor->next(); };
			};
		}

		enum class OverlayPass : uint8_t {
			OP_BOOTSTRAP = 0U,
			OP_PROPERTY_NAME = 1U,
			OP_PROPERTY_REVERSE = 2U,
			OP_SCHEMA_NAME = 3U,
			OP_SCHEMA_REVERSE = 4U,
			OP_PROPERTY_SEQUENCE = 5U,
			OP_SCHEMA_SEQUENCE = 6U,
			OP_NODE_CARDINALITY = 7U,
			OP_EDGE_CARDINALITY = 8U,
			OP_DONE = 9U,
		};

		[[nodiscard]] std::optional<BTreePageEntry>
		overlayEntryFor(const NativeBulkEdgeCatalogTokenResolution &resolution, OverlayPass pass) {
			if (!resolution.newly_allocated) {
				return std::nullopt;
			}
			switch (pass) {
				case OverlayPass::OP_PROPERTY_NAME:
					if (resolution.kind == NativeBulkEdgeTokenKind::NBETK_PROPERTY) {
						return entry(SystemCatalogTokenKeyCodec::propertyNameKeyOwned(resolution.name), resolution.token_id);
					}
					return std::nullopt;
				case OverlayPass::OP_PROPERTY_REVERSE:
					if (resolution.kind == NativeBulkEdgeTokenKind::NBETK_PROPERTY) {
						return entry(SystemCatalogTokenKeyCodec::propertyTokenKeyOwned(
											 static_cast<uint32_t>(resolution.token_id), resolution.name),
									 resolution.token_id);
					}
					return std::nullopt;
				case OverlayPass::OP_SCHEMA_NAME:
					if (resolution.kind == NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE) {
						return entry(SystemCatalogTokenKeyCodec::schemaNameKeyOwned(
											 SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, resolution.name),
									 resolution.token_id);
					}
					return std::nullopt;
				case OverlayPass::OP_SCHEMA_REVERSE:
					if (resolution.kind == NativeBulkEdgeTokenKind::NBETK_RELATIONSHIP_TYPE) {
						return entry(SystemCatalogTokenKeyCodec::schemaTokenKeyOwned(
											 SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE, resolution.token_id,
											 resolution.name),
									 resolution.token_id);
					}
					return std::nullopt;
				default:
					return std::nullopt;
			}
		}

		[[nodiscard]] OrderedBTreeEntrySource
		makeOverlaySource(const NativeBulkEdgeCatalogTokenResolutionSource &resolutions, bool initialRoot,
						  uint64_t newPropertyTokens, uint64_t newSchemaTokens, uint64_t nextProperty,
						  uint64_t nextSchema, std::optional<NativeGraphCardinalityStatistics> graphStatistics) {
			return [resolutions, initialRoot, newPropertyTokens, newSchemaTokens, nextProperty, nextSchema,
					graphStatistics = std::move(graphStatistics)] {
				struct Cursor final {
					NativeBulkEdgeCatalogTokenResolutionSource resolutions;
					bool initial_root = false;
					uint64_t new_property_tokens = 0U;
					uint64_t new_schema_tokens = 0U;
					uint64_t next_property = 0U;
					uint64_t next_schema = 0U;
					std::optional<NativeGraphCardinalityStatistics> graph_statistics;
					OverlayPass pass = OverlayPass::OP_BOOTSTRAP;
					NativeBulkEdgeCatalogTokenResolutionCursor current;
					std::optional<std::vector<std::byte>> previous_key;

					[[nodiscard]] std::optional<BTreePageEntry> next() {
						for (;;) {
							if (pass == OverlayPass::OP_BOOTSTRAP) {
								pass = OverlayPass::OP_PROPERTY_NAME;
								if (initial_root) {
									return entry(SystemCatalogTokenKeyCodec::bootstrapKeyOwned(),
												 RSS_SYSTEM_CATALOG_BOOTSTRAP_VALUE);
								}
							}
							if (pass == OverlayPass::OP_PROPERTY_SEQUENCE) {
								pass = OverlayPass::OP_SCHEMA_SEQUENCE;
								if (new_property_tokens != 0U) {
									return entry(SystemCatalogTokenKeyCodec::propertyTokenSequenceKeyOwned(), next_property);
								}
								continue;
							}
							if (pass == OverlayPass::OP_SCHEMA_SEQUENCE) {
								pass = OverlayPass::OP_NODE_CARDINALITY;
								if (new_schema_tokens != 0U) {
									return entry(SystemCatalogTokenKeyCodec::schemaTokenSequenceKeyOwned(), next_schema);
								}
								continue;
							}
							if (pass == OverlayPass::OP_NODE_CARDINALITY) {
								pass = OverlayPass::OP_EDGE_CARDINALITY;
								if (graph_statistics.has_value()) {
									return entry(SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned(),
												 SystemCatalogGraphCardinalityCodec::encodeCount(
														 graph_statistics->node_count));
								}
								continue;
							}
							if (pass == OverlayPass::OP_EDGE_CARDINALITY) {
								pass = OverlayPass::OP_DONE;
								if (graph_statistics.has_value()) {
									return entry(SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned(),
												 SystemCatalogGraphCardinalityCodec::encodeCount(
														 graph_statistics->edge_count));
								}
								continue;
							}
							if (pass == OverlayPass::OP_DONE) {
								return std::nullopt;
							}
							if (!current) {
								current = resolutions();
								if (!current) {
									fail(FormatError::FERR_INVALID_STATE,
										 "native streamed Edge token resolution source did not rewind");
								}
							}
							while (const std::optional<NativeBulkEdgeCatalogTokenResolution> resolution = current()) {
								if (const std::optional<BTreePageEntry> entry = overlayEntryFor(*resolution, pass);
									entry.has_value()) {
									if (previous_key.has_value() && !lessKey(*previous_key, entry->key)) {
										fail(FormatError::FERR_INVALID_STATE,
											 "native streamed Edge Catalog overlay is not globally ordered");
									}
									previous_key = entry->key;
									return entry;
								}
							}
							current = {};
							switch (pass) {
								case OverlayPass::OP_PROPERTY_NAME:
									pass = OverlayPass::OP_PROPERTY_REVERSE;
									break;
								case OverlayPass::OP_PROPERTY_REVERSE:
									pass = OverlayPass::OP_SCHEMA_NAME;
									break;
								case OverlayPass::OP_SCHEMA_NAME:
									pass = OverlayPass::OP_SCHEMA_REVERSE;
									break;
								case OverlayPass::OP_SCHEMA_REVERSE:
									pass = OverlayPass::OP_PROPERTY_SEQUENCE;
									break;
								default:
									fail(FormatError::FERR_INVALID_STATE,
										 "native streamed Edge Catalog overlay has an invalid pass");
							}
						}
					}
				};
				auto cursor = std::make_shared<Cursor>(Cursor{resolutions,
															  initialRoot,
															  newPropertyTokens,
															  newSchemaTokens,
															  nextProperty,
															  nextSchema,
															  std::move(graphStatistics),
															  OverlayPass::OP_BOOTSTRAP,
															  {},
															  std::nullopt});
				return [cursor] { return cursor->next(); };
			};
		}

		[[nodiscard]] OrderedBTreeEntrySource mergeOverlay(OrderedBTreeEntrySource existing,
														   OrderedBTreeEntrySource overlay) {
			return [existing = std::move(existing), overlay = std::move(overlay)] {
				struct Cursor final {
					OrderedBTreeEntryCursor existing;
					OrderedBTreeEntryCursor overlay;
					std::optional<BTreePageEntry> current_existing;
					std::optional<BTreePageEntry> current_overlay;
					std::optional<std::vector<std::byte>> previous_key;

					[[nodiscard]] std::optional<BTreePageEntry> next() {
						if (!current_existing.has_value()) {
							current_existing = existing();
						}
						if (!current_overlay.has_value()) {
							current_overlay = overlay();
						}
						if (!current_existing.has_value() && !current_overlay.has_value()) {
							return std::nullopt;
						}
						BTreePageEntry result;
						if (!current_existing.has_value() ||
							(current_overlay.has_value() && lessKey(current_overlay->key, current_existing->key))) {
							result = std::move(*current_overlay);
							current_overlay.reset();
						} else if (!current_overlay.has_value() ||
								   lessKey(current_existing->key, current_overlay->key)) {
							result = std::move(*current_existing);
							current_existing.reset();
						} else {
							result = std::move(*current_overlay);
							current_existing.reset();
							current_overlay.reset();
						}
						if (previous_key.has_value() && !lessKey(*previous_key, result.key)) {
							fail(FormatError::FERR_INVALID_STATE,
								 "native streamed Edge Catalog merge produced duplicate or unordered keys");
						}
						previous_key = result.key;
						return result;
					}
				};
				auto left = existing();
				auto right = overlay();
				if (!left || !right) {
					fail(FormatError::FERR_INVALID_STATE, "native streamed Edge Catalog overlay source did not open");
				}
				auto cursor = std::make_shared<Cursor>(
						Cursor{std::move(left), std::move(right), std::nullopt, std::nullopt, std::nullopt});
				return [cursor] { return cursor->next(); };
			};
		}

	} // namespace

	OrderedBTreeEntrySource NativeBulkEdgeCatalogTokenMaterializer::overlayEntries(
			OrderedBTreeEntrySource existing, OrderedBTreeEntrySource updates) {
		if (!existing || !updates) fail(FormatError::FERR_INVALID_HEADER, "Catalog overlay requires two ordered sources");
		return mergeOverlay(std::move(existing), std::move(updates));
	}

	NativeBulkEdgeCatalogTokenMaterialization
	NativeBulkEdgeCatalogTokenMaterializer::prepare(SystemCatalog &catalog, const MetadataSnapshotPin &snapshot,
													const OrderedBTreeEntrySource &sortedTokenEntries,
													NativeBulkEdgeCatalogTokenMaterializationOptions options,
													uint64_t edgeCountIncrement) {
		if (!snapshot.isValid() || !sortedTokenEntries || options.maximum_token_entries == 0U ||
			options.maximum_catalog_entries == 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "native streamed Edge Catalog token materialization requires bounded live inputs");
		}
		const bool initialRoot =
				!catalog.metadataStore().findIndexRootOptional(snapshot, RSS_SYSTEM_CATALOG_INDEX_ID).has_value();
		const uint64_t initialProperty =
				nextSequence(catalog, snapshot, SystemCatalogTokenKeyCodec::propertyTokenSequenceKeyOwned(), 1U);
		const uint64_t initialSchema =
				nextSequence(catalog, snapshot, SystemCatalogTokenKeyCodec::schemaTokenSequenceKeyOwned(), 1U);
		const NativeBulkEdgeCatalogTokenResolutionSource resolutions =
				makeResolutionSource(catalog, snapshot, sortedTokenEntries, options);

		uint64_t newProperty = 0U;
		uint64_t newSchema = 0U;
		NativeBulkEdgeCatalogTokenResolutionCursor countCursor = resolutions();
		while (const std::optional<NativeBulkEdgeCatalogTokenResolution> resolution = countCursor()) {
			if (resolution->newly_allocated) {
				if (resolution->kind == NativeBulkEdgeTokenKind::NBETK_PROPERTY) {
					++newProperty;
				} else {
					++newSchema;
				}
			}
		}
		const bool changed = newProperty != 0U || newSchema != 0U;
		const uint64_t nextSchema = newSchema == 0U ? initialSchema
													: (initialSchema > std::numeric_limits<uint64_t>::max() - newSchema
															   ? 0U
															   : initialSchema + newSchema);
		std::optional<NativeGraphCardinalityStatistics> updatedStatistics;
		if (edgeCountIncrement != 0U) {
			if (const std::optional<NativeGraphCardinalityStatistics> current =
						catalog.readGraphCardinalityStatistics(snapshot);
				current.has_value()) {
				if (current->edge_count > std::numeric_limits<uint64_t>::max() - edgeCountIncrement) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native streamed Edge publication overflows the durable Edge cardinality");
				}
				updatedStatistics = *current;
				updatedStatistics->edge_count += edgeCountIncrement;
			}
		}
		const OrderedBTreeEntrySource overlay =
				makeOverlaySource(resolutions, initialRoot && changed, newProperty, newSchema,
								  initialProperty + newProperty, nextSchema, updatedStatistics);
		OrderedBTreeEntrySource existing = [] {
			return []() -> std::optional<BTreePageEntry> { return std::nullopt; };
		};
		if (!initialRoot) {
			existing = BTreeIndex(RSS_SYSTEM_CATALOG_INDEX_ID, catalog.pageStore(), catalog.metadataStore())
							   .orderedSource(snapshot, options.maximum_catalog_entries);
		}
		return {resolutions,
				mergeOverlay(std::move(existing), overlay),
				newProperty,
				newSchema,
				updatedStatistics.has_value(),
				changed || updatedStatistics.has_value()};
	}

} // namespace graph::storage
