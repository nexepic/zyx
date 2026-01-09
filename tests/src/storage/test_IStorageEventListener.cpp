/**
 * @file test_IStorageEventListener.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2026/1/8
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 **/

#include <gtest/gtest.h>
#include <gmock/gmock.h>
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
        for (auto it = listeners.begin(); it != listeners.end(); ) {
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