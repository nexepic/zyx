/**
 * @file StorageTypes.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

namespace graph::storage {

	enum class OpenMode {
		CREATE_NEW_FILE,     // Create new file. Fail if exists.
		OPEN_EXISTING_FILE,  // Open existing file. Fail if missing.
		CREATE_OR_OPEN_FILE  // Open if exists, create if missing (Legacy behavior)
	};

}