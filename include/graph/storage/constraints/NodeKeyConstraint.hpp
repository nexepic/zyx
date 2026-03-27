/**
 * @file NodeKeyConstraint.hpp
 * @author Nexepic
 * @date 2026/3/27
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include <memory>
#include <stdexcept>
#include "IConstraint.hpp"

namespace graph::query::indexes {
	class IndexManager;
}

namespace graph::storage::constraints {

class NodeKeyConstraint : public IConstraint {
public:
	NodeKeyConstraint(std::string name, std::string label, std::vector<std::string> properties,
	                  std::shared_ptr<query::indexes::IndexManager> indexManager)
		: name_(std::move(name)), label_(std::move(label)),
		  properties_(std::move(properties)), indexManager_(std::move(indexManager)) {}

	void validateInsert(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &props) override;

	void validateUpdate(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) override;

	ConstraintType getType() const override { return ConstraintType::CT_NODE_KEY; }
	std::string getName() const override { return name_; }
	std::string getLabel() const override { return label_; }
	std::vector<std::string> getProperties() const override { return properties_; }

private:
	void checkNotNull(const std::unordered_map<std::string, PropertyValue> &props) const;
	void checkCompositeUniqueness(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &props) const;
	std::string buildCompositeKey(const std::unordered_map<std::string, PropertyValue> &props) const;

	std::string name_;
	std::string label_;
	std::vector<std::string> properties_;
	std::shared_ptr<query::indexes::IndexManager> indexManager_;
};

} // namespace graph::storage::constraints
