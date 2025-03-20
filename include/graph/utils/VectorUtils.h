/**
 * @file VectorUtils.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <vector>
#include <cmath>

namespace graph::utils {

	class VectorUtils {
	public:
		static double cosineSimilarity(const std::vector<double>& v1,
									  const std::vector<double>& v2) {
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