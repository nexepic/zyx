/**
 * @file NotNullConstraint.hpp
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

#include "IConstraint.hpp"

namespace graph::storage::constraints {

class NotNullConstraint : public IConstraint {
public:
	NotNullConstraint(std::string name, std::string label, std::string property)
		: name_(std::move(name)), label_(std::move(label)), property_(std::move(property)) {}

	void validateInsert(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &props) override {
		auto it = props.find(property_);
		if (it == props.end() || it->second.getType() == PropertyType::NULL_TYPE) {
			throw std::runtime_error(
				"Constraint violation: '" + name_ + "' - property '" + property_ +
				"' must not be null");
		}
	}

	void validateUpdate(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) override {
		// Check the new state of the property
		auto it = newProps.find(property_);
		if (it == newProps.end() || it->second.getType() == PropertyType::NULL_TYPE) {
			throw std::runtime_error(
				"Constraint violation: '" + name_ + "' - property '" + property_ +
				"' must not be null");
		}
	}

	ConstraintType getType() const override { return ConstraintType::CT_NOT_NULL; }
	std::string getName() const override { return name_; }
	std::string getLabel() const override { return label_; }
	std::vector<std::string> getProperties() const override { return {property_}; }

private:
	std::string name_;
	std::string label_;
	std::string property_;
};

} // namespace graph::storage::constraints
