/**
 * @file UniqueConstraint.hpp
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

class UniqueConstraint : public IConstraint {
public:
	UniqueConstraint(std::string name, std::string label, std::string property,
	                 std::shared_ptr<query::indexes::IndexManager> indexManager)
		: name_(std::move(name)), label_(std::move(label)),
		  property_(std::move(property)), indexManager_(std::move(indexManager)) {}

	void validateInsert(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &props) override;

	void validateUpdate(int64_t entityId,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) override;

	ConstraintType getType() const override { return ConstraintType::CT_UNIQUE; }
	std::string getName() const override { return name_; }
	std::string getLabel() const override { return label_; }
	std::vector<std::string> getProperties() const override { return {property_}; }

private:
	void checkUniqueness(int64_t entityId, const PropertyValue &value) const;

	std::string name_;
	std::string label_;
	std::string property_;
	std::shared_ptr<query::indexes::IndexManager> indexManager_;
};

} // namespace graph::storage::constraints
