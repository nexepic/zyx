/**
 * @file PairHash.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <functional>

namespace graph::utils {

	struct PairHash {
		template <typename T1, typename T2>
		std::size_t operator()(const std::pair<T1, T2>& p) const {
			auto hash1 = std::hash<T1>{}(p.first);
			auto hash2 = std::hash<T2>{}(p.second);
			return hash1 ^ (hash2 << 1);
		}
	};

} // namespace graph::utils