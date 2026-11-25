#pragma once
#include <string>
namespace graph::storage {
	// Creates a new private directory with a native experimental main file.
	// Open the returned main path through the ordinary Database interface.
	std::string createNativeWalDatabase(const std::string &newDirectory);
} // namespace graph::storage
