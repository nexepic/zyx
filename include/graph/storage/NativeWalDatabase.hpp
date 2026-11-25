#pragma once
#include <string>
namespace graph::storage {
	// Creates a new private directory with a native experimental main file.
	// Open the returned main path through the ordinary Database interface.
	std::string createNativeWalDatabase(const std::string &newDirectory);
	// Declares one non-unique integer acceleration path for matching nodes.
	std::string createIndexedNativeWalDatabase(const std::string &newDirectory, const std::string &label,
											   const std::string &property);
} // namespace graph::storage
