/**
 * @file test_UndoLog.cpp
 * @date 2026/4/14
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

#include <gtest/gtest.h>
#include "graph/storage/wal/UndoLog.hpp"

using namespace graph::storage::wal;

TEST(UndoLogTest, InitiallyEmpty) {
	UndoLog log;
	EXPECT_TRUE(log.empty());
	EXPECT_EQ(log.size(), 0u);
	EXPECT_TRUE(log.entries().empty());
}

TEST(UndoLogTest, RecordAddsEntry) {
	UndoLog log;
	UndoEntry entry{
		static_cast<uint8_t>(1), // entityType
		42,                      // entityId
		UndoChangeType::UNDO_ADDED,
		{}                       // empty beforeImage
	};
	log.record(entry);
	EXPECT_FALSE(log.empty());
	EXPECT_EQ(log.size(), 1u);
}

TEST(UndoLogTest, RecordMultipleEntries) {
	UndoLog log;

	std::vector<uint8_t> image = {0x01, 0x02, 0x03};

	log.record({1, 10, UndoChangeType::UNDO_ADDED, {}});
	log.record({1, 20, UndoChangeType::UNDO_MODIFIED, image});
	log.record({1, 30, UndoChangeType::UNDO_DELETED, image});

	EXPECT_EQ(log.size(), 3u);

	const auto &entries = log.entries();
	EXPECT_EQ(entries[0].entityId, 10);
	EXPECT_EQ(entries[0].changeType, UndoChangeType::UNDO_ADDED);
	EXPECT_TRUE(entries[0].beforeImage.empty());

	EXPECT_EQ(entries[1].entityId, 20);
	EXPECT_EQ(entries[1].changeType, UndoChangeType::UNDO_MODIFIED);
	EXPECT_EQ(entries[1].beforeImage, image);

	EXPECT_EQ(entries[2].entityId, 30);
	EXPECT_EQ(entries[2].changeType, UndoChangeType::UNDO_DELETED);
}

TEST(UndoLogTest, ClearRemovesAllEntries) {
	UndoLog log;
	log.record({1, 10, UndoChangeType::UNDO_ADDED, {}});
	log.record({1, 20, UndoChangeType::UNDO_MODIFIED, {0x01}});

	ASSERT_EQ(log.size(), 2u);

	log.clear();
	EXPECT_TRUE(log.empty());
	EXPECT_EQ(log.size(), 0u);
	EXPECT_TRUE(log.entries().empty());
}

TEST(UndoLogTest, RecordMoveSemantics) {
	UndoLog log;
	std::vector<uint8_t> largeImage(256, 0xAB);

	UndoEntry entry{
		static_cast<uint8_t>(2),
		99,
		UndoChangeType::UNDO_DELETED,
		largeImage
	};
	log.record(std::move(entry));

	EXPECT_EQ(log.size(), 1u);
	EXPECT_EQ(log.entries()[0].beforeImage.size(), 256u);
	EXPECT_EQ(log.entries()[0].beforeImage[0], 0xAB);
}

TEST(UndoLogTest, EntriesOrderPreserved) {
	UndoLog log;

	for (int64_t i = 1; i <= 5; ++i) {
		log.record({static_cast<uint8_t>(1), i, UndoChangeType::UNDO_ADDED, {}});
	}

	const auto &entries = log.entries();
	ASSERT_EQ(entries.size(), 5u);
	for (int64_t i = 0; i < 5; ++i) {
		EXPECT_EQ(entries[static_cast<size_t>(i)].entityId, i + 1);
	}
}

TEST(UndoLogTest, UndoChangeTypeValues) {
	// Verify enum values are distinct (they drive switch logic)
	EXPECT_NE(UndoChangeType::UNDO_ADDED, UndoChangeType::UNDO_MODIFIED);
	EXPECT_NE(UndoChangeType::UNDO_ADDED, UndoChangeType::UNDO_DELETED);
	EXPECT_NE(UndoChangeType::UNDO_MODIFIED, UndoChangeType::UNDO_DELETED);
}
