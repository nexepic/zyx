#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	class NodePropertyColumnLoader {
	public:
		explicit NodePropertyColumnLoader(std::shared_ptr<storage::DataManager> dm,
		                                  concurrent::ThreadPool *threadPool = nullptr);

		[[nodiscard]] std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
		loadColumns(const std::vector<Node> &nodes,
		            const std::vector<uint8_t> &selected,
		            const std::vector<std::string> &requiredProperties) const;

	private:
		std::shared_ptr<storage::DataManager> dm_;
		concurrent::ThreadPool *threadPool_ = nullptr;
	};

} // namespace graph::query::execution
