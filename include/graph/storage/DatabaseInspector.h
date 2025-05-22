/**
 * @file DatabaseInspector.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/31
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <fstream>
#include "DataManager.h"
#include "StorageHeaders.h"

namespace graph::storage {
	class DatabaseInspector {
	public:
		explicit DatabaseInspector(FileHeader fileHeader, std::shared_ptr<std::fstream> &file,
								   DataManager &dataManager);
		~DatabaseInspector();

		void inspectDatabase();

		void displayDatabaseStructure() const;

		void displayNodeSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset) const;

		void displayEdgeSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset) const;

		static void displayPropertySegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset);

		static void displayBlobSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset);

	private:
		FileHeader fileHeader;

		std::shared_ptr<std::fstream> &file;

		DataManager &dataManager_;
	};
} // namespace graph::storage
