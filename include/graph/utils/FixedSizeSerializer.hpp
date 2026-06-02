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
#include <algorithm>
#include <cstring>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <vector>

namespace graph::utils {

	namespace detail {
		class FixedBufferStreamBuf : public std::streambuf {
		public:
			FixedBufferStreamBuf(char *buffer, size_t size) { setp(buffer, buffer + size); }

			[[nodiscard]] size_t bytesWritten() const { return static_cast<size_t>(pptr() - pbase()); }

		protected:
			std::streamsize xsputn(const char *s, std::streamsize count) override {
				const auto available = static_cast<std::streamsize>(epptr() - pptr());
				const auto toWrite = (std::min)(available, count);
				if (toWrite > 0) {
					std::memcpy(pptr(), s, static_cast<size_t>(toWrite));
					pbump(static_cast<int>(toWrite));
				}
				return toWrite;
			}

			int_type overflow(int_type ch) override {
				if (traits_type::eq_int_type(ch, traits_type::eof())) {
					return traits_type::not_eof(ch);
				}
				if (pptr() == epptr()) {
					return traits_type::eof();
				}
				*pptr() = traits_type::to_char_type(ch);
				pbump(1);
				return ch;
			}
		};
	} // namespace detail

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
			std::vector<char> buffer(fixedSize, 0);
			serializeInto(buffer.data(), obj, fixedSize);
			return buffer;
		}

		/**
		 * @brief Serialize directly into a pre-allocated buffer at the given destination.
		 *
		 * Eliminates the intermediate ostringstream/string/vector copies of serializeToBuffer.
		 * The destination must have at least fixedSize bytes available.
			 */
			template<typename T>
			static void serializeInto(char *dest, const T &obj, size_t fixedSize) {
				detail::FixedBufferStreamBuf buffer(dest, fixedSize);
				std::ostream os(&buffer);
				obj.serialize(os);

			if (!os) {
				throw std::runtime_error("Object serialized size exceeds allocated fixed size (" +
										 std::to_string(fixedSize) + ")");
			}

			const size_t bytesWritten = buffer.bytesWritten();
			if (bytesWritten < fixedSize) {
				std::memset(dest + bytesWritten, 0, fixedSize - bytesWritten);
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
