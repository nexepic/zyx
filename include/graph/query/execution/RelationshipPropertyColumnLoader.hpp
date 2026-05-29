#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/PropertyTypes.hpp"
#include "graph/query/execution/RelationshipMetadataColumnLoader.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	class RelationshipPropertyColumnLoader {
	public:
		explicit RelationshipPropertyColumnLoader(std::shared_ptr<storage::DataManager> dm,
		                                          concurrent::ThreadPool *threadPool = nullptr);

		[[nodiscard]] std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
		loadColumns(const std::vector<Edge> &edges,
		            const std::vector<uint8_t> &selected,
		            const std::vector<std::string> &requiredProperties) const;

		[[nodiscard]] std::unordered_map<std::string, std::vector<std::optional<PropertyValue>>>
		loadColumns(const RelationshipMetadataBatch &metadataBatch,
		            const std::vector<uint8_t> &selected,
		            const std::vector<std::string> &requiredProperties) const;

	private:
		std::shared_ptr<storage::DataManager> dm_;
		concurrent::ThreadPool *threadPool_ = nullptr;
	};

} // namespace graph::query::execution
