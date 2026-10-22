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
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/utils/OwnedAssociativeContainers.hpp"

namespace graph::storage {

	namespace {

        // The immutable page view already passed full codec validation at decode.
        // Authenticate its type/visibility at the caller; inspect only the requested
        // child here rather than revalidating every sibling for every lookup.
        std::optional<PhysicalPageRef> findDecodedRouteChild(const MetadataRadixPage &page, uint64_t ordinal) {
            if (page.kind == CatalogRadixPageKind::CRPK_ROOT) {
                uint64_t quotient = ordinal;
                for (uint16_t level = 0U; level <= page.level; ++level) quotient /= RSS_CATALOG_RADIX_FANOUT;
                if (quotient != 0U) return std::nullopt;
            }
            const auto child = page.children[CatalogRadixPageCodec::childIndexForLeafOrdinal(ordinal, page.level)];
            return child.isValid() ? std::optional<PhysicalPageRef>(child) : std::nullopt;
        }

		struct ReusableRouteLeaf final {
			PhysicalPageRef reference;
			size_t entries = 0U;
			bool unchanged = false;
		};

		// Per-call, charged work tables, never a persistent or snapshot cache.
		struct RouteBranchReuse final {
			utils::OwnedMap<uint64_t, ReusableRouteLeaf> leaves;
			utils::OwnedMap<std::pair<uint16_t, uint64_t>, PhysicalPageRef> nodes;
			PhysicalPageRef root;
			bool enabled = true;

			void disable() {
				leaves.clear();
				nodes.clear();
				root = {};
				enabled = false;
				debug::PerfTrace::addValue("native_graph.route.reuse_admission_fallback", 1);
			}

			template<class Map, class Key, class Value>
			void remember(Map &map, Key key, Value value) {
				if (!enabled) return;
				try { map.emplace(key, value); }
				catch (const query::execution::detail::ResourceAdmissionDenied &) {
					// Optional reuse must not turn index admission into a write failure.
					disable();
				}
			}
		};

		std::vector<MetadataPageRoute> collectRouteBranch(MetadataPageFile &pages, const Superblock &superblock,
			PhysicalPageRef root, RouteBranchReuse *reuse, std::span<const MetadataPageRoute> pending);

		template<class Visitor>
		void visitBatchBaseRoutes(MetadataPageFile &pages, const MetadataWriteBatch &batch,
								 PhysicalPageRef root, Visitor &&visitor, RouteBranchReuse *reuse = nullptr) {
			debug::PerfTrace::addValue("native_graph.route.publication_base_traversals", 1);
			MetadataPageRouteDirectory directory(pages);
			if (batch.metadataRoutePruningRequested()) {
				// Resolve logical children through the still-published bootstrap.
				// Substitute only its physical data-directory root for traversal.
				auto physical = batch.current();
				const auto header = pages.readPageDirectoryBootstrap(physical.page_directory_root);
				physical.page_directory_root = header.data_page_directory_root;
				for (const auto &route : directory.collectRoutesFromPhysicalDirectoryRoots(physical)) visitor(route);
			} else {
				if (reuse != nullptr && !batch.bootstrapRouteRefreshRequested()) {
					const auto pending = batch.metadataPageRoutes();
					try {
						for (const auto &route : collectRouteBranch(pages, batch.candidate(), root, reuse, pending)) visitor(route);
					} catch (const query::execution::detail::ResourceAdmissionDenied &) {
						// The optional table can also crowd a later mandatory codec read.
						// Refund it and validate the source again through the original path.
						if (!reuse->enabled) throw;
						reuse->disable();
						for (const auto &route : directory.collectRoutesFromRouteBranch(batch.candidate(), root)) visitor(route);
					}
				} else {
					for (const auto &route : directory.collectRoutesFromRouteBranch(batch.candidate(), root)) visitor(route);
				}
			}
		}

		[[noreturn]] void throwRouteDirectoryError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint64_t leafOrdinal(MetadataPageId pageId) {
			if (!pageId.isValid()) { // route leaf codecs reject zero logical identities before traversal.
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
			if (!generation.isValid() || !lsn.isValid() || generation > superblock.generation || // metadata page codecs require valid generation and LSN fields.
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
				if (factor > std::numeric_limits<uint64_t>::max() / RSS_CATALOG_RADIX_FANOUT) { // the radix codec bounds levels to the fixed identity domain.
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
			bool changed = false;
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
				if (!child.reference.isValid() || page.children[index].isValid()) { // buildFromCompleteRouteSet emits unique valid children.
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
				if (count > std::numeric_limits<size_t>::max() - frames) { // route vectors and frame counts share the size_t allocation bound.
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


	namespace {
		PhysicalPageRef buildRouteBranch(MetadataPageFile &pages, MetadataWriteBatch &batch,
			std::span<const MetadataPageRoute> routes, const RouteBranchReuse *reuse) {
			if (routes.empty()) throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
				"metadata PageId route branch requires a nonempty complete route set");
			uint64_t previousId = 0U;
			for (const auto &route : routes) {
				validateRoute(route, batch.candidate());
				if (route.page_id.value() <= previousId) throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
					"metadata PageId route set must be strictly ordered and unique");
				previousId = route.page_id.value();
			}
			const auto writeOrReuse = [&](uint16_t level, uint64_t ordinal,
				std::span<const Child> children, bool rootNode) -> Child {
				PhysicalPageRef previous;
				if (reuse != nullptr && reuse->enabled &&
					std::ranges::none_of(children, [](const Child &child) { return child.changed; })) {
					if (rootNode) previous = reuse->root;
					else if (const auto found = reuse->nodes.find({level, ordinal}); found != reuse->nodes.end())
						previous = found->second;
				}
				if (previous.isValid()) {
					const auto old = pages.readMetadataRadix(previous);
					std::array<PhysicalPageRef, RSS_CATALOG_RADIX_FANOUT> expected{};
					for (const auto &child : children) expected[child.ordinal % RSS_CATALOG_RADIX_FANOUT] = child.reference;
					if (old.kind == (rootNode ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL) &&
						old.directory_type == MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE && old.level == level &&
						!old.logical_children_enabled && old.children == expected) {
						debug::PerfTrace::addValue("native_graph.route.reused_radix_nodes", 1);
						return {ordinal, previous, false};
					}
				}
				return {ordinal, writeNode(pages, batch, level, children, rootNode), true};
			};
			std::vector<Child> children;
			for (size_t begin = 0U; begin < routes.size();) {
				const uint64_t ordinal = leafOrdinal(routes[begin].page_id);
				size_t end = begin;
				while (end < routes.size() && leafOrdinal(routes[end].page_id) == ordinal) ++end;
				PhysicalPageRef reference;
				bool changed = true;
				if (reuse != nullptr && reuse->enabled) {
					const auto found = reuse->leaves.find(ordinal);
					if (found != reuse->leaves.end() && found->second.unchanged && found->second.entries == end - begin) {
						reference = found->second.reference;
						changed = false;
						debug::PerfTrace::addValue("native_graph.route.reused_leaves", 1);
					}
				}
				if (changed) {
					MetadataPageRouteLeafPage leaf;
					leaf.page_id = batch.allocateMetadataPageId();
					leaf.generation = batch.generation();
					leaf.committed_lsn = batch.checkpointLsn();
					leaf.routes.assign(routes.begin() + static_cast<std::ptrdiff_t>(begin),
						routes.begin() + static_cast<std::ptrdiff_t>(end));
					reference = batch.allocatePage();
					pages.writeMetadataPageRouteLeaf(reference, leaf);
				}
				children.push_back({ordinal, reference, changed});
				begin = end;
			}
			uint16_t level = 0U;
			while (children.size() > 1U) {
				std::vector<Child> parents;
				for (size_t begin = 0U; begin < children.size();) {
					const uint64_t ordinal = children[begin].ordinal / RSS_CATALOG_RADIX_FANOUT;
					size_t end = begin;
					while (end < children.size() && children[end].ordinal / RSS_CATALOG_RADIX_FANOUT == ordinal) ++end;
					parents.push_back(writeOrReuse(level, ordinal,
						std::span<const Child>(children).subspan(begin, end - begin), false));
					begin = end;
				}
				children = std::move(parents);
				if (level == RSS_CATALOG_RADIX_MAX_LEVEL) throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
					"metadata PageId route set exceeds the radix identity domain");
				++level;
			}
			return writeOrReuse(level, 0U, children, true).reference;
		}
	} // namespace

		// Planning visits only changed paths. It owns private decoded images, never
		// mutates published pages, and allocates no persistent identity or frame.
		class MetadataRouteUpdatePlan final {
			struct Link {
				PhysicalPageRef reference{};
				size_t planned = std::numeric_limits<size_t>::max();
			};
			struct ChildPlan { uint32_t slot; size_t child; };
			struct Frame {
				using OwnedHeapElement = void;
				MetadataRadixPage radix;
				MetadataPageRouteLeafPage leaf;
				utils::OwnedStdVector<ChildPlan> children;
                PhysicalPageRef source{};
                utils::OwnedVector<PhysicalPageRef> replaced_targets;
				bool is_leaf = false;
			};
		public:
			MetadataRouteUpdatePlan(MetadataPageFile &pages, const MetadataWriteBatch &batch, PhysicalPageRef root)
				: pages_(pages), batch_(batch), source_root_(root), source_epoch_(pages.storageIO()->dataMutationEpoch()) {
				const auto pending = batch.metadataPageRoutes();
				source_routes_.assign(pending.begin(), pending.end());
				utils::OwnedSet<std::pair<uint64_t, uint8_t>> targets;
				for (const auto &route : pending) {
					validateRoute(route, batch.candidate());
					if (!batch.ownsAllocatedPage(route.physical_reference) ||
						!targets.emplace(route.physical_reference.physical_slot.value(), route.physical_reference.frame_index).second)
						throwRouteDirectoryError(FormatError::FERR_INVALID_STATE,
							"incremental metadata routes require unique batch-owned targets");
				}
				const auto old = readNode(root, true, std::nullopt);
				uint16_t level = old->level;
				if (!pending.empty()) {
					const uint64_t last = leafOrdinal(pending.back().page_id);
					while (last / radixFactor(level) >= RSS_CATALOG_RADIX_FANOUT) {
						if (level == RSS_CATALOG_RADIX_MAX_LEVEL)
							throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								"incremental metadata route growth exceeds its identity domain");
						++level;
					}
				}
				root_ = grow(root, *old, level, 0U, pending, true);
			}

            [[nodiscard]] bool matches(MetadataPageFile &pages, const MetadataWriteBatch &batch,
                PhysicalPageRef root) const {
                if (&pages != &pages_ || &batch != &batch_ || root != source_root_ ||
                    source_epoch_ == std::numeric_limits<uint64_t>::max() ||
                    pages.storageIO()->dataMutationEpoch() != source_epoch_) return false;
                const auto pending = batch.metadataPageRoutes();
                return pending.size() == source_routes_.size() &&
                    std::equal(pending.begin(), pending.end(), source_routes_.begin());
            }
			[[nodiscard]] size_t frameCount() const {
				if (frames_.size() == std::numeric_limits<size_t>::max())
					throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW, "incremental route frame count overflows");
				return frames_.size() + 1U; // Always reserve the bootstrap header.
			}

			PhysicalPageRef materialize(MetadataWriteBatch &batch) {
				utils::OwnedStdVector<PhysicalPageRef> written;
				written.reserve(frames_.size());
				for (const auto &frame : frames_) {
					const auto id = batch.allocateMetadataPageId();
					const auto reference = batch.allocatePage();
					if (frame->is_leaf) {
						frame->leaf.page_id = id;
						frame->leaf.generation = batch.generation();
						frame->leaf.committed_lsn = batch.checkpointLsn();
						pages_.writeMetadataPageRouteLeaf(reference, frame->leaf);
					} else {
						for (const auto &[slot, child] : frame->children) frame->radix.children[slot] = written.at(child);
						frame->radix.page_id = id;
						frame->radix.generation = batch.generation();
						frame->radix.committed_lsn = batch.checkpointLsn();
						pages_.writeMetadataRadix(reference, frame->radix);
					}
                    batch.retainBootstrapFrame(reference);
                    if (frame->source.isValid()) batch.retireMetadataFrame(frame->source);
                    for (const auto target : frame->replaced_targets) batch.retireMetadataFrame(target);
					written.push_back(reference);
				}
				debug::PerfTrace::addValue("native_graph.route.incremental_written_frames", static_cast<int64_t>(written.size()));
				return root_.planned == std::numeric_limits<size_t>::max() ? root_.reference : written.at(root_.planned);
			}

		private:
			std::shared_ptr<const MetadataRadixPage> readNode(PhysicalPageRef reference, bool root, std::optional<uint16_t> level) {
				validateReference(reference, batch_.candidate(), "incremental metadata route has an invalid source reference");
				debug::PerfTrace::addValue("native_graph.route.incremental_source_frames", 1);
				auto node = std::allocate_shared<MetadataRadixPage>(utils::OwnedPmrAllocator<MetadataRadixPage>{},
                    pages_.readMetadataRadix(reference));
				if (node->kind != (root ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL) ||
					node->directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE || node->logical_children_enabled ||
					(level && node->level != *level))
					throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER, "incremental metadata route has an invalid source node");
				validateVisible(node->generation, node->committed_lsn, batch_.candidate(),
					"incremental metadata route source is not visible");
				utils::OwnedSet<std::pair<uint64_t, uint8_t>> siblings;
				for (const auto child : node->children) if (child.isValid()) {
					validateReference(child, batch_.candidate(), "incremental route child is not visible");
					if (!siblings.emplace(child.physical_slot.value(), child.frame_index).second || child == reference)
						throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "incremental route source has aliased children");
				}
				return node;
			}

			Link append(std::shared_ptr<Frame> frame) {
				const auto index = frames_.size(); frames_.push_back(std::move(frame));
				return {{}, index};
			}

			Link leaf(PhysicalPageRef reference, uint64_t ordinal, std::span<const MetadataPageRoute> updates) {
				auto frame = std::allocate_shared<Frame>(utils::OwnedPmrAllocator<Frame>{}); frame->is_leaf = true; frame->source = reference;
				if (reference.isValid()) {
					validateReference(reference, batch_.candidate(), "incremental route leaf is not visible");
					debug::PerfTrace::addValue("native_graph.route.incremental_source_frames", 1);
					const auto old = pages_.readMetadataPageRouteLeafView(reference);
					validateVisible(old->generation, old->committed_lsn, batch_.candidate(), "incremental route leaf is not visible");
					for (const auto &route : old->routes) {
						if (leafOrdinal(route.page_id) != ordinal)
							throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER, "incremental route leaf is misrouted");
						validateRoute(route, batch_.candidate());
					}
					frame->leaf = *old;
				}
				bool changed = !reference.isValid();
				for (const auto &update : updates) {
					auto &routes = frame->leaf.routes;
					const auto found = std::lower_bound(routes.begin(), routes.end(), update.page_id.value(),
						[](const auto &route, uint64_t id) { return route.page_id.value() < id; });
					if (found != routes.end() && found->page_id == update.page_id) {
						if (found->physical_reference != update.physical_reference) {
                            frame->replaced_targets.push_back(found->physical_reference);
                            *found = update; changed = true;
                        }
					} else { routes.insert(found, update); changed = true; }
				}
				if (!changed) return {reference};
				return append(std::move(frame));
			}

			Link grow(PhysicalPageRef reference, const MetadataRadixPage &old, uint16_t level, uint64_t base,
				std::span<const MetadataPageRoute> updates, bool root) {
				if (level == old.level) return node(reference, old, base, updates, root);
                auto frame = std::allocate_shared<Frame>(utils::OwnedPmrAllocator<Frame>{});
                frame->radix.kind = root ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL;
                frame->radix.directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE;
                frame->radix.level = level;
				const uint64_t factor = radixFactor(level);
				size_t begin = 0U;
				// Existing root is always the zero-prefix subtree. Its kind must
				// change to INTERNAL in a private image, including when untouched.
				size_t end = 0U;
				while (end < updates.size() && leafOrdinal(updates[end].page_id) / factor == 0U) ++end;
				const auto previous = grow(reference, old, static_cast<uint16_t>(level - 1U), base, updates.first(end), false);
				frame->children.push_back({0U, previous.planned});
				begin = end;
				while (begin < updates.size()) {
					const uint64_t ordinal = leafOrdinal(updates[begin].page_id);
					const auto slot = static_cast<uint32_t>((ordinal / factor) % RSS_CATALOG_RADIX_FANOUT);
					end = begin + 1U;
					while (end < updates.size() && leafOrdinal(updates[end].page_id) / factor == ordinal / factor) ++end;
					auto blank = std::allocate_shared<MetadataRadixPage>(utils::OwnedPmrAllocator<MetadataRadixPage>{});
                    blank->kind = CatalogRadixPageKind::CRPK_INTERNAL;
                    blank->directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE; blank->level = level - 1U;
					const auto child = node({}, *blank, base + static_cast<uint64_t>(slot) * factor,
						updates.subspan(begin, end - begin), false);
					frame->children.push_back({slot, child.planned}); begin = end;
				}
				return append(std::move(frame));
			}

			Link node(PhysicalPageRef reference, const MetadataRadixPage &old, uint64_t base,
				std::span<const MetadataPageRoute> updates, bool root) {
                const auto outputKind = root ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL;
                if (updates.empty() && reference.isValid() && old.kind == outputKind) return {reference};
				auto frame = std::allocate_shared<Frame>(utils::OwnedPmrAllocator<Frame>{}); frame->radix = old; frame->source = reference;
				frame->radix.kind = root ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL;
				bool changed = !reference.isValid() || frame->radix.kind != old.kind;
				const uint64_t factor = radixFactor(old.level);
				for (size_t begin = 0U; begin < updates.size();) {
					const uint64_t ordinal = leafOrdinal(updates[begin].page_id);
					if (ordinal < base || (ordinal - base) / factor >= RSS_CATALOG_RADIX_FANOUT)
						throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "incremental route update is outside its source range");
					const auto slot = static_cast<uint32_t>((ordinal - base) / factor);
					size_t end = begin + 1U;
					while (end < updates.size() && (leafOrdinal(updates[end].page_id) - base) / factor == slot) ++end;
					const auto input = updates.subspan(begin, end - begin);
					const auto childRef = old.children[slot];
					Link child;
					if (old.level == 0U) child = leaf(childRef, base + slot, input);
					else {
                        std::shared_ptr<const MetadataRadixPage> source;
                        if (childRef.isValid()) source = readNode(childRef, false, old.level - 1U);
                        else {
                            auto blank = std::allocate_shared<MetadataRadixPage>(utils::OwnedPmrAllocator<MetadataRadixPage>{});
                            blank->kind = CatalogRadixPageKind::CRPK_INTERNAL;
                            blank->directory_type = MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE; blank->level = old.level - 1U;
                            source = std::move(blank);
                        }
                        child = node(childRef, *source, base + static_cast<uint64_t>(slot) * factor, input, false);
					}
					if (child.planned != std::numeric_limits<size_t>::max()) {
						frame->children.push_back({slot, child.planned}); changed = true;
					}
					begin = end;
				}
				return changed ? append(std::move(frame)) : Link{reference};
			}
			MetadataPageFile &pages_;
			const MetadataWriteBatch &batch_;
			utils::OwnedStdVector<std::shared_ptr<Frame>> frames_;
			Link root_;
            PhysicalPageRef source_root_;
            uint64_t source_epoch_;
            utils::OwnedVector<MetadataPageRoute> source_routes_;
		};

    std::shared_ptr<MetadataRouteUpdatePlan> MetadataPageRouteDirectory::incrementalPlan(
        const MetadataWriteBatch &batch, PhysicalPageRef root) const {
        if (!incremental_plan_ || !incremental_plan_->matches(pages_, batch, root)) {
            incremental_plan_.reset();
            incremental_plan_ = std::allocate_shared<MetadataRouteUpdatePlan>(
                utils::OwnedPmrAllocator<MetadataRouteUpdatePlan>{}, pages_, batch, root);
            debug::PerfTrace::addValue("native_graph.route.incremental_plans_built", 1);
        } else debug::PerfTrace::addValue("native_graph.route.incremental_plans_reused", 1);
        return incremental_plan_;
    }


	PhysicalPageRef MetadataPageRouteDirectory::buildFromCompleteRouteSet(MetadataWriteBatch &batch,
		std::span<const MetadataPageRoute> routes) {
		return buildRouteBranch(pages_, batch, routes, nullptr);
	}

	utils::OwnedStdVector<MetadataPageRoute>
	MetadataPageRouteDirectory::collectRoutesFromPhysicalDirectoryRoots(const Superblock &superblock) const {
		debug::ScopedPerfTimer timer("native_graph.route.collect_physical_roots");
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
		utils::OwnedSet<std::pair<uint64_t, uint8_t>> frames;
		utils::OwnedMap<uint64_t, PhysicalPageRef> routes;

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
				if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) { // codec-bounded radix paths stay inside the logical ordinal domain.
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
					if (ordinal > (std::numeric_limits<uint64_t>::max() - 1U) / RSS_ALLOCATOR_PAGE_SLOT_CAPACITY || // allocator radix ordinals are bounded by the typed slot domain.
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
		utils::OwnedStdVector<MetadataPageRoute> result;
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
		debug::ScopedPerfTimer timer("native_graph.route.count_bootstrap_frames");
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
		if (!batch.bootstrapRouteRefreshRequested() && !batch.metadataRoutePruningRequested())
			return incrementalPlan(batch, previous.metadata_page_directory_root)->frameCount();
        incremental_plan_.reset();
		std::map<uint64_t, PhysicalPageRef> routes;
		visitBatchBaseRoutes(pages_, batch, previous.metadata_page_directory_root, [&](const MetadataPageRoute &route) {
			routes.emplace(route.page_id.value(), route.physical_reference);
		});
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

	namespace {
		std::vector<MetadataPageRoute> collectRouteBranch(MetadataPageFile &pages, const Superblock &superblock,
			PhysicalPageRef root, RouteBranchReuse *reuse, std::span<const MetadataPageRoute> pending) {
			debug::ScopedPerfTimer timer("native_graph.route.collect_route_branch");
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
				const MetadataRadixPage node = pages.readMetadataRadix(reference);
				if (node.kind != (rootNode ? CatalogRadixPageKind::CRPK_ROOT : CatalogRadixPageKind::CRPK_INTERNAL) ||
					node.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE ||
					(!rootNode && node.level != expectedLevel)) {
					throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
										"metadata PageId route branch has an invalid radix page");
				}
				validateVisible(node.generation, node.committed_lsn, superblock,
								"metadata PageId route branch has a future radix page");
				if (reuse != nullptr && reuse->enabled) {
					if (rootNode) reuse->root = reference;
					else reuse->remember(reuse->nodes,
						std::pair{node.level, ordinalBase / radixFactor(node.level) / RSS_CATALOG_RADIX_FANOUT}, reference);
				}
				const uint64_t factor = radixFactor(node.level);
				for (uint32_t index = 0U; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
					const PhysicalPageRef child = node.children[index];
					if (!child.isValid()) {
						continue;
					}
					validateReference(child, superblock, "metadata PageId route branch has an invalid child reference");
					if (index != 0U && factor > (std::numeric_limits<uint64_t>::max() - ordinalBase) / index) { // codec-bounded radix paths stay inside the logical ordinal domain.
						throwRouteDirectoryError(FormatError::FERR_ARITHMETIC_OVERFLOW,
												"metadata PageId route branch ordinal overflows");
					}
					const uint64_t ordinal = ordinalBase + static_cast<uint64_t>(index) * factor;
					if (node.level != 0U) {
						self(self, child, false, static_cast<uint16_t>(node.level - 1U), ordinal);
						continue;
					}
					registerBootstrapFrame(child);
					const auto leaf = pages.readMetadataPageRouteLeafView(child);
					validateVisible(leaf->generation, leaf->committed_lsn, superblock,
										"metadata PageId route branch has a future leaf");
					for (const MetadataPageRoute &route: leaf->routes) {
						if (leafOrdinal(route.page_id) != ordinal) {
							throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
													"metadata PageId route leaf does not match its radix route");
						}
						registerRoute(route);
					}
					if (reuse != nullptr && reuse->enabled) {
						bool unchanged = true;
						auto update = std::lower_bound(pending.begin(), pending.end(), ordinal,
							[](const MetadataPageRoute &route, uint64_t value) { return leafOrdinal(route.page_id) < value; });
						for (; update != pending.end() && leafOrdinal(update->page_id) == ordinal; ++update) {
							const auto found = std::lower_bound(leaf->routes.begin(), leaf->routes.end(), update->page_id.value(),
								[](const MetadataPageRoute &route, uint64_t id) { return route.page_id.value() < id; });
							if (found == leaf->routes.end() || found->page_id != update->page_id ||
								found->physical_reference != update->physical_reference) { unchanged = false; break; }
						}
						reuse->remember(reuse->leaves, ordinal, ReusableRouteLeaf{child, leaf->routes.size(), unchanged});
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
	}

	std::vector<MetadataPageRoute>
	MetadataPageRouteDirectory::collectRoutesFromRouteBranch(const Superblock &superblock, PhysicalPageRef root) const {
		return collectRouteBranch(pages_, superblock, root, nullptr, {});
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
				const auto leaf = pages_.readMetadataPageRouteLeafView(child);
				validateVisible(leaf->generation, leaf->committed_lsn, superblock,
								"metadata PageId route branch has a future leaf");
			}
		};
		visitNode(visitNode, root, true, 0U);
		return result;
	}

	PhysicalPageRef MetadataPageRouteDirectory::buildNextRouteBranch(MetadataWriteBatch &batch,
																	PhysicalPageRef currentRoot) {
		if (!batch.bootstrapRouteRefreshRequested() && !batch.metadataRoutePruningRequested()) {
			auto plan = incrementalPlan(batch, currentRoot);
            incremental_plan_.reset();
            return plan->materialize(batch);
		}
        incremental_plan_.reset();
		RouteBranchReuse reuse;
		std::map<uint64_t, PhysicalPageRef> routes;
		visitBatchBaseRoutes(pages_, batch, currentRoot, [&](const MetadataPageRoute &route) {
			routes.emplace(route.page_id.value(), route.physical_reference);
		}, &reuse);
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			routes.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> complete;
		complete.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			complete.push_back({MetadataPageId(pageId), reference});
		}
		return buildRouteBranch(pages_, batch, complete, &reuse);
	}

	PhysicalPageRef MetadataPageRouteDirectory::publishNextBootstrap(MetadataWriteBatch &batch,
																 PhysicalPageRef dataRoot) {
		debug::ScopedPerfTimer timer("native_graph.route.publish_bootstrap");
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
		if (!batch.bootstrapRouteRefreshRequested() && !batch.metadataRoutePruningRequested()) {
			auto plan = incrementalPlan(batch, previous.metadata_page_directory_root);
            incremental_plan_.reset();
			replacement.metadata_page_directory_root = plan->materialize(batch);
			const std::array roots = {batch.candidate().catalog_root, batch.candidate().allocator_root,
				batch.candidate().extent_directory_root, batch.candidate().node_entity_directory_root,
				batch.candidate().edge_entity_directory_root, batch.candidate().entity_property_directory_root,
				batch.candidate().index_root_directory_root, batch.candidate().vector_location_directory_root,
				batch.candidate().vector_manifest_directory_root, batch.candidate().large_value_manifest_directory_root,
				batch.candidate().state_directory_root, batch.candidate().retire_queue_root};
			static_assert(roots.size() == RSS_METADATA_DIRECTORY_ROOT_COUNT);
			for (size_t i = 0U; i < roots.size(); ++i) {
				if (!roots[i].isValid()) { replacement.metadata_directory_roots[i] = {}; continue; }
				const auto rootPage = pages_.readMetadataRadixUnresolved(roots[i]);
				const auto id = rootPage.page_id;
				if (find(batch.candidate(), replacement.metadata_page_directory_root, id) != roots[i])
					throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "incremental bootstrap root has no matching route");
				replacement.metadata_directory_roots[i] = id;
			}
			const auto reference = batch.allocatePage();
            batch.retireMetadataFrame(batch.candidate().page_directory_root);
            batch.retainBootstrapFrame(reference);
			pages_.writePageDirectoryBootstrap(reference, replacement);
			batch.setPageDirectoryBootstrapRoot(reference); batch.markBootstrapRoutePublished();
            batch.markMetadataRetirementComplete();
			return reference;
		}
        incremental_plan_.reset();
		RouteBranchReuse reuse;
		std::map<uint64_t, PhysicalPageRef> routes;
		visitBatchBaseRoutes(pages_, batch, previous.metadata_page_directory_root, [&](const MetadataPageRoute &route) {
			routes.emplace(route.page_id.value(), route.physical_reference);
		}, &reuse);
		for (const MetadataPageRoute &route: batch.metadataPageRoutes()) {
			routes.insert_or_assign(route.page_id.value(), route.physical_reference);
		}
		std::vector<MetadataPageRoute> complete;
		complete.reserve(routes.size());
		for (const auto &[pageId, reference]: routes) {
			complete.push_back({MetadataPageId(pageId), reference});
		}
		// Route-branch pages are physical bootstrap plumbing, so building them
		// does not add routed targets. Reuse this validated complete set for the
		// logical root identities instead of collecting the published trees twice.
		replacement.metadata_page_directory_root = buildRouteBranch(pages_, batch, complete, &reuse);
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
        batch.retireMetadataFrame(batch.candidate().page_directory_root);
        batch.retainBootstrapFrame(reference);
		pages_.writePageDirectoryBootstrap(reference, header);
		batch.setPageDirectoryBootstrapRoot(reference);
		batch.markBootstrapRoutePublished();
        batch.markMetadataRetirementComplete();
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

	void MetadataPageRouteDirectory::findMany(const Superblock &superblock, PhysicalPageRef root,
			const std::span<const MetadataPageId> pageIds, const std::span<PhysicalPageRef> results) const {
		if (!root.isValid() || pageIds.size() != results.size() || pageIds.size() > RSS_CATALOG_RADIX_FANOUT ||
			std::ranges::any_of(pageIds, [](MetadataPageId id) { return !id.isValid(); })) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
					"metadata PageId route batch requires a root, valid identities, and bounded equal spans");
		}
		if (pageIds.empty()) return;
		auto *context = MetadataPageReadContext::current(*pages_.storageIO(), superblock);
		std::array<size_t, RSS_CATALOG_RADIX_FANOUT> order{};
		size_t missing = 0U;
		for (size_t index = 0U; index < pageIds.size(); ++index) {
			const auto cached = context ? context->findMetadataRoute(root, pageIds[index], superblock) : std::nullopt;
			if (cached) results[index] = *cached;
			else order[missing++] = index;
		}
		if (missing == 0U) return;
		std::sort(order.begin(), order.begin() + missing,
				[&](size_t left, size_t right) { return pageIds[left].value() < pageIds[right].value(); });
		const auto rootView = pages_.readMetadataRadixUnresolvedView(root);
        const auto &rootPage = *rootView;
		if (rootPage.kind != CatalogRadixPageKind::CRPK_ROOT ||
			rootPage.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER, "metadata PageId route root has an invalid type");
		}
		validateVisible(rootPage.generation, rootPage.committed_lsn, superblock,
				"metadata PageId route root is newer than its Superblock");
		const auto resolve = [&](const auto &self, const MetadataRadixPage &node, size_t begin, size_t end) -> void {
			while (begin < end) {
				const uint64_t ordinal = leafOrdinal(pageIds[order[begin]]);
				const auto child = findDecodedRouteChild(node, ordinal);
				if (!child.has_value()) {
					throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "metadata PageId route is missing");
				}
				const uint64_t factor = radixFactor(node.level);
				size_t next = begin + 1U;
				while (next < end && leafOrdinal(pageIds[order[next]]) / factor == ordinal / factor) ++next;
				if (node.level == 0U) {
					const auto leaf = pages_.readMetadataPageRouteLeafView(*child);
					validateVisible(leaf->generation, leaf->committed_lsn, superblock,
							"metadata PageId route leaf is newer than its Superblock");
                    // The route codec validates strict PageId order; ordinal
                    // division is monotonic, so checking both endpoints proves
                    // the same membership property for the immutable leaf.
                    if (!leaf->routes.empty() &&
                        (leafOrdinal(leaf->routes.front().page_id) != ordinal ||
                         leafOrdinal(leaf->routes.back().page_id) != ordinal)) {
                        throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
                            "metadata PageId route leaf is inconsistent with its radix path");
                    }
					for (size_t index = begin; index < next; ++index) {
						const auto id = pageIds[order[index]];
						const auto route = std::lower_bound(leaf->routes.begin(), leaf->routes.end(), id.value(),
								[](const MetadataPageRoute &entry, uint64_t value) { return entry.page_id.value() < value; });
						if (route == leaf->routes.end() || route->page_id != id) {
							throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "metadata PageId route is missing");
						}
						validateRoute(*route, superblock);
						results[order[index]] = route->physical_reference;
					}
				} else {
					const auto childView = pages_.readMetadataRadixUnresolvedView(*child);
                    const auto &childPage = *childView;
					if (childPage.kind != CatalogRadixPageKind::CRPK_INTERNAL ||
						childPage.directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE ||
						childPage.level + 1U != node.level) {
						throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
								"metadata PageId route has an invalid radix child");
					}
					validateVisible(childPage.generation, childPage.committed_lsn, superblock,
							"metadata PageId route child is newer than its Superblock");
					self(self, childPage, begin, next);
				}
				begin = next;
			}
		};
		resolve(resolve, rootPage, 0U, missing);
		if (context != nullptr) {
			for (size_t index = 0U; index < missing; ++index) {
				const size_t position = order[index];
				context->putMetadataRoute(root, pageIds[position], results[position], superblock);
			}
		}
	}

	PhysicalPageRef MetadataPageRouteDirectory::find(const Superblock &superblock, PhysicalPageRef root,
																			 MetadataPageId pageId) const {
		if (!root.isValid() || !pageId.isValid()) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route lookup requires a root and an identity");
		}
		auto *context = MetadataPageReadContext::current(*pages_.storageIO(), superblock);
		if (context != nullptr) {
			if (const auto cached = context->findMetadataRoute(root, pageId, superblock)) return *cached;
		}
		auto nodeView = pages_.readMetadataRadixUnresolvedView(root);
        const MetadataRadixPage *node = nodeView.get();
		if (node->kind != CatalogRadixPageKind::CRPK_ROOT ||
			node->directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE) {
			throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
									"metadata PageId route root has an invalid type");
		}
		validateVisible(node->generation, node->committed_lsn, superblock,
						"metadata PageId route root is newer than its Superblock");
		const uint64_t ordinal = leafOrdinal(pageId);
		for (;;) {
			const std::optional<PhysicalPageRef> child = findDecodedRouteChild(*node, ordinal);
			if (!child.has_value()) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "metadata PageId route is missing");
			}
			if (node->level == 0U) {
				const auto leaf = pages_.readMetadataPageRouteLeafView(*child);
				validateVisible(leaf->generation, leaf->committed_lsn, superblock,
								"metadata PageId route leaf is newer than its Superblock");
                // Validate the whole sorted leaf before returning a matching
                // prefix; a corrupt later ordinal must not be hidden by a hit.
                if (!leaf->routes.empty() &&
                    (leafOrdinal(leaf->routes.front().page_id) != ordinal ||
                     leafOrdinal(leaf->routes.back().page_id) != ordinal)) {
                    throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
                        "metadata PageId route leaf is inconsistent with its radix path");
                }
                const auto route = std::lower_bound(leaf->routes.begin(), leaf->routes.end(), pageId.value(),
                    [](const MetadataPageRoute &entry, uint64_t id) { return entry.page_id.value() < id; });
                if (route != leaf->routes.end() && route->page_id == pageId) {
                    validateRoute(*route, superblock);
                    if (context != nullptr)
                        context->putMetadataRoute(root, pageId, route->physical_reference, superblock);
                    return route->physical_reference;
                }
				throwRouteDirectoryError(FormatError::FERR_INVALID_STATE, "metadata PageId route is missing");
			}
			const auto parentLevel = node->level;
            nodeView = pages_.readMetadataRadixUnresolvedView(*child);
            node = nodeView.get();
			if (node->kind != CatalogRadixPageKind::CRPK_INTERNAL ||
				node->directory_type != MetadataDirectoryType::MDT_METADATA_PAGE_ROUTE ||
                node->level + 1U != parentLevel) {
				throwRouteDirectoryError(FormatError::FERR_INVALID_HEADER,
										"metadata PageId route has an invalid radix child");
			}
			validateVisible(node->generation, node->committed_lsn, superblock,
								"metadata PageId route child is newer than its Superblock");
		}
	}

} // namespace graph::storage
