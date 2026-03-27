/**
 * @file ConstraintManager.cpp
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

#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/storage/constraints/NotNullConstraint.hpp"
#include "graph/storage/constraints/TypeConstraint.hpp"
#include "graph/storage/constraints/UniqueConstraint.hpp"
#include "graph/storage/constraints/NodeKeyConstraint.hpp"
#include "graph/storage/FileStorage.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace graph::storage::constraints {

ConstraintManager::ConstraintManager(std::shared_ptr<FileStorage> storage,
									 std::shared_ptr<query::indexes::IndexManager> indexManager)
	: storage_(std::move(storage)), indexManager_(std::move(indexManager)) {
	dataManager_ = storage_->getDataManager();
}

ConstraintManager::~ConstraintManager() = default;

void ConstraintManager::initialize() {
	loadFromState();
}

void ConstraintManager::persistState() const {
	saveToState();
}

void ConstraintManager::createConstraint(const std::string &name, const std::string &entityType,
										  const std::string &constraintType, const std::string &label,
										  const std::vector<std::string> &properties,
										  const std::string &options) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	// Check for duplicate name
	if (constraintMeta_.contains(name)) {
		throw std::runtime_error("Constraint '" + name + "' already exists");
	}

	// For UNIQUE constraints, ensure a property index exists
	if (constraintType == "unique" && !properties.empty()) {
		if (!indexManager_->hasPropertyIndex(entityType, properties[0])) {
			std::string indexName = name + "_idx";
			indexManager_->createIndex(indexName, entityType, label, properties[0]);
		}
	}

	// For NODE KEY constraints, ensure property indexes exist for each property
	if (constraintType == "node_key" && !properties.empty()) {
		for (const auto &prop : properties) {
			if (!indexManager_->hasPropertyIndex(entityType, prop)) {
				std::string indexName = name + "_" + prop + "_idx";
				indexManager_->createIndex(indexName, entityType, label, prop);
			}
		}
	}

	// Create the constraint instance
	auto constraint = createConstraintInstance(constraintType, name, label, properties, options);

	// Validate existing data
	validateExistingData(entityType, label, *constraint);

	// Register constraint
	int64_t labelId = dataManager_->getOrCreateLabelId(label);

	auto &constraints = (entityType == "node") ? nodeConstraints_ : edgeConstraints_;
	constraints[labelId].push_back(std::move(constraint));

	// Store metadata
	std::string propsStr;
	for (size_t i = 0; i < properties.size(); ++i) {
		if (i > 0) propsStr += ",";
		propsStr += properties[i];
	}
	constraintMeta_[name] = {name, entityType, constraintType, label, propsStr, options};

	// Persist
	saveToState();
}

bool ConstraintManager::dropConstraint(const std::string &name) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);

	auto metaIt = constraintMeta_.find(name);
	if (metaIt == constraintMeta_.end()) {
		return false;
	}

	const auto &meta = metaIt->second;
	int64_t labelId = dataManager_->getOrCreateLabelId(meta.label);

	auto &constraints = (meta.entityType == "node") ? nodeConstraints_ : edgeConstraints_;
	auto it = constraints.find(labelId);
	if (it != constraints.end()) {
		auto &vec = it->second;
		vec.erase(std::remove_if(vec.begin(), vec.end(),
			[&name](const std::unique_ptr<IConstraint> &c) {
				return c->getName() == name;
			}), vec.end());
		if (vec.empty()) {
			constraints.erase(it);
		}
	}

	constraintMeta_.erase(metaIt);
	saveToState();
	return true;
}

std::vector<ConstraintMetadata> ConstraintManager::listConstraints() const {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::vector<ConstraintMetadata> result;
	result.reserve(constraintMeta_.size());
	for (const auto &[name, meta] : constraintMeta_) {
		result.push_back(meta);
	}
	return result;
}

// --- IEntityValidator implementation ---

void ConstraintManager::validateNodeInsert(const Node &node,
	const std::unordered_map<std::string, PropertyValue> &props) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	auto it = nodeConstraints_.find(node.getLabelId());
	if (it == nodeConstraints_.end()) return; // Fast path
	for (const auto &c : it->second) {
		c->validateInsert(node.getId(), props);
	}
}

void ConstraintManager::validateNodeUpdate(const Node &node,
	const std::unordered_map<std::string, PropertyValue> &oldProps,
	const std::unordered_map<std::string, PropertyValue> &newProps) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	auto it = nodeConstraints_.find(node.getLabelId());
	if (it == nodeConstraints_.end()) return;
	for (const auto &c : it->second) {
		c->validateUpdate(node.getId(), oldProps, newProps);
	}
}

void ConstraintManager::validateNodeDelete(const Node &node) {
	// No constraints block deletion currently
}

void ConstraintManager::validateEdgeInsert(const Edge &edge,
	const std::unordered_map<std::string, PropertyValue> &props) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	auto it = edgeConstraints_.find(edge.getLabelId());
	if (it == edgeConstraints_.end()) return;
	for (const auto &c : it->second) {
		c->validateInsert(edge.getId(), props);
	}
}

void ConstraintManager::validateEdgeUpdate(const Edge &edge,
	const std::unordered_map<std::string, PropertyValue> &oldProps,
	const std::unordered_map<std::string, PropertyValue> &newProps) {
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	auto it = edgeConstraints_.find(edge.getLabelId());
	if (it == edgeConstraints_.end()) return;
	for (const auto &c : it->second) {
		c->validateUpdate(edge.getId(), oldProps, newProps);
	}
}

void ConstraintManager::validateEdgeDelete(const Edge &edge) {
	// No constraints block deletion currently
}

// --- Private helpers ---

std::unique_ptr<IConstraint> ConstraintManager::createConstraintInstance(
	const std::string &constraintType, const std::string &name,
	const std::string &label, const std::vector<std::string> &properties,
	const std::string &options) {

	if (constraintType == "unique") {
		if (properties.size() != 1) {
			throw std::runtime_error("UNIQUE constraint requires exactly one property");
		}
		return std::make_unique<UniqueConstraint>(name, label, properties[0], indexManager_);
	}

	if (constraintType == "not_null") {
		if (properties.size() != 1) {
			throw std::runtime_error("NOT NULL constraint requires exactly one property");
		}
		return std::make_unique<NotNullConstraint>(name, label, properties[0]);
	}

	if (constraintType == "property_type") {
		if (properties.size() != 1) {
			throw std::runtime_error("Property type constraint requires exactly one property");
		}
		auto propType = TypeConstraint::parsePropertyType(options);
		return std::make_unique<TypeConstraint>(name, label, properties[0], propType);
	}

	if (constraintType == "node_key") {
		if (properties.empty()) {
			throw std::runtime_error("NODE KEY constraint requires at least one property");
		}
		return std::make_unique<NodeKeyConstraint>(name, label, properties, indexManager_);
	}

	throw std::runtime_error("Unknown constraint type: " + constraintType);
}

void ConstraintManager::validateExistingData(const std::string &entityType,
											  const std::string &label,
											  IConstraint &constraint) const {
	auto idAllocator = dataManager_->getIdAllocator();
	int64_t maxId = 0;
	if (entityType == "node") {
		maxId = idAllocator->getCurrentMaxNodeId();
	} else if (entityType == "edge") {
		maxId = idAllocator->getCurrentMaxEdgeId();
	} else {
		return;
	}

	if (maxId == 0) return; // No entities exist

	int64_t labelId = dataManager_->getOrCreateLabelId(label);

	// Scan all entities up to maxId in batches
	auto entities = (entityType == "node")
		? dataManager_->getNodesInRange(1, maxId, static_cast<size_t>(maxId))
		: std::vector<Node>{}; // Edge scan handled separately

	if (entityType == "node") {
		for (const auto &node : entities) {
			if (!node.isActive() || !node.hasLabelId(labelId)) continue;
			auto props = dataManager_->getNodeProperties(node.getId());
			try {
				constraint.validateInsert(node.getId(), props);
			} catch (const std::runtime_error &e) {
				throw std::runtime_error(
					"Cannot create constraint: existing data violates constraint - " +
					std::string(e.what()));
			}
		}
	} else {
		auto edges = dataManager_->getEdgesInRange(1, maxId, static_cast<size_t>(maxId));
		for (const auto &edge : edges) {
			if (!edge.isActive() || edge.getLabelId() != labelId) continue;
			auto props = dataManager_->getEdgeProperties(edge.getId());
			try {
				constraint.validateInsert(edge.getId(), props);
			} catch (const std::runtime_error &e) {
				throw std::runtime_error(
					"Cannot create constraint: existing data violates constraint - " +
					std::string(e.what()));
			}
		}
	}
}

void ConstraintManager::loadFromState() {
	auto props = dataManager_->getStateProperties("sys.constraints");
	for (const auto &[name, value] : props) {
		if (value.getType() != PropertyType::STRING) continue;
		auto meta = ConstraintMetadata::fromString(name, value.toString());

		// Parse properties back to vector
		std::vector<std::string> propList;
		std::stringstream ss(meta.properties);
		std::string prop;
		while (std::getline(ss, prop, ',')) {
			if (!prop.empty()) propList.push_back(prop);
		}

		try {
			auto constraint = createConstraintInstance(
				meta.constraintType, meta.name, meta.label, propList, meta.options);

			int64_t labelId = dataManager_->getOrCreateLabelId(meta.label);
			auto &constraints = (meta.entityType == "node") ? nodeConstraints_ : edgeConstraints_;
			constraints[labelId].push_back(std::move(constraint));
			constraintMeta_[name] = meta;
		} catch (const std::exception &e) {
			std::cerr << "[ConstraintManager] Failed to load constraint '" << name
					  << "': " << e.what() << std::endl;
		}
	}
}

void ConstraintManager::saveToState() const {
	// Build properties map from metadata
	std::unordered_map<std::string, PropertyValue> props;
	for (const auto &[name, meta] : constraintMeta_) {
		props[name] = PropertyValue(meta.toString());
	}

	// First remove existing state, then add new
	dataManager_->removeState("sys.constraints");
	if (!props.empty()) {
		dataManager_->addStateProperties("sys.constraints", props);
	}
}

} // namespace graph::storage::constraints
