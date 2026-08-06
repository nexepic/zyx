/**
 * @file CsvReader.hpp
 * @brief RFC 4180-compliant streaming CSV parser.
 *
 * Shared between LOAD CSV operator and CLI bulk import.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <cstddef>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::execution {

	struct CsvReaderLimits {
		size_t maximum_record_bytes = 1U * 1024U * 1024U;
		size_t maximum_columns = 16'384U;
		size_t maximum_field_bytes = 1U * 1024U * 1024U;
	};

class CsvReader {
public:
	explicit CsvReader(const std::string &filePath, const std::string &delimiter = ",",
					   CsvReaderLimits limits = {})
		: delimiter_(delimiter), limits_(limits) {
		if (delimiter_.empty() || delimiter_.size() > 8U || delimiter_.find_first_of("\r\n\"") != std::string::npos ||
			limits_.maximum_record_bytes == 0U || limits_.maximum_columns == 0U ||
			limits_.maximum_field_bytes == 0U || limits_.maximum_field_bytes > limits_.maximum_record_bytes) {
			throw std::invalid_argument("CsvReader: invalid delimiter or resource limits");
		}
		file_.open(filePath, std::ios::in);
		if (!file_.is_open()) {
			throw std::runtime_error("CsvReader: cannot open file: " + filePath);
		}
	}

	~CsvReader() {
		if (file_.is_open()) file_.close();
	}

	CsvReader(const CsvReader &) = delete;
	CsvReader &operator=(const CsvReader &) = delete;

	[[nodiscard]] bool hasNext() {
		if (peeked_) return true;
		if (!file_.is_open() || file_.eof()) return false;
		// Try to read the next line
		std::string line;
		if (!readLogicalLine(line)) return false;
		peekedFields_ = parseLine(line);
		peeked_ = true;
		return true;
	}

	std::vector<std::string> nextRow() {
		if (peeked_) {
			peeked_ = false;
			return std::move(peekedFields_);
		}
		std::string line;
		if (!readLogicalLine(line)) {
			throw std::runtime_error("CsvReader: no more rows");
		}
		return parseLine(line);
	}

	void reset() {
		file_.clear();
		file_.seekg(0, std::ios::beg);
		peeked_ = false;
		peekedFields_.clear();
	}

private:
	std::ifstream file_;
	std::string delimiter_;
	CsvReaderLimits limits_;
	bool peeked_ = false;
	std::vector<std::string> peekedFields_;

	bool readLogicalLine(std::string &result) {
		result.clear();
		std::string line;
		bool inQuote = false;
		bool fieldStart = true;
		bool quoteClosed = false;

		while (std::getline(file_, line)) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			const size_t separatorBytes = result.empty() ? 0U : 1U;
			if (line.size() > limits_.maximum_record_bytes - separatorBytes ||
				result.size() > limits_.maximum_record_bytes - separatorBytes - line.size()) {
				throw std::runtime_error("CsvReader: logical record exceeds its byte bound");
			}
			if (result.empty()) {
				result = line;
			} else {
				result += "\n" + line;
			}

			// Quotes are structural only at a field boundary. A quote in an
			// unquoted field remains literal for backwards-compatible CSV input.
			for (size_t index = 0U; index < line.size(); ++index) {
				if (inQuote) {
					if (line[index] != '"') {
						continue;
					}
					if (index + 1U < line.size() && line[index + 1U] == '"') {
						++index;
						continue;
					}
					inQuote = false;
					quoteClosed = true;
					continue;
				}
				if (quoteClosed) {
					if (matchDelimiter(line, index)) {
						index += delimiter_.size() - 1U;
						fieldStart = true;
						quoteClosed = false;
					} else {
						fieldStart = false;
						quoteClosed = false;
					}
					continue;
				}
				if (fieldStart && line[index] == '"') {
					inQuote = true;
					fieldStart = false;
				} else if (matchDelimiter(line, index)) {
					index += delimiter_.size() - 1U;
					fieldStart = true;
				} else {
					fieldStart = false;
				}
			}

			if (!inQuote) {
				return true;
			}
		}

		if (inQuote) {
			throw std::runtime_error("CsvReader: unterminated quoted field");
		}
		return !result.empty();
	}

	std::vector<std::string> parseLine(const std::string &line) const {
		std::vector<std::string> fields;
		if (line.empty()) {
			fields.emplace_back("");
			return fields;
		}

		std::string field;
		bool inQuotes = false;
		bool quoteClosed = false;
		size_t i = 0;
		const auto append = [&](char value) {
			if (field.size() == limits_.maximum_field_bytes) {
				throw std::runtime_error("CsvReader: field exceeds its byte bound");
			}
			field += value;
		};
		const auto appendField = [&]() {
			if (fields.size() == limits_.maximum_columns) {
				throw std::runtime_error("CsvReader: record exceeds its column bound");
			}
			fields.push_back(std::move(field));
			field.clear();
			quoteClosed = false;
		};

		while (i < line.size()) {
			if (inQuotes) {
				if (line[i] == '"') {
					if (i + 1 < line.size() && line[i + 1] == '"') {
						// Escaped quote
						append('"');
						i += 2;
					} else {
						// End of quoted field
						inQuotes = false;
						quoteClosed = true;
						i++;
					}
				} else {
					append(line[i]);
					i++;
				}
			} else {
				if (quoteClosed) {
					if (!matchDelimiter(line, i)) {
						throw std::runtime_error("CsvReader: unexpected data after a quoted field");
					}
					appendField();
					i += delimiter_.size();
					continue;
				}
				if (line[i] == '"' && field.empty()) {
					// Start of quoted field
					inQuotes = true;
					i++;
				} else if (matchDelimiter(line, i)) {
					appendField();
					i += delimiter_.size();
				} else {
					append(line[i]);
					i++;
				}
			}
		}

		if (inQuotes) {
			throw std::runtime_error("CsvReader: unterminated quoted field");
		}
		appendField();
		return fields;
	}

	bool matchDelimiter(const std::string &line, size_t pos) const {
		if (pos + delimiter_.size() > line.size()) return false;
		return line.compare(pos, delimiter_.size(), delimiter_) == 0;
	}
};

} // namespace graph::query::execution
