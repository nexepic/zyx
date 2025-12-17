/**
 * @file main.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/cli/CommandLineInterface.hpp"

int main(const int argc, char **argv) {
	graph::cli::CommandLineInterface cli;
	return cli.run(argc, argv);
}
