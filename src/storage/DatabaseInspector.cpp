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
#include <graph/storage/DatabaseInspector.h>
#include <graph/storage/PropertyStorage.h>
#include <graph/storage/StorageHeaders.h>
#include <graph/utils/TableFormatter.h>
#include <iostream>
#include <sstream>

namespace graph::storage {

	DatabaseInspector::DatabaseInspector(FileHeader fileHeader, std::shared_ptr<std::fstream> &file) : fileHeader(fileHeader), file(file) {};

	DatabaseInspector::~DatabaseInspector() = default;

	void DatabaseInspector::displayDatabaseStructure() {
		// // Check if the file stream is in a good state
		// if (!file->good()) {
		// 	std::cerr << "Error: File stream is not in a good state." << std::endl;
		// 	return;
		// }

		// // Reset the file pointer to the beginning
		// file->clear(); // Clear any error flags
		// file->seekg(0);
		//
		// // Read the file header
		// FileHeader fileHeader;
		// file->read(reinterpret_cast<char *>(&fileHeader), sizeof(FileHeader));
		//
		// // Check if the read was successful
		// if (!file->good()) {
		// 	std::cerr << "Error: Failed to read the file header." << std::endl;
		// 	return;
		// }



		std::cout << "FileHeader size: " << sizeof(FileHeader) << std::endl;
		std::cout << "SegmentHeader size: " << sizeof(SegmentHeader) << std::endl;
		std::cout << "PropertySegmentHeader size: " << sizeof(PropertySegmentHeader) << std::endl;


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
		table.addRow({"Blob Section Head", std::to_string(fileHeader.blob_section_head)});
		table.addRow({"Max Node ID", std::to_string(fileHeader.max_node_id)});
		table.addRow({"Max Edge ID", std::to_string(fileHeader.max_edge_id)});
		table.addRow({"Max Blob ID", std::to_string(fileHeader.max_blob_id)});
		table.addRow({"Page Size", std::to_string(fileHeader.page_size)});
		table.addRow({"Segment Count", std::to_string(fileHeader.segment_count)});
		table.addRow({"Version", std::to_string(fileHeader.version)});
		table.addRow({"Header CRC", std::to_string(fileHeader.header_crc)});

		// Print the file header table
		table.print();

		// Display node segments if they exist
		displayNodeSegmentChain(file, fileHeader.node_segment_head);

		// Display edge segments if they exist
		displayEdgeSegmentChain(file, fileHeader.edge_segment_head);

		// Display property segments if they exist
		displayPropertySegmentChain(file, fileHeader.property_segment_head);
	}

	void DatabaseInspector::displayNodeSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset) {
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

			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.addRow({"Segment CRC", std::to_string(segmentHeader.segment_crc)});
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
			const size_t nodeSize = sizeof(Node);

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos nodePosition = dataStart + static_cast<std::streamoff>(i * nodeSize);
				file->seekg(nodePosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << nodePosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					// This slot is used, deserialize and display the node
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
						table.addRow({"Active", node.isActive() ? "true" : "false"});

						// Load properties
						auto properties =
								property_storage::PropertyStorage::loadProperties(file, node.getPropertyReference());

						// Add Attribute data to the table
						for (const auto &[key, value]: properties) {
							std::string valueStr = std::visit(
									[](const auto &val) -> std::string {
										std::ostringstream oss;
										if constexpr (std::is_same_v<decltype(val), std::monostate>) {
											return "null";
										} else if constexpr (std::is_same_v<decltype(val), bool>) {
											return val ? "true" : "false";
										} else if constexpr (std::is_arithmetic_v<decltype(val)>) {
											oss << val;
											return oss.str();
										} else if constexpr (std::is_same_v<decltype(val), std::string>) {
											return val;
										} else if constexpr (std::is_same_v<decltype(val), std::vector<int64_t>> ||
															 std::is_same_v<decltype(val), std::vector<double>> ||
															 std::is_same_v<decltype(val), std::vector<std::string>>) {
											oss << "[";
											for (size_t i = 0; i < val.size(); ++i) {
												if (i > 0)
													oss << ", ";
												oss << val[i];
											}
											oss << "]";
											return oss.str();
										} else {
											return "unknown";
										}
									},
									value);

							table.addRow({"Attribute: " + key, valueStr});
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
					// This slot is unused
					table.clear();
					table.setTitle("Node Data");
					table.addColumn("Attribute");
					table.addColumn("Value");
					table.addRow({"Status", "UNUSED"});
					table.print();

					// Show raw data bytes for debugging purposes
					std::vector<char> rawData(nodeSize);
					file->read(rawData.data(), nodeSize);
					if (file->good()) {
						std::cout << "Raw data (first 16 bytes): ";
						for (size_t j = 0; j < std::min<size_t>(16, nodeSize); ++j) {
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

	void DatabaseInspector::displayEdgeSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset) {
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

			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
			table.addRow({"Segment CRC", std::to_string(segmentHeader.segment_crc)});
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
			const size_t edgeSize = sizeof(Edge);

			// Display all slots in the segment (both used and unused)
			for (uint32_t i = 0; i < segmentHeader.capacity; ++i) {
				std::streampos edgePosition = dataStart + static_cast<std::streamoff>(i * edgeSize);
				file->seekg(edgePosition);

				std::cout << "\n--- Slot " << i << " (Offset: " << edgePosition << ") ---" << std::endl;

				if (i < segmentHeader.used) {
					// This slot is used, deserialize and display the edge
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
						table.addRow({"Active", edge.isActive() ? "true" : "false"});

						// Load properties
						auto properties =
								property_storage::PropertyStorage::loadProperties(file, edge.getPropertyReference());

						// Add Attribute data to the table
						for (const auto &[key, value]: properties) {
							std::string valueStr = std::visit(
									[](const auto &val) -> std::string {
										std::ostringstream oss;
										if constexpr (std::is_same_v<decltype(val), std::monostate>) {
											return "null";
										} else if constexpr (std::is_same_v<decltype(val), bool>) {
											return val ? "true" : "false";
										} else if constexpr (std::is_arithmetic_v<decltype(val)>) {
											oss << val;
											return oss.str();
										} else if constexpr (std::is_same_v<decltype(val), std::string>) {
											return val;
										} else if constexpr (std::is_same_v<decltype(val), std::vector<int64_t>> ||
															 std::is_same_v<decltype(val), std::vector<double>> ||
															 std::is_same_v<decltype(val), std::vector<std::string>>) {
											oss << "[";
											for (size_t i = 0; i < val.size(); ++i) {
												if (i > 0)
													oss << ", ";
												oss << val[i];
											}
											oss << "]";
											return oss.str();
										} else {
											return "unknown";
										}
									},
									value);

							table.addRow({"Attribute: " + key, valueStr});
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
					// This slot is unused
					table.clear();
					table.setTitle("Edge Data");
					table.addColumn("Attribute");
					table.addColumn("Value");
					table.addRow({"Status", "UNUSED"});
					table.print();

					// Show raw data bytes for debugging purposes
					std::vector<char> rawData(edgeSize);
					file->read(rawData.data(), edgeSize);
					if (file->good()) {
						std::cout << "Raw data (first 16 bytes): ";
						for (size_t j = 0; j < std::min<size_t>(16, edgeSize); ++j) {
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

	void DatabaseInspector::displayPropertySegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset) {
		int segmentIndex = 0;

		while (segmentOffset != 0) {
			std::cout << "\n=== Property Segment #" << segmentIndex++ << " ===\n" << std::endl;

			// Seek to the segment header
			file->seekg(static_cast<std::streamoff>(segmentOffset));

			// Read property segment header
			PropertySegmentHeader segmentHeader;
			file->read(reinterpret_cast<char *>(&segmentHeader), sizeof(PropertySegmentHeader));

			if (!file->good()) {
				std::cerr << "Error: Failed to read property segment header at offset " << segmentOffset << std::endl;
				break;
			}

			// Display segment header information
			TableFormatter table;
			table.setTitle("Property Segment Header");
			table.addColumn("Attribute");
			table.addColumn("Value");

			table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
			table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
			table.addRow({"Used", std::to_string(segmentHeader.used)});
			table.addRow({"Segment CRC", std::to_string(segmentHeader.segment_crc)});
			table.print();

			// Calculate data start position
			std::streampos dataStart = file->tellg();

			// Display used space in the segment
			if (segmentHeader.used > 0) {
				std::cout << "\n--- Used Space (" << segmentHeader.used << " bytes) ---" << std::endl;

				// Property segments are more complex as they contain variable-length entries
				// Need to scan through all property entries
				std::streampos currentPos = dataStart;
				file->seekg(currentPos);

				uint32_t bytesProcessed = 0;
				while (bytesProcessed < segmentHeader.used) {
					// Try to read a property entry header
					PropertyEntryHeader entryHeader;
					file->read(reinterpret_cast<char *>(&entryHeader), sizeof(PropertyEntryHeader));

					if (!file->good()) {
						std::cerr << "Error: Failed to read property entry header at position " << currentPos
								  << std::endl;
						break;
					}

					table.clear();
					table.setTitle("Property Entry");
					table.addColumn("Attribute");
					table.addColumn("Value");

					table.addRow({"Entity ID", std::to_string(entryHeader.entity_id)});
					table.addRow({"Entity Type", entryHeader.entity_type == 0 ? "Node" : "Edge"});
					table.addRow({"Data Size", std::to_string(entryHeader.data_size)});
					table.addRow({"Property Count", std::to_string(entryHeader.property_count)});
					table.print();

					// Skip past the property data - in a real implementation, you would parse and display
					// the properties based on your property serialization format
					file->seekg(static_cast<std::streamoff>(entryHeader.data_size), std::ios::cur);

					// Update bytes processed and current position
					bytesProcessed += sizeof(PropertyEntryHeader) + entryHeader.data_size;
					currentPos = dataStart + static_cast<std::streamoff>(bytesProcessed);
					file->seekg(currentPos);
				}
			}

			// Display unused space
			if (segmentHeader.used < segmentHeader.capacity) {
				uint32_t unusedBytes = segmentHeader.capacity - segmentHeader.used;
				std::cout << "\n--- Unused Space (" << unusedBytes << " bytes) ---" << std::endl;

				table.clear();
				table.setTitle("Unused Space");
				table.addColumn("Attribute");
				table.addColumn("Value");
				table.addRow({"Offset", std::to_string(dataStart + static_cast<std::streamoff>(segmentHeader.used))});
				table.addRow({"Size", std::to_string(unusedBytes)});
				table.print();

				// Show a sample of the unused space for debugging
				file->seekg(dataStart + static_cast<std::streamoff>(segmentHeader.used));
				std::vector<char> rawData(std::min<uint32_t>(32, unusedBytes));
				file->read(rawData.data(), rawData.size());

				if (file->good()) {
					std::cout << "Sample raw data (first 32 bytes or less): " << std::endl;
					for (size_t j = 0; j < rawData.size(); ++j) {
						std::cout << std::hex << std::setw(2) << std::setfill('0')
								  << static_cast<int>(static_cast<unsigned char>(rawData[j])) << " ";
						if ((j + 1) % 16 == 0)
							std::cout << std::endl;
					}
					std::cout << std::dec << std::endl;
				}
			}

			// Move to the next segment in the chain
			segmentOffset = segmentHeader.next_segment_offset;
		}
	}

} // namespace graph::storage
