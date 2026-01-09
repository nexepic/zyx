/**
 * @file DatabaseInspector.cpp
 * @author Nexepic
 * @date 2025/3/31
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "graph/storage/DatabaseInspector.hpp"
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#include "graph/utils/TableFormatter.hpp"

namespace graph::storage {

	DatabaseInspector::DatabaseInspector(const FileHeader &fileHeader, std::shared_ptr<std::fstream> file,
										 DataManager &dataManager) :
		fileHeader(fileHeader), file(std::move(file)), dataManager_(dataManager) {}

	DatabaseInspector::~DatabaseInspector() = default;

	// --- Helpers ---

	std::string
	DatabaseInspector::formatPropertiesCompact(const std::unordered_map<std::string, PropertyValue> &props) {
		if (props.empty())
			return "{}";
		std::stringstream ss;
		ss << "{";
		size_t count = 0;
		for (const auto &[k, v]: props) {
			ss << k << ":" << v.toString();
			if (++count < props.size())
				ss << ", ";
			if (ss.tellp() > 50) { // Limit length to avoid breaking table layout
				ss << "...";
				break;
			}
		}
		ss << "}";
		return ss.str();
	}

	uint64_t DatabaseInspector::navigateToSegment(uint64_t headOffset, uint32_t targetPage) const {
		uint64_t currentOffset = headOffset;
		uint32_t currentPage = 0;

		while (currentOffset != 0 && currentPage < targetPage) {
			file->seekg(static_cast<std::streamoff>(currentOffset));
			SegmentHeader header;
			if (!file->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader))) {
				return 0; // Read error
			}
			currentOffset = header.next_segment_offset;
			currentPage++;
		}
		return currentOffset;
	}

	// --- Implementation ---

	void DatabaseInspector::inspectSummary() const {
		TableFormatter table;
		table.setTitle("Database Summary");
		table.addColumn("Type");
		table.addColumn("Head Offset");
		table.addColumn("Max ID");
		table.addColumn("Items/Seg");

		table.addRow({"File Magic", std::string(fileHeader.magic, 8), "-", "-"});
		table.addRow({"Version", std::to_string(fileHeader.version), "-", "-"});

		table.addRow({"Nodes", std::to_string(fileHeader.node_segment_head), std::to_string(fileHeader.max_node_id),
					  std::to_string(NODES_PER_SEGMENT)});
		table.addRow({"Edges", std::to_string(fileHeader.edge_segment_head), std::to_string(fileHeader.max_edge_id),
					  std::to_string(EDGES_PER_SEGMENT)});
		table.addRow({"Properties", std::to_string(fileHeader.property_segment_head),
					  std::to_string(fileHeader.max_prop_id), std::to_string(PROPERTIES_PER_SEGMENT)});
		table.addRow({"Blobs", std::to_string(fileHeader.blob_segment_head), std::to_string(fileHeader.max_blob_id),
					  std::to_string(BLOBS_PER_SEGMENT)});
		table.addRow({"Indexes", std::to_string(fileHeader.index_segment_head), std::to_string(fileHeader.max_index_id),
					  std::to_string(INDEXES_PER_SEGMENT)});
		table.addRow({"States", std::to_string(fileHeader.state_segment_head), std::to_string(fileHeader.max_state_id),
					  std::to_string(STATES_PER_SEGMENT)});

		table.print();
		std::cout << "\nUse 'debug <type> [page]' to inspect segments.\n";
	}

	void DatabaseInspector::inspectNodeSegments(uint32_t pageIndex, bool showUnused) const {
		uint64_t offset = navigateToSegment(fileHeader.node_segment_head, pageIndex);

		if (offset == 0) {
			std::cout << "Node segment page " << pageIndex << " does not exist." << std::endl;
			return;
		}

		file->seekg(static_cast<std::streamoff>(offset));
		SegmentHeader header;
		file->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		std::cout << "\n=== Node Segment Page " << pageIndex << " ===\n";
		std::cout << "Offset: " << offset << " | Used: " << header.used << "/" << header.capacity
				  << " | Next Seg: " << header.next_segment_offset << "\n";

		TableFormatter table;
		table.setTitle("Nodes");
		table.addColumn("Slot");
		table.addColumn("ID");
		table.addColumn("Status");
		table.addColumn("Label");
		table.addColumn("Props");
		table.addColumn("Edges(Out/In)");

		std::streampos dataStart = static_cast<std::streamoff>(offset + sizeof(SegmentHeader));

		for (uint32_t i = 0; i < header.capacity; ++i) {
			bool isActive = bitmap::getBit(header.activity_bitmap, i);

			// Removed check to always display inactive data
			// if (!isActive && !showUnused) continue;

			file->seekg(dataStart + static_cast<std::streamoff>(i * Node::getTotalSize()));

			if (isActive) {
				Node node = Node::deserialize(*file);
				auto props = dataManager_.getNodeProperties(node.getId());

				table.addRow(
						{std::to_string(i), std::to_string(node.getId()), "ACTIVE", node.getLabel(),
						 formatPropertiesCompact(props),
						 std::to_string(node.getFirstOutEdgeId()) + "/" + std::to_string(node.getFirstInEdgeId())});
			} else {
				table.addRow({std::to_string(i), "-", "UNUSED", "-", "-", "-"});
			}
		}
		table.print();
	}

	void DatabaseInspector::inspectEdgeSegments(uint32_t pageIndex, bool showUnused) const {
		uint64_t offset = navigateToSegment(fileHeader.edge_segment_head, pageIndex);

		if (offset == 0) {
			std::cout << "Edge segment page " << pageIndex << " does not exist." << std::endl;
			return;
		}

		file->seekg(static_cast<std::streamoff>(offset));
		SegmentHeader header;
		file->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		std::cout << "\n=== Edge Segment Page " << pageIndex << " ===\n";

		TableFormatter table;
		table.setTitle("Edges");
		table.addColumn("Slot");
		table.addColumn("ID");
		table.addColumn("Status");
		table.addColumn("Label");
		table.addColumn("Src->Dst");
		table.addColumn("Props");
		table.addColumn("Link(P/N)");

		std::streampos dataStart = static_cast<std::streamoff>(offset + sizeof(SegmentHeader));

		for (uint32_t i = 0; i < header.capacity; ++i) {
			bool isActive = bitmap::getBit(header.activity_bitmap, i);

			// Removed check to always display inactive data
			// if (!isActive && !showUnused) continue;

			file->seekg(dataStart + static_cast<std::streamoff>(i * Edge::getTotalSize()));

			if (isActive) {
				Edge edge = Edge::deserialize(*file);
				auto props = dataManager_.getEdgeProperties(edge.getId());

				table.addRow({std::to_string(i), std::to_string(edge.getId()), "ACTIVE", edge.getLabel(),
							  std::to_string(edge.getSourceNodeId()) + "->" + std::to_string(edge.getTargetNodeId()),
							  formatPropertiesCompact(props),
							  std::to_string(edge.getPrevOutEdgeId()) + "/" + std::to_string(edge.getNextOutEdgeId())});
			} else {
				table.addRow({std::to_string(i), "-", "UNUSED", "-", "-", "-", "-"});
			}
		}
		table.print();
	}

	void DatabaseInspector::inspectPropertySegments(uint32_t pageIndex, bool showUnused) const {
		uint64_t offset = navigateToSegment(fileHeader.property_segment_head, pageIndex);
		if (offset == 0) {
			std::cout << "Property segment page " << pageIndex << " does not exist." << std::endl;
			return;
		}

		file->seekg(static_cast<std::streamoff>(offset));
		SegmentHeader header;
		file->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		std::cout << "\n=== Property Segment Page " << pageIndex << " ===\n";

		TableFormatter table;
		table.setTitle("Properties");
		table.addColumn("Slot");
		table.addColumn("ID");
		table.addColumn("Status");
		table.addColumn("Owner EntityID");
		table.addColumn("Owner Type");
		table.addColumn("Content");

		std::streampos dataStart = static_cast<std::streamoff>(offset + sizeof(SegmentHeader));

		for (uint32_t i = 0; i < header.capacity; ++i) {
			bool isActive = bitmap::getBit(header.activity_bitmap, i);

			// Removed check to always display inactive data
			// if (!isActive && !showUnused) continue;

			file->seekg(dataStart + static_cast<std::streamoff>(i * Property::getTotalSize()));

			if (isActive) {
				Property prop = Property::deserialize(*file);
				const auto &meta = prop.getMetadata();
				std::string contentStr = formatPropertiesCompact(prop.getPropertyValues());

				table.addRow({std::to_string(i), std::to_string(meta.id), "ACTIVE", std::to_string(meta.entityId),
							  (meta.entityType == 0 ? "Node" : "Edge"), contentStr});
			} else {
				table.addRow({std::to_string(i), "-", "UNUSED", "-", "-", "-"});
			}
		}
		table.print();
	}

	void DatabaseInspector::inspectBlobSegments(uint32_t pageIndex, bool showUnused) const {
		uint64_t offset = navigateToSegment(fileHeader.blob_segment_head, pageIndex);
		if (offset == 0) {
			std::cout << "Blob segment page " << pageIndex << " does not exist." << std::endl;
			return;
		}

		file->seekg(static_cast<std::streamoff>(offset));
		SegmentHeader header;
		file->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		std::cout << "\n=== Blob Segment Page " << pageIndex << " ===\n";

		TableFormatter table;
		table.setTitle("Blobs");
		table.addColumn("Slot");
		table.addColumn("ID");
		table.addColumn("Status");
		table.addColumn("Size");
		table.addColumn("EntityID");
		table.addColumn("Prev/Next");

		std::streampos dataStart = static_cast<std::streamoff>(offset + sizeof(SegmentHeader));

		for (uint32_t i = 0; i < header.capacity; ++i) {
			bool isActive = bitmap::getBit(header.activity_bitmap, i);

			// Removed check to always display inactive data
			// if (!isActive && !showUnused) continue;

			file->seekg(dataStart + static_cast<std::streamoff>(i * Blob::getTotalSize()));

			if (isActive) {
				Blob blob = Blob::deserialize(*file);
				table.addRow({std::to_string(i), std::to_string(blob.getId()), "ACTIVE", std::to_string(blob.getSize()),
							  std::to_string(blob.getEntityId()),
							  std::to_string(blob.getPrevBlobId()) + "/" + std::to_string(blob.getNextBlobId())});
			} else {
				table.addRow({std::to_string(i), "-", "UNUSED", "-", "-", "-"});
			}
		}
		table.print();
	}

	void DatabaseInspector::inspectIndexSegments(uint32_t pageIndex, bool showUnused) const {
		uint64_t offset = navigateToSegment(fileHeader.index_segment_head, pageIndex);
		if (offset == 0) {
			std::cout << "Index segment page " << pageIndex << " does not exist." << std::endl;
			return;
		}

		file->seekg(static_cast<std::streamoff>(offset));
		SegmentHeader header;
		file->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		std::cout << "\n=== Index Segment Page " << pageIndex << " ===\n";

		TableFormatter table;
		table.setTitle("Indexes (B-Tree Nodes)");
		table.addColumn("Slot");
		table.addColumn("ID");
		table.addColumn("Status");
		table.addColumn("Type");
		table.addColumn("Level");
		table.addColumn("Entries");
		table.addColumn("Parent");

		std::streampos dataStart = static_cast<std::streamoff>(offset + sizeof(SegmentHeader));

		for (uint32_t i = 0; i < header.capacity; ++i) {
			bool isActive = bitmap::getBit(header.activity_bitmap, i);

			// Removed check to always display inactive data
			// if (!isActive && !showUnused) continue;

			file->seekg(dataStart + static_cast<std::streamoff>(i * Index::getTotalSize()));

			if (isActive) {
				Index idx = Index::deserialize(*file);
				table.addRow({std::to_string(i), std::to_string(idx.getId()), "ACTIVE",
							  idx.isLeaf() ? "LEAF" : "INTERNAL", std::to_string(idx.getLevel()),
							  std::to_string(idx.getEntryCount()), std::to_string(idx.getParentId())});
			} else {
				table.addRow({std::to_string(i), "-", "UNUSED", "-", "-", "-", "-"});
			}
		}
		table.print();
	}

	void DatabaseInspector::inspectStateSegments(uint32_t pageIndex, bool showUnused) const {
		uint64_t offset = navigateToSegment(fileHeader.state_segment_head, pageIndex);
		if (offset == 0) {
			std::cout << "State segment page " << pageIndex << " does not exist." << std::endl;
			return;
		}

		file->seekg(static_cast<std::streamoff>(offset));
		SegmentHeader header;
		file->read(reinterpret_cast<char *>(&header), sizeof(SegmentHeader));

		std::cout << "\n=== State Segment Page " << pageIndex << " ===\n";

		TableFormatter table;
		table.setTitle("State Chain Blocks");
		table.addColumn("Slot");
		table.addColumn("ID");
		table.addColumn("Status");
		table.addColumn("Key");
		table.addColumn("Chunk Size");
		table.addColumn("Link(Prev/Next)");

		std::streampos dataStart = static_cast<std::streamoff>(offset + sizeof(SegmentHeader));

		for (uint32_t i = 0; i < header.capacity; ++i) {
			bool isActive = bitmap::getBit(header.activity_bitmap, i);

			// Removed check to always display inactive data
			// if (!isActive && !showUnused) continue;

			file->seekg(dataStart + static_cast<std::streamoff>(i * State::getTotalSize()));

			if (isActive) {
				State state = State::deserialize(*file);
				table.addRow({std::to_string(i), std::to_string(state.getId()), "ACTIVE", state.getKey(),
							  std::to_string(state.getSize()),
							  std::to_string(state.getPrevStateId()) + "/" + std::to_string(state.getNextStateId())});
			} else {
				table.addRow({std::to_string(i), "-", "UNUSED", "-", "-", "-"});
			}
		}
		table.print();
	}

	void DatabaseInspector::inspectStateData(const std::string &stateKey) const {
		std::cout << "\n=== State Inspection: '" << stateKey << "' ===\n";

		// 1. Locate the State Entity
		// Note: findStateByKey checks the in-memory map first
		State state = dataManager_.findStateByKey(stateKey);

		if (state.getId() == 0) {
			std::cout << "State with key '" << stateKey << "' not found.\n";
			return;
		}

		// 2. Display Metadata
		TableFormatter metaTable;
		metaTable.setTitle("Metadata");
		metaTable.addColumn("Field");
		metaTable.addColumn("Value");

		metaTable.addRow({"ID", std::to_string(state.getId())});
		metaTable.addRow({"Chain Head", state.getKey()});
		metaTable.addRow({"First Chunk Size", std::to_string(state.getSize())});
		metaTable.addRow({"Next Chain ID", std::to_string(state.getNextStateId())});
		metaTable.print();

		std::cout << "\n";

		// 3. Retrieve and Display Content
		// DataManager handles the complex chain traversal and deserialization
		auto properties = dataManager_.getStateProperties(stateKey);

		if (properties.empty()) {
			std::cout << "No properties found (Empty Map).\n";
			return;
		}

		TableFormatter propTable;
		propTable.setTitle("State Properties");
		propTable.addColumn("Key");
		propTable.addColumn("Value");
		propTable.addColumn("Type");

		for (const auto &[key, value]: properties) {
			// FIX: Use .typeName() method provided by PropertyValue class
			std::string typeStr = value.typeName();

			propTable.addRow({key, value.toString(), typeStr});
		}
		propTable.print();
	}

} // namespace graph::storage
