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
#include "StorageHeaders.h"


#include <fstream>

namespace graph::storage {
	class DatabaseInspector {
	public:
		explicit DatabaseInspector(FileHeader fileHeader, std::shared_ptr<std::fstream> &file);
		~DatabaseInspector();

		void inspectDatabase();

		void displayDatabaseStructure();

		void displayNodeSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset);

		void displayEdgeSegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset);

		void displayPropertySegmentChain(const std::shared_ptr<std::fstream> &file, uint64_t segmentOffset);

	private:
		FileHeader fileHeader;

		std::shared_ptr<std::fstream> &file;
	};
} // namespace graph::storage
