/**
 * @file CommandLineInterface.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

namespace graph::cli {

	class CommandLineInterface {
	public:
		CommandLineInterface();
		~CommandLineInterface() = default;

		/**
		 * @brief Parses arguments and executes the corresponding command.
		 * @param argc Argument count
		 * @param argv Argument values
		 * @return Exit code (0 for success, non-zero for failure)
		 */
		static int run(int argc, char** argv);
	};

} // namespace graph::cli