/**
 * @file DatabaseInspector.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/31
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include <iostream>
#include <fstream>
#include <sstream>
#include <graph/storage/DatabaseInspector.h>
#include <graph/storage/StorageHeaders.h>
#include <graph/storage/PropertyStorage.h>
#include <graph/utils/TableFormatter.h>

namespace graph::storage {

void displayDatabaseStructure(const std::shared_ptr<std::ifstream>& file) {
    // Check if the file stream is in a good state
    if (!file->good()) {
        std::cerr << "Error: File stream is not in a good state." << std::endl;
        return;
    }

    // Reset the file pointer to the beginning
    file->clear(); // Clear any error flags
    file->seekg(0);

    // Read the file header
    FileHeader fileHeader;
    file->read(reinterpret_cast<char*>(&fileHeader), sizeof(FileHeader));

    // Check if the read was successful
    if (!file->good()) {
        std::cerr << "Error: Failed to read the file header." << std::endl;
        return;
    }

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
    table.addRow({"Free Pool Head", std::to_string(fileHeader.free_pool_head)});
    table.addRow({"Max Node ID", std::to_string(fileHeader.max_node_id)});
    table.addRow({"Max Edge ID", std::to_string(fileHeader.max_edge_id)});
    table.addRow({"Max Blob ID", std::to_string(fileHeader.max_blob_id)});
    table.addRow({"Page Size", std::to_string(fileHeader.page_size)});
    table.addRow({"Segment Count", std::to_string(fileHeader.segment_count)});
    table.addRow({"Version", std::to_string(fileHeader.version)});
    table.addRow({"Header CRC", std::to_string(fileHeader.header_crc)});

    // Print the file header table
    table.print();

    // Display segment headers if they exist
    if (fileHeader.node_segment_head != 0) {
        SegmentHeader segmentHeader;
        file->seekg(static_cast<std::streamoff>(fileHeader.node_segment_head));
        file->read(reinterpret_cast<char*>(&segmentHeader), sizeof(SegmentHeader));

        // Create a new table for segment header
        table.clear();
        table.setTitle("Node Segment Header");
        table.addColumn("Attribute");
        table.addColumn("Value");

        table.addRow({"Next Segment Offset", std::to_string(segmentHeader.next_segment_offset)});
        table.addRow({"Start ID", std::to_string(segmentHeader.start_id)});
        table.addRow({"Capacity", std::to_string(segmentHeader.capacity)});
        table.addRow({"Used", std::to_string(segmentHeader.used)});
        table.addRow({"Data Type", std::to_string(segmentHeader.data_type)});
        table.addRow({"Segment CRC", std::to_string(segmentHeader.segment_crc)});

        // Print the segment header table
        table.print();

        // Read and print each node's details
        for (uint32_t i = 0; i < segmentHeader.used; ++i) {
            Node node;
            file->read(reinterpret_cast<char*>(&node), sizeof(Node));

            // Create a new table for node info
            table.clear();
            table.setTitle("Node " + std::to_string(i+1) + " Info");
            table.addColumn("Attribute");
            table.addColumn("Value");

            table.addRow({"Node ID", std::to_string(node.getId())});
            table.addRow({"Label", node.getLabel()});

            // Load properties
            auto properties = property_storage::PropertyStorage::loadProperties(file, node.getPropertyReference());

            // Add Attribute data to the table
            for (const auto& [key, value] : properties) {
                std::string valueStr = std::visit([](const auto& val) -> std::string {
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
                            if (i > 0) oss << ", ";
                            oss << val[i];
                        }
                        oss << "]";
                        return oss.str();
                    } else {
                        return "unknown";
                    }
                }, value);

                table.addRow({"Attribute: " + key, valueStr});
            }

            // Print the node info table
            table.print();
            std::cout << std::endl;
        }
    }
}

} // namespace graph::storage