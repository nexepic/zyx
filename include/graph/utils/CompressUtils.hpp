/**
 * @file CompressUtils.hpp
 * @author Nexepic
 * @date 2025/3/26
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
#include <cstdint>
#include <string>

namespace graph::utils {

	std::string zlibCompress(const std::string &data);

	std::string zlibDecompress(const std::string &data, uint32_t originalSize);

} // namespace graph::utils
