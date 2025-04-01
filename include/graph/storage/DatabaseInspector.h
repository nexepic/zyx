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

namespace graph::storage {

    void displayDatabaseStructure(const std::shared_ptr<std::ifstream>& file);

} // namespace graph::storage