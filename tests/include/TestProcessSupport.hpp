/**
 * @file TestProcessSupport.hpp
 * @brief Shared test-binary identity for direct child-process regressions.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <filesystem>

namespace graph::test {

	void initializeTestExecutablePath(const char *argv0);
	[[nodiscard]] const std::filesystem::path &testExecutablePath();

} // namespace graph::test
