/**
 * @file ConstraintManager.hpp
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
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "ConstraintMeta.hpp"
#include "IConstraint.hpp"
#include "IEntityValidator.hpp"

namespace graph::storage {
	class DataManager;
	class FileStorage;
	namespace state {
		class SystemStateManager;
	}
}

namespace graph::query::indexes {
	class IndexManager;
}

namespace graph::storage::constraints {

class ConstraintManager : public IEntityValidator {
public:
	explicit ConstraintManager(std::shared_ptr<FileStorage> storage,
							   std::shared_ptr<query::indexes::IndexManager> indexManager);
	~ConstraintManager() override;

	void initialize();
	void persistState() const;

	// DDL operations
	void createConstraint(const std::string &name, const std::string &entityType,
						  const std::string &constraintType, const std::string &label,
						  const std::vector<std::string> &properties,
						  const std::string &options = "");
	bool dropConstraint(const std::string &name);
	std::vector<ConstraintMetadata> listConstraints() const;

	// IEntityValidator implementation
	void validateNodeInsert(const Node &node,
		const std::unordered_map<std::string, PropertyValue> &props) override;
	void validateNodeUpdate(const Node &node,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) override;
	void validateNodeDelete(const Node &node) override;

	void validateEdgeInsert(const Edge &edge,
		const std::unordered_map<std::string, PropertyValue> &props) override;
	void validateEdgeUpdate(const Edge &edge,
		const std::unordered_map<std::string, PropertyValue> &oldProps,
		const std::unordered_map<std::string, PropertyValue> &newProps) override;
	void validateEdgeDelete(const Edge &edge) override;

private:
	std::shared_ptr<FileStorage> storage_;
	std::shared_ptr<DataManager> dataManager_;
	std::shared_ptr<query::indexes::IndexManager> indexManager_;

	// Label ID -> constraints for that label. O(1) lookup, fast path when empty.
	std::unordered_map<int64_t, std::vector<std::unique_ptr<IConstraint>>> nodeConstraints_;
	std::unordered_map<int64_t, std::vector<std::unique_ptr<IConstraint>>> edgeConstraints_;

	// Name -> metadata for persistence
	std::unordered_map<std::string, ConstraintMetadata> constraintMeta_;

	mutable std::recursive_mutex mutex_;

	// Helpers
	std::unique_ptr<IConstraint> createConstraintInstance(
		const std::string &constraintType, const std::string &name,
		const std::string &label, const std::vector<std::string> &properties,
		const std::string &options);

	void validateExistingData(const std::string &entityType, const std::string &label,
							  IConstraint &constraint) const;

	void loadFromState();
	void saveToState() const;
};

} // namespace graph::storage::constraints
