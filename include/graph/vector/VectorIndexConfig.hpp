/**
 * @file VectorIndexConfig.hpp
 * @author Nexepic
 * @date 2026/1/22
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include <cstdint>
#include <string>
#include <unordered_map>
#include "graph/core/PropertyTypes.hpp"

namespace graph::vector {
	struct VectorIndexConfig {
		uint32_t dimension = 0;
		uint32_t metricType = 0;
		uint32_t pqSubspaces = 0;

		// Metadata IDs
		int64_t mappingIndexId = 0;
		int64_t entryPointNodeId = 0;

		// Key to store the Codebook Blob via StateManager
		std::string codebookKey;
		bool isTrained = false;

		std::unordered_map<std::string, PropertyValue> toProperties() const {
			return {{"dim", dimension},			  {"metric", metricType},	   {"pq_m", pqSubspaces},
					{"map_root", mappingIndexId}, {"entry", entryPointNodeId}, {"cb_key", codebookKey},
					{"trained", isTrained}};
		}

		static VectorIndexConfig fromProperties(const std::unordered_map<std::string, PropertyValue> &props) {
			VectorIndexConfig cfg;
			if (props.contains("dim"))
				cfg.dimension = std::get<int64_t>(props.at("dim").getVariant());
			if (props.contains("metric"))
				cfg.metricType = std::get<int64_t>(props.at("metric").getVariant());
			if (props.contains("pq_m"))
				cfg.pqSubspaces = std::get<int64_t>(props.at("pq_m").getVariant());
			if (props.contains("map_root"))
				cfg.mappingIndexId = std::get<int64_t>(props.at("map_root").getVariant());
			if (props.contains("entry"))
				cfg.entryPointNodeId = std::get<int64_t>(props.at("entry").getVariant());
			if (props.contains("cb_key"))
				cfg.codebookKey = std::get<std::string>(props.at("cb_key").getVariant());

			if (props.contains("trained")) {
				if (const auto var = props.at("trained").getVariant(); std::holds_alternative<bool>(var))
					cfg.isTrained = std::get<bool>(var);
				else
					cfg.isTrained = std::get<int64_t>(var) != 0;
			}
			return cfg;
		}
	};
} // namespace graph::vector
