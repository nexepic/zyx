/**
 * @file Repl.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/27
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include "graph/core/Database.hpp"

namespace graph {
	class REPL {
	public:
		explicit REPL(Database &db);
		void run() const;

		void runScript(const std::string& scriptPath) const;

	private:
		Database &db;
		void handleCommand(const std::string &command) const;
	};

	class REPLTest : public REPL {
	public:
		using REPL::REPL;
	};

} // namespace graph
