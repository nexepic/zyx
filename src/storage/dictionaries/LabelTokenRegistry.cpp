/**
 * @file LabelTokenRegistry.cpp
 * @author Nexepic
 * @date 2026/1/13
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

#include "graph/storage/dictionaries/LabelTokenRegistry.hpp"
#include "graph/storage/data/BlobManager.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace graph::storage {

	LabelTokenRegistry::LabelTokenRegistry(std::shared_ptr<DataManager> dataManager,
										   std::shared_ptr<state::SystemStateManager> stateManager,
										   size_t cacheSize) :
		dataManager_(std::move(dataManager)), stateManager_(std::move(stateManager)), stringToIdCache_(cacheSize),
		idToStringCache_(cacheSize) {

		indexTree_ = std::make_shared<query::indexes::IndexTreeManager>(
				dataManager_, query::indexes::IndexTypes::SYSTEM_LABEL_DICTIONARY, PropertyType::STRING);

		initialize();
	}

	void LabelTokenRegistry::initialize() {
		rootIndexId_ = stateManager_->get<int64_t>(STORAGE_KEY, "root_id", 0);

		if (rootIndexId_ == 0) {
			rootIndexId_ = indexTree_->initialize();
			saveState();
		}
	}

	void LabelTokenRegistry::saveState() const { stateManager_->set<int64_t>(STORAGE_KEY, "root_id", rootIndexId_); }

	int64_t LabelTokenRegistry::getOrCreateLabelId(const std::string &label) {
		if (label.empty())
			return NULL_LABEL_ID;

		// 1. Check Cache
		{
			// Exclusive lock required for LRU get() as it modifies list order
			std::lock_guard<std::mutex> lock(cacheMutex_);
			if (const int64_t cachedId = stringToIdCache_.get(label); cachedId != 0) {
				return cachedId;
			}
		}

		// 2. Check Index (Disk)
		std::vector<int64_t> ids = indexTree_->find(rootIndexId_, PropertyValue(label));
		if (!ids.empty()) {
			addToCache(label, ids[0]);
			return ids[0];
		}

		// 3. Create New
		// Store String in Blob
		const auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, label);
		const int64_t newId = blobs[0].getId();

		// Map String -> ID in Index

		if (const int64_t newRoot = indexTree_->insert(rootIndexId_, PropertyValue(label), newId);
			newRoot != rootIndexId_) {
			rootIndexId_ = newRoot;
			saveState();
		}

		addToCache(label, newId);
		return newId;
	}

	std::string LabelTokenRegistry::getLabelString(int64_t labelId) {
		if (labelId == NULL_LABEL_ID)
			return "";

		// 1. Check Cache
		{
			std::lock_guard<std::mutex> lock(cacheMutex_);
			std::string cachedLabel = idToStringCache_.get(labelId);
			if (!cachedLabel.empty()) {
				return cachedLabel;
			}
		}

		// 2. Load from Blob (Disk)
		try {
			std::string label = dataManager_->getBlobManager()->readBlobChain(labelId);

			if (!label.empty()) {
				addToCache(label, labelId);
			}
			return label;
		} catch (...) {
			return ""; // Fail gracefully
		}
	}

	void LabelTokenRegistry::addToCache(const std::string &label, int64_t id) {
		std::lock_guard<std::mutex> lock(cacheMutex_);
		// LRUCache handles eviction automatically
		stringToIdCache_.put(label, id);
		idToStringCache_.put(id, label);
	}

} // namespace graph::storage
