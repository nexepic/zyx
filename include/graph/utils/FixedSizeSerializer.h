/**
 * @file FixedSizeSerializer.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/4/7
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once
#include <vector>
#include <stdexcept>

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
				throw std::runtime_error("Object serialized size (" +
										std::to_string(bytesWritten) +
										") exceeds allocated fixed size (" +
										std::to_string(fixedSize) + ")");
			}

			// Pad with zeros to reach the fixed size
			if (bytesWritten < static_cast<std::streamoff>(fixedSize)) {
				std::vector<char> padding(static_cast<std::streamoff>(fixedSize) - bytesWritten, 0);
				os.write(padding.data(), static_cast<std::streamsize>(padding.size()));
			}
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