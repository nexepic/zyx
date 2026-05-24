/**
 * @file addon.cpp
 * @brief Node.js bindings for ZYX graph database using node-addon-api.
 *
 * Uses the Driver ABI as the native engine contract and exposes async operations.
 */

#include <napi.h>
#include "zyx/zyx_driver_abi.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

class DriverError {
public:
    DriverError() = default;
    ~DriverError() { zyx_driver_error_free(error_); }

    DriverError(const DriverError &) = delete;
    DriverError &operator=(const DriverError &) = delete;

    zyx_driver_error_t **out() {
        zyx_driver_error_free(error_);
        error_ = nullptr;
        return &error_;
    }

    std::string message() const {
        const char *message = zyx_driver_error_message(error_);
        return message == nullptr ? std::string("Driver ABI operation failed") : std::string(message);
    }

private:
    zyx_driver_error_t *error_ = nullptr;
};

void ThrowIfNotOk(zyx_driver_status_t status, DriverError &error) {
    if (status != ZYX_DRIVER_OK) {
        throw std::runtime_error(error.message());
    }
}

struct JsValue;
using JsArray = std::vector<JsValue>;
using JsObject = std::unordered_map<std::string, JsValue>;

struct NodeData {
    int64_t id = 0;
    std::vector<std::string> labels;
    JsObject properties;
};

struct EdgeData {
    int64_t id = 0;
    int64_t sourceId = 0;
    int64_t targetId = 0;
    std::string type;
    JsObject properties;
};

struct JsValue {
    using Variant = std::variant<std::monostate, bool, int64_t, double, std::string, JsArray, JsObject, NodeData, EdgeData>;
    Variant value;

    JsValue() : value(std::monostate{}) {}
    template <typename T>
    explicit JsValue(T v) : value(std::move(v)) {}
};

class JsonParser {
public:
    explicit JsonParser(std::string text) : text_(std::move(text)) {}

    JsValue parse() {
        skipWhitespace();
        JsValue value = parseValue();
        skipWhitespace();
        if (pos_ != text_.size()) {
            throw std::runtime_error("Unexpected trailing data in Driver ABI JSON value");
        }
        return value;
    }

private:
    JsValue parseValue() {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            throw std::runtime_error("Unexpected end of Driver ABI JSON value");
        }
        const char ch = text_[pos_];
        if (ch == 'n') {
            consumeLiteral("null");
            return JsValue();
        }
        if (ch == 't') {
            consumeLiteral("true");
            return JsValue(true);
        }
        if (ch == 'f') {
            consumeLiteral("false");
            return JsValue(false);
        }
        if (ch == '"') {
            return JsValue(parseString());
        }
        if (ch == '[') {
            return JsValue(parseArray());
        }
        if (ch == '{') {
            return JsValue(parseObject());
        }
        return parseNumber();
    }

    JsArray parseArray() {
        expect('[');
        JsArray values;
        skipWhitespace();
        if (tryConsume(']')) {
            return values;
        }
        for (;;) {
            values.push_back(parseValue());
            skipWhitespace();
            if (tryConsume(']')) {
                return values;
            }
            expect(',');
        }
    }

    JsObject parseObject() {
        expect('{');
        JsObject values;
        skipWhitespace();
        if (tryConsume('}')) {
            return values;
        }
        for (;;) {
            skipWhitespace();
            std::string key = parseString();
            skipWhitespace();
            expect(':');
            values.emplace(std::move(key), parseValue());
            skipWhitespace();
            if (tryConsume('}')) {
                return values;
            }
            expect(',');
        }
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            char ch = text_[pos_++];
            if (ch == '"') {
                return out;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) {
                throw std::runtime_error("Invalid escape in Driver ABI JSON string");
            }
            char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': appendUtf8(out, parseHexCodepoint()); break;
                default:
                    throw std::runtime_error("Unsupported escape in Driver ABI JSON string");
            }
        }
        throw std::runtime_error("Unterminated Driver ABI JSON string");
    }

    uint32_t parseHexCodepoint() {
        if (pos_ + 4 > text_.size()) {
            throw std::runtime_error("Invalid unicode escape in Driver ABI JSON string");
        }
        char buffer[5] = {text_[pos_], text_[pos_ + 1], text_[pos_ + 2], text_[pos_ + 3], '\0'};
        char *end = nullptr;
        unsigned long value = std::strtoul(buffer, &end, 16);
        if (end != buffer + 4 || value > 0x10FFFFUL) {
            throw std::runtime_error("Invalid unicode escape in Driver ABI JSON string");
        }
        pos_ += 4;
        return static_cast<uint32_t>(value);
    }

    void appendUtf8(std::string &out, uint32_t codepoint) {
        if (codepoint <= 0x7F) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    JsValue parseNumber() {
        const size_t start = pos_;
        if (text_[pos_] == '-') {
            ++pos_;
        }
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        bool isDouble = false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            isDouble = true;
            ++pos_;
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            isDouble = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
                ++pos_;
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        std::string token = text_.substr(start, pos_ - start);
        if (token.empty() || token == "-") {
            throw std::runtime_error("Invalid Driver ABI JSON number");
        }
        if (isDouble) {
            return JsValue(std::stod(token));
        }
        return JsValue(static_cast<int64_t>(std::stoll(token)));
    }

    void consumeLiteral(const char *literal) {
        const std::string expected(literal);
        if (text_.compare(pos_, expected.size(), expected) != 0) {
            throw std::runtime_error("Invalid Driver ABI JSON literal");
        }
        pos_ += expected.size();
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) {
            ++pos_;
        }
    }

    bool tryConsume(char ch) {
        if (pos_ < text_.size() && text_[pos_] == ch) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char ch) {
        if (!tryConsume(ch)) {
            throw std::runtime_error("Unexpected character in Driver ABI JSON value");
        }
    }

    std::string text_;
    size_t pos_ = 0;
};

JsObject ParsePropertiesJson(const char *json) {
    JsonParser parser(json == nullptr ? "{}" : std::string(json));
    JsValue parsed = parser.parse();
    if (auto *object = std::get_if<JsObject>(&parsed.value)) {
        return std::move(*object);
    }
    throw std::runtime_error("Driver ABI entity properties JSON was not an object");
}

Napi::Value ValueToNapi(Napi::Env env, const JsValue &value);

Napi::Object ObjectToNapi(Napi::Env env, const JsObject &object) {
    Napi::Object result = Napi::Object::New(env);
    for (const auto &[key, val] : object) {
        result.Set(key, ValueToNapi(env, val));
    }
    return result;
}

Napi::Value ValueToNapi(Napi::Env env, const JsValue &value) {
    return std::visit(
        [&env](const auto &arg) -> Napi::Value {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return env.Null();
            } else if constexpr (std::is_same_v<T, bool>) {
                return Napi::Boolean::New(env, arg);
            } else if constexpr (std::is_same_v<T, int64_t>) {
                return Napi::Number::New(env, static_cast<double>(arg));
            } else if constexpr (std::is_same_v<T, double>) {
                return Napi::Number::New(env, arg);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return Napi::String::New(env, arg);
            } else if constexpr (std::is_same_v<T, JsArray>) {
                Napi::Array array = Napi::Array::New(env, arg.size());
                for (size_t i = 0; i < arg.size(); ++i) {
                    array[i] = ValueToNapi(env, arg[i]);
                }
                return array;
            } else if constexpr (std::is_same_v<T, JsObject>) {
                return ObjectToNapi(env, arg);
            } else if constexpr (std::is_same_v<T, NodeData>) {
                Napi::Object node = Napi::Object::New(env);
                node.Set("id", Napi::Number::New(env, static_cast<double>(arg.id)));
                Napi::Array labels = Napi::Array::New(env, arg.labels.size());
                for (size_t i = 0; i < arg.labels.size(); ++i) {
                    labels[i] = Napi::String::New(env, arg.labels[i]);
                }
                node.Set("labels", labels);
                node.Set("label", arg.labels.empty() ? Napi::String::New(env, "") : Napi::String::New(env, arg.labels[0]));
                node.Set("properties", ObjectToNapi(env, arg.properties));
                return node;
            } else if constexpr (std::is_same_v<T, EdgeData>) {
                Napi::Object edge = Napi::Object::New(env);
                edge.Set("id", Napi::Number::New(env, static_cast<double>(arg.id)));
                edge.Set("sourceId", Napi::Number::New(env, static_cast<double>(arg.sourceId)));
                edge.Set("targetId", Napi::Number::New(env, static_cast<double>(arg.targetId)));
                edge.Set("type", Napi::String::New(env, arg.type));
                edge.Set("properties", ObjectToNapi(env, arg.properties));
                return edge;
            }
        },
        value.value);
}

class DriverValue {
public:
    explicit DriverValue(zyx_driver_value_t *value) : value_(value) {}
    ~DriverValue() { zyx_driver_value_free(value_, nullptr); }

    DriverValue(const DriverValue &) = delete;
    DriverValue &operator=(const DriverValue &) = delete;

    DriverValue(DriverValue &&other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    DriverValue &operator=(DriverValue &&other) noexcept {
        if (this != &other) {
            zyx_driver_value_free(value_, nullptr);
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    zyx_driver_value_t *get() const { return value_; }

private:
    zyx_driver_value_t *value_ = nullptr;
};

DriverValue BuildDriverValue(const JsValue &value) {
    DriverError error;
    zyx_driver_value_t *raw = nullptr;
    if (std::holds_alternative<std::monostate>(value.value)) {
        ThrowIfNotOk(zyx_driver_value_null_create(&raw, error.out()), error);
    } else if (const auto *typed = std::get_if<bool>(&value.value)) {
        ThrowIfNotOk(zyx_driver_value_bool_create(*typed, &raw, error.out()), error);
    } else if (const auto *typed = std::get_if<int64_t>(&value.value)) {
        ThrowIfNotOk(zyx_driver_value_int64_create(*typed, &raw, error.out()), error);
    } else if (const auto *typed = std::get_if<double>(&value.value)) {
        ThrowIfNotOk(zyx_driver_value_double_create(*typed, &raw, error.out()), error);
    } else if (const auto *typed = std::get_if<std::string>(&value.value)) {
        ThrowIfNotOk(zyx_driver_value_string_create(typed->c_str(), &raw, error.out()), error);
    } else if (const auto *typed = std::get_if<JsArray>(&value.value)) {
        ThrowIfNotOk(zyx_driver_value_list_create(&raw, error.out()), error);
        DriverValue list(raw);
        for (const auto &item : *typed) {
            DriverValue nested = BuildDriverValue(item);
            ThrowIfNotOk(zyx_driver_value_list_append_value(list.get(), nested.get(), error.out()), error);
        }
        return list;
    } else if (const auto *typed = std::get_if<JsObject>(&value.value)) {
        ThrowIfNotOk(zyx_driver_value_map_create(&raw, error.out()), error);
        DriverValue map(raw);
        for (const auto &[key, nested] : *typed) {
            DriverValue nestedValue = BuildDriverValue(nested);
            ThrowIfNotOk(zyx_driver_value_map_set_value(map.get(), key.c_str(), nestedValue.get(), error.out()), error);
        }
        return map;
    } else {
        throw std::runtime_error("Unsupported Driver ABI parameter value type");
    }
    return DriverValue(raw);
}

class DriverParams {
public:
    explicit DriverParams(const JsObject &values) {
        DriverError error;
        ThrowIfNotOk(zyx_driver_params_create(&params_, error.out()), error);
        try {
            for (const auto &[key, value] : values) {
                set(key, value);
            }
        } catch (...) {
            zyx_driver_params_free(params_, nullptr);
            params_ = nullptr;
            throw;
        }
    }

    ~DriverParams() { zyx_driver_params_free(params_, nullptr); }

    DriverParams(const DriverParams &) = delete;
    DriverParams &operator=(const DriverParams &) = delete;

    zyx_driver_params_t *get() const { return params_; }

private:
    void set(const std::string &key, const JsValue &value) {
        DriverError error;
        DriverValue built = BuildDriverValue(value);
        ThrowIfNotOk(zyx_driver_params_set_value(params_, key.c_str(), built.get(), error.out()), error);
    }

    zyx_driver_params_t *params_ = nullptr;
};

JsValue NapiToValue(const Napi::Value &value) {
    if (value.IsNull() || value.IsUndefined()) {
        return JsValue();
    }
    if (value.IsBoolean()) {
        return JsValue(value.As<Napi::Boolean>().Value());
    }
    if (value.IsNumber()) {
        double num = value.As<Napi::Number>().DoubleValue();
        if (num == std::floor(num) && num >= static_cast<double>(INT64_MIN) && num <= static_cast<double>(INT64_MAX)) {
            return JsValue(static_cast<int64_t>(num));
        }
        return JsValue(num);
    }
    if (value.IsString()) {
        return JsValue(value.As<Napi::String>().Utf8Value());
    }
    if (value.IsArray()) {
        Napi::Array input = value.As<Napi::Array>();
        JsArray values;
        values.reserve(input.Length());
        for (uint32_t i = 0; i < input.Length(); ++i) {
            values.push_back(NapiToValue(input.Get(i)));
        }
        return JsValue(std::move(values));
    }
    if (value.IsObject()) {
        Napi::Object input = value.As<Napi::Object>();
        Napi::Array keys = input.GetPropertyNames();
        JsObject values;
        for (uint32_t i = 0; i < keys.Length(); ++i) {
            std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
            values.emplace(std::move(key), NapiToValue(input.Get(keys.Get(i))));
        }
        return JsValue(std::move(values));
    }
    throw Napi::Error::New(value.Env(), "Cannot convert JavaScript value to Driver ABI parameter");
}

JsObject ObjectToParams(const Napi::Object &object) {
    JsObject params;
    Napi::Array keys = object.GetPropertyNames();
    for (uint32_t i = 0; i < keys.Length(); ++i) {
        std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
        params.emplace(std::move(key), NapiToValue(object.Get(keys.Get(i))));
    }
    return params;
}

struct ResultData {
    bool isSuccess = true;
    std::string error;
    double duration = 0.0;
    std::vector<std::string> columns;
    std::vector<JsObject> rows;
};

class ResultHandle {
public:
    explicit ResultHandle(zyx_driver_result_t *result) : result_(result) {}
    ~ResultHandle() { zyx_driver_result_free(result_); }

    ResultHandle(const ResultHandle &) = delete;
    ResultHandle &operator=(const ResultHandle &) = delete;

    zyx_driver_result_t *get() const { return result_; }

private:
    zyx_driver_result_t *result_ = nullptr;
};

JsObject EntityProperties(zyx_driver_result_t *result, const zyx_driver_value_ref_t &ref) {
    const char *json = nullptr;
    DriverError error;
    ThrowIfNotOk(zyx_driver_value_ref_get_entity_properties_json(result, &ref, &json, error.out()), error);
    return ParsePropertiesJson(json);
}

JsValue ValueRefToJsValue(zyx_driver_result_t *result, const zyx_driver_value_ref_t &ref);

JsValue NodeRefToJsValue(zyx_driver_result_t *result, const zyx_driver_value_ref_t &ref) {
    DriverError error;
    NodeData node;
    uint32_t labelCount = 0;
    ThrowIfNotOk(zyx_driver_value_ref_get_node_id(&ref, &node.id, error.out()), error);
    ThrowIfNotOk(zyx_driver_value_ref_get_node_label_count(&ref, &labelCount, error.out()), error);
    node.labels.reserve(labelCount);
    for (uint32_t i = 0; i < labelCount; ++i) {
        const char *label = nullptr;
        ThrowIfNotOk(zyx_driver_value_ref_get_node_label(result, &ref, i, &label, error.out()), error);
        node.labels.emplace_back(label == nullptr ? "" : label);
    }
    node.properties = EntityProperties(result, ref);
    return JsValue(std::move(node));
}

JsValue EdgeRefToJsValue(zyx_driver_result_t *result, const zyx_driver_value_ref_t &ref) {
    DriverError error;
    EdgeData edge;
    const char *type = nullptr;
    ThrowIfNotOk(zyx_driver_value_ref_get_edge_id(&ref, &edge.id, error.out()), error);
    ThrowIfNotOk(zyx_driver_value_ref_get_edge_source_id(&ref, &edge.sourceId, error.out()), error);
    ThrowIfNotOk(zyx_driver_value_ref_get_edge_target_id(&ref, &edge.targetId, error.out()), error);
    ThrowIfNotOk(zyx_driver_value_ref_get_edge_type(result, &ref, &type, error.out()), error);
    edge.type = type == nullptr ? "" : type;
    edge.properties = EntityProperties(result, ref);
    return JsValue(std::move(edge));
}

JsValue ListRefToJsValue(zyx_driver_result_t *result, const zyx_driver_value_ref_t &ref) {
    DriverError error;
    uint32_t count = 0;
    ThrowIfNotOk(zyx_driver_value_ref_list_count(&ref, &count, error.out()), error);
    JsArray values;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        zyx_driver_value_ref_t item{};
        ThrowIfNotOk(zyx_driver_value_ref_list_get(&ref, i, &item, error.out()), error);
        values.push_back(ValueRefToJsValue(result, item));
    }
    return JsValue(std::move(values));
}

JsValue MapRefToJsValue(zyx_driver_result_t *result, const zyx_driver_value_ref_t &ref) {
    DriverError error;
    uint32_t count = 0;
    ThrowIfNotOk(zyx_driver_value_ref_map_count(&ref, &count, error.out()), error);
    JsObject values;
    for (uint32_t i = 0; i < count; ++i) {
        const char *key = nullptr;
        ThrowIfNotOk(zyx_driver_value_ref_map_key(result, &ref, i, &key, error.out()), error);
        zyx_driver_value_ref_t nested{};
        ThrowIfNotOk(zyx_driver_value_ref_map_get(&ref, key, &nested, error.out()), error);
        values.emplace(key == nullptr ? "" : key, ValueRefToJsValue(result, nested));
    }
    return JsValue(std::move(values));
}

JsValue ValueRefToJsValue(zyx_driver_result_t *result, const zyx_driver_value_ref_t &ref) {
    DriverError error;
    switch (zyx_driver_value_ref_type(&ref)) {
        case ZYX_DRIVER_VALUE_NULL:
            return JsValue();
        case ZYX_DRIVER_VALUE_BOOL: {
            bool value = false;
            ThrowIfNotOk(zyx_driver_value_ref_get_bool(&ref, &value, error.out()), error);
            return JsValue(value);
        }
        case ZYX_DRIVER_VALUE_INT64: {
            int64_t value = 0;
            ThrowIfNotOk(zyx_driver_value_ref_get_int64(&ref, &value, error.out()), error);
            return JsValue(value);
        }
        case ZYX_DRIVER_VALUE_DOUBLE: {
            double value = 0.0;
            ThrowIfNotOk(zyx_driver_value_ref_get_double(&ref, &value, error.out()), error);
            return JsValue(value);
        }
        case ZYX_DRIVER_VALUE_STRING: {
            const char *value = nullptr;
            ThrowIfNotOk(zyx_driver_value_ref_get_string(result, &ref, &value, error.out()), error);
            return JsValue(std::string(value == nullptr ? "" : value));
        }
        case ZYX_DRIVER_VALUE_NODE:
            return NodeRefToJsValue(result, ref);
        case ZYX_DRIVER_VALUE_EDGE:
            return EdgeRefToJsValue(result, ref);
        case ZYX_DRIVER_VALUE_LIST:
            return ListRefToJsValue(result, ref);
        case ZYX_DRIVER_VALUE_MAP:
            return MapRefToJsValue(result, ref);
    }
    throw std::runtime_error("Unsupported Driver ABI result value type");
}

JsValue ValueAt(zyx_driver_result_t *result, uint32_t column) {
    DriverError error;
    zyx_driver_value_ref_t ref{};
    ThrowIfNotOk(zyx_driver_result_get_value(result, column, &ref, error.out()), error);
    return ValueRefToJsValue(result, ref);
}

void CollectResult(zyx_driver_result_t *raw, ResultData &resultData) {
    ResultHandle result(raw);
    const uint32_t colCount = zyx_driver_result_column_count(result.get());
    resultData.columns.reserve(colCount);
    for (uint32_t i = 0; i < colCount; ++i) {
        const char *name = zyx_driver_result_column_name(result.get(), i);
        resultData.columns.emplace_back(name == nullptr ? "" : name);
    }

    for (;;) {
        DriverError error;
        const auto status = zyx_driver_result_next(result.get(), error.out());
        if (status == ZYX_DRIVER_DONE) {
            break;
        }
        if (status != ZYX_DRIVER_ROW) {
            ThrowIfNotOk(status, error);
        }
        JsObject row;
        for (uint32_t i = 0; i < colCount; ++i) {
            row.emplace(resultData.columns[i], ValueAt(result.get(), i));
        }
        resultData.rows.push_back(std::move(row));
    }
}

Napi::Object ResultToNapi(Napi::Env env, const ResultData &resultData) {
    Napi::Object result = Napi::Object::New(env);
    result.Set("isSuccess", Napi::Boolean::New(env, resultData.isSuccess));
    result.Set("error", resultData.error.empty() ? env.Null() : Napi::String::New(env, resultData.error));
    result.Set("duration", Napi::Number::New(env, resultData.duration));

    Napi::Array columns = Napi::Array::New(env, resultData.columns.size());
    for (size_t i = 0; i < resultData.columns.size(); ++i) {
        columns[i] = Napi::String::New(env, resultData.columns[i]);
    }
    result.Set("columns", columns);

    Napi::Array rows = Napi::Array::New(env, resultData.rows.size());
    for (size_t i = 0; i < resultData.rows.size(); ++i) {
        rows[i] = ObjectToNapi(env, resultData.rows[i]);
    }
    result.Set("rows", rows);
    return result;
}

struct DbHandle {
    explicit DbHandle(std::string dbPath) : path(std::move(dbPath)) {}

    ~DbHandle() {
        std::lock_guard<std::mutex> lock(mutex);
        if (db != nullptr) {
            const auto status = zyx_driver_db_close(db, nullptr);
            if (status == ZYX_DRIVER_OK) {
                db = nullptr;
            }
        }
    }

    zyx_driver_db_t *require() const {
        if (db == nullptr) {
            throw std::runtime_error("database is not open");
        }
        return db;
    }

    std::string path;
    zyx_driver_db_t *db = nullptr;
    mutable std::mutex mutex;
};

struct TxnHandle {
    TxnHandle(zyx_driver_txn_t *txnHandle, bool readOnly) : txn(txnHandle), read_only(readOnly) {}

    ~TxnHandle() {
        std::lock_guard<std::mutex> lock(mutex);
        if (txn != nullptr) {
            zyx_driver_txn_close(txn, nullptr);
            txn = nullptr;
        }
    }

    zyx_driver_txn_t *require() const {
        if (txn == nullptr || !active) {
            throw std::runtime_error("Transaction already closed");
        }
        return txn;
    }

    zyx_driver_txn_t *txn = nullptr;
    bool read_only = false;
    bool active = true;
    mutable std::mutex mutex;
};

class ExecuteWorker : public Napi::AsyncWorker {
public:
    ExecuteWorker(Napi::Env env, std::shared_ptr<DbHandle> db, std::string cypher, JsObject params)
        : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)), db_(std::move(db)),
          cypher_(std::move(cypher)), params_(std::move(params)) {}

    Napi::Promise GetPromise() { return deferred_.Promise(); }

protected:
    void Execute() override {
        try {
            DriverParams params(params_);
            zyx_driver_result_t *result = nullptr;
            DriverError error;
            const auto start = std::chrono::steady_clock::now();
            zyx_driver_status_t status = ZYX_DRIVER_OK;
            {
                std::lock_guard<std::mutex> lock(db_->mutex);
                status = zyx_driver_db_execute(db_->require(), cypher_.c_str(), params.get(), &result, error.out());
            }
            const auto end = std::chrono::steady_clock::now();
            resultData_.duration = std::chrono::duration<double, std::milli>(end - start).count();
            if (status != ZYX_DRIVER_OK) {
                resultData_.isSuccess = false;
                resultData_.error = error.message();
                return;
            }
            CollectResult(result, resultData_);
        } catch (const std::exception &e) {
            SetError(e.what());
        }
    }

    void OnOK() override { deferred_.Resolve(ResultToNapi(Env(), resultData_)); }
    void OnError(const Napi::Error &e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<DbHandle> db_;
    std::string cypher_;
    JsObject params_;
    ResultData resultData_;
};

class TxExecuteWorker : public Napi::AsyncWorker {
public:
    TxExecuteWorker(Napi::Env env, std::shared_ptr<TxnHandle> tx, std::string cypher, JsObject params)
        : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)), tx_(std::move(tx)),
          cypher_(std::move(cypher)), params_(std::move(params)) {}

    Napi::Promise GetPromise() { return deferred_.Promise(); }

protected:
    void Execute() override {
        try {
            DriverParams params(params_);
            zyx_driver_result_t *result = nullptr;
            DriverError error;
            const auto start = std::chrono::steady_clock::now();
            zyx_driver_status_t status = ZYX_DRIVER_OK;
            {
                std::lock_guard<std::mutex> lock(tx_->mutex);
                status = zyx_driver_txn_execute(tx_->require(), cypher_.c_str(), params.get(), &result, error.out());
            }
            const auto end = std::chrono::steady_clock::now();
            resultData_.duration = std::chrono::duration<double, std::milli>(end - start).count();
            if (status != ZYX_DRIVER_OK) {
                resultData_.isSuccess = false;
                resultData_.error = error.message();
                return;
            }
            CollectResult(result, resultData_);
        } catch (const std::exception &e) {
            SetError(e.what());
        }
    }

    void OnOK() override { deferred_.Resolve(ResultToNapi(Env(), resultData_)); }
    void OnError(const Napi::Error &e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<TxnHandle> tx_;
    std::string cypher_;
    JsObject params_;
    ResultData resultData_;
};

class VoidWorker : public Napi::AsyncWorker {
public:
    VoidWorker(Napi::Env env, std::function<void()> work)
        : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)), work_(std::move(work)) {}

    Napi::Promise GetPromise() { return deferred_.Promise(); }

protected:
    void Execute() override {
        try {
            work_();
        } catch (const std::exception &e) {
            SetError(e.what());
        }
    }

    void OnOK() override { deferred_.Resolve(Env().Undefined()); }
    void OnError(const Napi::Error &e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::function<void()> work_;
};

template <typename T>
class ValueWorker : public Napi::AsyncWorker {
public:
    ValueWorker(Napi::Env env, std::function<T()> work, std::function<Napi::Value(Napi::Env, const T &)> convert)
        : Napi::AsyncWorker(env), deferred_(Napi::Promise::Deferred::New(env)), work_(std::move(work)),
          convert_(std::move(convert)) {}

    Napi::Promise GetPromise() { return deferred_.Promise(); }

protected:
    void Execute() override {
        try {
            result_ = work_();
        } catch (const std::exception &e) {
            SetError(e.what());
        }
    }

    void OnOK() override { deferred_.Resolve(convert_(Env(), result_)); }
    void OnError(const Napi::Error &e) override { deferred_.Reject(e.Value()); }

private:
    Napi::Promise::Deferred deferred_;
    std::function<T()> work_;
    std::function<Napi::Value(Napi::Env, const T &)> convert_;
    T result_{};
};

Napi::Value IdToNapi(Napi::Env env, const int64_t &id) {
    return Napi::Number::New(env, static_cast<double>(id));
}

Napi::Value IdsToNapi(Napi::Env env, const std::vector<int64_t> &ids) {
    Napi::Array result = Napi::Array::New(env, ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        result[i] = Napi::Number::New(env, static_cast<double>(ids[i]));
    }
    return result;
}

class TransactionWrap : public Napi::ObjectWrap<TransactionWrap> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "Transaction", {
            InstanceMethod("execute", &TransactionWrap::Execute),
            InstanceMethod("commit", &TransactionWrap::Commit),
            InstanceMethod("rollback", &TransactionWrap::Rollback),
            InstanceAccessor("isActive", &TransactionWrap::IsActive, nullptr),
            InstanceAccessor("isReadOnly", &TransactionWrap::IsReadOnly, nullptr),
        });

        constructor_ = Napi::Persistent(func);
        constructor_.SuppressDestruct();
        exports.Set("Transaction", func);
        return exports;
    }

    static Napi::Object NewInstance(Napi::Env env, std::shared_ptr<TxnHandle> tx) {
        Napi::Object obj = constructor_.New({});
        TransactionWrap *wrap = Napi::ObjectWrap<TransactionWrap>::Unwrap(obj);
        wrap->tx_ = std::move(tx);
        return obj;
    }

    explicit TransactionWrap(const Napi::CallbackInfo &info) : Napi::ObjectWrap<TransactionWrap>(info) {}

private:
    Napi::Value Execute(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Expected cypher string").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (!tx_) {
            Napi::Error::New(env, "Transaction already closed").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string cypher = info[0].As<Napi::String>().Utf8Value();
        JsObject params;
        if (info.Length() > 1 && info[1].IsObject()) {
            params = ObjectToParams(info[1].As<Napi::Object>());
        }

        auto worker = new TxExecuteWorker(env, tx_, std::move(cypher), std::move(params));
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value Commit(const Napi::CallbackInfo &info) {
        return Finish(info, true);
    }

    Napi::Value Rollback(const Napi::CallbackInfo &info) {
        return Finish(info, false);
    }

    Napi::Value Finish(const Napi::CallbackInfo &info, bool commit) {
        Napi::Env env = info.Env();
        if (!tx_) {
            Napi::Error::New(env, "Transaction already closed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        auto tx = std::move(tx_);
        auto worker = new VoidWorker(env, [tx, commit]() {
            std::lock_guard<std::mutex> lock(tx->mutex);
            zyx_driver_txn_t *handle = tx->require();
            DriverError error;
            if (commit) {
                ThrowIfNotOk(zyx_driver_txn_commit(handle, error.out()), error);
            } else {
                ThrowIfNotOk(zyx_driver_txn_rollback(handle, error.out()), error);
            }
            tx->active = false;
            ThrowIfNotOk(zyx_driver_txn_close(handle, error.out()), error);
            tx->txn = nullptr;
        });
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value IsActive(const Napi::CallbackInfo &info) {
        if (!tx_) {
            return Napi::Boolean::New(info.Env(), false);
        }
        std::lock_guard<std::mutex> lock(tx_->mutex);
        return Napi::Boolean::New(info.Env(), tx_->txn != nullptr && tx_->active);
    }

    Napi::Value IsReadOnly(const Napi::CallbackInfo &info) {
        return Napi::Boolean::New(info.Env(), tx_ && tx_->read_only);
    }

    std::shared_ptr<TxnHandle> tx_;
    static Napi::FunctionReference constructor_;
};

Napi::FunctionReference TransactionWrap::constructor_;

class DatabaseWrap : public Napi::ObjectWrap<DatabaseWrap> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        Napi::Function func = DefineClass(env, "Database", {
            InstanceMethod("open", &DatabaseWrap::Open),
            InstanceMethod("close", &DatabaseWrap::Close),
            InstanceMethod("save", &DatabaseWrap::Save),
            InstanceMethod("execute", &DatabaseWrap::Execute),
            InstanceMethod("beginTransaction", &DatabaseWrap::BeginTransaction),
            InstanceMethod("beginReadOnlyTransaction", &DatabaseWrap::BeginReadOnlyTransaction),
            InstanceMethod("createNode", &DatabaseWrap::CreateNode),
            InstanceMethod("createNodes", &DatabaseWrap::CreateNodes),
            InstanceMethod("createEdge", &DatabaseWrap::CreateEdge),
            InstanceMethod("createEdges", &DatabaseWrap::CreateEdges),
            InstanceMethod("getShortestPath", &DatabaseWrap::GetShortestPath),
            InstanceAccessor("hasActiveTransaction", &DatabaseWrap::HasActiveTransaction, nullptr),
        });

        constructor_ = Napi::Persistent(func);
        constructor_.SuppressDestruct();
        exports.Set("Database", func);
        return exports;
    }

    explicit DatabaseWrap(const Napi::CallbackInfo &info) : Napi::ObjectWrap<DatabaseWrap>(info) {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Expected database path string").ThrowAsJavaScriptException();
            return;
        }
        db_ = std::make_shared<DbHandle>(info[0].As<Napi::String>().Utf8Value());
    }

private:
    Napi::Value Open(const Napi::CallbackInfo &info) {
        auto db = db_;
        auto worker = new VoidWorker(info.Env(), [db]() {
            std::lock_guard<std::mutex> lock(db->mutex);
            if (db->db != nullptr) {
                DriverError closeError;
                ThrowIfNotOk(zyx_driver_db_close(db->db, closeError.out()), closeError);
                db->db = nullptr;
            }
            DriverError error;
            ThrowIfNotOk(zyx_driver_db_open(db->path.c_str(), &db->db, error.out()), error);
        });
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value Close(const Napi::CallbackInfo &info) {
        auto db = db_;
        auto worker = new VoidWorker(info.Env(), [db]() {
            std::lock_guard<std::mutex> lock(db->mutex);
            if (db->db == nullptr) {
                return;
            }
            DriverError error;
            ThrowIfNotOk(zyx_driver_db_close(db->db, error.out()), error);
            db->db = nullptr;
        });
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value Save(const Napi::CallbackInfo &info) {
        auto db = db_;
        auto worker = new VoidWorker(info.Env(), [db]() {
            std::lock_guard<std::mutex> lock(db->mutex);
            DriverError error;
            ThrowIfNotOk(zyx_driver_db_save(db->require(), error.out()), error);
        });
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value Execute(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Expected cypher string").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string cypher = info[0].As<Napi::String>().Utf8Value();
        JsObject params;
        if (info.Length() > 1 && info[1].IsObject()) {
            params = ObjectToParams(info[1].As<Napi::Object>());
        }

        auto worker = new ExecuteWorker(env, db_, std::move(cypher), std::move(params));
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value BeginTransaction(const Napi::CallbackInfo &info) {
        return Begin(info, false);
    }

    Napi::Value BeginReadOnlyTransaction(const Napi::CallbackInfo &info) {
        return Begin(info, true);
    }

    Napi::Value Begin(const Napi::CallbackInfo &info, bool readOnly) {
        Napi::Env env = info.Env();
        try {
            zyx_driver_txn_t *txn = nullptr;
            DriverError error;
            {
                std::lock_guard<std::mutex> lock(db_->mutex);
                const auto status = readOnly
                    ? zyx_driver_txn_begin_read_only(db_->require(), &txn, error.out())
                    : zyx_driver_txn_begin(db_->require(), &txn, error.out());
                ThrowIfNotOk(status, error);
            }
            return TransactionWrap::NewInstance(env, std::make_shared<TxnHandle>(txn, readOnly));
        } catch (const std::exception &e) {
            Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    Napi::Value CreateNode(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 1) {
            Napi::TypeError::New(env, "Expected label argument").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        JsObject props;
        if (info.Length() > 1 && info[1].IsObject()) {
            props = ObjectToParams(info[1].As<Napi::Object>());
        }

        auto db = db_;
        if (info[0].IsArray()) {
            Napi::Array input = info[0].As<Napi::Array>();
            std::vector<std::string> labels;
            labels.reserve(input.Length());
            for (uint32_t i = 0; i < input.Length(); ++i) {
                labels.push_back(input.Get(i).As<Napi::String>().Utf8Value());
            }
            auto worker = new ValueWorker<int64_t>(env, [db, labels, props]() {
                DriverParams params(props);
                std::vector<const char *> labelPtrs;
                labelPtrs.reserve(labels.size());
                for (const auto &label : labels) {
                    labelPtrs.push_back(label.c_str());
                }
                int64_t id = 0;
                DriverError error;
                std::lock_guard<std::mutex> lock(db->mutex);
                ThrowIfNotOk(zyx_driver_db_create_node_with_labels(db->require(), labelPtrs.data(), static_cast<uint32_t>(labelPtrs.size()), params.get(), &id, error.out()), error);
                return id;
            }, IdToNapi);
            worker->Queue();
            return worker->GetPromise();
        }

        if (!info[0].IsString()) {
            Napi::TypeError::New(env, "Expected label string or string array").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string label = info[0].As<Napi::String>().Utf8Value();
        auto worker = new ValueWorker<int64_t>(env, [db, label, props]() {
            DriverParams params(props);
            int64_t id = 0;
            DriverError error;
            std::lock_guard<std::mutex> lock(db->mutex);
            ThrowIfNotOk(zyx_driver_db_create_node(db->require(), label.c_str(), params.get(), &id, error.out()), error);
            return id;
        }, IdToNapi);
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value CreateNodes(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
            Napi::TypeError::New(env, "Expected (label, propsArray)").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string label = info[0].As<Napi::String>().Utf8Value();
        Napi::Array input = info[1].As<Napi::Array>();
        std::vector<JsObject> propsList;
        propsList.reserve(input.Length());
        for (uint32_t i = 0; i < input.Length(); ++i) {
            propsList.push_back(ObjectToParams(input.Get(i).As<Napi::Object>()));
        }

        auto db = db_;
        auto worker = new ValueWorker<std::vector<int64_t>>(env, [db, label, propsList]() {
            std::vector<int64_t> ids;
            ids.reserve(propsList.size());
            std::lock_guard<std::mutex> lock(db->mutex);
            for (const auto &props : propsList) {
                DriverParams params(props);
                int64_t id = 0;
                DriverError error;
                ThrowIfNotOk(zyx_driver_db_create_node(db->require(), label.c_str(), params.get(), &id, error.out()), error);
                ids.push_back(id);
            }
            return ids;
        }, IdsToNapi);
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value CreateEdge(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 3) {
            Napi::TypeError::New(env, "Expected (srcId, dstId, edgeType, [props])").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        int64_t srcId = info[0].As<Napi::Number>().Int64Value();
        int64_t dstId = info[1].As<Napi::Number>().Int64Value();
        std::string edgeType = info[2].As<Napi::String>().Utf8Value();
        JsObject props;
        if (info.Length() > 3 && info[3].IsObject()) {
            props = ObjectToParams(info[3].As<Napi::Object>());
        }

        auto db = db_;
        auto worker = new ValueWorker<int64_t>(env, [db, srcId, dstId, edgeType, props]() {
            DriverParams params(props);
            int64_t id = 0;
            DriverError error;
            std::lock_guard<std::mutex> lock(db->mutex);
            ThrowIfNotOk(zyx_driver_db_create_edge(db->require(), srcId, dstId, edgeType.c_str(), params.get(), &id, error.out()), error);
            return id;
        }, IdToNapi);
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value CreateEdges(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
            Napi::TypeError::New(env, "Expected (edgeType, edgesArray)").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        struct EdgeInput {
            int64_t src = 0;
            int64_t dst = 0;
            JsObject props;
        };

        std::string edgeType = info[0].As<Napi::String>().Utf8Value();
        Napi::Array input = info[1].As<Napi::Array>();
        std::vector<EdgeInput> edges;
        edges.reserve(input.Length());
        for (uint32_t i = 0; i < input.Length(); ++i) {
            Napi::Array tuple = input.Get(i).As<Napi::Array>();
            EdgeInput edge;
            edge.src = tuple.Get(static_cast<uint32_t>(0)).As<Napi::Number>().Int64Value();
            edge.dst = tuple.Get(static_cast<uint32_t>(1)).As<Napi::Number>().Int64Value();
            if (tuple.Length() > 2 && tuple.Get(static_cast<uint32_t>(2)).IsObject()) {
                edge.props = ObjectToParams(tuple.Get(static_cast<uint32_t>(2)).As<Napi::Object>());
            }
            edges.push_back(std::move(edge));
        }

        auto db = db_;
        auto worker = new ValueWorker<std::vector<int64_t>>(env, [db, edgeType, edges]() {
            std::vector<int64_t> ids;
            ids.reserve(edges.size());
            std::lock_guard<std::mutex> lock(db->mutex);
            for (const auto &edge : edges) {
                DriverParams params(edge.props);
                int64_t id = 0;
                DriverError error;
                ThrowIfNotOk(zyx_driver_db_create_edge(db->require(), edge.src, edge.dst, edgeType.c_str(), params.get(), &id, error.out()), error);
                ids.push_back(id);
            }
            return ids;
        }, IdsToNapi);
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value GetShortestPath(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 2) {
            Napi::TypeError::New(env, "Expected (startId, endId, [maxDepth])").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        int64_t startId = info[0].As<Napi::Number>().Int64Value();
        int64_t endId = info[1].As<Napi::Number>().Int64Value();
        int maxDepth = 15;
        if (info.Length() > 2 && info[2].IsNumber()) {
            maxDepth = info[2].As<Napi::Number>().Int32Value();
        }
        auto db = db_;
        auto worker = new ValueWorker<std::vector<JsValue>>(env, [db, startId, endId, maxDepth]() {
            ResultData resultData;
            zyx_driver_result_t *result = nullptr;
            DriverError error;
            const std::string cypher = "CALL algo.shortestPath(" + std::to_string(startId) + ", " + std::to_string(endId) + ", " + std::to_string(maxDepth) + ")";
            {
                std::lock_guard<std::mutex> lock(db->mutex);
                ThrowIfNotOk(zyx_driver_db_execute(db->require(), cypher.c_str(), nullptr, &result, error.out()), error);
            }
            CollectResult(result, resultData);
            std::vector<JsValue> path;
            for (const auto &row : resultData.rows) {
                auto it = row.find("node");
                if (it != row.end()) {
                    path.push_back(it->second);
                }
            }
            return path;
        }, [](Napi::Env e, const std::vector<JsValue> &path) -> Napi::Value {
            Napi::Array result = Napi::Array::New(e, path.size());
            for (size_t i = 0; i < path.size(); ++i) {
                result[i] = ValueToNapi(e, path[i]);
            }
            return result;
        });
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value HasActiveTransaction(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        try {
            bool active = false;
            DriverError error;
            std::lock_guard<std::mutex> lock(db_->mutex);
            ThrowIfNotOk(zyx_driver_db_has_active_transaction(db_->require(), &active, error.out()), error);
            return Napi::Boolean::New(env, active);
        } catch (const std::exception &e) {
            Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    std::shared_ptr<DbHandle> db_;
    static Napi::FunctionReference constructor_;
};

Napi::FunctionReference DatabaseWrap::constructor_;

} // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    DatabaseWrap::Init(env, exports);
    TransactionWrap::Init(env, exports);
    return exports;
}

NODE_API_MODULE(zyxdb, Init)
