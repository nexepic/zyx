/**
 * @file ImportCommand.cpp
 * @brief CLI bulk import command implementation.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include "graph/cli/ImportCommand.hpp"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "graph/core/Database.hpp"
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/execution/CsvReader.hpp"

namespace graph::cli {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

struct ColumnInfo {
	std::string name;       // Property name (empty for special columns)
	std::string typeHint;   // STRING, INT, LONG, FLOAT, DOUBLE, BOOLEAN, INT[], STRING[], etc.
	bool isId = false;
	bool isLabel = false;
	bool isStartId = false;
	bool isEndId = false;
	bool isType = false;
	std::string idGroup;    // For :ID(GroupName)
};

/**
 * Parse a Neo4j CSV column header like "name:STRING", ":ID", ":LABEL", "age:INT"
 */
ColumnInfo parseNeo4jHeader(const std::string &header) {
	ColumnInfo info;

	if (header == ":ID" || header.find(":ID(") == 0) {
		info.isId = true;
		if (header.find(":ID(") == 0 && header.back() == ')') {
			info.idGroup = header.substr(4, header.size() - 5);
		}
		return info;
	}
	if (header == ":LABEL") { info.isLabel = true; return info; }
	if (header == ":START_ID" || header.find(":START_ID(") == 0) {
		info.isStartId = true;
		if (header.find(":START_ID(") == 0 && header.back() == ')') {
			info.idGroup = header.substr(10, header.size() - 11);
		}
		return info;
	}
	if (header == ":END_ID" || header.find(":END_ID(") == 0) {
		info.isEndId = true;
		if (header.find(":END_ID(") == 0 && header.back() == ')') {
			info.idGroup = header.substr(8, header.size() - 9);
		}
		return info;
	}
	if (header == ":TYPE") { info.isType = true; return info; }

	// Property column: "name" or "name:TYPE"
	auto colonPos = header.find(':');
	if (colonPos != std::string::npos) {
		info.name = header.substr(0, colonPos);
		info.typeHint = header.substr(colonPos + 1);
		// Normalize type hint
		std::transform(info.typeHint.begin(), info.typeHint.end(),
		               info.typeHint.begin(), ::toupper);

		// Check if it's actually an ID column with name: "personId:ID"
		if (info.typeHint == "ID" || info.typeHint.substr(0, 3) == "ID(") {
			info.isId = true;
			if (info.typeHint.size() > 3 && info.typeHint.back() == ')') {
				info.idGroup = info.typeHint.substr(3, info.typeHint.size() - 4);
			}
			info.typeHint.clear();
		}
	} else {
		info.name = header;
		info.typeHint = "STRING";
	}

	return info;
}

/**
 * Convert a string value to PropertyValue based on type hint.
 */
PropertyValue convertValue(const std::string &value, const std::string &typeHint,
                           const std::string &arrayDelimiter = ";") {
	if (value.empty()) return PropertyValue();

	// Array types
	if (typeHint.size() > 2 && typeHint.substr(typeHint.size() - 2) == "[]") {
		std::string elementType = typeHint.substr(0, typeHint.size() - 2);
		std::vector<PropertyValue> arr;
		std::stringstream ss(value);
		std::string elem;
		while (std::getline(ss, elem, arrayDelimiter[0])) {
			if (elementType == "INT" || elementType == "INTEGER" || elementType == "LONG") {
				try { arr.emplace_back(static_cast<int64_t>(std::stoll(elem))); }
				catch (...) { arr.emplace_back(elem); }
			} else if (elementType == "FLOAT" || elementType == "DOUBLE") {
				try { arr.emplace_back(std::stod(elem)); }
				catch (...) { arr.emplace_back(elem); }
			} else if (elementType == "BOOLEAN") {
				arr.emplace_back(elem == "true" || elem == "TRUE" || elem == "1");
			} else {
				arr.emplace_back(elem);
			}
		}
		return PropertyValue(std::move(arr));
	}

	// Scalar types
	if (typeHint == "INT" || typeHint == "INTEGER" || typeHint == "LONG") {
		try { return PropertyValue(static_cast<int64_t>(std::stoll(value))); }
		catch (...) { return PropertyValue(value); }
	}
	if (typeHint == "FLOAT" || typeHint == "DOUBLE") {
		try { return PropertyValue(std::stod(value)); }
		catch (...) { return PropertyValue(value); }
	}
	if (typeHint == "BOOLEAN") {
		return PropertyValue(value == "true" || value == "TRUE" || value == "1");
	}

	return PropertyValue(value);
}

/**
 * Split a label string like "Person;Employee" into individual labels.
 */
std::vector<std::string> splitLabels(const std::string &labelStr) {
	std::vector<std::string> labels;
	std::stringstream ss(labelStr);
	std::string label;
	while (std::getline(ss, label, ';')) {
		if (!label.empty()) labels.push_back(label);
	}
	return labels;
}

std::string formatValueForCypher(const PropertyValue &v) {
        if (v.getType() == PropertyType::STRING) {
                std::string escaped = v.toString();
                size_t pos = 0;
                while ((pos = escaped.find('\'', pos)) != std::string::npos) {
                        escaped.replace(pos, 1, "\\'");
                        pos += 2;
                }
                return "'" + escaped + "'";
        } else if (v.getType() == PropertyType::LIST) {
                std::string res = "[";
                const auto& list = v.getList();
                for (size_t i = 0; i < list.size(); ++i) {
                        if (i > 0) res += ", ";
                        res += formatValueForCypher(list[i]);
                }
                res += "]";
                return res;
        } else if (v.getType() == PropertyType::DATE) {
                return "date('" + v.toString() + "')";
        } else if (v.getType() == PropertyType::DATETIME) {
                return "datetime('" + v.toString() + "')";
        } else if (v.getType() == PropertyType::DURATION) {
                return "duration('" + v.toString() + "')";
        }
        return v.toString();
}

/**
 * Import nodes from a Neo4j-compatible CSV file.
 */
void importNodesCsv(Database &db, const std::string &filePath,
                    ImportContext &ctx, const std::string &arrayDelimiter,
                    bool skipBadEntries) {
	query::execution::CsvReader reader(filePath);

	// Read headers
	if (!reader.hasNext()) {
		throw std::runtime_error("Empty CSV file: " + filePath);
	}
	auto headers = reader.nextRow();
	std::vector<ColumnInfo> columns;
	columns.reserve(headers.size());
	for (const auto &h : headers) {
		columns.push_back(parseNeo4jHeader(h));
	}

	auto queryEngine = db.getQueryEngine();

	while (reader.hasNext()) {
		auto fields = reader.nextRow();
		if (fields.size() != columns.size()) {
			if (skipBadEntries) { ctx.errorsSkipped++; continue; }
			throw std::runtime_error("Row has " + std::to_string(fields.size()) +
			                         " fields but header has " + std::to_string(columns.size()));
		}

		// Build CREATE statement
		std::string externalId;
		std::vector<std::string> labels;
		std::vector<std::pair<std::string, PropertyValue>> properties;

		for (size_t i = 0; i < fields.size(); ++i) {
			const auto &col = columns[i];
			const auto &val = fields[i];

			if (col.isId) {
				externalId = val;
			} else if (col.isLabel) {
				labels = splitLabels(val);
			} else if (!col.name.empty()) {
				auto pv = convertValue(val, col.typeHint, arrayDelimiter);
				if (pv.getType() != PropertyType::NULL_TYPE) {
					properties.emplace_back(col.name, std::move(pv));
				}
			}
		}

		// Build and execute CREATE query
		std::string labelStr;
		for (const auto &l : labels) {
			labelStr += ":" + l;
		}
		if (labelStr.empty()) labelStr = ":Node"; // Default label

		std::string propsStr;
		if (!properties.empty()) {
			propsStr = " {";
			bool first = true;
			for (const auto &[k, v] : properties) {
				if (!first) propsStr += ", ";
				propsStr += k + ": " + formatValueForCypher(v);
				first = false;
				ctx.propertiesSet++;
			}
			propsStr += "}";
		}
		std::string query = "CREATE (n" + labelStr + propsStr + ") RETURN id(n)";

		try {
			auto result = queryEngine->execute(query);
			if (result.rowCount() > 0 && !result.getRows().empty()) {
				auto &row = result.getRows()[0];
				if (row.count("id(n)")) {
					auto &idVal = row.at("id(n)");
					if (idVal.isPrimitive() && idVal.asPrimitive().getType() == PropertyType::INTEGER) {
						uint64_t internalId = static_cast<uint64_t>(std::get<int64_t>(idVal.asPrimitive().getVariant()));
						if (!externalId.empty()) {
							ctx.nodeIdMap[externalId] = internalId;
						}
					}
				}
			}
			ctx.nodesCreated++;
		} catch (const std::exception &e) {
			if (skipBadEntries) {
				ctx.errorsSkipped++;
				std::cerr << "Warning: skipping node: " << e.what() << std::endl;
			} else {
				throw;
			}
		}
	}
}

/**
 * Import relationships from a Neo4j-compatible CSV file.
 */
void importRelationshipsCsv(Database &db, const std::string &filePath,
                            ImportContext &ctx, const std::string &arrayDelimiter,
                            bool skipBadEntries) {
	query::execution::CsvReader reader(filePath);

	if (!reader.hasNext()) {
		throw std::runtime_error("Empty CSV file: " + filePath);
	}
	auto headers = reader.nextRow();
	std::vector<ColumnInfo> columns;
	columns.reserve(headers.size());
	for (const auto &h : headers) {
		columns.push_back(parseNeo4jHeader(h));
	}

	auto queryEngine = db.getQueryEngine();

	while (reader.hasNext()) {
		auto fields = reader.nextRow();
		if (fields.size() != columns.size()) {
			if (skipBadEntries) { ctx.errorsSkipped++; continue; }
			throw std::runtime_error("Row field count mismatch");
		}

		std::string startId, endId, relType;
		std::vector<std::pair<std::string, PropertyValue>> properties;

		for (size_t i = 0; i < fields.size(); ++i) {
			const auto &col = columns[i];
			const auto &val = fields[i];

			if (col.isStartId) startId = val;
			else if (col.isEndId) endId = val;
			else if (col.isType) relType = val;
			else if (!col.name.empty()) {
				auto pv = convertValue(val, col.typeHint, arrayDelimiter);
				if (pv.getType() != PropertyType::NULL_TYPE) {
					properties.emplace_back(col.name, std::move(pv));
				}
			}
		}

		// Resolve IDs
		auto startIt = ctx.nodeIdMap.find(startId);
		auto endIt = ctx.nodeIdMap.find(endId);
		if (startIt == ctx.nodeIdMap.end() || endIt == ctx.nodeIdMap.end()) {
			if (skipBadEntries) {
				ctx.errorsSkipped++;
				std::cerr << "Warning: cannot resolve relationship " << startId << " -> " << endId << std::endl;
				continue;
			}
			throw std::runtime_error("Cannot resolve node IDs for relationship: " +
			                         startId + " -> " + endId);
		}

		// Build MATCH + CREATE query using internal IDs
		std::string propsStr;
		if (!properties.empty()) {
			propsStr = " {";
			bool first = true;
			for (const auto &[k, v] : properties) {
				if (!first) propsStr += ", ";
				propsStr += k + ": " + formatValueForCypher(v);
				first = false;
				ctx.propertiesSet++;
			}
			propsStr += "}";
		}
		std::string query = "MATCH (a), (b) WHERE id(a) = " +
		                    std::to_string(startIt->second) + " AND id(b) = " +
		                    std::to_string(endIt->second) +
		                    " CREATE (a)-[:" + relType + propsStr + "]->(b)";

		try {
			queryEngine->execute(query);
			ctx.edgesCreated++;
		} catch (const std::exception &e) {
			if (skipBadEntries) {
				ctx.errorsSkipped++;
				std::cerr << "Warning: skipping relationship: " << e.what() << std::endl;
			} else {
				throw;
			}
		}
	}
}

/**
 * Import nodes from a JSONL file.
 * Each line: {"_id": "p1", "_labels": ["Person"], "name": "Alice", "age": 30}
 */
void importNodesJsonl(Database &db, const std::string &filePath,
                      ImportContext &ctx, bool skipBadEntries) {
	std::ifstream file(filePath);
	if (!file.is_open()) {
		throw std::runtime_error("Cannot open JSONL file: " + filePath);
	}

	auto queryEngine = db.getQueryEngine();
	std::string line;

	while (std::getline(file, line)) {
		if (line.empty()) continue;

		// Simple JSON parsing — extract key-value pairs
		// For robustness, we use a Cypher query approach:
		// Parse the JSON line and build a CREATE query from it.
		// We'll do minimal JSON parsing for the reserved fields.

		// Find _id
		std::string externalId;
		auto idPos = line.find("\"_id\"");
		if (idPos != std::string::npos) {
			auto colonPos = line.find(':', idPos + 5);
			auto valStart = line.find('"', colonPos + 1);
			if (valStart != std::string::npos) {
				auto valEnd = line.find('"', valStart + 1);
				externalId = line.substr(valStart + 1, valEnd - valStart - 1);
			}
		}

		// Find _labels
		std::vector<std::string> labels;
		auto labelsPos = line.find("\"_labels\"");
		if (labelsPos != std::string::npos) {
			auto bracketStart = line.find('[', labelsPos);
			auto bracketEnd = line.find(']', bracketStart);
			if (bracketStart != std::string::npos && bracketEnd != std::string::npos) {
				std::string labelsStr = line.substr(bracketStart + 1, bracketEnd - bracketStart - 1);
				size_t pos = 0;
				while ((pos = labelsStr.find('"', pos)) != std::string::npos) {
					auto endQuote = labelsStr.find('"', pos + 1);
					if (endQuote != std::string::npos) {
						labels.push_back(labelsStr.substr(pos + 1, endQuote - pos - 1));
						pos = endQuote + 1;
					} else break;
				}
			}
		}

		std::string labelStr;
		for (const auto &l : labels) {
			labelStr += ":" + l;
		}
		if (labelStr.empty()) labelStr = ":Node";

		// For JSONL, create node with the whole line as properties via Cypher
		// Simple approach: strip reserved fields, pass rest as properties
		// For now, create with labels only and use SET for properties
		std::string query = "CREATE (n" + labelStr + ") RETURN id(n)";

		try {
			auto result = queryEngine->execute(query);
			if (result.rowCount() > 0 && !result.getRows().empty()) {
				auto &row = result.getRows()[0];
				if (row.count("id(n)")) {
					auto &idVal = row.at("id(n)");
					if (idVal.isPrimitive() && idVal.asPrimitive().getType() == PropertyType::INTEGER) {
						uint64_t internalId = static_cast<uint64_t>(std::get<int64_t>(idVal.asPrimitive().getVariant()));
						if (!externalId.empty()) {
							ctx.nodeIdMap[externalId] = internalId;
						}
					}
				}
			}
			ctx.nodesCreated++;
		} catch (const std::exception &e) {
			if (skipBadEntries) {
				ctx.errorsSkipped++;
				std::cerr << "Warning: skipping JSONL node: " << e.what() << std::endl;
			} else {
				throw;
			}
		}
	}
}

/**
 * Import relationships from a JSONL file.
 * Each line: {"_start": "p1", "_end": "p2", "_type": "KNOWS", "since": 2020}
 */
void importRelationshipsJsonl(Database &db, const std::string &filePath,
                              ImportContext &ctx, bool skipBadEntries) {
	std::ifstream file(filePath);
	if (!file.is_open()) {
		throw std::runtime_error("Cannot open JSONL file: " + filePath);
	}

	auto queryEngine = db.getQueryEngine();
	std::string line;

	while (std::getline(file, line)) {
		if (line.empty()) continue;

		// Extract _start, _end, _type
		auto extractStr = [&line](const std::string &key) -> std::string {
			auto pos = line.find("\"" + key + "\"");
			if (pos == std::string::npos) return "";
			auto colonPos = line.find(':', pos + key.size() + 2);
			auto valStart = line.find('"', colonPos + 1);
			if (valStart == std::string::npos) return "";
			auto valEnd = line.find('"', valStart + 1);
			return line.substr(valStart + 1, valEnd - valStart - 1);
		};

		std::string startId = extractStr("_start");
		std::string endId = extractStr("_end");
		std::string relType = extractStr("_type");

		auto startIt = ctx.nodeIdMap.find(startId);
		auto endIt = ctx.nodeIdMap.find(endId);
		if (startIt == ctx.nodeIdMap.end() || endIt == ctx.nodeIdMap.end()) {
			if (skipBadEntries) {
				ctx.errorsSkipped++;
				continue;
			}
			throw std::runtime_error("Cannot resolve JSONL relationship: " + startId + " -> " + endId);
		}

		std::string query = "MATCH (a), (b) WHERE id(a) = " +
		                    std::to_string(startIt->second) + " AND id(b) = " +
		                    std::to_string(endIt->second) +
		                    " CREATE (a)-[:" + relType + "]->(b)";

		try {
			queryEngine->execute(query);
			ctx.edgesCreated++;
		} catch (const std::exception &e) {
			if (skipBadEntries) {
				ctx.errorsSkipped++;
				std::cerr << "Warning: skipping JSONL relationship: " << e.what() << std::endl;
			} else {
				throw;
			}
		}
	}
}

/**
 * Determine file format from extension.
 */
std::string detectFormat(const std::string &filePath) {
	if (filePath.size() >= 6 && filePath.substr(filePath.size() - 6) == ".jsonl") return "jsonl";
	if (filePath.size() >= 5 && filePath.substr(filePath.size() - 5) == ".json") return "jsonl";
	return "csv"; // Default to CSV
}

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

struct ImportOptions {
	std::vector<std::string> nodeFiles;
	std::vector<std::string> relFiles;
	std::string dbPath;
	std::string format = "auto";
	std::string arrayDelimiter = ";";
	bool skipBadEntries = false;
};

void registerImportCommand(CLI::App &parentApp) {
	auto *importCmd = parentApp.add_subcommand("import", "Bulk import data from CSV/JSONL files");

	auto options = std::make_shared<ImportOptions>();

	importCmd->add_option("--nodes", options->nodeFiles, "Node data files (CSV or JSONL)")
		->required();
	importCmd->add_option("--relationships,--rels", options->relFiles, "Relationship data files");
	importCmd->add_option("--database,--db", options->dbPath, "Database path")->required();
	importCmd->add_option("--format", options->format, "File format: auto, csv, jsonl (default: auto)");
	importCmd->add_option("--array-delimiter", options->arrayDelimiter, "Array value delimiter (default: ;)");
	importCmd->add_flag("--skip-bad-entries", options->skipBadEntries, "Skip malformed rows");

	importCmd->callback([options]() {
		try {
			Database db(options->dbPath, storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
			db.open();

			ImportContext ctx;

			std::cout << "Importing nodes..." << std::endl;
			for (const auto &file : options->nodeFiles) {
				std::string fmt = (options->format == "auto") ? detectFormat(file) : options->format;
				std::cout << "  " << file << " (format: " << fmt << ")" << std::endl;
				if (fmt == "jsonl") {
					importNodesJsonl(db, file, ctx, options->skipBadEntries);
				} else {
					importNodesCsv(db, file, ctx, options->arrayDelimiter, options->skipBadEntries);
				}
			}

			if (!options->relFiles.empty()) {
				std::cout << "Importing relationships..." << std::endl;
				for (const auto &file : options->relFiles) {
					std::string fmt = (options->format == "auto") ? detectFormat(file) : options->format;
					std::cout << "  " << file << " (format: " << fmt << ")" << std::endl;
					if (fmt == "jsonl") {
						importRelationshipsJsonl(db, file, ctx, options->skipBadEntries);
					} else {
						importRelationshipsCsv(db, file, ctx, options->arrayDelimiter, options->skipBadEntries);
					}
				}
			}

			// Flush and close
			if (auto storage = db.getStorage()) {
				storage->flush();
			}
			db.close();

			std::cout << "\nImport complete:" << std::endl;
			std::cout << "  Nodes created:      " << ctx.nodesCreated << std::endl;
			std::cout << "  Relationships created: " << ctx.edgesCreated << std::endl;
			std::cout << "  Properties set:     " << ctx.propertiesSet << std::endl;
			if (ctx.errorsSkipped > 0) {
				std::cout << "  Errors skipped:     " << ctx.errorsSkipped << std::endl;
			}
		} catch (const std::exception &e) {
			std::cerr << "Import error: " << e.what() << std::endl;
		}
	});
}

} // namespace graph::cli
