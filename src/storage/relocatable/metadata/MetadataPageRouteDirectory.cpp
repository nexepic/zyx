/**
 * @file MetadataPageRouteDirectory.cpp
 * @brief Physical COW construction and lookup for metadata PageId routes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace graph::storage {

	namespace {

		[[noreturn]] void throwRouteDirectoryError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint64_t leafOrdinal(MetadataPageId pageId) {
			if (!pageId.isValid()) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route requires a nonzero identity");
			}
			return (pageId.value() - 1U) / RSS_METADATA_PAGE_ROUTE_LEAF_MAX_ENTRIES;
		}

		void validateRoute(const MetadataPageRoute &route, const Superblock &superblock) {
			if (!route.page_id.isValid() || !route.physical_reference.isValid() ||
				route.physical_reference.generation > superblock.generation) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route has an invalid or future physical reference");
			}
		}

		void validateVisible(Generation generation, LogSequenceNumber lsn, const Superblock &superblock,
							 const char *message) {
			if (!generation.isValid() || !lsn.isValid() || generation > superblock.generation ||
				lsn > superblock.checkpoint_lsn) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER, message);
			}
		}

		void validateReference(PhysicalPageRef reference, const Superblock &superblock, const char *message) {
			if (!reference.isValid() || reference.generation > superblock.generation) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER, message);
			}
		}

		[[nodiscard]] uint64_t radixFactor(uint16_t level) {
			uint64_t factor = 1U;
			for (uint16_t index = 0U; index < level; ++index) {
				if (factor > std::numeric_limits<uint64_t>::max() / RSS_CATALOG_RADIX_FANOUT) {
					throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										"metadata PageId route radix level exceeds the identity domain");
				}
				factor *= RSS_CATALOG_RADIX_FANOUT;
			}
			return factor;
		}

		struct Child final {
			uint64_t ordinal = 0U;
			PhysicalPageRef reference{};
		};

		[[nodiscard]] PhysicalPageRef writeNode(MetadataPageFile &pages, MetadataWriteBatch &batch, uint16_t level,
														std::span<const Child> children, bool root) {
			MetadataRadixPage page;
			page.kind = root ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL;
			page.directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE;
			page.level = level;
			page.page_id = batch.allocateMetadataPageId();
			page.generation = batch.generation();
			page.committed_lsn = batch.checkpointLsn();
			for (const Child &child: children) {
				const uint32_t index = static_cast<uint32_t>(child.ordinal % RSS_CATALOG_RADIX_FANOUT);
				if (!child.reference.isValid() || page.children[index].isValid()) {
					throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
										"metadata PageId route radix construction has an invalid child");
				}
				page.children[index] = child.reference;
			}
			const PhysicalPageRef reference = batch.allocatePage();
			pages.writeMetadataRadix(reference, page);
			return reference;
		}

		[[nodiscard]] size_t bootstrapFrameCount(std::span<const MetadataPageRoute> routes) {
			if (routes.empty()) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
								  "metadata PageId bootstrap needs a nonempty route set");
			}
			std::set<uint64_t> children;
			for (const MetadataPageRoute &route: routes) {
				children.emplace(leafOrdinal(route.page_id));
			}
			size_t frames = children.size(); // One route leaf per occupied leaf ordinal.
			const auto addChecked = [&frames](size_t count) {
				if (count > std::numeric_limits<size_t>::max() - frames) {
					throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										  "metadata PageId bootstrap frame count overflows size_t");
				}
				frames += count;
			};
			while (children.size() > 1U) {
				std::set<uint64_t> parents;
				for (const uint64_t ordinal: children) {
					parents.emplace(ordinal / RSS_CATALOG_RADIX_FANOUT);
				}
				addChecked(parents.size());
				children = std::move(parents);
			}
			addChecked(1U); // The route root.
			addChecked(1U); // The PageDirectoryBootstrap header.
			return frames;
		}

		[[nodiscard]] MetadataPageId rootPageId(std::span<const MetadataPageRoute> routes, PhysicalPageRef reference) {
			for (const MetadataPageRoute &route: routes) {
				if (route.physical_reference == reference) {
					return route.page_id;
				}
			}
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
								  "metadata bootstrap root is missing from its complete route set");
		}

		void assignLogicalRoots(PageDirectoryBootstrap &bootstrap, const Superblock &superblock,
								std::span<const MetadataPageRoute> routes) {
			const std::array roots = {
					superblock.catalog_root,
					superblock.allocator_root,
					superblock.extent_directory_root,
					superblock.node_entity_directory_root,
					superblock.edge_entity_directory_root,
					superblock.entity_property_directory_root,
					superblock.index_root_directory_root,
					superblock.vector_location_directory_root,
					superblock.vector_manifest_directory_root,
					superblock.large_value_manifest_directory_root,
					superblock.state_directory_root,
					superblock.retire_queue_root,
			};
			static_assert(roots.size() == RSS_METADATA_DIRECTORY_ROOT_COUNT);
			for (size_t index = 0U; index < roots.size(); ++index) {
				if (roots[index].isValid()) {
					bootstrap.metadata_directory_roots[index] = rootPageId(routes, roots[index]);
				}
			}
		}

	} // namespace

	MetadataPageRouteDirectory::MetadataPageRouteDirectory(MetadataPageFile &pages) : pages_(pages) {}

	PhysicalPageRef MetadataPageRouteDirectory::buildFromCompleteRouteSet(MetadataWriteBatch &batch,
																					std::span<const MetadataPageRoute> routes) {
		if (routes.empty()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
									"metadata PageId route branch requires a nonempty complete route set");
		}
		uint64_t previousId = 0U;
		for (const MetadataPageRoute &route: routes) {
			validateRoute(route, batch.candidate());
			if (route.page_id.value() <= previousId) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route set must be strictly ordered and unique");
			}
			previousId = route.page_id.value();
		}

		std::vector<Child> children;
		for (size_t begin = 0U; begin < routes.size();) {
			const uint64_t ordinal = leafOrdinal(routes[begin].page_id);
			size_t end = begin;
			while (end < routes.size() && leafOrdinal(routes[end].page_id) == ordinal) {
				++end;
			}
			MetadataPageRouteLeafPage leaf;
			leaf.page_id = batch.allocateMetadataPageId();
			leaf.generation = batch.generation();
			leaf.committed_lsn = batch.checkpointLsn();
			leaf.routes.assign(routes.begin() + static_cast<std::ptrdiff_t>(begin),
								   routes.begin() + static_cast<std::ptrdiff_t>(end));
			const PhysicalPageRef reference = batch.allocatePage();
			pages_.writeMetadataPageRouteLeaf(reference, leaf);
			children.push_back({ordinal, reference});
			begin = end;
		}

		uint16_t level = 0U;
		while (children.size() > 1U) {
			std::vector<Child> parents;
			for (size_t begin = 0U; begin < children.size();) {
				const uint64_t parentOrdinal = children[begin].ordinal / RSS_CATALOG_RADIX_FANOUT;
				size_t end = begin;
				while (end < children.size() && children[end].ordinal / RSS_CATALOG_RADIX_FANOUT == parentOrdinal) {
					++end;
				}
				parents.push_back({parentOrdinal,
								   writeNode(pages_, batch, level,
											 std::span<const Child>(children).subspan(begin, end - begin), false)});
				begin = end;
			}
			children = std::move(parents);
			if (level == RSS_CATALOG_RADIX_MAX_LEVEL) {
				throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
										"metadata PageId route set exceeds the radix identity domain");
			}
			++level;
		}
		return writeNode(pages_, batch, level, children, true);
	}

	std::vector<MetadataPageRoute>
	MetadataPageRouteDirectory::collectRoutesFromPhysicalDirectoryRoots(const Superblock &superblock) const {
		struct Root final {
			PhysicalPageRef reference;
			MetadataDirectoryType directory_type;
		};
		const std::array roots = {
				Root{superblock.catalog_root, MetadataDirectoryType::MDT_CATALOG},
				Root{superblock.allocator_root, MetadataDirectoryType::MDT_ALLOCATOR},
				Root{superblock.page_directory_root, MetadataDirectoryType::MDT_PAGE},
				Root{superblock.extent_directory_root, MetadataDirectoryType::MDT_EXTENT},
				Root{superblock.node_entity_directory_root, MetadataDirectoryType::MDT_NODE_ENTITY},
				Root{superblock.edge_entity_directory_root, MetadataDirectoryType::MDT_EDGE_ENTITY},
				Root{superblock.entity_property_directory_root, MetadataDirectoryType::MDT_ENTITY_PROPERTY},
				Root{superblock.index_root_directory_root, MetadataDirectoryType::MDT_INDEX_ROOT},
				Root{superblock.vector_location_directory_root, MetadataDirectoryType::MDT_VECTOR_LOCATION},
				Root{superblock.vector_manifest_directory_root, MetadataDirectoryType::MDT_VECTOR_MANIFEST},
				Root{superblock.large_value_manifest_directory_root,
					 MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST},
				Root{superblock.state_directory_root, MetadataDirectoryType::MDT_STATE},
				Root{superblock.retire_queue_root, MetadataDirectoryType::MDT_RETIRE_QUEUE},
		};
		std::set<std::pair<uint64_t, uint8_t>> frames;
		std::map<uint64_t, PhysicalPageRef> routes;

		const auto registerPage = [&](MetadataPageId pageId, PhysicalPageRef reference) {
			validateReference(reference, superblock, "metadata PageId route graph has an invalid physical reference");
			if (!pageId.isValid() || !frames.emplace(reference.physical_slot.value(), reference.frame_index).second ||
				!routes.emplace(pageId.value(), reference).second) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
									"metadata PageId route graph has duplicate page identities or frames");
			}
		};
		const auto validateLeafOrdinal = [](uint64_t logicalId, uint64_t ordinal, uint64_t capacity) {
			return logicalId != 0U && (logicalId - 1U) / capacity == ordinal;
		};

		const auto visitDirectory = [&](auto &&self, PhysicalPageRef reference, MetadataDirectoryType directoryType,
											 bool root, uint16_t expectedLevel, uint64_t ordinalBase) -> void {
			validateReference(reference, superblock, "metadata PageId route graph has an invalid directory reference");
			const MetadataRadixPage page = pages_.readMetadataRadix(reference);
			if (page.kind != (root ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL) ||
				page.directory_type != directoryType || (!root && page.level != expectedLevel)) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route graph has an invalid directory radix page");
			}
			validateVisible(page.generation, page.committed_lsn, superblock,
							"metadata PageId route graph has a future directory radix page");
			registerPage(page.page_id, reference);
			const uint64_t factor = radixFactor(page.level);
			for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef child = page.children[index];
				if (!child.isValid()) {
					continue;
				}
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) {
					throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
											"metadata PageId route graph ordinal overflows");
				}
				const uint64_t ordinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				if (page.level != 0U) {
					self(self, child, directoryType, false, static_cast<uint16_t>(page.level - 1U), ordinal);
					continue;
				}
				validateReference(child, superblock, "metadata PageId route graph has an invalid leaf reference");
				if (directoryType == MetadataDirectoryType::MDT_CATALOG) {
					const CatalogLeafPage leaf = pages_.readCatalogLeaf(child);
					validateVisible(leaf.generation, leaf.committed_lsn, superblock,
									"metadata PageId route graph has a future Catalog leaf");
					if (leaf.entries.empty() || !std::ranges::all_of(leaf.entries, [ordinal, &validateLeafOrdinal](const CatalogEntry &entry) {
							return validateLeafOrdinal(entry.segment_id.value(), ordinal, RSS_CATALOG_PAGE_MAX_ENTRIES);
						})) {
						throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
												"metadata PageId route graph has a misrouted Catalog leaf");
					}
					registerPage(leaf.page_id, child);
				} else if (directoryType == MetadataDirectoryType::MDT_ALLOCATOR) {
					const AllocatorLeafPage leaf = pages_.readAllocatorLeaf(child);
					validateVisible(leaf.generation, leaf.committed_lsn, superblock,
									"metadata PageId route graph has a future Allocator leaf");
					if (ordinal > (std::numeric_limits<uint64_t>::max() - 1U) / RSS_ALLOCATOR_PAGE_SLOT_CAPACITY ||
						leaf.first_slot != PhysicalSlotId(ordinal * RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U)) {
						throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
												"metadata PageId route graph has a misrouted Allocator leaf");
					}
					registerPage(leaf.page_id, child);
				} else {
					const LocationLeafPage leaf = pages_.readLocationLeaf(child);
					validateVisible(leaf.generation, leaf.committed_lsn, superblock,
									"metadata PageId route graph has a future location leaf");
					if (leaf.directory_type != directoryType ||
						!std::ranges::all_of(leaf.entries, [ordinal, &validateLeafOrdinal](const LocationLeafEntry &entry) {
							return validateLeafOrdinal(entry.logical_id, ordinal, RSS_LOCATION_LEAF_MAX_ENTRIES);
						})) {
						throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
												"metadata PageId route graph has a misrouted location leaf");
					}
					registerPage(leaf.page_id, child);
				}
			}
		};

		for (const Root &root: roots) {
			if (root.reference.isValid()) {
				visitDirectory(visitDirectory, root.reference, root.directory_type, true, 0U, 0U);
			}
		}
		std::vector<MetadataPageRoute> result;
		result.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			result.push_back({MetadataPageId(pageId), reference});
		}
		return result;
	}

	size_t MetadataPageRouteDirectory::initialBootstrapFrameCount(const Superblock &superblock) const {
		const std::vector<MetadataPageRoute> routes = collectRoutesFromPhysicalDirectoryRoots(superblock);
		return bootstrapFrameCount(routes);
	}

	size_t MetadataPageRouteDirectory::initialBootstrapFrameCount(const MetadataWriteBatch &batch) const {
		std::map<uint64_t, PhysicalPageRef> routeMap;
		for (const MetadataPageRoute &route: collectRoutesFromPhysicalDirectoryRoots(batch.current())) {
			routeMap.emplace(route.page_id.value(), route.physical_reference);
		}
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			routeMap.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> routes;
		routes.reserve(routeMap.size());
		for (const auto &[pageId, reference]: routeMap) {
			routes.push_back({MetadataPageId(pageId), reference});
		}
		return bootstrapFrameCount(routes);
	}

	size_t MetadataPageRouteDirectory::nextBootstrapFrameCount(const MetadataWriteBatch &batch) const {
		if (!batch.candidate().usesPageDirectoryBootstrap() || !batch.candidate().page_directory_root.isValid()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
								  "metadata PageId route preflight requires an existing bootstrap header");
		}
		const PageDirectoryBootstrap previous =
				pages_.readPageDirectoryBootstrap(batch.candidate().page_directory_root);
		if (previous.generation > batch.generation() || previous.committed_lsn > batch.checkpointLsn()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
								  "Page Directory bootstrap is newer than the metadata write batch");
		}
		std::map<uint64_t, PhysicalPageRef> routes;
		for (const MetadataPageRoute &route:
			 collectRoutesFromRouteBranch(batch.candidate(), previous.metadata_page_directory_root)) {
			routes.emplace(route.page_id.value(), route.physical_reference);
		}
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			routes.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> complete;
		complete.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			complete.push_back({MetadataPageId(pageId), reference});
		}
		return bootstrapFrameCount(complete);
	}

	PhysicalPageRef MetadataPageRouteDirectory::buildInitialRouteBranch(MetadataWriteBatch &batch) {
		std::map<uint64_t, PhysicalPageRef> routes;
		for (const MetadataPageRoute &route: collectRoutesFromPhysicalDirectoryRoots(batch.current())) {
			routes.emplace(route.page_id.value(), route.physical_reference);
		}
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			routes.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> complete;
		complete.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			complete.push_back({MetadataPageId(pageId), reference});
		}
		return buildFromCompleteRouteSet(batch, complete);
	}

	std::vector<MetadataPageRoute>
	MetadataPageRouteDirectory::collectRoutesFromRouteBranch(const Superblock &superblock, PhysicalPageRef root) const {
		validateReference(root, superblock, "metadata PageId route branch has an invalid root reference");
		std::set<std::pair<uint64_t, uint8_t>> bootstrapFrames;
		std::set<std::pair<uint64_t, uint8_t>> targetFrames;
		std::map<uint64_t, PhysicalPageRef> routes;

		const auto registerBootstrapFrame = [&bootstrapFrames](PhysicalPageRef reference) {
			if (!bootstrapFrames.emplace(reference.physical_slot.value(), reference.frame_index).second) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
									"metadata PageId route branch aliases or cycles through bootstrap frames");
			}
		};
		const auto registerRoute = [&](const MetadataPageRoute &route) {
			validateRoute(route, superblock);
			if (!targetFrames.emplace(route.physical_reference.physical_slot.value(),
									  route.physical_reference.frame_index)
					 .second ||
				!routes.emplace(route.page_id.value(), route.physical_reference).second) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
									"metadata PageId route branch has duplicate logical pages or physical frames");
			}
		};

		const auto visitNode = [&](auto &&self, PhysicalPageRef reference, bool rootNode,
									  uint16_t expectedLevel, uint64_t ordinalBase) -> void {
			validateReference(reference, superblock, "metadata PageId route branch has an invalid radix reference");
			registerBootstrapFrame(reference);
			const MetadataRadixPage node = pages_.readMetadataRadix(reference);
			if (node.kind != (rootNode ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL) ||
				node.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE ||
				(!rootNode && node.level != expectedLevel)) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route branch has an invalid radix page");
			}
			validateVisible(node.generation, node.committed_lsn, superblock,
							"metadata PageId route branch has a future radix page");
			const uint64_t factor = radixFactor(node.level);
			for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
				const PhysicalPageRef child = node.children[index];
				if (!child.isValid()) {
					continue;
				}
				validateReference(child, superblock, "metadata PageId route branch has an invalid child reference");
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) {
					throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
											"metadata PageId route branch ordinal overflows");
				}
				const uint64_t ordinal = ordinalBase + static_cast<uint64_t>(index) * factor;
				if (node.level != 0U) {
					self(self, child, false, static_cast<uint16_t>(node.level - 1U), ordinal);
					continue;
				}
				registerBootstrapFrame(child);
				const MetadataPageRouteLeafPage leaf = pages_.readMetadataPageRouteLeaf(child);
				validateVisible(leaf.generation, leaf.committed_lsn, superblock,
									"metadata PageId route branch has a future leaf");
				for (const MetadataPageRoute &route: leaf.routes) {
					if (leafOrdinal(route.page_id) != ordinal) {
						throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
												"metadata PageId route leaf does not match its radix route");
					}
					registerRoute(route);
				}
			}
		};
		visitNode(visitNode, root, true, 0U, 0U);
		for (const auto &[pageId, reference]: routes) {
			if (bootstrapFrames.contains({reference.physical_slot.value(), reference.frame_index})) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
									"metadata PageId route target aliases bootstrap plumbing");
			}
		}
		std::vector<MetadataPageRoute> result;
		result.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			result.push_back({MetadataPageId(pageId), reference});
		}
		return result;
	}

	std::vector<PhysicalPageRef>
	MetadataPageRouteDirectory::collectRouteBranchFrames(const Superblock &superblock, PhysicalPageRef root) const {
		// Reuse the complete route traversal first: it validates route targets as
		// well as all radix/leaf ordering and rejects target-to-plumbing aliases.
		static_cast<void>(collectRoutesFromRouteBranch(superblock, root));
		std::set<std::pair<uint64_t, uint8_t>> seen;
		std::vector<PhysicalPageRef> result;
		const auto registerFrame = [&seen, &result](PhysicalPageRef reference) {
			if (!seen.emplace(reference.physical_slot.value(), reference.frame_index).second) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
											"metadata PageId route branch aliases or cycles through bootstrap frames");
			}
			result.push_back(reference);
		};
		const auto visitNode = [&](auto &&self, PhysicalPageRef reference, bool rootNode,
								  uint16_t expectedLevel) -> void {
			validateReference(reference, superblock, "metadata PageId route branch has an invalid radix reference");
			registerFrame(reference);
			const MetadataRadixPage node = pages_.readMetadataRadix(reference);
			if (node.kind != (rootNode ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL) ||
				node.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE ||
				(!rootNode && node.level != expectedLevel)) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
										"metadata PageId route branch has an invalid radix page");
			}
			validateVisible(node.generation, node.committed_lsn, superblock,
							"metadata PageId route branch has a future radix page");
			for (const PhysicalPageRef child: node.children) {
				if (!child.isValid()) {
					continue;
				}
				validateReference(child, superblock, "metadata PageId route branch has an invalid child reference");
				if (node.level != 0U) {
					self(self, child, false, static_cast<uint16_t>(node.level - 1U));
					continue;
				}
				registerFrame(child);
				const MetadataPageRouteLeafPage leaf = pages_.readMetadataPageRouteLeaf(child);
				validateVisible(leaf.generation, leaf.committed_lsn, superblock,
								"metadata PageId route branch has a future leaf");
			}
		};
		visitNode(visitNode, root, true, 0U);
		return result;
	}

	PhysicalPageRef MetadataPageRouteDirectory::buildNextRouteBranch(MetadataWriteBatch &batch,
																	PhysicalPageRef currentRoot) {
		std::map<uint64_t, PhysicalPageRef> routes;
		for (const MetadataPageRoute &route: collectRoutesFromRouteBranch(batch.candidate(), currentRoot)) {
			routes.emplace(route.page_id.value(), route.physical_reference);
		}
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			routes.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> complete;
		complete.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			complete.push_back({MetadataPageId(pageId), reference});
		}
		return buildFromCompleteRouteSet(batch, complete);
	}

	PhysicalPageRef MetadataPageRouteDirectory::publishNextBootstrap(MetadataWriteBatch &batch,
																 PhysicalPageRef dataRoot) {
		if (!batch.candidate().usesPageDirectoryBootstrap() || !batch.candidate().page_directory_root.isValid()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
									"metadata PageId route publication requires an existing bootstrap header");
		}
		const PageDirectoryBootstrap previous = pages_.readPageDirectoryBootstrap(batch.candidate().page_directory_root);
		if (previous.generation > batch.generation() || previous.committed_lsn > batch.checkpointLsn()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
									"Page Directory bootstrap is newer than the metadata write batch");
		}
		PageDirectoryBootstrap replacement = previous;
		replacement.generation = batch.generation();
		replacement.committed_lsn = batch.checkpointLsn();
		if (dataRoot.isValid()) {
			replacement.data_page_directory_root = dataRoot;
		}
		replacement.metadata_page_directory_root = buildNextRouteBranch(batch, previous.metadata_page_directory_root);
		std::map<uint64_t, PhysicalPageRef> routes;
		for (const MetadataPageRoute &route:
			 collectRoutesFromRouteBranch(batch.candidate(), previous.metadata_page_directory_root)) {
			routes.emplace(route.page_id.value(), route.physical_reference);
		}
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			routes.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> complete;
		complete.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			complete.push_back({MetadataPageId(pageId), reference});
		}
		assignLogicalRoots(replacement, batch.candidate(), complete);
		const PhysicalPageRef reference = batch.allocatePage();
		pages_.writePageDirectoryBootstrap(reference, replacement);
		batch.setPageDirectoryBootstrapRoot(reference);
		batch.markBootstrapRoutePublished();
		return reference;
	}

	PhysicalPageRef MetadataPageRouteDirectory::publishIdentityOnlyBootstrap(MetadataWriteBatch &batch) {
		if (!batch.candidate().usesPageDirectoryBootstrap() || !batch.candidate().page_directory_root.isValid() ||
			!batch.metadataPageRoutes().empty() || batch.pendingBootstrapDataRoot().has_value() ||
			batch.bootstrapRouteRefreshRequested()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
								"identity-only bootstrap publication has pending route changes");
		}
		PageDirectoryBootstrap header = pages_.readPageDirectoryBootstrap(batch.candidate().page_directory_root);
		if (header.generation > batch.generation() || header.committed_lsn > batch.checkpointLsn() ||
			!header.metadata_page_directory_root.isValid()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
								"identity-only bootstrap publication has an invalid prior header");
		}
		header.generation = batch.generation();
		header.committed_lsn = batch.checkpointLsn();
		const PhysicalPageRef reference = batch.allocatePage();
		pages_.writePageDirectoryBootstrap(reference, header);
		batch.setPageDirectoryBootstrapRoot(reference);
		batch.markBootstrapRoutePublished();
		return reference;
	}

	PhysicalPageRef MetadataPageRouteDirectory::prepareInitialBootstrapMigration(MetadataWriteBatch &batch) {
		const std::vector<MetadataPageRoute> routes = collectRoutesFromPhysicalDirectoryRoots(batch.current());
		PageDirectoryBootstrap bootstrap;
		bootstrap.generation = batch.generation();
		bootstrap.committed_lsn = batch.checkpointLsn();
		bootstrap.data_page_directory_root = batch.candidate().page_directory_root;
		std::map<uint64_t, PhysicalPageRef> completeRoutes;
		for (const MetadataPageRoute &route: routes) {
			completeRoutes.emplace(route.page_id.value(), route.physical_reference);
		}
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			completeRoutes.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> complete;
		complete.reserve(completeRoutes.size());
		for (const auto &[pageId, reference]: completeRoutes) {
			complete.push_back({MetadataPageId(pageId), reference});
		}
		assignLogicalRoots(bootstrap, batch.candidate(), complete);
		bootstrap.metadata_page_directory_root = buildFromCompleteRouteSet(batch, complete);
		const PhysicalPageRef reference = batch.allocatePage();
		pages_.writePageDirectoryBootstrap(reference, bootstrap);
		batch.setPageDirectoryBootstrapRoot(reference);
		batch.markBootstrapRoutePublished();
		return reference;
	}

	PhysicalPageRef MetadataPageRouteDirectory::find(const Superblock &superblock, PhysicalPageRef root,
																			 MetadataPageId pageId) const {
		if (!root.isValid() || !pageId.isValid()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route lookup requires a root and an identity");
		}
		MetadataRadixPage node = pages_.readMetadataRadix(root);
		if (node.kind != CatalogRadixPageKind::CRPK_ROOT ||
			node.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route root has an invalid type");
		}
		validateVisible(node.generation, node.committed_lsn, superblock,
						"metadata PageId route root is newer than its Superblock");
		const uint64_t ordinal = leafOrdinal(pageId);
		for (;;) {
			const std::optional<PhysicalPageRef> child = CatalogRadixPageCodec::findChildForLeafOrdinal(node, ordinal);
			if (!child.has_value()) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "metadata PageId route is missing");
			}
			if (node.level == 0U) {
				const MetadataPageRouteLeafPage leaf = pages_.readMetadataPageRouteLeaf(*child);
				validateVisible(leaf.generation, leaf.committed_lsn, superblock,
								"metadata PageId route leaf is newer than its Superblock");
				for (const MetadataPageRoute &route: leaf.routes) {
					if (leafOrdinal(route.page_id) != ordinal) {
						throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
											"metadata PageId route leaf is inconsistent with its radix path");
					}
					if (route.page_id == pageId) {
						validateRoute(route, superblock);
						return route.physical_reference;
					}
				}
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "metadata PageId route is missing");
			}
			node = pages_.readMetadataRadix(*child);
			if (node.kind != CatalogRadixPageKind::CRPK_INTERNAL ||
				node.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
										"metadata PageId route has an invalid radix child");
			}
			validateVisible(node.generation, node.committed_lsn, superblock,
								"metadata PageId route child is newer than its Superblock");
		}
	}

} // namespace graph::storage
