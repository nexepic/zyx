// Export into a new, private binary stream. Check close before publication.
#pragma once
#include <iosfwd>
#include "graph/storage/LogicalGraphRead.hpp"
namespace graph::storage {
	void exportSegmentedSnapshot(const LogicalGraphReadSnapshot &source, std::ostream &out);
}
