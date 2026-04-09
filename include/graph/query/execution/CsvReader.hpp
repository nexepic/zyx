/**
 * @file CsvReader.hpp
 * @brief RFC 4180-compliant streaming CSV parser.
 *
 * Shared between LOAD CSV operator and CLI bulk import.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph::query::execution {

class CsvReader {
public:
	explicit CsvReader(const std::string &filePath, const std::string &delimiter = ",")
		: delimiter_(delimiter) {
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
	bool peeked_ = false;
	std::vector<std::string> peekedFields_;

	bool readLogicalLine(std::string &result) {
		result.clear();
		std::string line;
		bool inQuote = false;

		while (std::getline(file_, line)) {
			if (result.empty()) {
				result = line;
			} else {
				result += "\n" + line;
			}

			// Count unescaped quotes to determine if we're inside a quoted field
			for (char c : line) {
				if (c == '"') inQuote = !inQuote;
			}

			if (!inQuote) {
				return true;
			}
		}

		// Handle last line without trailing newline
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
		size_t i = 0;

		while (i < line.size()) {
			if (inQuotes) {
				if (line[i] == '"') {
					if (i + 1 < line.size() && line[i + 1] == '"') {
						// Escaped quote
						field += '"';
						i += 2;
					} else {
						// End of quoted field
						inQuotes = false;
						i++;
					}
				} else {
					field += line[i];
					i++;
				}
			} else {
				if (line[i] == '"' && field.empty()) {
					// Start of quoted field
					inQuotes = true;
					i++;
				} else if (matchDelimiter(line, i)) {
					fields.push_back(field);
					field.clear();
					i += delimiter_.size();
				} else {
					field += line[i];
					i++;
				}
			}
		}

		fields.push_back(field);
		return fields;
	}

	bool matchDelimiter(const std::string &line, size_t pos) const {
		if (pos + delimiter_.size() > line.size()) return false;
		return line.compare(pos, delimiter_.size(), delimiter_) == 0;
	}
};

} // namespace graph::query::execution
