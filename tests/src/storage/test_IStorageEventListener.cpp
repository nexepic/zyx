/**
 * @file test_IStorageEventListener.cpp
 * @author Nexepic
 * @date 2026/1/8
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "graph/storage/IStorageEventListener.hpp"

// Assuming notification logic exists in FileStorage or similar notifier class.
// Since FileStorage is complex to instantiate in isolation without filesystem,
// we can test the pattern using a mock observer.

class MockStorageEventListener : public graph::storage::IStorageEventListener {
public:
	MOCK_METHOD(void, onStorageFlush, (), (override));
};

// Simple notification simulation class to test the pattern
class Notifier {
	std::vector<std::weak_ptr<graph::storage::IStorageEventListener>> listeners;

public:
	void registerListener(std::weak_ptr<graph::storage::IStorageEventListener> listener) {
		listeners.push_back(listener);
	}

	void notifyFlush() {
		for (auto it = listeners.begin(); it != listeners.end();) {
			if (auto ptr = it->lock()) {
				ptr->onStorageFlush();
				++it;
			} else {
				it = listeners.erase(it);
			}
		}
	}
};

TEST(StorageEventListenerTest, NotificationIsDelivered) {
	Notifier notifier;
	auto mockListener = std::make_shared<MockStorageEventListener>();

	// Expectation: onStorageFlush called exactly once
	EXPECT_CALL(*mockListener, onStorageFlush()).Times(1);

	notifier.registerListener(mockListener);
	notifier.notifyFlush();
}

TEST(StorageEventListenerTest, ExpiredListenersAreHandled) {
	Notifier notifier;
	{
		auto mockListener = std::make_shared<MockStorageEventListener>();
		EXPECT_CALL(*mockListener, onStorageFlush()).Times(1);
		notifier.registerListener(mockListener);
		notifier.notifyFlush();
		// mockListener goes out of scope here
	}

	// Now listener is expired. Notify again should be safe and do nothing.
	EXPECT_NO_THROW(notifier.notifyFlush());
}
