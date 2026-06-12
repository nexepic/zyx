#pragma once

#include <cstdint>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace graph::query::planner {

struct AccessPathSummary {
	std::string kind;
	std::string reason;
	std::optional<int64_t> estimatedCardinality;
	double estimatedCost = 0.0;
	bool exactCardinality = false;
	std::string estimateSource;
	bool directCandidateLookup = false;
	bool valid = true;
};

inline std::string formatAccessPathCost(double cost) {
	std::ostringstream oss;
	oss << std::setprecision(6) << cost;
	return oss.str();
}

inline std::vector<std::pair<std::string, std::string>>
toAccessPathAttributes(const AccessPathSummary &summary, const std::string &prefix = "access_path") {
	std::vector<std::pair<std::string, std::string>> attributes;
	attributes.reserve(8);
	attributes.emplace_back(prefix + ".kind", summary.kind);
	attributes.emplace_back(prefix + ".reason", summary.reason);
	attributes.emplace_back(prefix + ".estimated_cardinality",
	                        summary.estimatedCardinality.has_value()
	                                ? std::to_string(*summary.estimatedCardinality)
	                                : std::string("unknown"));
	attributes.emplace_back(prefix + ".estimated_cost", formatAccessPathCost(summary.estimatedCost));
	attributes.emplace_back(prefix + ".exact_cardinality", summary.exactCardinality ? "true" : "false");
	attributes.emplace_back(prefix + ".estimate_source", summary.estimateSource);
	attributes.emplace_back(prefix + ".direct_candidate_lookup",
	                        summary.directCandidateLookup ? "true" : "false");
	attributes.emplace_back(prefix + ".valid", summary.valid ? "true" : "false");
	return attributes;
}

inline void appendAccessPathAttributes(std::vector<std::pair<std::string, std::string>> &attributes,
                                       const AccessPathSummary &summary,
                                       const std::string &prefix) {
	auto summaryAttributes = toAccessPathAttributes(summary, prefix);
	attributes.insert(attributes.end(),
	                  std::make_move_iterator(summaryAttributes.begin()),
	                  std::make_move_iterator(summaryAttributes.end()));
}

} // namespace graph::query::planner
