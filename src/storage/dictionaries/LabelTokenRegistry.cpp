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
                                           std::shared_ptr<state::SystemStateManager> stateManager)
        : dataManager_(std::move(dataManager)),
          stateManager_(std::move(stateManager)) {

        // Use a specific internal index type for the dictionary
        indexTree_ = std::make_shared<query::indexes::IndexTreeManager>(
                dataManager_, query::indexes::IndexTypes::SYSTEM_LABEL_DICTIONARY, PropertyType::STRING);

        // Load the root ID from storage immediately upon construction
        initialize();
    }

    void LabelTokenRegistry::initialize() {
        // Try to load the root ID from the system state
        // If the key doesn't exist, it returns 0 (default)
        rootIndexId_ = stateManager_->get<int64_t>(STORAGE_KEY, "root_id", 0);

        if (rootIndexId_ == 0) {
            // New database or first time usage: initialize the B+ Tree
            rootIndexId_ = indexTree_->initialize();
            saveState(); // Persist the initial root ID
        }
    }

    void LabelTokenRegistry::saveState() {
        // Save the current root ID to the SystemStateManager
        // This marks the State entity as dirty, so FileStorage will flush it to disk.
        stateManager_->set<int64_t>(STORAGE_KEY, "root_id", rootIndexId_);
    }

    int64_t LabelTokenRegistry::getOrCreateLabelId(const std::string &label) {
        if (label.empty())
            return NULL_LABEL_ID;

        {
            std::shared_lock lock(cacheMutex_);
            if (auto it = stringToIdCache_.find(label); it != stringToIdCache_.end()) {
                return it->second;
            }
        }

        // Check Index
        std::vector<int64_t> ids = indexTree_->find(rootIndexId_, PropertyValue(label));
        if (!ids.empty()) {
            addToCache(label, ids[0]);
            return ids[0];
        }

        // Create New
        // 1. Store String in Blob
        // passing 0,0 as entityId/Type since this is a system blob
        auto blobs = dataManager_->getBlobManager()->createBlobChain(0, 0, label);
        if (blobs.empty())
            throw std::runtime_error("Failed to store label string");

        int64_t newId = blobs[0].getId();

        // 2. Map String -> ID in Index
        int64_t newRoot = indexTree_->insert(rootIndexId_, PropertyValue(label), newId);

        // Update root ID if the tree structure caused a root split/change
        if (newRoot != rootIndexId_) {
            rootIndexId_ = newRoot;
            // CRITICAL: The root of the B+Tree changed, we MUST persist this pointer.
            saveState();
        }

        addToCache(label, newId);
        return newId;
    }

    std::string LabelTokenRegistry::getLabelString(int64_t labelId) {
        if (labelId == NULL_LABEL_ID)
            return "";

        {
            std::shared_lock lock(cacheMutex_);
            if (auto it = idToStringCache_.find(labelId); it != idToStringCache_.end()) {
                return it->second;
            }
        }

        // ID points directly to Blob
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
        std::unique_lock lock(cacheMutex_);
        if (stringToIdCache_.size() > CACHE_SIZE) {
            stringToIdCache_.clear();
            idToStringCache_.clear();
        }
        stringToIdCache_[label] = id;
        idToStringCache_[id] = label;
    }

} // namespace graph::storage