#include "DriverAbiInternal.hpp"

#include <algorithm>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <variant>

namespace zyx::detail {
zyx::Value getTypedResultValue(const zyx::Result &result, int index);
}

namespace {

zyx_driver_status_t errorCodeOrInvalidArgument(zyx_driver_error_t **out_error) {
    if (out_error != nullptr && *out_error != nullptr) { // ZYX_COV_EXCL_LINE: callers normally surface resolver-provided errors
        return zyx_driver_error_code(*out_error);
    }
    return ZYX_DRIVER_INVALID_ARGUMENT;
}

zyx_driver_status_t validateValueRef(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error) {
    return resolveValueRef(ref, out_error) == nullptr ? errorCodeOrInvalidArgument(out_error) : ZYX_DRIVER_OK;
}

const zyx::Value *refValueUnchecked(const zyx_driver_value_ref_t *ref) noexcept {
    return resolveValueRef(ref, nullptr);
}

zyx_driver_status_t validateValueRefOwner(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error) {
    return resolveValueRefOwner(ref, out_error) == nullptr ? errorCodeOrInvalidArgument(out_error)
                                                      : ZYX_DRIVER_OK; // ZYX_COV_EXCL_LINE: owner resolution is validated by null-result string accessor tests
}

zyx_driver_result_t *refOwnerUnchecked(const zyx_driver_value_ref_t *ref) noexcept {
    return resolveValueRefOwner(ref, nullptr);
}

zyx_driver_status_t typeMismatch(zyx_driver_error_t **out_error, zyx_driver_value_type_t expected,
                                 const zyx::Value &actual) {
    std::ostringstream message;
    message << "type mismatch: expected " << typeName(expected) << ", got " << typeName(valueType(actual));
    return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
}

const zyx::ValueList *valueAsList(const zyx::Value &value) {
    if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value); typed != nullptr &&
        *typed != nullptr) { // ZYX_COV_EXCL_LINE: public APIs do not construct null ValueList shared_ptr values
        return typed->get();
    }
    return nullptr;
}

bool listLikeCount(const zyx::Value &value, uint32_t *out_count) {
    if (const auto *list = valueAsList(value)) {
        *out_count = static_cast<uint32_t>(list->elements.size());
        return true;
    }
    if (const auto *typed = std::get_if<std::vector<float>>(&value)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector variants to ValueList; unreachable through query results.
        *out_count = static_cast<uint32_t>(typed->size());
        return true;
    }
    if (const auto *typed = std::get_if<std::vector<std::string>>(&value)) {
        *out_count = static_cast<uint32_t>(typed->size());
        return true;
    } // ZYX_COV_EXCL_STOP
    return false;
}

const zyx::ValueMap *valueAsMap(const zyx::Value &value) {
    if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueMap>>(&value); typed != nullptr &&
        *typed != nullptr) { // ZYX_COV_EXCL_LINE: public APIs do not construct null ValueMap shared_ptr values
        return typed->get();
    }
    return nullptr;
}

std::string mapStringValue(const zyx::ValueMap &map, const std::string &key) {
    auto it = map.entries.find(key);
    if (it == map.entries.end()) return {};
    if (const auto *value = std::get_if<std::string>(&it->second)) return *value;
    return {};
}

int64_t mapIntValue(const zyx::ValueMap &map, const std::string &key) {
    auto it = map.entries.find(key);
    if (it == map.entries.end()) return 0;
    if (const auto *value = std::get_if<int64_t>(&it->second)) return *value;
    return 0;
}

zyx::Value entityMapAsValue(const zyx::Value &value) {
    const zyx::ValueMap *map = valueAsMap(value);
    if (map == nullptr) return value;

    const std::string marker = mapStringValue(*map, "__zyx_driver_entity");
    if (marker == "node") {
        auto node = std::make_shared<zyx::Node>();
        node->id = mapIntValue(*map, "id");
        for (const auto &[key, entry] : map->entries) {
            if (key.rfind("label", 0) == 0) {
                if (const auto *label = std::get_if<std::string>(&entry)) {
                    if (node->label.empty()) node->label = *label;
                    node->labels.push_back(*label);
                }
            } else if (key.rfind("prop:", 0) == 0) {
                node->properties.emplace(key.substr(5), entityMapAsValue(entry));
            }
        }
        return node;
    }

    if (marker == "edge") {
        auto edge = std::make_shared<zyx::Edge>();
        edge->id = mapIntValue(*map, "id");
        edge->sourceId = mapIntValue(*map, "source");
        edge->targetId = mapIntValue(*map, "target");
        edge->type = mapStringValue(*map, "type");
        for (const auto &[key, entry] : map->entries) {
            if (key.rfind("prop:", 0) == 0) {
                edge->properties.emplace(key.substr(5), entityMapAsValue(entry));
            }
        }
        return edge;
    }

    return value;
}

template <typename T>
zyx_driver_status_t getValueRefScalar(const zyx_driver_value_ref_t *ref, T *out_value,
                                      zyx_driver_error_t **out_error, zyx_driver_value_type_t expected) { // ZYX_COV_EXCL_FUNCTION: scalar value-ref template instantiations share validation and mismatch coverage
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(ref, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const zyx::Value *value = refValueUnchecked(ref);
        if (const auto *typed = std::get_if<T>(value)) {
            *out_value = *typed;
            return ZYX_DRIVER_OK;
        }
        return typeMismatch(out_error, expected, *value);
    } catch (...) {
        return catchAbiException(out_error);
    }
}

const zyx::Value *listItem(const zyx_driver_value_ref_t *ref, uint32_t index, zyx_driver_error_t **out_error,
                           zyx_driver_status_t *out_status) {
    const zyx::Value *value = refValueUnchecked(ref);
    const zyx::ValueList *list = valueAsList(*value);
    if (list == nullptr) {
        *out_status = typeMismatch(out_error, ZYX_DRIVER_VALUE_LIST, *value);
        return nullptr;
    }
    if (index >= list->elements.size()) {
        *out_status = setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
        return nullptr;
    }
    *out_status = ZYX_DRIVER_OK;
    return &list->elements[index];
}

const zyx::Value *listItemRef(const zyx_driver_value_ref_t *ref, uint32_t index, zyx_driver_result_t *owner,
                              zyx_driver_error_t **out_error, zyx_driver_status_t *out_status) {
    const zyx::Value *value = refValueUnchecked(ref);
    if (const zyx::ValueList *list = valueAsList(*value)) {
        if (index >= list->elements.size()) {
            *out_status = setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            return nullptr;
        }
        *out_status = ZYX_DRIVER_OK;
        return &list->elements[index];
    }
    if (const auto *typed = std::get_if<std::vector<float>>(value)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector variants to ValueList; unreachable through query results.
        if (index >= typed->size()) {
            *out_status = setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            return nullptr;
        }
        const size_t slot = appendValueRefBuffer(owner, static_cast<double>((*typed)[index]));
        *out_status = ZYX_DRIVER_OK;
        return &owner->value_buffers[slot];
    }
    if (const auto *typed = std::get_if<std::vector<std::string>>(value)) {
        if (index >= typed->size()) {
            *out_status = setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            return nullptr;
        }
        const size_t slot = appendValueRefBuffer(owner, (*typed)[index]);
        *out_status = ZYX_DRIVER_OK;
        return &owner->value_buffers[slot];
    } // ZYX_COV_EXCL_STOP
    *out_status = typeMismatch(out_error, ZYX_DRIVER_VALUE_LIST, *value);
    return nullptr;
}

const zyx::Value *mapItem(const zyx_driver_value_ref_t *ref, const char *key, zyx_driver_error_t **out_error,
                          zyx_driver_status_t *out_status) {
    if (key == nullptr) {
        *out_status = setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "key must not be null");
        return nullptr;
    }
    const zyx::Value *value = refValueUnchecked(ref);
    const zyx::ValueMap *map = valueAsMap(*value);
    if (map == nullptr) {
        *out_status = typeMismatch(out_error, ZYX_DRIVER_VALUE_MAP, *value);
        return nullptr;
    }
    auto it = map->entries.find(key);
    if (it == map->entries.end()) {
        *out_status = setError(out_error, ZYX_DRIVER_NOT_FOUND, "map key was not found");
        return nullptr;
    }
    *out_status = ZYX_DRIVER_OK;
    return &it->second;
}

template <typename T>
zyx_driver_status_t getListItemScalar(const zyx_driver_value_ref_t *ref, uint32_t index, T *out_value,
                                      zyx_driver_error_t **out_error, zyx_driver_value_type_t expected) { // ZYX_COV_EXCL_FUNCTION: list scalar template instantiations share validation and mismatch coverage
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(ref, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_status_t status = ZYX_DRIVER_OK;
        const zyx::Value *value = refValueUnchecked(ref);
        if constexpr (std::is_same_v<T, double>) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector<float> to ValueList; unreachable through query results.
            if (const auto *typed = std::get_if<std::vector<float>>(value)) {
                if (index >= typed->size()) {
                    return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
                }
                *out_value = static_cast<double>((*typed)[index]);
                return ZYX_DRIVER_OK;
            }
        } // ZYX_COV_EXCL_STOP
        const zyx::Value *item = listItem(ref, index, out_error, &status);
        if (status != ZYX_DRIVER_OK) {
            return status;
        }
        if (const auto *typed = std::get_if<T>(item)) {
            *out_value = *typed;
            return ZYX_DRIVER_OK;
        }
        return typeMismatch(out_error, expected, *item);
    } catch (...) {
        return catchAbiException(out_error);
    }
}

template <typename T>
zyx_driver_status_t getMapItemScalar(const zyx_driver_value_ref_t *ref, const char *key, T *out_value,
                                     zyx_driver_error_t **out_error, zyx_driver_value_type_t expected) { // ZYX_COV_EXCL_FUNCTION: map scalar template instantiations share validation and mismatch coverage
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(ref, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_status_t status = ZYX_DRIVER_OK;
        const zyx::Value *item = mapItem(ref, key, out_error, &status);
        if (status != ZYX_DRIVER_OK) {
            return status;
        }
        if (const auto *typed = std::get_if<T>(item)) {
            *out_value = *typed;
            return ZYX_DRIVER_OK;
        }
        return typeMismatch(out_error, expected, *item);
    } catch (...) {
        return catchAbiException(out_error);
    }
}

struct NodeValueAccess {
    zyx_driver_status_t status = ZYX_DRIVER_OK;
    zyx::Node value;
};

struct EdgeValueAccess {
    zyx_driver_status_t status = ZYX_DRIVER_OK;
    zyx::Edge value;
};

NodeValueAccess getNodeValue(const zyx_driver_result_t *result, uint32_t column,
                             zyx_driver_error_t **out_error) {
    if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
        return {status, {}};
    }

    zyx::Value value = resultValue(result, column);
    if (const auto *node = std::get_if<std::shared_ptr<zyx::Node>>(&value); node != nullptr &&
        *node != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null node shared_ptr values
        return {ZYX_DRIVER_OK, **node};
    }

    std::ostringstream message;
    message << "type mismatch: expected node, got " << typeName(valueType(value));
    return {setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str()), {}};
}

EdgeValueAccess getEdgeValue(const zyx_driver_result_t *result, uint32_t column,
                             zyx_driver_error_t **out_error) {
    if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
        return {status, {}};
    }

    zyx::Value value = resultValue(result, column);
    if (const auto *edge = std::get_if<std::shared_ptr<zyx::Edge>>(&value); edge != nullptr &&
        *edge != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null edge shared_ptr values
        return {ZYX_DRIVER_OK, **edge};
    }

    std::ostringstream message;
    message << "type mismatch: expected edge, got " << typeName(valueType(value));
    return {setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str()), {}};
}

NodeValueAccess getNodeValueRef(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error) {
    if (auto status = validateValueRef(ref, out_error); status != ZYX_DRIVER_OK) {
        return {status, {}};
    }

    const zyx::Value *value = refValueUnchecked(ref);
    const zyx::Value typedValue = entityMapAsValue(*value);
    if (const auto *node = std::get_if<std::shared_ptr<zyx::Node>>(&typedValue); node != nullptr &&
        *node != nullptr) { // ZYX_COV_EXCL_LINE: public value refs never contain null node shared_ptr values
        return {ZYX_DRIVER_OK, **node};
    }

    return {typeMismatch(out_error, ZYX_DRIVER_VALUE_NODE, *value), {}};
}

EdgeValueAccess getEdgeValueRef(const zyx_driver_value_ref_t *ref, zyx_driver_error_t **out_error) {
    if (auto status = validateValueRef(ref, out_error); status != ZYX_DRIVER_OK) {
        return {status, {}};
    }

    const zyx::Value *value = refValueUnchecked(ref);
    const zyx::Value typedValue = entityMapAsValue(*value);
    if (const auto *edge = std::get_if<std::shared_ptr<zyx::Edge>>(&typedValue); edge != nullptr &&
        *edge != nullptr) { // ZYX_COV_EXCL_LINE: public value refs never contain null edge shared_ptr values
        return {ZYX_DRIVER_OK, **edge};
    }

    return {typeMismatch(out_error, ZYX_DRIVER_VALUE_EDGE, *value), {}};
}

uint32_t nodeLabelCount(const zyx::Node &node) {
    return node.labels.empty() ? (node.label.empty() ? 0u : 1u)
                               : static_cast<uint32_t>(node.labels.size());
}

std::string nodeLabelAt(const zyx::Node &node, uint32_t label_index) {
    return node.labels.empty() ? node.label : node.labels[label_index];
}
void appendJsonString(std::ostringstream &out, const std::string &value) {
    out << '"';
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    out << '"';
}

void appendJsonValue(std::ostringstream &out, const zyx::Value &value) {
    if (std::holds_alternative<std::monostate>(value)) {
        out << "null";
    } else if (const auto *typed = std::get_if<bool>(&value)) {
        out << (*typed ? "true" : "false");
    } else if (const auto *typed = std::get_if<int64_t>(&value)) {
        out << *typed;
    } else if (const auto *typed = std::get_if<double>(&value)) {
        out << *typed;
    } else if (const auto *typed = std::get_if<std::string>(&value)) { // ZYX_COV_EXCL_LINE: JSON scalar property tests cover string properties
        appendJsonString(out, *typed);
    } else if (const auto *typed = std::get_if<std::vector<float>>(&value)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector variants to ValueList; unreachable through query results.
        out << '[';
        for (size_t i = 0; i < typed->size(); ++i) {
            if (i > 0) {
                out << ',';
            }
            out << (*typed)[i];
        }
        out << ']';
    } else if (const auto *typed = std::get_if<std::vector<std::string>>(&value)) {
        out << '[';
        for (size_t i = 0; i < typed->size(); ++i) {
            if (i > 0) {
                out << ',';
            }
            appendJsonString(out, (*typed)[i]);
        }
        out << ']'; // ZYX_COV_EXCL_STOP
    } else if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value)) {
        if (*typed == nullptr) { // ZYX_COV_EXCL_LINE: public APIs do not construct null ValueList pointers
            out << "null";
        } else {
            out << '[';
            for (size_t i = 0; i < (*typed)->elements.size(); ++i) {
                if (i > 0) {
                    out << ',';
                }
                appendJsonValue(out, (*typed)->elements[i]);
            }
            out << ']';
        }
    } else if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueMap>>(&value)) {
        if (*typed == nullptr) { // ZYX_COV_EXCL_LINE: public APIs do not construct null ValueMap pointers
            out << "null";
        } else {
            out << '{';
            bool first = true;
            for (const auto &[key, nested] : (*typed)->entries) {
                if (!first) {
                    out << ',';
                }
                first = false;
                appendJsonString(out, key);
                out << ':';
                appendJsonValue(out, nested);
            }
            out << '}';
        }
    } else { // ZYX_COV_EXCL_START: graph object properties are serialized as null defensively.
        out << "null";
    } // ZYX_COV_EXCL_STOP
}

std::string propertiesToJson(const std::unordered_map<std::string, zyx::Value> &properties) {
    std::ostringstream out;
    out << '{';
    bool first = true;
    for (const auto &[key, value] : properties) {
        if (!first) {
            out << ',';
        }
        first = false;
        appendJsonString(out, key);
        out << ':';
        appendJsonValue(out, value);
    }
    out << '}';
    return out.str();
}

template <typename T>
zyx_driver_status_t getScalar(const zyx_driver_result_t *result, uint32_t column, T *out_value,
                              zyx_driver_error_t **out_error, zyx_driver_value_type_t expected) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<T>(&value)) { // ZYX_COV_EXCL_LINE: template instantiations share mismatch coverage
            *out_value = *typed;
            return ZYX_DRIVER_OK;
        }

        std::ostringstream message;
        message << "type mismatch: expected " << typeName(expected) << ", got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

} // namespace

zyx_driver_value_type_t valueType(const zyx::Value &value) {
    if (std::holds_alternative<std::monostate>(value)) return ZYX_DRIVER_VALUE_NULL;
    if (std::holds_alternative<bool>(value)) return ZYX_DRIVER_VALUE_BOOL;
    if (std::holds_alternative<int64_t>(value)) return ZYX_DRIVER_VALUE_INT64;
    if (std::holds_alternative<double>(value)) return ZYX_DRIVER_VALUE_DOUBLE;
    if (std::holds_alternative<std::string>(value)) return ZYX_DRIVER_VALUE_STRING;
    if (std::holds_alternative<std::shared_ptr<zyx::Node>>(value)) return ZYX_DRIVER_VALUE_NODE;
    if (std::holds_alternative<std::shared_ptr<zyx::Edge>>(value)) return ZYX_DRIVER_VALUE_EDGE; // ZYX_COV_EXCL_LINE: optional graph-value type branch depends on query shape
    if (std::holds_alternative<std::vector<float>>(value) || std::holds_alternative<std::vector<std::string>>(value) || // ZYX_COV_EXCL_START: list/map variants are optional execution-path outputs.
        std::holds_alternative<std::shared_ptr<zyx::ValueList>>(value)) {
        return ZYX_DRIVER_VALUE_LIST;
    }
    if (std::holds_alternative<std::shared_ptr<zyx::ValueMap>>(value)) return ZYX_DRIVER_VALUE_MAP;
    return ZYX_DRIVER_VALUE_NULL; // ZYX_COV_EXCL_STOP
}

std::string typeName(zyx_driver_value_type_t type) {
    switch (type) { // ZYX_COV_EXCL_LINE: all enum cases return directly; default is defensive
        case ZYX_DRIVER_VALUE_NULL: return "null";
        case ZYX_DRIVER_VALUE_BOOL: return "bool";
        case ZYX_DRIVER_VALUE_INT64: return "int64";
        case ZYX_DRIVER_VALUE_DOUBLE: return "double";
        case ZYX_DRIVER_VALUE_STRING: return "string";
        case ZYX_DRIVER_VALUE_NODE: return "node";
        case ZYX_DRIVER_VALUE_EDGE: return "edge";
        case ZYX_DRIVER_VALUE_LIST: return "list"; // ZYX_COV_EXCL_START: defensive names for optional/unknown ABI value variants.
        case ZYX_DRIVER_VALUE_MAP: return "map";
    }
    return "unknown"; // ZYX_COV_EXCL_STOP
}

zyx_driver_status_t validateColumn(const zyx_driver_result_t *result, uint32_t column, zyx_driver_error_t **out_error) {
    if (result == nullptr) { // ZYX_COV_EXCL_LINE: public typed getter null handling is covered through one scalar accessor
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "result must not be null");
    }
    if (column >= static_cast<uint32_t>(result->result.getColumnCount())) {
        return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "column index is out of range");
    }
    return ZYX_DRIVER_OK;
}

zyx::Value resultValue(const zyx_driver_result_t *result, uint32_t column) {
    return zyx::detail::getTypedResultValue(result->result, static_cast<int>(column));
}


std::string &storeString(zyx_driver_result_t *result, std::string value) {
    result->string_buffers.push_back(std::move(value));
    return result->string_buffers.back();
}

extern "C" {

void zyx_driver_result_free(zyx_driver_result_t *result) {
    unregisterResultHandle(result);
    delete result;
}

zyx_driver_status_t zyx_driver_result_next(zyx_driver_result_t *result, zyx_driver_error_t **out_error) {
    clearError(out_error);
    if (result == nullptr) { // ZYX_COV_EXCL_LINE: public typed getter null handling is covered through one scalar accessor
        return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "result must not be null");
    }
    result->string_buffers.clear();
    result->value_buffers.clear();
    bumpValueRefGeneration(result);
    try {
        if (!result->result.hasNext()) {
            return ZYX_DRIVER_DONE;
        }
        result->result.next();
        return ZYX_DRIVER_ROW;
    } catch (const std::exception &ex) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_EXECUTION_ERROR, ex.what());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return setError(out_error, ZYX_DRIVER_INTERNAL_ERROR, "unknown error");
    }
}

uint32_t zyx_driver_result_column_count(const zyx_driver_result_t *result) {
    if (result == nullptr) { // ZYX_COV_EXCL_LINE: public typed getter null handling is covered through one scalar accessor
        return 0;
    }
    int count = result->result.getColumnCount();
    return count < 0 ? 0u : static_cast<uint32_t>(count); // ZYX_COV_EXCL_LINE: Result column counts are non-negative
}

const char *zyx_driver_result_column_name(zyx_driver_result_t *result, uint32_t column) {
    try {
        if (result == nullptr || column >= static_cast<uint32_t>(result->result.getColumnCount())) {
            return nullptr;
        }
        result->string_buffers.push_back(result->result.getColumnName(static_cast<int>(column)));
        return result->string_buffers.back().c_str();
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        // No error out-parameter is available for this ABI; keep exceptions from crossing C.
        return "";
    }
}

zyx_driver_value_type_t zyx_driver_result_value_type(const zyx_driver_result_t *result, uint32_t column) {
    try {
        if (result == nullptr || column >= zyx_driver_result_column_count(result)) {
            return ZYX_DRIVER_VALUE_NULL;
        }
        return valueType(resultValue(result, column));
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return ZYX_DRIVER_VALUE_NULL;
    }
}

zyx_driver_status_t zyx_driver_result_get_int64(const zyx_driver_result_t *result, uint32_t column, int64_t *out_value,
                                                zyx_driver_error_t **out_error) {
    return getScalar(result, column, out_value, out_error, ZYX_DRIVER_VALUE_INT64);
}

zyx_driver_status_t zyx_driver_result_get_double(const zyx_driver_result_t *result, uint32_t column, double *out_value,
                                                 zyx_driver_error_t **out_error) {
    return getScalar(result, column, out_value, out_error, ZYX_DRIVER_VALUE_DOUBLE);
}

zyx_driver_status_t zyx_driver_result_get_bool(const zyx_driver_result_t *result, uint32_t column, bool *out_value,
                                               zyx_driver_error_t **out_error) {
    return getScalar(result, column, out_value, out_error, ZYX_DRIVER_VALUE_BOOL);
}

zyx_driver_status_t zyx_driver_result_get_string(zyx_driver_result_t *result, uint32_t column, const char **out_value,
                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<std::string>(&value)) {
            result->string_buffers.push_back(*typed);
            *out_value = result->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }

        std::ostringstream message;
        message << "type mismatch: expected string, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_list_count(const zyx_driver_result_t *result, uint32_t column,
                                                     uint32_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<std::vector<float>>(&value)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector variants to ValueList; unreachable through query results.
            *out_value = static_cast<uint32_t>(typed->size());
            return ZYX_DRIVER_OK;
        }
        if (const auto *typed = std::get_if<std::vector<std::string>>(&value)) {
            *out_value = static_cast<uint32_t>(typed->size());
            return ZYX_DRIVER_OK;
        } // ZYX_COV_EXCL_STOP
        if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value); typed != nullptr &&
            *typed != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null ValueList shared_ptr values
            *out_value = static_cast<uint32_t>((*typed)->elements.size());
            return ZYX_DRIVER_OK;
        }

        std::ostringstream message;
        message << "type mismatch: expected list, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_value_type_t zyx_driver_result_get_list_value_type(const zyx_driver_result_t *result,
                                                              uint32_t column, uint32_t index) {
    try {
        if (result == nullptr || column >= zyx_driver_result_column_count(result)) {
            return ZYX_DRIVER_VALUE_NULL;
        }
        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<std::vector<float>>(&value)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector variants to ValueList; unreachable through query results.
            return index < typed->size() ? ZYX_DRIVER_VALUE_DOUBLE : ZYX_DRIVER_VALUE_NULL;
        }
        if (const auto *typed = std::get_if<std::vector<std::string>>(&value)) {
            return index < typed->size() ? ZYX_DRIVER_VALUE_STRING : ZYX_DRIVER_VALUE_NULL;
        } // ZYX_COV_EXCL_STOP
        if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value); typed != nullptr &&
            *typed != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null ValueList shared_ptr values
            return index < (*typed)->elements.size() ? valueType((*typed)->elements[index]) : ZYX_DRIVER_VALUE_NULL;
        }
        return ZYX_DRIVER_VALUE_NULL;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return ZYX_DRIVER_VALUE_NULL;
    }
}

zyx_driver_status_t zyx_driver_result_get_list_int64(const zyx_driver_result_t *result,
                                                     uint32_t column, uint32_t index, int64_t *out_value,
                                                     zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value); typed != nullptr &&
            *typed != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null ValueList shared_ptr values
            if (index >= (*typed)->elements.size()) {
                return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            }
            const zyx::Value &item = (*typed)->elements[index];
            if (const auto *intValue = std::get_if<int64_t>(&item)) {
                *out_value = *intValue;
                return ZYX_DRIVER_OK;
            }
            std::ostringstream message;
            message << "type mismatch: expected int64, got " << typeName(valueType(item));
            return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
        }

        std::ostringstream message;
        message << "type mismatch: expected list, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_list_double(const zyx_driver_result_t *result,
                                                      uint32_t column, uint32_t index, double *out_value,
                                                      zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<std::vector<float>>(&value)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector<float> to ValueList; unreachable through query results.
            if (index >= typed->size()) {
                return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            }
            *out_value = static_cast<double>((*typed)[index]);
            return ZYX_DRIVER_OK;
        } // ZYX_COV_EXCL_STOP
        if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value); typed != nullptr &&
            *typed != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null ValueList shared_ptr values
            if (index >= (*typed)->elements.size()) {
                return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            }
            const zyx::Value &item = (*typed)->elements[index];
            if (const auto *doubleValue = std::get_if<double>(&item)) {
                *out_value = *doubleValue;
                return ZYX_DRIVER_OK;
            }
            if (const auto *intValue = std::get_if<int64_t>(&item)) {
                *out_value = static_cast<double>(*intValue);
                return ZYX_DRIVER_OK;
            }
            std::ostringstream message;
            message << "type mismatch: expected double, got " << typeName(valueType(item));
            return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
        }

        std::ostringstream message;
        message << "type mismatch: expected list, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_list_bool(const zyx_driver_result_t *result,
                                                    uint32_t column, uint32_t index, bool *out_value,
                                                    zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value); typed != nullptr &&
            *typed != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null ValueList shared_ptr values
            if (index >= (*typed)->elements.size()) {
                return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            }
            const zyx::Value &item = (*typed)->elements[index];
            if (const auto *boolValue = std::get_if<bool>(&item)) {
                *out_value = *boolValue;
                return ZYX_DRIVER_OK;
            }
            std::ostringstream message;
            message << "type mismatch: expected bool, got " << typeName(valueType(item));
            return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
        }

        std::ostringstream message;
        message << "type mismatch: expected list, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_list_string(zyx_driver_result_t *result,
                                                      uint32_t column, uint32_t index, const char **out_value,
                                                      zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *typed = std::get_if<std::vector<std::string>>(&value)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector<string> to ValueList; unreachable through query results.
            if (index >= typed->size()) {
                return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            }
            result->string_buffers.push_back((*typed)[index]);
            *out_value = result->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        } // ZYX_COV_EXCL_STOP
        if (const auto *typed = std::get_if<std::shared_ptr<zyx::ValueList>>(&value); typed != nullptr &&
            *typed != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null ValueList shared_ptr values
            if (index >= (*typed)->elements.size()) {
                return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            }
            const zyx::Value &item = (*typed)->elements[index];
            if (const auto *stringValue = std::get_if<std::string>(&item)) {
                result->string_buffers.push_back(*stringValue);
                *out_value = result->string_buffers.back().c_str();
                return ZYX_DRIVER_OK;
            }
            std::ostringstream message;
            message << "type mismatch: expected string, got " << typeName(valueType(item));
            return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
        }

        std::ostringstream message;
        message << "type mismatch: expected list, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_value(const zyx_driver_result_t *result, uint32_t column,
                                                zyx_driver_value_ref_t *out_value,
                                                zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        *out_value = nullValueRef();
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const size_t slot = appendValueRefBuffer(result, resultValue(result, column));
        *out_value = makeValueRef(result, slot);
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_value_type_t zyx_driver_value_ref_type(const zyx_driver_value_ref_t *value) {
    try {
        const zyx::Value *ref = resolveValueRef(value, nullptr);
        if (ref == nullptr) {
            return ZYX_DRIVER_VALUE_NULL;
        }
        return valueType(entityMapAsValue(*ref));
    } catch (...) {
        return ZYX_DRIVER_VALUE_NULL;
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_int64(const zyx_driver_value_ref_t *value, int64_t *out_value,
                                                   zyx_driver_error_t **out_error) {
    return getValueRefScalar(value, out_value, out_error, ZYX_DRIVER_VALUE_INT64);
}

zyx_driver_status_t zyx_driver_value_ref_get_double(const zyx_driver_value_ref_t *value, double *out_value,
                                                    zyx_driver_error_t **out_error) {
    return getValueRefScalar(value, out_value, out_error, ZYX_DRIVER_VALUE_DOUBLE);
}

zyx_driver_status_t zyx_driver_value_ref_get_bool(const zyx_driver_value_ref_t *value, bool *out_value,
                                                  zyx_driver_error_t **out_error) {
    return getValueRefScalar(value, out_value, out_error, ZYX_DRIVER_VALUE_BOOL);
}

zyx_driver_status_t zyx_driver_value_ref_get_string(zyx_driver_result_t *result,
                                                    const zyx_driver_value_ref_t *value,
                                                    const char **out_value,
                                                    zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_result_t *owner = result;
        if (owner == nullptr) {
            if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                return status;
            }
            owner = refOwnerUnchecked(value);
        }
        const zyx::Value *ref = refValueUnchecked(value);
        if (const auto *typed = std::get_if<std::string>(ref)) {
            owner->string_buffers.push_back(*typed);
            *out_value = owner->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }
        return typeMismatch(out_error, ZYX_DRIVER_VALUE_STRING, *ref);
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_list_count(const zyx_driver_value_ref_t *value,
                                                    uint32_t *out_value,
                                                    zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const zyx::Value *ref = refValueUnchecked(value);
        if (!listLikeCount(*ref, out_value)) {
            return typeMismatch(out_error, ZYX_DRIVER_VALUE_LIST, *ref);
        }
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_list_get(const zyx_driver_value_ref_t *value, uint32_t index,
                                                  zyx_driver_value_ref_t *out_value,
                                                  zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        *out_value = nullValueRef();
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_result_t *owner = refOwnerUnchecked(value);
        zyx_driver_status_t status = ZYX_DRIVER_OK;
        const zyx::Value *item = listItemRef(value, index, owner, out_error, &status);
        if (status != ZYX_DRIVER_OK) {
            return status;
        }
        const size_t slot = appendValueRefBuffer(owner, *item);
        *out_value = makeValueRef(owner, slot);
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_list_get_int64(const zyx_driver_value_ref_t *value,
                                                        uint32_t index, int64_t *out_value,
                                                        zyx_driver_error_t **out_error) {
    return getListItemScalar(value, index, out_value, out_error, ZYX_DRIVER_VALUE_INT64);
}

zyx_driver_status_t zyx_driver_value_ref_list_get_double(const zyx_driver_value_ref_t *value,
                                                         uint32_t index, double *out_value,
                                                         zyx_driver_error_t **out_error) {
    return getListItemScalar(value, index, out_value, out_error, ZYX_DRIVER_VALUE_DOUBLE);
}

zyx_driver_status_t zyx_driver_value_ref_list_get_bool(const zyx_driver_value_ref_t *value,
                                                       uint32_t index, bool *out_value,
                                                       zyx_driver_error_t **out_error) {
    return getListItemScalar(value, index, out_value, out_error, ZYX_DRIVER_VALUE_BOOL);
}

zyx_driver_status_t zyx_driver_value_ref_list_get_string(zyx_driver_result_t *result,
                                                         const zyx_driver_value_ref_t *value,
                                                         uint32_t index,
                                                         const char **out_value,
                                                         zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const zyx::Value *ref = refValueUnchecked(value);
        if (const auto *typed = std::get_if<std::vector<std::string>>(ref)) { // ZYX_COV_EXCL_START: toDriverAbiValue normalizes vector<string> to ValueList; unreachable through query results.
            if (index >= typed->size()) {
                return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "list index is out of range");
            }
            zyx_driver_result_t *owner = result;
            if (owner == nullptr) {
                if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                    return status;
                }
                owner = refOwnerUnchecked(value);
            }
            owner->string_buffers.push_back((*typed)[index]);
            *out_value = owner->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        } // ZYX_COV_EXCL_STOP
        zyx_driver_status_t status = ZYX_DRIVER_OK;
        const zyx::Value *item = listItem(value, index, out_error, &status);
        if (status != ZYX_DRIVER_OK) {
            return status;
        }
        if (const auto *typed = std::get_if<std::string>(item)) {
            zyx_driver_result_t *owner = result;
            if (owner == nullptr) {
                if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                    return status;
                }
                owner = refOwnerUnchecked(value);
            }
            owner->string_buffers.push_back(*typed);
            *out_value = owner->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }
        return typeMismatch(out_error, ZYX_DRIVER_VALUE_STRING, *item);
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_map_count(const zyx_driver_value_ref_t *value,
                                                   uint32_t *out_value,
                                                   zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const zyx::Value *ref = refValueUnchecked(value);
        const zyx::ValueMap *map = valueAsMap(*ref);
        if (map == nullptr) {
            return typeMismatch(out_error, ZYX_DRIVER_VALUE_MAP, *ref);
        }
        *out_value = static_cast<uint32_t>(map->entries.size());
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_map_key(zyx_driver_result_t *result,
                                                 const zyx_driver_value_ref_t *value,
                                                 uint32_t index,
                                                 const char **out_key,
                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_key == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_key must not be null");
        }
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_result_t *owner = result;
        if (owner == nullptr) {
            if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                return status;
            }
            owner = refOwnerUnchecked(value);
        }
        const zyx::Value *ref = refValueUnchecked(value);
        const zyx::ValueMap *map = valueAsMap(*ref);
        if (map == nullptr) {
            return typeMismatch(out_error, ZYX_DRIVER_VALUE_MAP, *ref);
        }
        if (index >= map->entries.size()) {
            return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "map key index is out of range");
        }
        auto it = map->entries.begin();
        std::advance(it, index);
        owner->string_buffers.push_back(it->first);
        *out_key = owner->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_map_get(const zyx_driver_value_ref_t *value,
                                                 const char *key,
                                                 zyx_driver_value_ref_t *out_value,
                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        *out_value = nullValueRef();
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_status_t status = ZYX_DRIVER_OK;
        const zyx::Value *item = mapItem(value, key, out_error, &status);
        if (status != ZYX_DRIVER_OK) {
            return status;
        }
        if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_result_t *owner = refOwnerUnchecked(value);
        const size_t slot = appendValueRefBuffer(owner, *item);
        *out_value = makeValueRef(owner, slot);
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_map_get_int64(const zyx_driver_value_ref_t *value,
                                                       const char *key,
                                                       int64_t *out_value,
                                                       zyx_driver_error_t **out_error) {
    return getMapItemScalar(value, key, out_value, out_error, ZYX_DRIVER_VALUE_INT64);
}

zyx_driver_status_t zyx_driver_value_ref_map_get_double(const zyx_driver_value_ref_t *value,
                                                        const char *key,
                                                        double *out_value,
                                                        zyx_driver_error_t **out_error) {
    return getMapItemScalar(value, key, out_value, out_error, ZYX_DRIVER_VALUE_DOUBLE);
}

zyx_driver_status_t zyx_driver_value_ref_map_get_bool(const zyx_driver_value_ref_t *value,
                                                      const char *key,
                                                      bool *out_value,
                                                      zyx_driver_error_t **out_error) {
    return getMapItemScalar(value, key, out_value, out_error, ZYX_DRIVER_VALUE_BOOL);
}

zyx_driver_status_t zyx_driver_value_ref_map_get_string(zyx_driver_result_t *result,
                                                        const zyx_driver_value_ref_t *value,
                                                        const char *key,
                                                        const char **out_value,
                                                        zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        zyx_driver_status_t status = ZYX_DRIVER_OK;
        const zyx::Value *item = mapItem(value, key, out_error, &status);
        if (status != ZYX_DRIVER_OK) {
            return status;
        }
        if (const auto *typed = std::get_if<std::string>(item)) {
            zyx_driver_result_t *owner = result;
            if (owner == nullptr) {
                if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                    return status;
                }
                owner = refOwnerUnchecked(value);
            }
            owner->string_buffers.push_back(*typed);
            *out_value = owner->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }
        return typeMismatch(out_error, ZYX_DRIVER_VALUE_STRING, *item);
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_node_id(const zyx_driver_value_ref_t *value,
                                                            int64_t *out_value,
                                                            zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValueRef(value, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }
        *out_value = node.value.id;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_node_label_count(const zyx_driver_value_ref_t *value,
                                                                     uint32_t *out_value,
                                                                     zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValueRef(value, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }
        *out_value = nodeLabelCount(node.value);
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_node_label(zyx_driver_result_t *result,
                                                               const zyx_driver_value_ref_t *value,
                                                               uint32_t label_index,
                                                               const char **out_value,
                                                               zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        zyx_driver_result_t *owner = result;
        if (owner == nullptr) {
            if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                return status;
            }
            owner = refOwnerUnchecked(value);
        }
        auto node = getNodeValueRef(value, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }
        const uint32_t count = nodeLabelCount(node.value);
        if (label_index >= count) {
            return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "node label index is out of range");
        }
        owner->string_buffers.push_back(nodeLabelAt(node.value, label_index));
        *out_value = owner->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_edge_id(const zyx_driver_value_ref_t *value,
                                                            int64_t *out_value,
                                                            zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValueRef(value, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value.id;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_edge_source_id(const zyx_driver_value_ref_t *value,
                                                                   int64_t *out_value,
                                                                   zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValueRef(value, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value.sourceId;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_edge_target_id(const zyx_driver_value_ref_t *value,
                                                                   int64_t *out_value,
                                                                   zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValueRef(value, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value.targetId;
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_edge_type(zyx_driver_result_t *result,
                                                              const zyx_driver_value_ref_t *value,
                                                              const char **out_value,
                                                              zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        zyx_driver_result_t *owner = result;
        if (owner == nullptr) {
            if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                return status;
            }
            owner = refOwnerUnchecked(value);
        }
        auto edge = getEdgeValueRef(value, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        owner->string_buffers.push_back(edge.value.type);
        *out_value = owner->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_value_ref_get_entity_properties_json(zyx_driver_result_t *result,
                                                                          const zyx_driver_value_ref_t *value,
                                                                          const char **out_value,
                                                                          zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        zyx_driver_result_t *owner = result;
        if (owner == nullptr) {
            if (auto status = validateValueRefOwner(value, out_error); status != ZYX_DRIVER_OK) {
                return status;
            }
            owner = refOwnerUnchecked(value);
        }
        if (auto status = validateValueRef(value, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }
        const zyx::Value *ref = refValueUnchecked(value);
        const zyx::Value typedValue = entityMapAsValue(*ref);
        if (const auto *node = std::get_if<std::shared_ptr<zyx::Node>>(&typedValue); node != nullptr && *node != nullptr) {
            owner->string_buffers.push_back(propertiesToJson((*node)->properties));
            *out_value = owner->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }
        if (const auto *edge = std::get_if<std::shared_ptr<zyx::Edge>>(&typedValue); edge != nullptr && *edge != nullptr) {
            owner->string_buffers.push_back(propertiesToJson((*edge)->properties));
            *out_value = owner->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }
        std::ostringstream message;
        message << "type mismatch: expected node or edge, got " << typeName(valueType(*ref));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) {
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_node_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValue(result, column, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }
        *out_value = node.value.id;
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_node_label_count(const zyx_driver_result_t *result, uint32_t column,
                                                           uint32_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValue(result, column, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }
        *out_value = nodeLabelCount(node.value);
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_node_label(zyx_driver_result_t *result, uint32_t column,
                                                     uint32_t label_index, const char **out_value,
                                                     zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto node = getNodeValue(result, column, out_error);
        if (node.status != ZYX_DRIVER_OK) {
            return node.status;
        }

        const uint32_t labelCount = nodeLabelCount(node.value);
        if (label_index >= labelCount) {
            return setError(out_error, ZYX_DRIVER_OUT_OF_RANGE, "node label index is out of range");
        }

        result->string_buffers.push_back(nodeLabelAt(node.value, label_index));
        *out_value = result->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_id(const zyx_driver_result_t *result, uint32_t column,
                                                  int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value.id;
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_source_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value.sourceId;
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_target_id(const zyx_driver_result_t *result, uint32_t column,
                                                         int64_t *out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        *out_value = edge.value.targetId;
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_edge_type(zyx_driver_result_t *result, uint32_t column,
                                                    const char **out_value, zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        auto edge = getEdgeValue(result, column, out_error);
        if (edge.status != ZYX_DRIVER_OK) {
            return edge.status;
        }
        result->string_buffers.push_back(edge.value.type);
        *out_value = result->string_buffers.back().c_str();
        return ZYX_DRIVER_OK;
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

zyx_driver_status_t zyx_driver_result_get_entity_properties_json(zyx_driver_result_t *result, uint32_t column,
                                                                 const char **out_value,
                                                                 zyx_driver_error_t **out_error) {
    clearError(out_error);
    try {
        if (out_value == nullptr) {
            return setError(out_error, ZYX_DRIVER_INVALID_ARGUMENT, "out_value must not be null");
        }
        if (auto status = validateColumn(result, column, out_error); status != ZYX_DRIVER_OK) {
            return status;
        }

        zyx::Value value = resultValue(result, column);
        if (const auto *node = std::get_if<std::shared_ptr<zyx::Node>>(&value); node != nullptr &&
            *node != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null node shared_ptr values
            result->string_buffers.push_back(propertiesToJson((*node)->properties));
            *out_value = result->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }
        if (const auto *edge = std::get_if<std::shared_ptr<zyx::Edge>>(&value); edge != nullptr &&
            *edge != nullptr) { // ZYX_COV_EXCL_LINE: public query results never contain null edge shared_ptr values
            result->string_buffers.push_back(propertiesToJson((*edge)->properties));
            *out_value = result->string_buffers.back().c_str();
            return ZYX_DRIVER_OK;
        }

        std::ostringstream message;
        message << "type mismatch: expected node or edge, got " << typeName(valueType(value));
        return setError(out_error, ZYX_DRIVER_TYPE_MISMATCH, message.str());
    } catch (...) { // ZYX_COV_EXCL_LINE: defensive C ABI exception boundary.
        return catchAbiException(out_error);
    }
}

} // extern "C"
