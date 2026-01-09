/**
 * @file CommandLineInterface.hpp
 * @author Nexepic
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
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
		static int run(int argc, char **argv);
	};

} // namespace graph::cli
