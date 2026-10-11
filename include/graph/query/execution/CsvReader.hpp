/**
 * @file CsvReader.hpp
 * @brief RFC 4180-compliant streaming CSV parser.
 *
 * Shared between LOAD CSV operator and CLI bulk import.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#pragma once

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include "graph/query/execution/StringBufferBytes.hpp"

namespace graph::query::execution {

	struct CsvReaderLimits {
		size_t maximum_record_bytes = 1U * 1024U * 1024U;
		size_t maximum_columns = 16'384U;
		size_t maximum_field_bytes = 1U * 1024U * 1024U;
		size_t maximum_buffered_bytes = std::numeric_limits<size_t>::max();
	};

class CsvReader {
public:
	explicit CsvReader(const std::string &filePath, const std::string &delimiter = ",",
					   CsvReaderLimits limits = {}, std::function<void()> checkpoint = {})
		: delimiter_(delimiter), limits_(limits), checkpoint_(std::move(checkpoint)) {
		if (delimiter_.empty() || delimiter_.size() > 8U || delimiter_.find_first_of("\r\n\"") != std::string::npos ||
			limits_.maximum_record_bytes == 0U || limits_.maximum_columns == 0U ||
			limits_.maximum_field_bytes == 0U || limits_.maximum_field_bytes > limits_.maximum_record_bytes ||
			limits_.maximum_buffered_bytes == 0U) {
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
		if (peeked_) {
			if (checkpoint_) checkpoint_();
			return true;
		}
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
			if (checkpoint_) checkpoint_();
			peeked_ = false;
			std::vector<std::string> row;
			row.swap(peekedFields_);
			return row;
		}
		std::string line;
		if (!readLogicalLine(line)) {
			throw std::runtime_error("CsvReader: no more rows");
		}
		return parseLine(line);
	}

	// Probe EOF without allocating a record. A shared execution budget may be
	// exhausted by retained downstream rows even when no further input exists.
	[[nodiscard]] bool atEnd() {
		if (checkpoint_) checkpoint_();
		return !peeked_ && (!file_.is_open() ||
			std::char_traits<char>::eq_int_type(file_.rdbuf()->sgetc(), std::char_traits<char>::eof()));
	}

	void setMaximumBufferedBytes(size_t bytes) {
		if (peeked_ || bytes == 0U) throw std::runtime_error("CsvReader: invalid buffered byte allowance");
		limits_.maximum_buffered_bytes = bytes;
	}

	void reset() {
		file_.clear();
		file_.seekg(0, std::ios::beg);
		peeked_ = false;
		std::vector<std::string>().swap(peekedFields_);
	}

private:
	std::ifstream file_;
	std::string delimiter_;
	CsvReaderLimits limits_;
	std::function<void()> checkpoint_;
	bool peeked_ = false;
	std::vector<std::string> peekedFields_;

	// Limit growth while reading, before a physical line can allocate its full
	// length. Use the stream buffer directly to avoid a sentry per character.
	bool appendPhysicalLine(std::string &line, size_t maximumBytes) {
		if (checkpoint_) checkpoint_();
		auto *buffer = file_.rdbuf();
		using Traits = std::char_traits<char>;
		size_t consumed = 0U;
		while (true) {
			const auto value = buffer->sbumpc();
			if (Traits::eq_int_type(value, Traits::eof())) return consumed != 0U;
			++consumed;
			if (checkpoint_ && consumed % 4096U == 0U) checkpoint_();
			const char character = Traits::to_char_type(value);
			if (character == '\n') return true;
			if (line.size() == maximumBytes) {
				// A trailing CR is normalized away and does not consume record bytes.
				const auto next = buffer->sgetc();
				if (character == '\r' && (Traits::eq_int_type(next, Traits::eof()) ||
						Traits::to_char_type(next) == '\n')) continue;
				throw std::runtime_error("CsvReader: logical record exceeds its byte bound");
			}
			ensureStringCapacity(line, line.size() + 1U, 0U);
			line.push_back(character);
			if (dynamicStringCapacity(line) > limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: source buffer capacity exceeds its byte bound");
			}
		}
	}

	bool readLogicalLine(std::string &result) {
		result.clear();
		bool inQuote = false;
		bool fieldStart = true;
		bool quoteClosed = false;

		while (true) {
			const size_t separator = result.empty() ? 0U : 1U;
			if (separator > limits_.maximum_record_bytes - result.size()) {
				throw std::runtime_error("CsvReader: logical record exceeds its byte bound");
			}
			if (result.size() > limits_.maximum_buffered_bytes ||
				separator > limits_.maximum_buffered_bytes - result.size()) {
				throw std::runtime_error("CsvReader: source buffers exceed their byte bound");
			}
			// Append directly to the logical record: a physical-line scratch string
			// would retain and copy the same bytes before parsing can begin.
			if (separator != 0U) {
				ensureStringCapacity(result, result.size() + 1U, 0U);
				result.push_back('\n');
				if (dynamicStringCapacity(result) > limits_.maximum_buffered_bytes) {
					throw std::runtime_error("CsvReader: source buffer capacity exceeds its byte bound");
				}
			}
			const size_t segmentBegin = result.size();
			if (!appendPhysicalLine(result, std::min(limits_.maximum_buffered_bytes,
					limits_.maximum_record_bytes))) break;
			if (dynamicStringCapacity(result) > limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: source buffer capacity exceeds its byte bound");
			}
			if (result.size() > segmentBegin && result.back() == '\r') {
				result.pop_back();
			}

			// Quotes are structural only at a field boundary. A quote in an
			// unquoted field remains literal for backwards-compatible CSV input.
			size_t nextCheckpoint = segmentBegin;
			for (size_t index = segmentBegin; index < result.size(); ++index) {
				if (checkpoint_ && index >= nextCheckpoint) {
					checkpoint_();
					nextCheckpoint = index + 4096U;
				}
				if (inQuote) {
					if (result[index] != '"') {
						continue;
					}
					if (index + 1U < result.size() && result[index + 1U] == '"') {
						++index;
						continue;
					}
					inQuote = false;
					quoteClosed = true;
					continue;
				}
				if (quoteClosed) {
					if (matchDelimiter(result, index)) {
						index += delimiter_.size() - 1U;
						fieldStart = true;
						quoteClosed = false;
					} else {
						fieldStart = false;
						quoteClosed = false;
					}
					continue;
				}
				if (fieldStart && result[index] == '"') {
					inQuote = true;
					fieldStart = false;
				} else if (matchDelimiter(result, index)) {
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
			if (sizeof(std::string) > limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: source buffers exceed their byte bound");
			}
			fields.emplace_back("");
			validateParsedCapacity(line, fields);
			return fields;
		}

		size_t bufferedBytes = line.size();
		const auto charge = [&](size_t bytes) {
			if (bufferedBytes > limits_.maximum_buffered_bytes ||
				bytes > limits_.maximum_buffered_bytes - bufferedBytes) {
				throw std::runtime_error("CsvReader: source buffers exceed their byte bound");
			}
			bufferedBytes += bytes;
		};
		std::string field;
		bool inQuotes = false;
		bool quoteClosed = false;
		size_t i = 0;
		size_t completedFieldBytes = 0U;
		const auto retainedFieldBytes = [&]() {
			return checkedAdd(checkedAdd(dynamicStringCapacity(line), completedFieldBytes),
				checkedMultiply(fields.capacity(), sizeof(std::string)));
		};
		const auto validateCurrentCapacity = [&]() {
			if (checkedAdd(retainedFieldBytes(), dynamicStringCapacity(field)) > limits_.maximum_buffered_bytes)
				throw std::runtime_error("CsvReader: parsed buffers exceed their byte bound");
		};

		const auto append = [&](char value) {
			if (field.size() == limits_.maximum_field_bytes) {
				throw std::runtime_error("CsvReader: field exceeds its byte bound");
			}
			charge(1U);
			ensureStringCapacity(field, field.size() + 1U, retainedFieldBytes());
			field += value;
			validateCurrentCapacity();
		};
		const auto appendField = [&]() {
			if (fields.size() == limits_.maximum_columns) {
				throw std::runtime_error("CsvReader: record exceeds its column bound");
			}
			charge(sizeof(std::string));
			if (fields.size() == fields.capacity()) {
				size_t retainedBytes = checkedAdd(dynamicStringCapacity(line), completedFieldBytes);
				retainedBytes = checkedAdd(retainedBytes, dynamicStringCapacity(field));
				ensureFieldVectorCapacity(fields, fields.size() + 1U, retainedBytes);
				// Recheck moved string capacities after vector relocation for portability.
				completedFieldBytes = 0U;
				for (size_t index = 0U; index < fields.size(); ++index) {
					if (checkpoint_ && (index & 255U) == 0U) checkpoint_();
					completedFieldBytes = checkedAdd(completedFieldBytes, dynamicStringCapacity(fields[index]));
				}
			}
			fields.push_back(std::move(field));
			completedFieldBytes = checkedAdd(completedFieldBytes, dynamicStringCapacity(fields.back()));
			field.clear();
			validateCurrentCapacity();
			quoteClosed = false;
		};

		size_t nextCheckpoint = 0U;
		while (i < line.size()) {
			if (checkpoint_ && i >= nextCheckpoint) {
				checkpoint_();
				nextCheckpoint = i + 4096U;
			}
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
		validateParsedCapacity(line, fields);
		return fields;
	}

		void validateParsedCapacity(const std::string &line, const std::vector<std::string> &fields,
				const std::string *activeField = nullptr) const {
			size_t bytes = dynamicStringCapacity(line);
			if (fields.capacity() > std::numeric_limits<size_t>::max() / sizeof(std::string)) {
				throw std::runtime_error("CsvReader: parsed field capacity overflowed");
			}
			bytes = checkedAdd(bytes, fields.capacity() * sizeof(std::string));
			for (size_t index = 0U; index < fields.size(); ++index) {
				if (checkpoint_ && (index & 255U) == 0U) checkpoint_();
				bytes = checkedAdd(bytes, dynamicStringCapacity(fields[index]));
			}
			if (activeField != nullptr) bytes = checkedAdd(bytes, dynamicStringCapacity(*activeField));
			if (bytes > limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: parsed buffers exceed their byte bound");
			}
		}

		static size_t checkedAdd(size_t left, size_t right) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				throw std::overflow_error("CsvReader: buffered byte count overflowed");
			}
			return left + right;
		}

		static size_t checkedMultiply(size_t left, size_t right) {
			if (right != 0U && left > std::numeric_limits<size_t>::max() / right) {
				throw std::overflow_error("CsvReader: buffered capacity count overflowed");
			}
			return left * right;
		}

		static size_t dynamicStringCapacity(const std::string &value) {
			return detail::dynamicStringBufferBytes(value);
		}

		size_t growthCapacity(size_t current, size_t required) const {
			const size_t growth = std::max<size_t>(1U, current / 2U);
			const size_t grown = growth > std::numeric_limits<size_t>::max() - current
					? required : current + growth;
			return std::max(required, grown);
		}

		void ensureStringCapacity(std::string &value, size_t required, size_t retainedBytes) const {
			if (required <= value.capacity()) return;
			const size_t target = growthCapacity(value.capacity(), required);
			const size_t oldBytes = dynamicStringCapacity(value);
			// Growing beyond current capacity requires a new external buffer.
			const size_t targetBytes = checkedAdd(target, 1U);
			const size_t peakBytes = checkedAdd(checkedAdd(retainedBytes, oldBytes), targetBytes);
			if (peakBytes > limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: buffer growth peak exceeds its byte bound");
			}
			// Reserving on the existing string can apply another geometric growth
			// policy (for example, libstdc++ doubles its old capacity). Start from
			// an empty buffer and validate the actual coexistence peak before copying.
			std::string replacement;
			replacement.reserve(target);
			if (checkedAdd(checkedAdd(retainedBytes, oldBytes), dynamicStringCapacity(replacement)) >
				limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: string capacity exceeds its byte bound");
			}
			replacement.assign(value);
			value.swap(replacement);
		}

		void ensureFieldVectorCapacity(std::vector<std::string> &fields, size_t required,
				size_t retainedBytes) const {
			if (required <= fields.capacity()) return;
			const size_t target = growthCapacity(fields.capacity(), required);
			const size_t oldBytes = checkedMultiply(fields.capacity(), sizeof(std::string));
			const size_t targetBytes = checkedMultiply(target, sizeof(std::string));
			const size_t peakBytes = checkedAdd(checkedAdd(retainedBytes, oldBytes), targetBytes);
			if (peakBytes > limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: field-vector growth peak exceeds its byte bound");
			}
			fields.reserve(target);
			const size_t actualBytes = checkedMultiply(fields.capacity(), sizeof(std::string));
			if (checkedAdd(retainedBytes, actualBytes) > limits_.maximum_buffered_bytes) {
				throw std::runtime_error("CsvReader: field-vector capacity exceeds its byte bound");
			}
		}

	bool matchDelimiter(const std::string &line, size_t pos) const {
		if (pos + delimiter_.size() > line.size()) return false;
		return line.compare(pos, delimiter_.size(), delimiter_) == 0;
	}
};

} // namespace graph::query::execution
