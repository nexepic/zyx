/**
 * @file Serializer.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/utils/Serializer.hpp"

namespace graph::utils {

	template<typename T>
	void Serializer::writePOD(std::ostream &os, const T &pod) {
		os.write(reinterpret_cast<const char *>(&pod), sizeof(T));
	}

	template<typename T>
	T Serializer::readPOD(std::istream &is) {
		T pod;
		is.read(reinterpret_cast<char *>(&pod), sizeof(T));
		return pod;
	}

	void Serializer::writeBuffer(std::ostream &os, const char *buffer, size_t size) {
		os.write(buffer, static_cast<std::streamsize>(size));
	}

	void Serializer::readBuffer(std::istream &is, char *buffer, size_t size) {
		is.read(buffer, static_cast<std::streamsize>(size));
	}

	template void Serializer::writePOD<unsigned long>(std::ostream &os, const unsigned long &pod);
	template unsigned long Serializer::readPOD<unsigned long>(std::istream &is);

	template void Serializer::writePOD<unsigned char>(std::ostream &os, const unsigned char &pod);
	template unsigned char Serializer::readPOD<unsigned char>(std::istream &is);

	// int
	template void Serializer::writePOD<int>(std::ostream &os, const int &pod);
	template int Serializer::readPOD<int>(std::istream &is);

	template void Serializer::writePOD<unsigned int>(std::ostream &os, const unsigned int &pod);
	template unsigned int Serializer::readPOD<unsigned int>(std::istream &is);

	template void Serializer::writePOD<unsigned long long>(std::ostream &os, const unsigned long long &pod);
	template unsigned long long Serializer::readPOD<unsigned long long>(std::istream &is);

	template void Serializer::writePOD<bool>(std::ostream &os, const bool &pod);
	template bool Serializer::readPOD<bool>(std::istream &is);

	template void Serializer::writePOD<int64_t>(std::ostream &os, const int64_t &pod);
	template int64_t Serializer::readPOD<int64_t>(std::istream &is);

	// Explicit instantiation of other template functions
	// Explicit instantiation of serialize for std::string
	template void Serializer::serialize<std::string>(std::ostream &os, const std::string &value);

	// Explicit instantiation of serialize for double
	template void Serializer::serialize<double>(std::ostream &os, const double &value);

	// Explicit instantiation of serialize for std::vector<double>
	template void Serializer::serialize<std::vector<double>>(std::ostream &os, const std::vector<double> &value);

	// Explicit instantiation of serialize for bool
	template void Serializer::serialize<bool>(std::ostream &os, const bool &value);

	// Explicit instantiation of serialize for int64_t
	template void Serializer::serialize<int64_t>(std::ostream &os, const int64_t &value);

	// Explicit instantiation of serialize for std::vector<std::string>
	template void Serializer::serialize<std::vector<std::string>>(std::ostream &os,
																  const std::vector<std::string> &value);

	// Explicit instantiation of serialize for float
	template void Serializer::serialize<float>(std::ostream &os, const float &value);

	// Explicit instantiation of serialize for int
	template void Serializer::serialize<int>(std::ostream &os, const int &value);

	// Explicit instantiation of serialize for std::vector<long long>
	template void Serializer::serialize<std::vector<long long>>(std::ostream &os, const std::vector<long long> &value);

	// Explicit instantiation of serialize for std::monostate
	template void Serializer::serialize<std::monostate>(std::ostream &os, const std::monostate &value);

	void Serializer::writeString(std::ostream &os, const std::string &str) {
		writePOD(os, static_cast<uint32_t>(str.size()));
		os.write(str.data(), static_cast<std::streamsize>(str.size()));
	}

	std::string Serializer::readString(std::istream &is) {
		auto size = readPOD<uint32_t>(is);
		std::string str(size, '\0');
		is.read(&str[0], size);
		return str;
	}

	void Serializer::writePropertyValue(std::ostream &os, const PropertyValue &value) {
		std::visit(
				[&os]<typename T0>(T0 &&arg) {
					using T = std::decay_t<T0>;
					if constexpr (std::is_same_v<T, int64_t>) {
						writePOD(os, static_cast<uint8_t>(0));
						writePOD(os, arg);
					} else if constexpr (std::is_same_v<T, double>) {
						writePOD(os, static_cast<uint8_t>(1));
						writePOD(os, arg);
					} else if constexpr (std::is_same_v<T, std::string>) {
						writePOD(os, static_cast<uint8_t>(2));
						writeString(os, arg);
					} else if constexpr (std::is_same_v<T, std::vector<double>>) {
						writePOD(os, static_cast<uint8_t>(3));
						serialize(os, arg);
					} else if constexpr (std::is_same_v<T, bool>) {
						writePOD(os, static_cast<uint8_t>(4));
						writePOD(os, arg);
					} else {
						throw std::runtime_error("Unsupported PropertyValue type");
					}
				},
				value);
	}

	PropertyValue Serializer::readPropertyValue(std::istream &is) {
		auto type = readPOD<uint8_t>(is);
		switch (type) {
			case 0:
				return readPOD<int64_t>(is);
			case 1:
				return readPOD<double>(is);
			case 2:
				return readString(is);
			case 3:
				return deserialize<std::vector<double>>(is);
			case 4:
				return readPOD<bool>(is);
			default:
				throw std::runtime_error("Invalid type tag");
		}
	}

	template<typename T>
	void Serializer::serialize(std::ostream &os, const T &value) {
		if constexpr (std::is_same_v<T, std::string>) {
			writeString(os, value);
		} else if constexpr (std::is_same_v<T, std::vector<double>>) {
			writePOD(os, static_cast<uint32_t>(value.size()));
			os.write(reinterpret_cast<const char *>(value.data()), value.size() * sizeof(double));
		} else {
			writePOD(os, value);
		}
	}

	template<typename T>
	T Serializer::deserialize(std::istream &is) {
		if constexpr (std::is_same_v<T, std::string>) {
			return readString(is);
		} else if constexpr (std::is_same_v<T, std::vector<double>>) {
			auto size = readPOD<uint32_t>(is);
			std::vector<double> vec(size);
			is.read(reinterpret_cast<char *>(vec.data()), static_cast<std::streamsize>(size * sizeof(double)));
			return vec;
		} else {
			return readPOD<T>(is);
		}
	}

	PropertyValue Serializer::deserializeVariant(std::istream &is) {
		switch (readPOD<uint8_t>(is)) {
			case 0:
				return readPOD<int64_t>(is);
			case 1:
				return readPOD<double>(is);
			case 2:
				return readString(is);
			case 3:
				return deserialize<std::vector<double>>(is);
			case 4:
				return readPOD<bool>(is);
			default:
				throw std::runtime_error("Invalid type tag");
		}
	}

} // namespace graph::utils
