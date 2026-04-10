/**
 * @file NamedPathOperator.hpp
 * @brief Physical operator that constructs a named path from pattern variables.
 *
 * On each record, collects node and edge values and builds a path LIST.
 */

#pragma once

#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include <string>
#include <vector>

namespace graph::query::execution::operators {

class NamedPathOperator : public PhysicalOperator {
public:
	NamedPathOperator(
		std::unique_ptr<PhysicalOperator> child,
		std::shared_ptr<graph::storage::DataManager> dm,
		std::string pathVariable,
		std::vector<std::string> nodeVariables,
		std::vector<std::string> edgeVariables
	) : child_(std::move(child)),
		dm_(std::move(dm)),
		pathVariable_(std::move(pathVariable)),
		nodeVariables_(std::move(nodeVariables)),
		edgeVariables_(std::move(edgeVariables)) {}

	void open() override {
		if (child_) child_->open();
	}

	std::optional<RecordBatch> next() override {
		if (!child_) return std::nullopt;

		auto batch = child_->next();
		if (!batch) return std::nullopt;

		for (auto& record : *batch) {
			// Build path as alternating node/edge MAPs
			std::vector<PropertyValue> pathList;

			for (size_t i = 0; i < nodeVariables_.size(); ++i) {
				// Add node
				auto nodeOpt = record.getNode(nodeVariables_[i]);
				if (nodeOpt) {
					std::unordered_map<std::string, PropertyValue> nodeMap;
					nodeMap["_type"] = PropertyValue(std::string("node"));
					nodeMap["_id"] = PropertyValue(nodeOpt->getId());
					if (dm_) {
						auto props = dm_->getNodeProperties(nodeOpt->getId());
						for (const auto& [k, v] : props) {
							nodeMap[k] = v;
						}
					}
					pathList.push_back(PropertyValue(std::move(nodeMap)));
				}

				// Add edge (if there's one between this node and the next)
				if (i < edgeVariables_.size()) {
					auto edgeOpt = record.getEdge(edgeVariables_[i]);
					if (edgeOpt) {
						std::unordered_map<std::string, PropertyValue> edgeMap;
						edgeMap["_type"] = PropertyValue(std::string("relationship"));
						edgeMap["_id"] = PropertyValue(edgeOpt->getId());
						if (dm_) {
							auto props = dm_->getEdgeProperties(edgeOpt->getId());
							for (const auto& [k, v] : props) {
								edgeMap[k] = v;
							}
						}
						pathList.push_back(PropertyValue(std::move(edgeMap)));
					}
				}
			}

			record.setValue(pathVariable_, PropertyValue(std::move(pathList)));
		}

		return batch;
	}

	void close() override {
		if (child_) child_->close();
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
		vars.push_back(pathVariable_);
		return vars;
	}

	[[nodiscard]] std::string toString() const override {
		return "NamedPath(" + pathVariable_ + ")";
	}

	[[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
		if (child_) return {child_.get()};
		return {};
	}

private:
	std::unique_ptr<PhysicalOperator> child_;
	std::shared_ptr<graph::storage::DataManager> dm_;
	std::string pathVariable_;
	std::vector<std::string> nodeVariables_;
	std::vector<std::string> edgeVariables_;
};

} // namespace graph::query::execution::operators
