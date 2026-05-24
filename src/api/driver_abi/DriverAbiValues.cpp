#include "DriverAbiInternal.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace {

zyx::Value makeListValue() {
    return std::make_shared<zyx::ValueList>();
}

zyx::Value makeMapValue() {
    return std::make_shared<zyx::ValueMap>();
}

} // namespace

zyx::Value deepCopyValue(const zyx::Value &value) {
    if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value)) {
        if (*typed == nullptr) { // ZYX_COV_EXCL_LINE: public APIs do not construct null ValueList pointers
            return std::shared_ptr<zyx::ValueList>{};
        }
        auto copy = std::make_shared<zyx::ValueList>();
        copy->elements.reserve((*typed)->elements.size());
        for (const auto &element : (*typed)->elements) {
            copy->elements.push_back(deepCopyValue(element));
        }
        return copy;
    }
    if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueMap>>(&value)) {
        if (*typed == nullptr) { // ZYX_COV_EXCL_LINE: public APIs do not construct null ValueMap pointers
            return std::shared_ptr<zyx::ValueMap>{};
        }
        auto copy = std::make_shared<zyx::ValueMap>();
        copy->entries.reserve((*typed)->entries.size());
        for (const auto &[key, nested] : (*typed)->entries) {
            copy->entries.emplace(key, deepCopyValue(nested));
        }
        return copy;
    }
    return value;
}

namespace {

zyx::ValueList &asMutableList(zyx_driver_value_t *value) {
    if (value == nullptr) {
        throw std::invalid_argument("list must not be null");
    }
    auto *list = std::get_if<std::shared_ptr<zyx::ValueList>>(&value->value);
    if (list == nullptr || *list == nullptr) {
        throw std::domain_error("value is not a list");
    }
    return **list;
}

zyx::ValueMap &asMutableMap(zyx_driver_value_t *value) {
    if (value == nullptr) {
        throw std::invalid_argument("map must not be null");
    }
    auto *map = std::get_if<std::shared_ptr<zyx::ValueMap>>(&value->value);
    if (map == nullptr || *map == nullptr) {
        throw std::domain_error("value is not a map");
    }
    return **map;
}

zyx_driver_status_t createValue(zyx::Value value, zyx_driver_value_t **out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        *out_value = nullptr;
        *out_value = new zyx_driver_value_t{std::move(value)};
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t appendListValue(zyx_driver_value_t *list, zyx::Value value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        asMutableList(list).elements.push_back(std::move(value));
        return ZYX_DRIVER_OK;
    } catch (const std::invalid_argument &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (const std::domain_error &ex) {
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, ex.what());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t setMapValue(zyx_driver_value_t *map, const char *key, zyx::Value value,
                                zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (key == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
        }
        asMutableMap(map).entries[key] = std::move(value);
        return ZYX_DRIVER_OK;
    } catch (const std::invalid_argument &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (const std::domain_error &ex) {
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, ex.what());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

} // namespace

extern "C" {

zyx_driver_status_t zyx_driver_value_null_create(zyx_driver_value_t **out_value,
                                                 zyx_driver_error_t **out_error) {
    return createValue(std::monostate{}, out_value, out_error);
}

zyx_driver_status_t zyx_driver_value_bool_create(bool value, zyx_driver_value_t **out_value,
                                                 zyx_driver_error_t **out_error) {
    return createValue(value, out_value, out_error);
}

zyx_driver_status_t zyx_driver_value_int64_create(int64_t value, zyx_driver_value_t **out_value,
                                                  zyx_driver_error_t **out_error) {
    return createValue(value, out_value, out_error);
}

zyx_driver_status_t zyx_driver_value_double_create(double value, zyx_driver_value_t **out_value,
                                                   zyx_driver_error_t **out_error) {
    return createValue(value, out_value, out_error);
}

zyx_driver_status_t zyx_driver_value_string_create(const char *value, zyx_driver_value_t **out_value,
                                                   zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        *out_value = nullptr;
        if (value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value must not be null");
        }

        *out_value = new zyx_driver_value_t{std::string(value)};
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_list_create(zyx_driver_value_t **out_value,
                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        *out_value = nullptr;
        *out_value = new zyx_driver_value_t{makeListValue()};
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_map_create(zyx_driver_value_t **out_value,
                                                zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        *out_value = nullptr;
        *out_value = new zyx_driver_value_t{makeMapValue()};
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

void zyx_driver_value_free(zyx_driver_value_t *value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    delete value;
}

zyx_driver_status_t zyx_driver_value_list_append_null(zyx_driver_value_t *list,
                                                      zyx_driver_error_t **out_error) {
    return appendListValue(list, std::monostate{}, out_error);
}

zyx_driver_status_t zyx_driver_value_list_append_bool(zyx_driver_value_t *list, bool value,
                                                      zyx_driver_error_t **out_error) {
    return appendListValue(list, value, out_error);
}

zyx_driver_status_t zyx_driver_value_list_append_int64(zyx_driver_value_t *list, int64_t value,
                                                       zyx_driver_error_t **out_error) {
    return appendListValue(list, value, out_error);
}

zyx_driver_status_t zyx_driver_value_list_append_double(zyx_driver_value_t *list, double value,
                                                        zyx_driver_error_t **out_error) {
    return appendListValue(list, value, out_error);
}

zyx_driver_status_t zyx_driver_value_list_append_string(zyx_driver_value_t *list, const char *value,
                                                        zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value must not be null");
        }

        asMutableList(list).elements.emplace_back(std::string(value));
        return ZYX_DRIVER_OK;
    } catch (const std::invalid_argument &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (const std::domain_error &ex) {
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, ex.what());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_list_append_value(zyx_driver_value_t *list,
                                                       const zyx_driver_value_t *value,
                                                       zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value must not be null");
        }
        asMutableList(list).elements.push_back(deepCopyValue(value->value));
        return ZYX_DRIVER_OK;
    } catch (const std::invalid_argument &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (const std::domain_error &ex) {
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, ex.what());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_map_set_null(zyx_driver_value_t *map, const char *key,
                                                  zyx_driver_error_t **out_error) {
    return setMapValue(map, key, std::monostate{}, out_error);
}

zyx_driver_status_t zyx_driver_value_map_set_bool(zyx_driver_value_t *map, const char *key, bool value,
                                                  zyx_driver_error_t **out_error) {
    return setMapValue(map, key, value, out_error);
}

zyx_driver_status_t zyx_driver_value_map_set_int64(zyx_driver_value_t *map, const char *key, int64_t value,
                                                   zyx_driver_error_t **out_error) {
    return setMapValue(map, key, value, out_error);
}

zyx_driver_status_t zyx_driver_value_map_set_double(zyx_driver_value_t *map, const char *key, double value,
                                                    zyx_driver_error_t **out_error) {
    return setMapValue(map, key, value, out_error);
}

zyx_driver_status_t zyx_driver_value_map_set_string(zyx_driver_value_t *map, const char *key,
                                                    const char *value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (key == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
        }
        if (value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value must not be null");
        }

        asMutableMap(map).entries[key] = std::string(value);
        return ZYX_DRIVER_OK;
    } catch (const std::invalid_argument &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (const std::domain_error &ex) {
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, ex.what());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_map_set_value(zyx_driver_value_t *map, const char *key,
                                                   const zyx_driver_value_t *value,
                                                   zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (key == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
        }
        if (value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "value must not be null");
        }
        asMutableMap(map).entries[key] = deepCopyValue(value->value);
        return ZYX_DRIVER_OK;
    } catch (const std::invalid_argument &ex) {
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, ex.what());
    } catch (const std::domain_error &ex) {
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, ex.what());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

} // extern "C"
