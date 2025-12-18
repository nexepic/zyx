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
		CREATE_NEW,     // Create new file. Fail if exists.
		OPEN_EXISTING,  // Open existing file. Fail if missing.
		CREATE_OR_OPEN  // Open if exists, create if missing (Legacy behavior)
	};

}