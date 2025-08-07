/**
 * @file DatabaseInspector.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/31
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <fstream>
#include <graph/storage/DatabaseInspector.hpp>
#include <graph/storage/StorageHeaders.hpp>
#include <graph/utils/TableFormatter.hpp>
#include <iostream>
#include <sstream>

namespace graph::storage {

	DatabaseInspector::DatabaseInspector(const FileHeader &fileHeader, std::shared_ptr<std::fstream> &file,
										 DataManager &dataManager) :
		fileHeader(fileHeader), file(file), dataManager_(dataManager){};

	DatabaseInspector::~DatabaseInspector() = default;

	std::string propertyValueToString(const PropertyValue& value) {
		return std::visit([](const auto& arg) -> std::string {
			using T = std::decay_t<decltype(arg)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
				return "null";
			} else if constexpr (std::is_same_v<T, bool>) {
				return arg ? "true" : "false";
			} else if constexpr (std::is_same_v<T, std::string>) {
				return "\"" + arg + "\"";
			} else { // Catches int64_t and double
				return std::to_string(arg);
			}
		}, value.getVariant());
	}

	void DatabaseInspector::displayDatabaseStructure() const {
		std::cout << "FileHeader size: " << sizeof(FileHeader) << std::endl;
		std::cout << "SegmentHeader size: " << sizeof(SegmentHeader) << std::endl;
		std::cout << "Node METADATA_SIZE: " << Node::METADATA_SIZE << std::endl;
		std::cout << "Node LABEL_BUFFER_SIZE: " << Node::LABEL_BUFFER_SIZE << std::endl;
		std::cout << "Blob METADATA_SIZE: " << Blob::METADATA_SIZE << std::endl;

		// Create a table formatter for displaying file header
		TableFormatter table;
		table.setTitle("File Header Info");
		table.addColumn("Attribute");
		table.addColumn("Value");

		// Add file header data to the table
		table.addRow({"Magic", std::string(fileHeader.magic, 8)});
		table.addRow({"Node Segment Head", std::to_string(fileHeader.node_segment_head)});
		table.addRow({"Edge Segment Head", std::to_string(fileHeader.edge_segment_head)});
		table.addRow({"Property Segment Head", std::to_string(fileHeader.property_segment_head)});
		table.addRow({"Blob Segment Head", std::to_string(fileHeader.blob_segment_head)});
		table.addRow({"Index Segment Head", std::to_string(fileHeader.index_segment_head)});
		table.addRow({"State Segment Head", std::to_string(fileHeader.state_segment_head)});
		table.addRow({"Max Node ID", std::to_string(fileHeader.max_node_id)});
		table.addRow({"Max Edge ID", std::to_string(fileHeader.max_edge_id)});
		table.addRow({"Max Prop ID", std::to_string(fileHeader.max_prop_id)});
		table.addRow({"Max Blob ID", std::to_string(fileHeader.max_blob_id)});
		table.addRow({"Max Index ID", std::to_string(fileHeader.max_index_id)});
		table.addRow({"Max State ID", std::to_string(fileHeader.max_state_id)});
		table.addRow({"Version", std::to_string(fileHeader.version)});

		// Print the file header table
		table.print();

		// Display node segments if they exist
		displayNodeSegmentChain(file, fileHeader.node_segment_head);

		// Display edge segments if they exist
		displayEdgeSegmentChain(file, fileHeader.edge_segment_head);

		// Display property segments if they exist
		displayPropertySegmentChain(file, fileHeader.property_segment_head);

		// Display blob segments if they exist
		displayBlobSegmentChain(file, fileHeader.blob_segment_head);

		// Display index segments if they exist
		displayIndexSegmentChain(file, fileHeader.index_segment_head);

		// Display state segments if they exist
		displayStateSegmentChain(file, fileHeader.state_segment_head);
	}

	void DatabaseInspector::displayNodeSegmentChain(const std::shared_ptr<std::fstream> &file,
													uint64_t segmentOffset) const {
		int segmentIndex = 0;

		while (segmentOffset != 0) {
			std::cout << "\n=== Node Segment #" << segmentIndex++ << " ===\n" << std::endl;

			// Seek to the segment header
			file->seekg(static_cast<std::streamoff>(segmentOffset));

			// Read segment header
			SegmentHeader segmentHeader;
			file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

			if (!file->good()) {
				std::cerr << "Error: Failed to read segment header at offset " << segmentOffset << std::endl;
				break;
			}

			// Display segment header information
			TableFormatter table;
			table.setTitle("Node Segment Header");
			table.addColumn("Attribute");
			table.addColumn("Value");

			table.addRow({"Segment Offset", std::to_string(segmentHeader.file_offset)});
			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Inactive Count", std::to_string(segmentHeader.inactive_count)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.print();

			// Print activity bitmap
			std::cout << "Activity Bitmap: ";
			for (uint32_t i = 0; i < segmentHeader.bitmap_size; ++i) {
				for (int bit = 7; bit >= 0; --bit) {
					std::cout << ((segmentHeader.activity_bitmap[i] >> bit) & 1);
				}
				std::cout << " ";
			}
			std::cout << std::endl;

			// Calculate data start position
			std::streampos dataStart = file->tellg();

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos nodePosition =
						dataStart +
						static_cast<std::streamoff>(static_cast<std::make_signed_t<size_t>>(i * Node::getTotalSize()));
				file->seekg(nodePosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << nodePosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					// Attempt to deserialize and display the node regardless of activity
					try {
						Node node = Node::deserialize(*file);

						// Display node information
						table.clear();
						table.setTitle("Node Data");
						table.addColumn("Attribute");
						table.addColumn("Value");

						table.addRow({"Status", "USED"});
						table.addRow({"Node ID", std::to_string(node.getId())});
						table.addRow({"Label", node.getLabel()});
						table.addRow({"propertyEntityId", std::to_string(node.getPropertyEntityId())});
						// firstOutEdgeId
						table.addRow({"First Out Edge ID", std::to_string(node.getFirstOutEdgeId())});
						// firstInEdgeId
						table.addRow({"First In Edge ID", std::to_string(node.getFirstInEdgeId())});
						table.addRow({"Active", node.isActive() ? "true" : "false"});

						// Load properties
						auto properties = dataManager_.getNodeProperties(node.getId());

						// Add Attribute data to the table
						for (const auto &[key, value] : properties) {
							// Use the simplified helper function
							std::string valueStr = propertyValueToString(value);
							std::string preview = (valueStr.length() > 50) ? valueStr.substr(0, 47) + "..." : valueStr;
							table.addRow({"Attribute: " + key, preview});
						}

						table.print();
					} catch (const std::exception &e) {
						std::cerr << "Error deserializing node: " << e.what() << std::endl;
						table.clear();
						table.setTitle("Node Data");
						table.addColumn("Attribute");
						table.addColumn("Value");
						table.addRow({"Status", "ERROR"});
						table.addRow({"Error", e.what()});
						table.print();
					}
				} else {
					// Slot is unused
					table.clear();
					table.setTitle("Node Data");
					table.addColumn("Attribute");
					table.addColumn("Value");
					table.addRow({"Status", "UNUSED"});
					table.print();

					// Show raw data bytes
					std::vector<char> rawData(Node::getTotalSize());
					file->read(rawData.data(), Node::getTotalSize());
					if (file->good()) {
						std::cout << "Raw data (first 16 bytes): ";
						for (size_t j = 0; j < std::min<size_t>(16, Node::getTotalSize()); ++j) {
							std::cout << std::hex << std::setw(2) << std::setfill('0')
									  << static_cast<int>(static_cast<unsigned char>(rawData[j])) << " ";
						}
						std::cout << std::dec << std::endl;
					}
				}
			}

			// Move to the next segment in the chain
			segmentOffset = segmentHeader.next_segment_offset;
		}
	}

	void DatabaseInspector::displayEdgeSegmentChain(const std::shared_ptr<std::fstream> &file,
													uint64_t segmentOffset) const {
		int segmentIndex = 0;

		while (segmentOffset != 0) {
			std::cout << "\n=== Edge Segment #" << segmentIndex++ << " ===\n" << std::endl;

			// Seek to the segment header
			file->seekg(static_cast<std::streamoff>(segmentOffset));

			// Read segment header
			SegmentHeader segmentHeader;
			file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

			if (!file->good()) {
				std::cerr << "Error: Failed to read segment header at offset " << segmentOffset << std::endl;
				break;
			}

			// Display segment header information
			TableFormatter table;
			table.setTitle("Edge Segment Header");
			table.addColumn("Attribute");
			table.addColumn("Value");

			table.addRow({"Segment Offset", std::to_string(segmentHeader.file_offset)});
			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Inactive Count", std::to_string(segmentHeader.inactive_count)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.print();

			// Print activity bitmap
			std::cout << "Activity Bitmap: ";
			for (uint32_t i = 0; i < segmentHeader.bitmap_size; ++i) {
				for (int bit = 7; bit >= 0; --bit) {
					std::cout << ((segmentHeader.activity_bitmap[i] >> bit) & 1);
				}
				std::cout << " ";
			}
			std::cout << std::endl;

			// Calculate data start position
			std::streampos dataStart = file->tellg();

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos edgePosition =
						dataStart +
						static_cast<std::streamoff>(static_cast<std::make_signed_t<size_t>>(i * Edge::getTotalSize()));
				file->seekg(edgePosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << edgePosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					// Attempt to deserialize and display the edge regardless of activity
					try {
						Edge edge = Edge::deserialize(*file);

						// Display edge information
						table.clear();
						table.setTitle("Edge Data");
						table.addColumn("Attribute");
						table.addColumn("Value");

						table.addRow({"Status", "USED"});
						table.addRow({"Edge ID", std::to_string(edge.getId())});
						table.addRow({"Label", edge.getLabel()});
						table.addRow({"Source Node ID", std::to_string(edge.getSourceNodeId())});
						table.addRow({"Target Node ID", std::to_string(edge.getTargetNodeId())});
						// prevOutEdgeId
						table.addRow({"Previous Out Edge ID", std::to_string(edge.getPrevOutEdgeId())});
						// nextOutEdgeId
						table.addRow({"Next Out Edge ID", std::to_string(edge.getNextOutEdgeId())});
						// prevInEdgeId
						table.addRow({"Previous In Edge ID", std::to_string(edge.getPrevInEdgeId())});
						// nextInEdgeId
						table.addRow({"Next In Edge ID", std::to_string(edge.getNextInEdgeId())});
						table.addRow({"propertyEntityId", std::to_string(edge.getPropertyEntityId())});
						table.addRow({"Active", edge.isActive() ? "true" : "false"});

						// Load properties
						auto properties = dataManager_.getEdgeProperties(edge.getId());

						// Add Attribute data to the table
						for (const auto &[key, value] : properties) {
							// Use the simplified helper function
							std::string valueStr = propertyValueToString(value);
							std::string preview = (valueStr.length() > 50) ? valueStr.substr(0, 47) + "..." : valueStr;
							table.addRow({"Attribute: " + key, preview});
						}

						table.print();
					} catch (const std::exception &e) {
						std::cerr << "Error deserializing edge: " << e.what() << std::endl;
						table.clear();
						table.setTitle("Edge Data");
						table.addColumn("Attribute");
						table.addColumn("Value");
						table.addRow({"Status", "ERROR"});
						table.addRow({"Error", e.what()});
						table.print();
					}
				} else {
					// Slot is unused
					table.clear();
					table.setTitle("Edge Data");
					table.addColumn("Attribute");
					table.addColumn("Value");
					table.addRow({"Status", "UNUSED"});
					table.print();

					// Show raw data bytes
					std::vector<char> rawData(Edge::getTotalSize());
					file->read(rawData.data(), Edge::getTotalSize());
					if (file->good()) {
						std::cout << "Raw data (first 16 bytes): ";
						for (size_t j = 0; j < std::min<size_t>(16, Edge::getTotalSize()); ++j) {
							std::cout << std::hex << std::setw(2) << std::setfill('0')
									  << static_cast<int>(static_cast<unsigned char>(rawData[j])) << " ";
						}
						std::cout << std::dec << std::endl;
					}
				}
			}

			// Move to the next segment in the chain
			segmentOffset = segmentHeader.next_segment_offset;
		}
	}

	void DatabaseInspector::displayPropertySegmentChain(const std::shared_ptr<std::fstream> &file,
														uint64_t segmentOffset) {
		int segmentIndex = 0;

		while (segmentOffset != 0) {
			std::cout << "\n=== Property Segment #" << segmentIndex++ << " ===\n" << std::endl;

			// Seek to the segment header
			file->seekg(static_cast<std::streamoff>(segmentOffset));

			// Read segment header
			SegmentHeader segmentHeader;
			file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

			if (!file->good()) {
				std::cerr << "Error: Failed to read property segment header at offset " << segmentOffset << std::endl;
				break;
			}

			// Display segment header information
			TableFormatter table;
			table.setTitle("Property Segment Header");
			table.addColumn("Attribute");
			table.addColumn("Value");

			table.addRow({"Segment Offset", std::to_string(segmentHeader.file_offset)});
			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Previous Segment Offset", std::to_string(segmentHeader.prev_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Inactive Count", std::to_string(segmentHeader.inactive_count)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.print();

			// Print activity bitmap
			std::cout << "Activity Bitmap: ";
			for (uint32_t i = 0; i < segmentHeader.bitmap_size; ++i) {
				for (int bit = 7; bit >= 0; --bit) {
					std::cout << ((segmentHeader.activity_bitmap[i] >> bit) & 1);
				}
				std::cout << " ";
			}
			std::cout << std::endl;

			// Calculate data start position
			std::streampos dataStart = file->tellg();

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos propertyPosition =
						dataStart + static_cast<std::streamoff>(
											static_cast<std::make_signed_t<size_t>>(i * Property::getTotalSize()));
				file->seekg(propertyPosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << propertyPosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					// Attempt to deserialize and display the property regardless of activity
					try {
						Property property = Property::deserialize(*file);

						// Display property information
						table.clear();
						table.setTitle("Property Data");
						table.addColumn("Attribute");
						table.addColumn("Value");

						table.addRow({"Status", "USED"});
						table.addRow({"Property ID", std::to_string(property.getId())});
						table.addRow({"Entity ID", std::to_string(property.getEntityId())});
						table.addRow({"Entity Type", property.getEntityType() == 0 ? "Node" : "Edge"});

						table.print();
					} catch (const std::exception &e) {
						std::cerr << "Error deserializing property: " << e.what() << std::endl;
						table.clear();
						table.setTitle("Property Data");
						table.addColumn("Attribute");
						table.addColumn("Value");
						table.addRow({"Status", "ERROR"});
						table.addRow({"Error", e.what()});
						table.print();
					}
				} else {
					// Slot is unused
					table.clear();
					table.setTitle("Property Data");
					table.addColumn("Attribute");
					table.addColumn("Value");
					table.addRow({"Status", "UNUSED"});
					table.print();

					// Show raw data bytes
					std::vector<char> rawData(Property::getTotalSize());
					file->read(rawData.data(), Property::getTotalSize());
					if (file->good()) {
						std::cout << "Raw data (first 16 bytes): ";
						for (size_t j = 0; j < std::min<size_t>(16, Property::getTotalSize()); ++j) {
							std::cout << std::hex << std::setw(2) << std::setfill('0')
									  << static_cast<int>(static_cast<unsigned char>(rawData[j])) << " ";
						}
						std::cout << std::dec << std::endl;
					}
				}
			}

			// Move to the next segment in the chain
			segmentOffset = segmentHeader.next_segment_offset;
		}
	}

	// New method to display blob segments
	void DatabaseInspector::displayBlobSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset) {
		int segmentIndex = 0;

		while (segmentOffset != 0) {
			std::cout << "\n=== Blob Segment #" << segmentIndex++ << " ===\n" << std::endl;

			// Seek to the segment header
			file->seekg(static_cast<std::streamoff>(segmentOffset));

			// Read segment header
			SegmentHeader segmentHeader;
			file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

			if (!file->good()) {
				std::cerr << "Error: Failed to read blob segment header at offset " << segmentOffset << std::endl;
				break;
			}

			// Display segment header information
			TableFormatter table;
			table.setTitle("Blob Segment Header");
			table.addColumn("Attribute");
			table.addColumn("Value");

			table.addRow({"Segment Offset", std::to_string(segmentHeader.file_offset)});
			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Previous Segment Offset", std::to_string(segmentHeader.prev_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Inactive Count", std::to_string(segmentHeader.inactive_count)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.print();

			// Print activity bitmap
			std::cout << "Activity Bitmap: ";
			for (uint32_t i = 0; i < segmentHeader.bitmap_size; ++i) {
				for (int bit = 7; bit >= 0; --bit) {
					std::cout << ((segmentHeader.activity_bitmap[i] >> bit) & 1);
				}
				std::cout << " ";
			}
			std::cout << std::endl;

			// Calculate data start position
			std::streampos dataStart = file->tellg();

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos blobPosition =
						dataStart +
						static_cast<std::streamoff>(static_cast<std::make_signed_t<size_t>>(i * Blob::getTotalSize()));
				file->seekg(blobPosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << blobPosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					// Attempt to deserialize and display the blob regardless of activity
					try {
						Blob blob = Blob::deserialize(*file);

						// Display blob information
						table.clear();
						table.setTitle("Blob Data");
						table.addColumn("Attribute");
						table.addColumn("Value");

						table.addRow({"Status", "USED"});
						table.addRow({"Blob ID", std::to_string(blob.getId())});
						table.addRow({"Size", std::to_string(blob.getSize())});
						table.addRow({"Entity ID", std::to_string(blob.getEntityId())});
						table.addRow({"Entity Type", blob.getEntityType() == 0 ? "Node" : "Edge"});
						table.addRow({"Prev Blob ID", std::to_string(blob.getPrevBlobId())});
						table.addRow({"Next Blob ID", std::to_string(blob.getNextBlobId())});
						table.addRow({"Active", blob.isActive() ? "true" : "false"});

						table.print();
					} catch (const std::exception &e) {
						std::cerr << "Error deserializing blob: " << e.what() << std::endl;
						table.clear();
						table.setTitle("Blob Data");
						table.addColumn("Attribute");
						table.addColumn("Value");
						table.addRow({"Status", "ERROR"});
						table.addRow({"Error", e.what()});
						table.print();
					}
				} else {
					// Slot is unused
					table.clear();
					table.setTitle("Blob Data");
					table.addColumn("Attribute");
					table.addColumn("Value");
					table.addRow({"Status", "UNUSED"});
					table.print();

					// Show raw data bytes
					std::vector<char> rawData(Blob::getTotalSize());
					file->read(rawData.data(), Blob::getTotalSize());
					if (file->good()) {
						std::cout << "Raw data (first 16 bytes): ";
						for (size_t j = 0; j < std::min<size_t>(16, Blob::getTotalSize()); ++j) {
							std::cout << std::hex << std::setw(2) << std::setfill('0')
									  << static_cast<int>(static_cast<unsigned char>(rawData[j])) << " ";
						}
						std::cout << std::dec << std::endl;
					}
				}
			}

			// Move to the next segment in the chain
			segmentOffset = segmentHeader.next_segment_offset;
		}
	}

	void DatabaseInspector::displayIndexSegmentChain(const std::shared_ptr<std::fstream> &file,
													 uint64_t segmentOffset) {
		int segmentIndex = 0;

		while (segmentOffset != 0) {
			std::cout << "\n=== Index Segment #" << segmentIndex++ << " ===\n" << std::endl;

			// Seek to the segment header
			file->seekg(static_cast<std::streamoff>(segmentOffset));

			// Read segment header
			SegmentHeader segmentHeader;
			file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

			if (!file->good()) {
				std::cerr << "Error: Failed to read index segment header at offset " << segmentOffset << std::endl;
				break;
			}

			// Display segment header information
			TableFormatter table;
			table.setTitle("Index Segment Header");
			table.addColumn("Attribute");
			table.addColumn("Value");

			table.addRow({"Segment Offset", std::to_string(segmentHeader.file_offset)});
			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Inactive Count", std::to_string(segmentHeader.inactive_count)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.print();

			// Print activity bitmap
			std::cout << "Activity Bitmap: ";
			for (uint32_t i = 0; i < segmentHeader.bitmap_size; ++i) {
				for (int bit = 7; bit >= 0; --bit) {
					std::cout << ((segmentHeader.activity_bitmap[i] >> bit) & 1);
				}
				std::cout << " ";
			}
			std::cout << std::endl;

			// Calculate data start position
			std::streampos dataStart = file->tellg();

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos indexPosition =
						dataStart +
						static_cast<std::streamoff>(static_cast<std::make_signed_t<size_t>>(i * Index::getTotalSize()));
				file->seekg(indexPosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << indexPosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					try {
						Index index = Index::deserialize(*file);

						// Display index information
						table.clear();
						table.setTitle("Index Data");
						table.addColumn("Attribute");
						table.addColumn("Value");

						table.addRow({"Status", "USED"});
						table.addRow({"Index ID", std::to_string(index.getId())});
						table.addRow({"Node Type", index.isLeaf() ? "LEAF" : "INTERNAL"});
						table.addRow({"Key Count", std::to_string(index.getKeyCount())});
						table.addRow({"Active", index.isActive() ? "true" : "false"});

						table.print();
					} catch (const std::exception &e) {
						std::cerr << "Error deserializing index: " << e.what() << std::endl;
					}
				} else {
					std::cout << "Slot is unused." << std::endl;
				}
			}

			// Move to the next segment in the chain
			segmentOffset = segmentHeader.next_segment_offset;
		}
	}

	void DatabaseInspector::displayStateSegmentChain(const std::shared_ptr<std::fstream> &file,
													 uint64_t segmentOffset) {
		int segmentIndex = 0;

		while (segmentOffset != 0) {
			std::cout << "\n=== State Segment #" << segmentIndex++ << " ===\n" << std::endl;

			// Seek to the segment header
			file->seekg(static_cast<std::streamoff>(segmentOffset));

			// Read segment header
			SegmentHeader segmentHeader;
			file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(SegmentHeader));

			if (!file->good()) {
				std::cerr << "Error: Failed to read state segment header at offset " << segmentOffset << std::endl;
				break;
			}

			// Display segment header information
			TableFormatter table;
			table.setTitle("State Segment Header");
			table.addColumn("Attribute");
			table.addColumn("Value");

			table.addRow({"Segment Offset", std::to_string(segmentHeader.file_offset)});
			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Inactive Count", std::to_string(segmentHeader.inactive_count)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.print();

			// Print activity bitmap
			std::cout << "Activity Bitmap: ";
			for (uint32_t i = 0; i < segmentHeader.bitmap_size; ++i) {
				for (int bit = 7; bit >= 0; --bit) {
					std::cout << ((segmentHeader.activity_bitmap[i] >> bit) & 1);
				}
				std::cout << " ";
			}
			std::cout << std::endl;

			// Calculate data start position
			std::streampos dataStart = file->tellg();

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos statePosition =
						dataStart +
						static_cast<std::streamoff>(static_cast<std::make_signed_t<size_t>>(i * State::getTotalSize()));
				file->seekg(statePosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << statePosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					try {
						State state = State::deserialize(*file);

						// Display state information
						table.clear();
						table.setTitle("State Data");
						table.addColumn("Attribute");
						table.addColumn("Value");

						table.addRow({"Status", "USED"});
						table.addRow({"State ID", std::to_string(state.getId())});
						table.addRow({"Key", state.getKey()});
						table.addRow({"Data Size", std::to_string(state.getSize())});
						table.addRow({"Active", state.isActive() ? "true" : "false"});

						table.print();
					} catch (const std::exception &e) {
						std::cerr << "Error deserializing state: " << e.what() << std::endl;
					}
				} else {
					std::cout << "Slot is unused." << std::endl;
				}
			}

			// Move to the next segment in the chain
			segmentOffset = segmentHeader.next_segment_offset;
		}
	}

} // namespace graph::storage
