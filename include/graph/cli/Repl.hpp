/**
 * @file Repl.hpp
 * @author Nexepic
 * @date 2025/2/27
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
#include "DebugCommandHandler.hpp"
#include "graph/core/Database.hpp"

namespace graph {
	class REPL {
	public:
		explicit REPL(Database &db);
		void run() const;

		void runScript(const std::string &scriptPath) const;

	private:
		Database &db;
		void handleCommand(const std::string &command) const;
		void runBasic() const; // Fallback for non-TTY environments
		void printBanner(bool basicMode) const;
		void printHelp() const;
		void echoMultilineBuffer(const std::string &buffer) const;
		cli::DebugCommandHandler debugHandler_{db};

		// Allow REPLTest to access private members for testing
		friend class REPLTest;
	};

	class REPLTest : public REPL {
	public:
		using REPL::REPL;

		// Expose private methods for testing
		void runBasicPublic() const { REPL::runBasic(); }
	};

} // namespace graph
