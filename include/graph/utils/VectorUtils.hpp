/**
 * @file VectorUtils.hpp
 * @author Nexepic
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
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
#include <cmath>
#include <vector>

namespace graph::utils {

	class VectorUtils {
	public:
		static double cosineSimilarity(const std::vector<double> &v1, const std::vector<double> &v2) {
			if (v1.size() != v2.size()) {
				throw std::invalid_argument("Vector dimensions mismatch");
			}

			double dot = 0.0, norm1 = 0.0, norm2 = 0.0;
			for (size_t i = 0; i < v1.size(); ++i) {
				dot += v1[i] * v2[i];
				norm1 += v1[i] * v1[i];
				norm2 += v2[i] * v2[i];
			}

			return dot / (std::sqrt(norm1) * std::sqrt(norm2));
		}
	};

} // namespace graph::utils
