/**
 * @file GraphProjectionManager.hpp
 * @author Nexepic
 * @date 2026/4/9
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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "CsrProjection.hpp"
#include "GraphProjection.hpp"

namespace graph::query::algorithm {

	struct GraphProjectionDescriptor {
		std::string name;
		ProjectionSpec spec;
		size_t nodeCount = 0;
		size_t edgeCount = 0;
		bool isWeighted = false;
		int64_t createdAtEpochMillis = 0;
		int64_t buildMillis = 0;
		size_t memoryBytes = 0;
		bool hasCsr = false;
		size_t csrMemoryBytes = 0;
		uint64_t sourceRevision = 0;
		uint64_t currentRevision = 0;
		bool stale = false;
	};

	/**
	 * @brief Manages named graph projections.
	 *
	 * Stores projections and their catalog descriptors in memory with
	 * thread-safe access. Projections are never persisted to the database file;
	 * they live until explicitly dropped or the manager is destroyed.
	 */
	class GraphProjectionManager {
	public:
		void createProjection(const std::string &name, std::shared_ptr<GraphProjection> projection) {
			ProjectionSpec spec;
			spec.name = name;
			createProjection(std::move(spec), std::move(projection), 0, std::chrono::nanoseconds{0});
		}

		void createProjection(ProjectionSpec spec,
							  std::shared_ptr<GraphProjection> projection,
							  uint64_t sourceRevision,
							  std::chrono::nanoseconds buildTime) {
			if (!projection) {
				throw std::runtime_error("Graph projection must not be null");
			}
			if (spec.name.empty()) {
				throw std::runtime_error("Graph projection name must not be empty");
			}

			std::lock_guard lock(mutex_);
			if (projections_.contains(spec.name)) {
				throw std::runtime_error("Graph projection '" + spec.name + "' already exists");
			}

			const auto name = spec.name;
			GraphProjectionDescriptor descriptor;
			descriptor.name = name;
			descriptor.spec = std::move(spec);
			descriptor.nodeCount = projection->nodeCount();
			descriptor.edgeCount = projection->edgeCount();
			descriptor.isWeighted = projection->isWeighted();
			descriptor.createdAtEpochMillis = currentEpochMillis();
			descriptor.buildMillis = static_cast<int64_t>(
				std::chrono::duration_cast<std::chrono::milliseconds>(buildTime).count());
			descriptor.memoryBytes = projection->estimatedMemoryBytes();
			descriptor.sourceRevision = sourceRevision;
			descriptor.currentRevision = sourceRevision;

			projections_[name] = std::move(projection);
			descriptors_[name] = std::move(descriptor);
		}

		[[nodiscard]] std::shared_ptr<GraphProjection> getProjection(const std::string &name) const {
			std::lock_guard lock(mutex_);
			auto it = projections_.find(name);
			if (it == projections_.end()) {
				throw std::runtime_error("Graph projection '" + name + "' not found");
			}
			return it->second;
		}

		bool dropProjection(const std::string &name) {
			std::lock_guard lock(mutex_);
			csrProjections_.erase(name);
			descriptors_.erase(name);
			return projections_.erase(name) > 0;
		}

		size_t dropAll() {
			std::lock_guard lock(mutex_);
			const size_t dropped = projections_.size();
			projections_.clear();
			csrProjections_.clear();
			descriptors_.clear();
			return dropped;
		}

		[[nodiscard]] bool exists(const std::string &name) const {
			std::lock_guard lock(mutex_);
			return projections_.contains(name);
		}

		[[nodiscard]] std::optional<GraphProjectionDescriptor>
		describe(const std::string &name, uint64_t currentRevision) const {
			std::lock_guard lock(mutex_);
			auto it = descriptors_.find(name);
			if (it == descriptors_.end()) {
				return std::nullopt;
			}
			return withCurrentRevision(it->second, currentRevision);
		}

		[[nodiscard]] std::vector<GraphProjectionDescriptor> list(uint64_t currentRevision) const {
			std::lock_guard lock(mutex_);
			std::vector<GraphProjectionDescriptor> result;
			result.reserve(descriptors_.size());
			for (const auto &[_, descriptor] : descriptors_) {
				result.push_back(withCurrentRevision(descriptor, currentRevision));
			}
			std::sort(result.begin(), result.end(), [](const auto &lhs, const auto &rhs) {
				return lhs.name < rhs.name;
			});
			return result;
		}

		/// Cache or retrieve a compact CSR projection for algorithms that benefit
		/// from a cache-friendly layout (e.g. Leiden). Built lazily from the
		/// stored GraphProjection on first access; reused across subsequent runs.
		std::shared_ptr<CsrProjection> getOrBuildCsr(const std::string &name,
													 concurrent::ThreadPool *pool = nullptr) {
			std::lock_guard lock(mutex_);
			auto it = csrProjections_.find(name);
			if (it != csrProjections_.end()) return it->second;
			auto projIt = projections_.find(name);
			if (projIt == projections_.end()) {
				throw std::runtime_error("Graph projection '" + name + "' not found");
			}
			auto csr = CsrProjection::build(*projIt->second, pool);
			csrProjections_[name] = csr;
			auto &descriptor = descriptors_.at(name);
			descriptor.hasCsr = true;
			descriptor.csrMemoryBytes = csr->estimatedMemoryBytes();
			return csr;
		}

	private:
		static int64_t currentEpochMillis() {
			return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
		}

		static GraphProjectionDescriptor withCurrentRevision(GraphProjectionDescriptor descriptor,
															 uint64_t currentRevision) {
			descriptor.currentRevision = currentRevision;
			descriptor.stale = descriptor.sourceRevision != currentRevision;
			return descriptor;
		}

		mutable std::mutex mutex_;
		std::unordered_map<std::string, std::shared_ptr<GraphProjection>> projections_;
		std::unordered_map<std::string, std::shared_ptr<CsrProjection>> csrProjections_;
		std::unordered_map<std::string, GraphProjectionDescriptor> descriptors_;
	};

} // namespace graph::query::algorithm
