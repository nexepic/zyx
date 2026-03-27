/**
 * @file TypeConstraint.hpp
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

#include <algorithm>
#include <stdexcept>
#include "IConstraint.hpp"

namespace graph::storage::constraints {

class TypeConstraint : public IConstraint {
public:
	TypeConstraint(std::string name, std::string label, std::string property,
	               PropertyType expectedType)
		: name_(std::move(name)), label_(std::move(label)),
		  property_(std::move(property)), expectedType_(expectedType) {}

	void validateInsert(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &props) override {
		checkType(props);
	}

	void validateUpdate(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) override {
		checkType(newProps);
	}

	ConstraintType getType() const override { return ConstraintType::CT_PROPERTY_TYPE; }
	std::string getName() const override { return name_; }
	std::string getLabel() const override { return label_; }
	std::vector<std::string> getProperties() const override { return {property_}; }
	std::string getOptions() const override { return propertyTypeToString(expectedType_); }

	static PropertyType parsePropertyType(const std::string &typeStr) {
		std::string upper = typeStr;
		std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
		if (upper == "BOOLEAN") return PropertyType::BOOLEAN;
		if (upper == "INTEGER") return PropertyType::INTEGER;
		if (upper == "FLOAT" || upper == "DOUBLE") return PropertyType::DOUBLE;
		if (upper == "STRING") return PropertyType::STRING;
		if (upper == "LIST") return PropertyType::LIST;
		if (upper == "MAP") return PropertyType::MAP;
		throw std::runtime_error("Unknown property type: " + typeStr);
	}

	static std::string propertyTypeToString(PropertyType type) {
		switch (type) {
			case PropertyType::BOOLEAN: return "BOOLEAN";
			case PropertyType::INTEGER: return "INTEGER";
			case PropertyType::DOUBLE: return "FLOAT";
			case PropertyType::STRING: return "STRING";
			case PropertyType::LIST: return "LIST";
			case PropertyType::MAP: return "MAP";
			default: return "UNKNOWN";
		}
	}

private:
	void checkType(const std::unordered_map<std::string, PropertyValue> &props) const {
		auto it = props.find(property_);
		if (it == props.end()) return; // Property not present -- OK (use NOT NULL to require it)
		if (it->second.getType() == PropertyType::NULL_TYPE) return; // NULL is allowed
		if (it->second.getType() != expectedType_) {
			throw std::runtime_error(
				"Constraint violation: '" + name_ + "' - property '" + property_ +
				"' must be " + propertyTypeToString(expectedType_) +
				", got " + it->second.typeName());
		}
	}

	std::string name_;
	std::string label_;
	std::string property_;
	PropertyType expectedType_;
};

} // namespace graph::storage::constraints
