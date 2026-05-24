#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include "zyx/zyx_driver_abi.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

class DriverError {
public:
    ~DriverError() { zyx_driver_error_free(error_); }

    DriverError(const DriverError &) = delete;
    DriverError &operator=(const DriverError &) = delete;

    DriverError() = default;

    zyx_driver_error_t **out() {
        zyx_driver_error_free(error_);
        error_ = nullptr;
        return &error_;
    }

    std::string message() const {
        const char *message = zyx_driver_error_message(error_);
        return message == nullptr ? std::string() : std::string(message);
    }

private:
    zyx_driver_error_t *error_ = nullptr;
};

void throwIfNotOk(zyx_driver_status_t status, DriverError &error) {
    if (status != ZYX_DRIVER_OK) {
        throw std::runtime_error(error.message());
    }
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

DriverValue buildDriverValue(const py::handle &value) {
    DriverError error;
    zyx_driver_value_t *raw = nullptr;
    if (value.is_none()) {
        throwIfNotOk(zyx_driver_value_null_create(&raw, error.out()), error);
    } else if (py::isinstance<py::bool_>(value)) {
        throwIfNotOk(zyx_driver_value_bool_create(value.cast<bool>(), &raw, error.out()), error);
    } else if (py::isinstance<py::int_>(value)) {
        throwIfNotOk(zyx_driver_value_int64_create(value.cast<int64_t>(), &raw, error.out()), error);
    } else if (py::isinstance<py::float_>(value)) {
        throwIfNotOk(zyx_driver_value_double_create(value.cast<double>(), &raw, error.out()), error);
    } else if (py::isinstance<py::str>(value)) {
        const std::string text = value.cast<std::string>();
        throwIfNotOk(zyx_driver_value_string_create(text.c_str(), &raw, error.out()), error);
    } else if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
        throwIfNotOk(zyx_driver_value_list_create(&raw, error.out()), error);
        DriverValue list(raw);
        for (const auto &item : py::reinterpret_borrow<py::iterable>(value)) {
            DriverValue nested = buildDriverValue(item);
            throwIfNotOk(zyx_driver_value_list_append_value(list.get(), nested.get(), error.out()), error);
        }
        return list;
    } else if (py::isinstance<py::dict>(value)) {
        throwIfNotOk(zyx_driver_value_map_create(&raw, error.out()), error);
        DriverValue map(raw);
        for (const auto &[key, nested] : py::reinterpret_borrow<py::dict>(value)) {
            const std::string keyText = py::str(key).cast<std::string>();
            DriverValue nestedValue = buildDriverValue(nested);
            throwIfNotOk(zyx_driver_value_map_set_value(map.get(), keyText.c_str(), nestedValue.get(), error.out()), error);
        }
        return map;
    } else {
        throw py::type_error("Unsupported parameter type");
    }
    return DriverValue(raw);
}

class DriverParams {
public:
    explicit DriverParams(const py::dict &values) {
        DriverError error;
        throwIfNotOk(zyx_driver_params_create(&params_, error.out()), error);
        try {
            for (const auto &[key, value] : values) {
                set(key.cast<std::string>(), value);
            }
        } catch (...) {
            zyx_driver_params_free(params_, nullptr);
            params_ = nullptr;
            throw;
        }
    }

    explicit DriverParams(const py::kwargs &values) {
        DriverError error;
        throwIfNotOk(zyx_driver_params_create(&params_, error.out()), error);
        try {
            for (const auto &[key, value] : values) {
                set(key.cast<std::string>(), value);
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
    void set(const std::string &key, const py::handle &value) {
        DriverError error;
        DriverValue built = buildDriverValue(value);
        throwIfNotOk(zyx_driver_params_set_value(params_, key.c_str(), built.get(), error.out()), error);
    }

    zyx_driver_params_t *params_ = nullptr;
};

class PyResult {
public:
    PyResult() = default;

    PyResult(zyx_driver_result_t *result, double duration_ms) : result_(result), duration_ms_(duration_ms) {}

    PyResult(std::string error, double duration_ms)
        : success_(false), error_(std::move(error)), duration_ms_(duration_ms) {}

    ~PyResult() { zyx_driver_result_free(result_); }

    PyResult(const PyResult &) = delete;
    PyResult &operator=(const PyResult &) = delete;

    bool has_next() {
        if (result_ == nullptr || !success_) {
            return false;
        }
        if (peeked_row_) {
            return true;
        }
        return fetchNextRow(false);
    }

    void next() {
        if (peeked_row_) {
            peeked_row_ = false;
            row_active_ = true;
            return;
        }
        if (!fetchNextRow(true)) {
            throw py::stop_iteration();
        }
    }

    py::object get_by_index(uint32_t index) { return valueAt(index); }

    py::object get(const std::string &key) {
        const auto names = column_names();
        for (uint32_t i = 0; i < static_cast<uint32_t>(names.size()); ++i) {
            if (names[i].cast<std::string>() == key) {
                return valueAt(i);
            }
        }
        throw py::key_error(key);
    }

    uint32_t column_count() const {
        if (result_ == nullptr) {
            return 0;
        }
        return zyx_driver_result_column_count(result_);
    }

    const char *column_name(uint32_t index) { return zyx_driver_result_column_name(result_, index); }

    py::list column_names() const {
        py::list names;
        if (result_ == nullptr) {
            return names;
        }
        const uint32_t count = zyx_driver_result_column_count(result_);
        for (uint32_t i = 0; i < count; ++i) {
            const char *name = zyx_driver_result_column_name(result_, i);
            names.append(name == nullptr ? py::str("") : py::str(name));
        }
        return names;
    }

    double duration() const { return duration_ms_; }

    bool is_success() const { return success_; }

    py::object error() const {
        if (success_) {
            return py::none();
        }
        return py::str(error_);
    }

    py::dict row_dict() {
        py::dict row;
        if (result_ == nullptr) {
            return row;
        }
        const uint32_t count = zyx_driver_result_column_count(result_);
        for (uint32_t i = 0; i < count; ++i) {
            const char *name = zyx_driver_result_column_name(result_, i);
            row[py::str(name == nullptr ? "" : name)] = valueAt(i);
        }
        return row;
    }

    PyResult &iter() { return *this; }

    py::dict next_row() {
        next();
        return row_dict();
    }

private:
    bool fetchNextRow(bool activate) {
        if (result_ == nullptr || !success_) {
            return false;
        }
        DriverError error;
        const auto status = zyx_driver_result_next(result_, error.out());
        if (status == ZYX_DRIVER_DONE) {
            zyx_driver_result_free(result_);
            result_ = nullptr;
            row_active_ = false;
            peeked_row_ = false;
            return false;
        }
        if (status != ZYX_DRIVER_ROW) {
            throwIfNotOk(status, error);
        }
        row_active_ = activate;
        peeked_row_ = !activate;
        return true;
    }

    [[noreturn]] static void throwUnsupportedValue(zyx_driver_value_type_t type, const char *context) {
        throw std::runtime_error(std::string("Unsupported Driver ABI ") + context + " value type: " + valueTypeName(type));
    }

    static const char *valueTypeName(zyx_driver_value_type_t type) {
        switch (type) {
            case ZYX_DRIVER_VALUE_NULL: return "null";
            case ZYX_DRIVER_VALUE_BOOL: return "bool";
            case ZYX_DRIVER_VALUE_INT64: return "int64";
            case ZYX_DRIVER_VALUE_DOUBLE: return "double";
            case ZYX_DRIVER_VALUE_STRING: return "string";
            case ZYX_DRIVER_VALUE_NODE: return "node";
            case ZYX_DRIVER_VALUE_EDGE: return "edge";
            case ZYX_DRIVER_VALUE_LIST: return "list";
            case ZYX_DRIVER_VALUE_MAP: return "map";
        }
        return "unknown";
    }

    py::object valueAt(uint32_t column) {
        if (result_ == nullptr) {
            throw std::runtime_error("result is not active");
        }
        if (!row_active_) {
            throw std::runtime_error("result row is not active");
        }
        DriverError error;
        zyx_driver_value_ref_t ref{};
        throwIfNotOk(zyx_driver_result_get_value(result_, column, &ref, error.out()), error);
        return valueRefToPy(ref);
    }

    py::dict nodeAt(uint32_t column) {
        DriverError error;
        int64_t id = 0;
        uint32_t labelCount = 0;
        throwIfNotOk(zyx_driver_result_get_node_id(result_, column, &id, error.out()), error);
        throwIfNotOk(zyx_driver_result_get_node_label_count(result_, column, &labelCount, error.out()), error);
        py::list labels;
        for (uint32_t i = 0; i < labelCount; ++i) {
            const char *label = nullptr;
            throwIfNotOk(zyx_driver_result_get_node_label(result_, column, i, &label, error.out()), error);
            labels.append(py::str(label == nullptr ? "" : label));
        }
        py::dict node;
        node["id"] = py::int_(id);
        node["labels"] = labels;
        node["label"] = labelCount == 0 ? py::str("") : labels[0];
        node["properties"] = propertiesAt(column);
        return node;
    }

    py::dict edgeAt(uint32_t column) {
        DriverError error;
        int64_t id = 0;
        int64_t sourceId = 0;
        int64_t targetId = 0;
        const char *type = nullptr;
        throwIfNotOk(zyx_driver_result_get_edge_id(result_, column, &id, error.out()), error);
        throwIfNotOk(zyx_driver_result_get_edge_source_id(result_, column, &sourceId, error.out()), error);
        throwIfNotOk(zyx_driver_result_get_edge_target_id(result_, column, &targetId, error.out()), error);
        throwIfNotOk(zyx_driver_result_get_edge_type(result_, column, &type, error.out()), error);
        py::dict edge;
        edge["id"] = py::int_(id);
        edge["source_id"] = py::int_(sourceId);
        edge["target_id"] = py::int_(targetId);
        edge["type"] = py::str(type == nullptr ? "" : type);
        edge["properties"] = propertiesAt(column);
        return edge;
    }

    py::object propertiesAt(uint32_t column) {
        DriverError error;
        const char *json = nullptr;
        throwIfNotOk(zyx_driver_result_get_entity_properties_json(result_, column, &json, error.out()), error);
        py::module_ jsonModule = py::module_::import("json");
        return jsonModule.attr("loads")(json == nullptr ? "{}" : json);
    }

    py::object valueRefToPy(const zyx_driver_value_ref_t &ref) {
        DriverError error;
        switch (zyx_driver_value_ref_type(&ref)) {
            case ZYX_DRIVER_VALUE_NULL:
                return py::none();
            case ZYX_DRIVER_VALUE_BOOL: {
                bool value = false;
                throwIfNotOk(zyx_driver_value_ref_get_bool(&ref, &value, error.out()), error);
                return py::bool_(value);
            }
            case ZYX_DRIVER_VALUE_INT64: {
                int64_t value = 0;
                throwIfNotOk(zyx_driver_value_ref_get_int64(&ref, &value, error.out()), error);
                return py::int_(value);
            }
            case ZYX_DRIVER_VALUE_DOUBLE: {
                double value = 0.0;
                throwIfNotOk(zyx_driver_value_ref_get_double(&ref, &value, error.out()), error);
                return py::float_(value);
            }
            case ZYX_DRIVER_VALUE_STRING: {
                const char *value = nullptr;
                throwIfNotOk(zyx_driver_value_ref_get_string(result_, &ref, &value, error.out()), error);
                return py::str(value == nullptr ? "" : value);
            }
            case ZYX_DRIVER_VALUE_NODE:
                return nodeRefToPy(ref);
            case ZYX_DRIVER_VALUE_EDGE:
                return edgeRefToPy(ref);
            case ZYX_DRIVER_VALUE_LIST:
                return listRefToPy(ref);
            case ZYX_DRIVER_VALUE_MAP:
                return mapRefToPy(ref);
        }
        throwUnsupportedValue(zyx_driver_value_ref_type(&ref), "result");
    }

    py::dict nodeRefToPy(const zyx_driver_value_ref_t &ref) {
        DriverError error;
        int64_t id = 0;
        uint32_t labelCount = 0;
        throwIfNotOk(zyx_driver_value_ref_get_node_id(&ref, &id, error.out()), error);
        throwIfNotOk(zyx_driver_value_ref_get_node_label_count(&ref, &labelCount, error.out()), error);
        py::list labels;
        for (uint32_t i = 0; i < labelCount; ++i) {
            const char *label = nullptr;
            throwIfNotOk(zyx_driver_value_ref_get_node_label(result_, &ref, i, &label, error.out()), error);
            labels.append(py::str(label == nullptr ? "" : label));
        }
        py::dict node;
        node["id"] = py::int_(id);
        node["labels"] = labels;
        node["label"] = labelCount == 0 ? py::str("") : labels[0];
        node["properties"] = propertiesRefToPy(ref);
        return node;
    }

    py::dict edgeRefToPy(const zyx_driver_value_ref_t &ref) {
        DriverError error;
        int64_t id = 0;
        int64_t sourceId = 0;
        int64_t targetId = 0;
        const char *type = nullptr;
        throwIfNotOk(zyx_driver_value_ref_get_edge_id(&ref, &id, error.out()), error);
        throwIfNotOk(zyx_driver_value_ref_get_edge_source_id(&ref, &sourceId, error.out()), error);
        throwIfNotOk(zyx_driver_value_ref_get_edge_target_id(&ref, &targetId, error.out()), error);
        throwIfNotOk(zyx_driver_value_ref_get_edge_type(result_, &ref, &type, error.out()), error);
        py::dict edge;
        edge["id"] = py::int_(id);
        edge["source_id"] = py::int_(sourceId);
        edge["target_id"] = py::int_(targetId);
        edge["type"] = py::str(type == nullptr ? "" : type);
        edge["properties"] = propertiesRefToPy(ref);
        return edge;
    }

    py::object propertiesRefToPy(const zyx_driver_value_ref_t &ref) {
        DriverError error;
        const char *json = nullptr;
        throwIfNotOk(zyx_driver_value_ref_get_entity_properties_json(result_, &ref, &json, error.out()), error);
        py::module_ jsonModule = py::module_::import("json");
        return jsonModule.attr("loads")(json == nullptr ? "{}" : json);
    }

    py::list listRefToPy(const zyx_driver_value_ref_t &ref) {
        DriverError error;
        uint32_t count = 0;
        throwIfNotOk(zyx_driver_value_ref_list_count(&ref, &count, error.out()), error);
        py::list list;
        for (uint32_t i = 0; i < count; ++i) {
            zyx_driver_value_ref_t item{};
            throwIfNotOk(zyx_driver_value_ref_list_get(&ref, i, &item, error.out()), error);
            list.append(valueRefToPy(item));
        }
        return list;
    }

    py::dict mapRefToPy(const zyx_driver_value_ref_t &ref) {
        DriverError error;
        uint32_t count = 0;
        throwIfNotOk(zyx_driver_value_ref_map_count(&ref, &count, error.out()), error);
        py::dict map;
        for (uint32_t i = 0; i < count; ++i) {
            const char *key = nullptr;
            throwIfNotOk(zyx_driver_value_ref_map_key(result_, &ref, i, &key, error.out()), error);
            zyx_driver_value_ref_t nested{};
            throwIfNotOk(zyx_driver_value_ref_map_get(&ref, key, &nested, error.out()), error);
            map[py::str(key == nullptr ? "" : key)] = valueRefToPy(nested);
        }
        return map;
    }

    zyx_driver_result_t *result_ = nullptr;
    bool success_ = true;
    bool row_active_ = false;
    bool peeked_row_ = false;
    std::string error_;
    double duration_ms_ = 0.0;
};

class PyTransaction {
public:
    PyTransaction(zyx_driver_txn_t *txn, bool read_only) : txn_(txn), read_only_(read_only) {}

    ~PyTransaction() { zyx_driver_txn_close(txn_, nullptr); }

    PyTransaction(const PyTransaction &) = delete;
    PyTransaction &operator=(const PyTransaction &) = delete;

    std::unique_ptr<PyResult> execute(const std::string &cypher, py::kwargs kwargs) {
        if (txn_ == nullptr || !active_) {
            throw std::runtime_error("Transaction already closed");
        }

        DriverParams params(kwargs);
        zyx_driver_result_t *result = nullptr;
        DriverError error;
        const auto start = std::chrono::steady_clock::now();
        const auto status = zyx_driver_txn_execute(txn_, cypher.c_str(), params.get(), &result, error.out());
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        if (status != ZYX_DRIVER_OK) {
            return std::make_unique<PyResult>(error.message(), ms);
        }
        return std::make_unique<PyResult>(result, ms);
    }

    void commit() {
        if (txn_ == nullptr || !active_) {
            throw std::runtime_error("Transaction already closed");
        }

        DriverError error;
        throwIfNotOk(zyx_driver_txn_commit(txn_, error.out()), error);
        closeFinalizedHandle();
    }

    void rollback() {
        if (txn_ == nullptr || !active_) {
            throw std::runtime_error("Transaction already closed");
        }

        DriverError error;
        throwIfNotOk(zyx_driver_txn_rollback(txn_, error.out()), error);
        closeFinalizedHandle();
    }

    bool is_active() const { return active_; }
    bool is_read_only() const { return read_only_; }

private:
    void closeFinalizedHandle() {
        zyx_driver_txn_close(txn_, nullptr);
        txn_ = nullptr;
        active_ = false;
    }

    zyx_driver_txn_t *txn_ = nullptr;
    bool read_only_ = false;
    bool active_ = true;
};

class PyDatabase {
public:
    explicit PyDatabase(std::string path) : path_(std::move(path)) {}

    ~PyDatabase() { closeBestEffort(); }

    PyDatabase(const PyDatabase &) = delete;
    PyDatabase &operator=(const PyDatabase &) = delete;

    void open() {
        close();
        DriverError error;
        throwIfNotOk(zyx_driver_db_open(path_.c_str(), &db_, error.out()), error);
    }

    bool open_if_exists() {
        close();
        DriverError error;
        const auto status = zyx_driver_db_open_if_exists(path_.c_str(), &db_, error.out());
        if (status == ZYX_DRIVER_NOT_FOUND) {
            return false;
        }
        throwIfNotOk(status, error);
        return true;
    }

    void close() {
        if (db_ == nullptr) {
            return;
        }
        DriverError error;
        throwIfNotOk(closeHandle(error.out()), error);
        db_ = nullptr;
    }

    void save() {
        DriverError error;
        throwIfNotOk(zyx_driver_db_save(requireDb(), error.out()), error);
    }

    std::unique_ptr<PyTransaction> begin_transaction() {
        zyx_driver_txn_t *txn = nullptr;
        DriverError error;
        throwIfNotOk(zyx_driver_txn_begin(requireDb(), &txn, error.out()), error);
        return std::make_unique<PyTransaction>(txn, false);
    }

    std::unique_ptr<PyTransaction> begin_read_only_transaction() {
        zyx_driver_txn_t *txn = nullptr;
        DriverError error;
        throwIfNotOk(zyx_driver_txn_begin_read_only(requireDb(), &txn, error.out()), error);
        return std::make_unique<PyTransaction>(txn, true);
    }

    bool has_active_transaction() {
        bool active = false;
        DriverError error;
        throwIfNotOk(zyx_driver_db_has_active_transaction(requireDb(), &active, error.out()), error);
        return active;
    }

    void set_thread_pool_size(uint32_t size) {
        DriverError error;
        throwIfNotOk(zyx_driver_db_set_thread_pool_size(requireDb(), size, error.out()), error);
    }

    std::unique_ptr<PyResult> execute(const std::string &cypher, py::kwargs kwargs) {
        DriverParams params(kwargs);
        zyx_driver_result_t *result = nullptr;
        DriverError error;
        const auto start = std::chrono::steady_clock::now();
        const auto status = zyx_driver_db_execute(requireDb(), cypher.c_str(), params.get(), &result, error.out());
        const auto end = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        if (status != ZYX_DRIVER_OK) {
            return std::make_unique<PyResult>(error.message(), ms);
        }
        return std::make_unique<PyResult>(result, ms);
    }

    int64_t create_node(const py::object &label, const py::dict &props) {
        DriverParams params(props);
        int64_t id = 0;
        DriverError error;
        zyx_driver_status_t status = ZYX_DRIVER_OK;
        if (py::isinstance<py::list>(label)) {
            auto values = label.cast<py::list>();
            std::vector<std::string> labels;
            labels.reserve(values.size());
            for (const auto &item : values) {
                labels.push_back(item.cast<std::string>());
            }

            std::vector<const char *> labelPointers;
            labelPointers.reserve(labels.size());
            for (const auto &item : labels) {
                labelPointers.push_back(item.c_str());
            }
            status = zyx_driver_db_create_node_with_labels(requireDb(), labelPointers.data(),
                                                           static_cast<uint32_t>(labelPointers.size()), params.get(),
                                                           &id, error.out());
        } else {
            const auto labelText = label.cast<std::string>();
            status = zyx_driver_db_create_node(requireDb(), labelText.c_str(), params.get(), &id, error.out());
        }
        throwIfNotOk(status, error);
        return id;
    }

    std::vector<int64_t> create_nodes(const std::string &label, const py::list &props_list) {
        std::vector<int64_t> ids;
        ids.reserve(props_list.size());
        for (const auto &item : props_list) {
            ids.push_back(create_node(py::str(label), item.cast<py::dict>()));
        }
        return ids;
    }

    int64_t create_edge(int64_t source_id, int64_t target_id, const std::string &edge_type, const py::dict &props) {
        DriverParams params(props);
        int64_t id = 0;
        DriverError error;
        throwIfNotOk(zyx_driver_db_create_edge(requireDb(), source_id, target_id, edge_type.c_str(), params.get(), &id,
                                               error.out()),
                     error);
        return id;
    }

    std::vector<int64_t> create_edges(const std::string &edge_type, const py::list &edges) {
        std::vector<int64_t> ids;
        ids.reserve(edges.size());
        for (const auto &item : edges) {
            auto tuple = item.cast<py::tuple>();
            py::dict props;
            if (tuple.size() > 2 && !tuple[2].is_none()) {
                props = tuple[2].cast<py::dict>();
            }
            ids.push_back(create_edge(tuple[0].cast<int64_t>(), tuple[1].cast<int64_t>(), edge_type, props));
        }
        return ids;
    }

    py::list get_shortest_path(int64_t start_id, int64_t end_id, int max_depth) {
        py::kwargs kwargs;
        auto result = execute("CALL algo.shortestPath(" + std::to_string(start_id) + ", " + std::to_string(end_id) +
                                      ", " + std::to_string(max_depth) + ")",
                              kwargs);
        py::list path;
        for (;;) {
            try {
                result->next();
            } catch (const py::stop_iteration &) {
                break;
            }
            path.append(result->get("node"));
        }
        return path;
    }

    void bfs(int64_t start_id, const py::function &visitor) {
        py::kwargs kwargs;
        auto result = execute("MATCH (n) RETURN n", kwargs);
        bool startSeen = false;
        for (;;) {
            try {
                result->next();
            } catch (const py::stop_iteration &) {
                break;
            }
            py::dict node = result->get("n").cast<py::dict>();
            if (!startSeen && node[py::str("id")].cast<int64_t>() != start_id) {
                continue;
            }
            startSeen = true;
            if (!visitor(node).cast<bool>()) {
                break;
            }
        }
    }

private:
    zyx_driver_status_t closeHandle(zyx_driver_error_t **out_error) {
        return zyx_driver_db_close(db_, out_error);
    }

    void closeBestEffort() noexcept {
        if (db_ == nullptr) {
            return;
        }
        const auto status = closeHandle(nullptr);
        if (status == ZYX_DRIVER_OK) {
            db_ = nullptr;
        }
    }

    zyx_driver_db_t *requireDb() const {
        if (db_ == nullptr) {
            throw std::runtime_error("database is not open");
        }
        return db_;
    }

    std::string path_;
    zyx_driver_db_t *db_ = nullptr;
};

} // namespace

PYBIND11_MODULE(_core, m) {
    m.doc() = "ZYX graph database engine - Driver ABI core bindings";

    static py::exception<std::runtime_error> DatabaseError(m, "DatabaseError");

    py::register_exception_translator([](std::exception_ptr p) {
        try {
            if (p) std::rethrow_exception(p);
        } catch (const py::builtin_exception &) {
            throw;
        } catch (const std::runtime_error &e) {
            py::set_error(DatabaseError, e.what());
        }
    });

    py::class_<PyResult>(m, "Result")
        .def("has_next", &PyResult::has_next)
        .def("next", &PyResult::next)
        .def("get", &PyResult::get)
        .def("get_by_index", &PyResult::get_by_index)
        .def_property_readonly("column_count", &PyResult::column_count)
        .def("column_name", &PyResult::column_name)
        .def_property_readonly("column_names", &PyResult::column_names)
        .def_property_readonly("duration", &PyResult::duration)
        .def_property_readonly("is_success", &PyResult::is_success)
        .def_property_readonly("error", &PyResult::error)
        .def("row_dict", &PyResult::row_dict)
        .def("__iter__", &PyResult::iter, py::return_value_policy::reference_internal)
        .def("__next__", &PyResult::next_row);

    py::class_<PyTransaction>(m, "Transaction")
        .def("execute", &PyTransaction::execute, py::arg("cypher"))
        .def("commit", &PyTransaction::commit)
        .def("rollback", &PyTransaction::rollback)
        .def_property_readonly("is_active", &PyTransaction::is_active)
        .def_property_readonly("is_read_only", &PyTransaction::is_read_only)
        .def("__enter__", [](PyTransaction &tx) -> PyTransaction & { return tx; },
             py::return_value_policy::reference_internal)
        .def("__exit__", [](PyTransaction &tx, const py::object &exc_type, const py::object &, const py::object &) {
            if (tx.is_active()) {
                if (exc_type.is_none()) {
                    tx.commit();
                } else {
                    tx.rollback();
                }
            }
            return false;
        });

    py::class_<PyDatabase>(m, "Database")
        .def(py::init<std::string>(), py::arg("path"))
        .def("open", &PyDatabase::open)
        .def("open_if_exists", &PyDatabase::open_if_exists)
        .def("close", &PyDatabase::close)
        .def("save", &PyDatabase::save)
        .def("begin_transaction", &PyDatabase::begin_transaction)
        .def("begin_read_only_transaction", &PyDatabase::begin_read_only_transaction)
        .def_property_readonly("has_active_transaction", &PyDatabase::has_active_transaction)
        .def("set_thread_pool_size", &PyDatabase::set_thread_pool_size, py::arg("size"))
        .def("execute", &PyDatabase::execute, py::arg("cypher"))
        .def("create_node", &PyDatabase::create_node, py::arg("label"), py::arg("props") = py::dict())
        .def("create_nodes", &PyDatabase::create_nodes, py::arg("label"), py::arg("props_list"))
        .def("create_edge", &PyDatabase::create_edge, py::arg("src_id"), py::arg("dst_id"), py::arg("edge_type"),
             py::arg("props") = py::dict())
        .def("create_edges", &PyDatabase::create_edges, py::arg("edge_type"), py::arg("edges"))
        .def("get_shortest_path", &PyDatabase::get_shortest_path, py::arg("start_id"), py::arg("end_id"),
             py::arg("max_depth") = 15)
        .def("bfs", &PyDatabase::bfs, py::arg("start_id"), py::arg("visitor"))
        .def("__enter__", [](PyDatabase &db) -> PyDatabase & { return db; }, py::return_value_policy::reference_internal)
        .def("__exit__", [](PyDatabase &db, const py::object &, const py::object &, const py::object &) {
            db.close();
            return false;
        });
}
