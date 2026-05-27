/**
 * @file NodeCandidateSource.hpp
 * @date 2026/05/26
 *
 * Licensed under the Apache License, Version 2.0.
 **/

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution {

	class NodeCandidateSource {
	public:
		NodeCandidateSource(std::shared_ptr<storage::DataManager> dm,
							std::shared_ptr<indexes::IndexManager> im);

		[[nodiscard]] std::vector<int64_t> collect(const NodeScanConfig &config) const;

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
	};

} // namespace graph::query::execution
