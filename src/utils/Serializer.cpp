/**
 * @file Serializer
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/2/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/utils/Serializer.h"

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

    template void Serializer::writePOD<unsigned int>(std::ostream &os, const unsigned int &pod);
    template unsigned int Serializer::readPOD<unsigned int>(std::istream &is);

    template void Serializer::writePOD<unsigned long long>(std::ostream &os, const unsigned long long &pod);
    template unsigned long long Serializer::readPOD<unsigned long long>(std::istream &is);

    template void Serializer::writePOD<bool>(std::ostream &os, const bool &pod);
    template bool Serializer::readPOD<bool>(std::istream &is);

    template void Serializer::writePOD<int64_t>(std::ostream &os, const int64_t &pod);
    template int64_t Serializer::readPOD<int64_t>(std::istream &is);

    // Explicit instantiation of other template functions
    template void Serializer::serialize<std::string>(std::ostream &os, const std::string &value);
    template void Serializer::serialize<double>(std::ostream &os, const double &value);
    template void Serializer::serialize<std::vector<double>>(std::ostream &os, const std::vector<double> &value);
    template void Serializer::serialize<bool>(std::ostream &os, const bool &value);
    template void Serializer::serialize<int64_t>(std::ostream &os, const int64_t &value);

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
            case 0: return readPOD<int64_t>(is);
            case 1: return readPOD<double>(is);
            case 2: return readString(is);
            case 3: return deserialize<std::vector<double>>(is);
            case 4: return readPOD<bool>(is);
            default: throw std::runtime_error("Invalid type tag");
        }
    }

} // namespace graph::utils