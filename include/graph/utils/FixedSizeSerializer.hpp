/**
 * @file FixedSizeSerializer.hpp
 * @author Nexepic
 * @date 2025/4/7
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
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace graph::utils {

	class FixedSizeSerializer {
	public:
		template<typename T>
		static void serializeWithFixedSize(std::ostream &os, const T &obj, size_t fixedSize) {
			// Save the start position
			std::streampos startPos = os.tellp();

			// Serialize the object using its own serialization method
			obj.serialize(os);

			// Calculate how many bytes were written
			std::streampos endPos = os.tellp();
			std::streamoff bytesWritten = endPos - startPos;

			if (bytesWritten > static_cast<std::streamoff>(fixedSize)) {
				throw std::runtime_error("Object serialized size (" + std::to_string(bytesWritten) +
										 ") exceeds allocated fixed size (" + std::to_string(fixedSize) + ")");
			}

			// Pad with zeros to reach the fixed size
			if (bytesWritten < static_cast<std::streamoff>(fixedSize)) {
				std::vector<char> padding(static_cast<std::streamoff>(fixedSize) - bytesWritten, 0);
				os.write(padding.data(), static_cast<std::streamsize>(padding.size()));
			}
		}

		template<typename T>
		static std::vector<char> serializeToBuffer(const T &obj, size_t fixedSize) {
			std::ostringstream oss(std::ios::binary);
			serializeWithFixedSize(oss, obj, fixedSize);
			auto str = oss.str();
			return {str.begin(), str.end()};
		}

		/**
		 * @brief Serialize directly into a pre-allocated buffer at the given destination.
		 *
		 * Eliminates the intermediate ostringstream/string/vector copies of serializeToBuffer.
		 * The destination must have at least fixedSize bytes available.
		 */
		template<typename T>
		static void serializeInto(char *dest, const T &obj, size_t fixedSize) {
			std::ostringstream oss(std::ios::binary);
			serializeWithFixedSize(oss, obj, fixedSize);
			auto str = oss.str();
			std::memcpy(dest, str.data(), fixedSize);
		}

		template<typename T>
		static T deserializeWithFixedSize(std::istream &is, size_t fixedSize) {
			// Save the start position
			std::streampos startPos = is.tellg();

			// Deserialize the object using its static deserialize method
			T obj = T::deserialize(is);

			// Calculate how many bytes were read
			std::streampos endPos = is.tellg();
			std::streamoff bytesRead = endPos - startPos;

			// Skip any padding to ensure we consume exactly fixedSize bytes
			if (bytesRead < static_cast<std::streamoff>(fixedSize)) {
				is.seekg(static_cast<std::istream::off_type>(fixedSize) - bytesRead, std::ios::cur);
			}

			return obj;
		}
	};

} // namespace graph::utils
