#pragma once
#include <string>

namespace graph::parser::cypher::helpers {
// Decode source spelling once; doubled backticks encode one logical backtick.
inline std::string normalizeSymbolicName(std::string value) {
	if (value.size() < 2U || value.front() != '`' || value.back() != '`') return value;
	std::string result; result.reserve(value.size() - 2U);
	for (size_t index = 1U; index + 1U < value.size(); ++index) {
		result.push_back(value[index]);
		if (value[index] == '`' && index + 1U < value.size() - 1U && value[index + 1U] == '`') ++index;
	}
	return result;
}
}
