/**
 * @file addon.cpp
 * @brief Node.js bindings for ZYX graph database using node-addon-api.
 *
 * Wraps the C++ API (zyx::Database, zyx::Transaction, zyx::Result)
 * into a Node.js native addon with async operations.
 */

#include <napi.h>
#include "zyx/zyx.hpp"
#include "zyx/value.hpp"
#include <cmath>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

// =============================================================================
// Forward declarations
// =============================================================================

class Database;
class Transaction;

// =============================================================================
// Value conversion helpers
// =============================================================================

static Napi::Value ValueToNapi(Napi::Env env, const zyx::Value &val);
static zyx::Value NapiToValue(const Napi::Value &val);

static Napi::Object PropsToObject(Napi::Env env, const std::unordered_map<std::string, zyx::Value> &props) {
    Napi::Object obj = Napi::Object::New(env);
    for (const auto &[k, v] : props) {
        obj.Set(k, ValueToNapi(env, v));
    }
    return obj;
}

static Napi::Value ValueToNapi(Napi::Env env, const zyx::Value &val) {
    return std::visit(
        [&env](auto &&arg) -> Napi::Value {
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
            } else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Node>>) {
                if (!arg) return env.Null();
                Napi::Object obj = Napi::Object::New(env);
                obj.Set("id", Napi::Number::New(env, static_cast<double>(arg->id)));
                obj.Set("label", Napi::String::New(env, arg->label));
                Napi::Array labels = Napi::Array::New(env, arg->labels.size());
                for (size_t i = 0; i < arg->labels.size(); ++i) {
                    labels[i] = Napi::String::New(env, arg->labels[i]);
                }
                obj.Set("labels", labels);
                obj.Set("properties", PropsToObject(env, arg->properties));
                return obj;
            } else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Edge>>) {
                if (!arg) return env.Null();
                Napi::Object obj = Napi::Object::New(env);
                obj.Set("id", Napi::Number::New(env, static_cast<double>(arg->id)));
                obj.Set("sourceId", Napi::Number::New(env, static_cast<double>(arg->sourceId)));
                obj.Set("targetId", Napi::Number::New(env, static_cast<double>(arg->targetId)));
                obj.Set("type", Napi::String::New(env, arg->type));
                obj.Set("properties", PropsToObject(env, arg->properties));
                return obj;
            } else if constexpr (std::is_same_v<T, std::vector<float>>) {
                Napi::Array arr = Napi::Array::New(env, arg.size());
                for (size_t i = 0; i < arg.size(); ++i) {
                    arr[i] = Napi::Number::New(env, arg[i]);
                }
                return arr;
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
                Napi::Array arr = Napi::Array::New(env, arg.size());
                for (size_t i = 0; i < arg.size(); ++i) {
                    arr[i] = Napi::String::New(env, arg[i]);
                }
                return arr;
            } else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::ValueList>>) {
                if (!arg) return env.Null();
                Napi::Array arr = Napi::Array::New(env, arg->elements.size());
                for (size_t i = 0; i < arg->elements.size(); ++i) {
                    arr[i] = ValueToNapi(env, arg->elements[i]);
                }
                return arr;
            } else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::ValueMap>>) {
                if (!arg) return env.Null();
                Napi::Object obj = Napi::Object::New(env);
                for (const auto &[k, v] : arg->entries) {
                    obj.Set(k, ValueToNapi(env, v));
                }
                return obj;
            } else {
                return env.Null();
            }
        },
        val);
}

static zyx::Value NapiToValue(const Napi::Value &val) {
    if (val.IsNull() || val.IsUndefined()) {
        return std::monostate{};
    }
    if (val.IsBoolean()) {
        return val.As<Napi::Boolean>().Value();
    }
    if (val.IsNumber()) {
        double num = val.As<Napi::Number>().DoubleValue();
        // Check if it's an integer
        if (num == std::floor(num) && num >= INT64_MIN && num <= INT64_MAX) {
            return static_cast<int64_t>(num);
        }
        return num;
    }
    if (val.IsString()) {
        return val.As<Napi::String>().Utf8Value();
    }
    if (val.IsArray()) {
        Napi::Array arr = val.As<Napi::Array>();
        uint32_t len = arr.Length();
        if (len == 0) {
            return std::make_shared<zyx::ValueList>();
        }

        // Check if all elements are numbers (for float vector)
        bool allNumbers = true;
        bool allStrings = true;
        for (uint32_t i = 0; i < len; ++i) {
            Napi::Value elem = arr[i];
            if (!elem.IsNumber()) allNumbers = false;
            if (!elem.IsString()) allStrings = false;
        }

        if (allNumbers) {
            std::vector<float> vec;
            vec.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                vec.push_back(arr.Get(i).As<Napi::Number>().FloatValue());
            }
            return vec;
        }

        if (allStrings) {
            std::vector<std::string> vec;
            vec.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                vec.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
            }
            return vec;
        }

        // Heterogeneous list
        auto vl = std::make_shared<zyx::ValueList>();
        for (uint32_t i = 0; i < len; ++i) {
            vl->elements.push_back(NapiToValue(arr[i]));
        }
        return vl;
    }
    if (val.IsObject()) {
        Napi::Object obj = val.As<Napi::Object>();
        auto vm = std::make_shared<zyx::ValueMap>();
        Napi::Array keys = obj.GetPropertyNames();
        for (uint32_t i = 0; i < keys.Length(); ++i) {
            std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
            vm->entries[key] = NapiToValue(obj.Get(key));
        }
        return vm;
    }

    Napi::Error::New(val.Env(), "Cannot convert JavaScript value to zyx::Value").ThrowAsJavaScriptException();
    return std::monostate{};
}

static std::unordered_map<std::string, zyx::Value> ObjectToParams(const Napi::Object &obj) {
    std::unordered_map<std::string, zyx::Value> params;
    Napi::Array keys = obj.GetPropertyNames();
    for (uint32_t i = 0; i < keys.Length(); ++i) {
        std::string key = keys.Get(i).As<Napi::String>().Utf8Value();
        params[key] = NapiToValue(obj.Get(key));
    }
    return params;
}

// =============================================================================
// Result data structure (collected in worker thread)
// =============================================================================

struct ResultData {
    bool isSuccess;
    std::string error;
    double duration;
    std::vector<std::string> columns;
    std::vector<std::unordered_map<std::string, zyx::Value>> rows;
};

// =============================================================================
// AsyncWorker for execute operations
// =============================================================================

class ExecuteWorker : public Napi::AsyncWorker {
public:
    ExecuteWorker(Napi::Env env,
                  std::shared_ptr<zyx::Database> db,
                  std::string cypher,
                  std::unordered_map<std::string, zyx::Value> params)
        : Napi::AsyncWorker(env),
          deferred_(Napi::Promise::Deferred::New(env)),
          db_(db),
          cypher_(std::move(cypher)),
          params_(std::move(params)) {}

    Napi::Promise GetPromise() { return deferred_.Promise(); }

protected:
    void Execute() override {
        try {
            zyx::Result result = params_.empty()
                ? db_->execute(cypher_)
                : db_->execute(cypher_, params_);

            resultData_.isSuccess = result.isSuccess();
            resultData_.error = result.getError();
            resultData_.duration = result.getDuration();

            int colCount = result.getColumnCount();
            for (int i = 0; i < colCount; ++i) {
                resultData_.columns.push_back(result.getColumnName(i));
            }

            while (result.hasNext()) {
                result.next();
                std::unordered_map<std::string, zyx::Value> row;
                for (int i = 0; i < colCount; ++i) {
                    row[resultData_.columns[i]] = result.get(i);
                }
                resultData_.rows.push_back(std::move(row));
            }
        } catch (const std::exception &e) {
            SetError(e.what());
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        Napi::Object result = Napi::Object::New(env);

        result.Set("isSuccess", Napi::Boolean::New(env, resultData_.isSuccess));
        result.Set("error", resultData_.error.empty() ? env.Null() : Napi::String::New(env, resultData_.error));
        result.Set("duration", Napi::Number::New(env, resultData_.duration));

        Napi::Array columns = Napi::Array::New(env, resultData_.columns.size());
        for (size_t i = 0; i < resultData_.columns.size(); ++i) {
            columns[i] = Napi::String::New(env, resultData_.columns[i]);
        }
        result.Set("columns", columns);

        Napi::Array rows = Napi::Array::New(env, resultData_.rows.size());
        for (size_t i = 0; i < resultData_.rows.size(); ++i) {
            Napi::Object row = Napi::Object::New(env);
            for (const auto &[key, val] : resultData_.rows[i]) {
                row.Set(key, ValueToNapi(env, val));
            }
            rows[i] = row;
        }
        result.Set("rows", rows);

        deferred_.Resolve(result);
    }

    void OnError(const Napi::Error &e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<zyx::Database> db_;
    std::string cypher_;
    std::unordered_map<std::string, zyx::Value> params_;
    ResultData resultData_;
};

// =============================================================================
// Transaction execute worker
// =============================================================================

class TxExecuteWorker : public Napi::AsyncWorker {
public:
    TxExecuteWorker(Napi::Env env,
                    std::shared_ptr<zyx::Transaction> tx,
                    std::string cypher,
                    std::unordered_map<std::string, zyx::Value> params)
        : Napi::AsyncWorker(env),
          deferred_(Napi::Promise::Deferred::New(env)),
          tx_(tx),
          cypher_(std::move(cypher)),
          params_(std::move(params)) {}

    Napi::Promise GetPromise() { return deferred_.Promise(); }

protected:
    void Execute() override {
        try {
            zyx::Result result = params_.empty()
                ? tx_->execute(cypher_)
                : tx_->execute(cypher_, params_);

            resultData_.isSuccess = result.isSuccess();
            resultData_.error = result.getError();
            resultData_.duration = result.getDuration();

            int colCount = result.getColumnCount();
            for (int i = 0; i < colCount; ++i) {
                resultData_.columns.push_back(result.getColumnName(i));
            }

            while (result.hasNext()) {
                result.next();
                std::unordered_map<std::string, zyx::Value> row;
                for (int i = 0; i < colCount; ++i) {
                    row[resultData_.columns[i]] = result.get(i);
                }
                resultData_.rows.push_back(std::move(row));
            }
        } catch (const std::exception &e) {
            SetError(e.what());
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        Napi::Object result = Napi::Object::New(env);

        result.Set("isSuccess", Napi::Boolean::New(env, resultData_.isSuccess));
        result.Set("error", resultData_.error.empty() ? env.Null() : Napi::String::New(env, resultData_.error));
        result.Set("duration", Napi::Number::New(env, resultData_.duration));

        Napi::Array columns = Napi::Array::New(env, resultData_.columns.size());
        for (size_t i = 0; i < resultData_.columns.size(); ++i) {
            columns[i] = Napi::String::New(env, resultData_.columns[i]);
        }
        result.Set("columns", columns);

        Napi::Array rows = Napi::Array::New(env, resultData_.rows.size());
        for (size_t i = 0; i < resultData_.rows.size(); ++i) {
            Napi::Object row = Napi::Object::New(env);
            for (const auto &[key, val] : resultData_.rows[i]) {
                row.Set(key, ValueToNapi(env, val));
            }
            rows[i] = row;
        }
        result.Set("rows", rows);

        deferred_.Resolve(result);
    }

    void OnError(const Napi::Error &e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    std::shared_ptr<zyx::Transaction> tx_;
    std::string cypher_;
    std::unordered_map<std::string, zyx::Value> params_;
    ResultData resultData_;
};

// =============================================================================
// Simple async workers for void operations
// =============================================================================

class VoidWorker : public Napi::AsyncWorker {
public:
    VoidWorker(Napi::Env env, std::function<void()> work)
        : Napi::AsyncWorker(env),
          deferred_(Napi::Promise::Deferred::New(env)),
          work_(std::move(work)) {}

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

template<typename T>
class ValueWorker : public Napi::AsyncWorker {
public:
    ValueWorker(Napi::Env env, std::function<T()> work, std::function<Napi::Value(Napi::Env, T)> convert)
        : Napi::AsyncWorker(env),
          deferred_(Napi::Promise::Deferred::New(env)),
          work_(std::move(work)),
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
    std::function<Napi::Value(Napi::Env, T)> convert_;
    T result_;
};

// =============================================================================
// Transaction wrapper class
// =============================================================================

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

    static Napi::Object NewInstance(Napi::Env env, std::shared_ptr<zyx::Transaction> tx) {
        Napi::Object obj = constructor_.New({});
        TransactionWrap *wrap = Napi::ObjectWrap<TransactionWrap>::Unwrap(obj);
        wrap->tx_ = tx;
        return obj;
    }

    TransactionWrap(const Napi::CallbackInfo &info)
        : Napi::ObjectWrap<TransactionWrap>(info) {}

private:
    Napi::Value Execute(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Expected cypher string").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string cypher = info[0].As<Napi::String>().Utf8Value();
        std::unordered_map<std::string, zyx::Value> params;
        if (info.Length() > 1 && info[1].IsObject()) {
            params = ObjectToParams(info[1].As<Napi::Object>());
        }

        auto worker = new TxExecuteWorker(env, tx_, std::move(cypher), std::move(params));
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value Commit(const Napi::CallbackInfo &info) {
        if (!tx_) {
            Napi::Error::New(info.Env(), "Transaction already closed").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        auto tx = std::move(tx_);
        // Use a custom callback that destroys the transaction after commit
        auto deferred = Napi::Promise::Deferred::New(info.Env());
        auto promise = deferred.Promise();

        struct CommitWorker : public Napi::AsyncWorker {
            CommitWorker(Napi::Env env, Napi::Promise::Deferred def, std::shared_ptr<zyx::Transaction> t)
                : Napi::AsyncWorker(env), deferred_(def), tx_(std::move(t)) {}
            void Execute() override {
                try {
                    tx_->commit();
                } catch (const std::exception &e) {
                    SetError(e.what());
                }
                tx_.reset(); // Destroy transaction in worker thread
            }
            void OnOK() override { deferred_.Resolve(Env().Undefined()); }
            void OnError(const Napi::Error &e) override { deferred_.Reject(e.Value()); }
            Napi::Promise::Deferred deferred_;
            std::shared_ptr<zyx::Transaction> tx_;
        };

        auto worker = new CommitWorker(info.Env(), deferred, std::move(tx));
        worker->Queue();
        return promise;
    }

    Napi::Value Rollback(const Napi::CallbackInfo &info) {
        if (!tx_) {
            Napi::Error::New(info.Env(), "Transaction already closed").ThrowAsJavaScriptException();
            return info.Env().Undefined();
        }
        auto tx = std::move(tx_);
        auto deferred = Napi::Promise::Deferred::New(info.Env());
        auto promise = deferred.Promise();

        struct RollbackWorker : public Napi::AsyncWorker {
            RollbackWorker(Napi::Env env, Napi::Promise::Deferred def, std::shared_ptr<zyx::Transaction> t)
                : Napi::AsyncWorker(env), deferred_(def), tx_(std::move(t)) {}
            void Execute() override {
                try {
                    tx_->rollback();
                } catch (const std::exception &e) {
                    SetError(e.what());
                }
                tx_.reset(); // Destroy transaction in worker thread
            }
            void OnOK() override { deferred_.Resolve(Env().Undefined()); }
            void OnError(const Napi::Error &e) override { deferred_.Reject(e.Value()); }
            Napi::Promise::Deferred deferred_;
            std::shared_ptr<zyx::Transaction> tx_;
        };

        auto worker = new RollbackWorker(info.Env(), deferred, std::move(tx));
        worker->Queue();
        return promise;
    }

    Napi::Value IsActive(const Napi::CallbackInfo &info) {
        return Napi::Boolean::New(info.Env(), tx_ && tx_->isActive());
    }

    Napi::Value IsReadOnly(const Napi::CallbackInfo &info) {
        return Napi::Boolean::New(info.Env(), tx_ && tx_->isReadOnly());
    }

    std::shared_ptr<zyx::Transaction> tx_;
    static Napi::FunctionReference constructor_;
};

Napi::FunctionReference TransactionWrap::constructor_;

// =============================================================================
// Database wrapper class
// =============================================================================

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

    DatabaseWrap(const Napi::CallbackInfo &info)
        : Napi::ObjectWrap<DatabaseWrap>(info) {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "Expected database path string").ThrowAsJavaScriptException();
            return;
        }
        std::string path = info[0].As<Napi::String>().Utf8Value();
        db_ = std::make_shared<zyx::Database>(path);
    }

private:
    Napi::Value Open(const Napi::CallbackInfo &info) {
        auto db = db_;
        auto worker = new VoidWorker(info.Env(), [db]() { db->open(); });
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value Close(const Napi::CallbackInfo &info) {
        auto db = db_;
        auto worker = new VoidWorker(info.Env(), [db]() { db->close(); });
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value Save(const Napi::CallbackInfo &info) {
        auto db = db_;
        auto worker = new VoidWorker(info.Env(), [db]() { db->save(); });
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
        std::unordered_map<std::string, zyx::Value> params;
        if (info.Length() > 1 && info[1].IsObject()) {
            params = ObjectToParams(info[1].As<Napi::Object>());
        }

        auto worker = new ExecuteWorker(env, db_, std::move(cypher), std::move(params));
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value BeginTransaction(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        try {
            auto tx = std::make_shared<zyx::Transaction>(db_->beginTransaction());
            return TransactionWrap::NewInstance(env, tx);
        } catch (const std::exception &e) {
            Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    Napi::Value BeginReadOnlyTransaction(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        try {
            auto tx = std::make_shared<zyx::Transaction>(db_->beginReadOnlyTransaction());
            return TransactionWrap::NewInstance(env, tx);
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

        std::unordered_map<std::string, zyx::Value> props;
        if (info.Length() > 1 && info[1].IsObject()) {
            props = ObjectToParams(info[1].As<Napi::Object>());
        }

        auto db = db_;
        if (info[0].IsArray()) {
            std::vector<std::string> labels;
            Napi::Array arr = info[0].As<Napi::Array>();
            for (uint32_t i = 0; i < arr.Length(); ++i) {
                labels.push_back(arr.Get(i).As<Napi::String>().Utf8Value());
            }
            auto worker = new ValueWorker<int64_t>(
                env,
                [db, labels, props]() { return db->createNode(labels, props); },
                [](Napi::Env e, int64_t id) -> Napi::Value {
                    return Napi::Number::New(e, static_cast<double>(id));
                }
            );
            worker->Queue();
            return worker->GetPromise();
        }

        std::string label = info[0].As<Napi::String>().Utf8Value();
        auto worker = new ValueWorker<int64_t>(
            env,
            [db, label, props]() { return db->createNode(label, props); },
            [](Napi::Env e, int64_t id) -> Napi::Value {
                return Napi::Number::New(e, static_cast<double>(id));
            }
        );
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
        Napi::Array arr = info[1].As<Napi::Array>();
        std::vector<std::unordered_map<std::string, zyx::Value>> propsList;
        propsList.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            propsList.push_back(ObjectToParams(arr.Get(i).As<Napi::Object>()));
        }

        auto db = db_;
        auto worker = new ValueWorker<std::vector<int64_t>>(
            env,
            [db, label, propsList]() { return db->createNodes(label, propsList); },
            [](Napi::Env e, std::vector<int64_t> ids) -> Napi::Value {
                Napi::Array result = Napi::Array::New(e, ids.size());
                for (size_t i = 0; i < ids.size(); ++i) {
                    result[i] = Napi::Number::New(e, static_cast<double>(ids[i]));
                }
                return result;
            }
        );
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value CreateEdge(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 3) {
            Napi::TypeError::New(env, "Expected (srcId, dstId, edgeType, [props])").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        int64_t srcId = static_cast<int64_t>(info[0].As<Napi::Number>().Int64Value());
        int64_t dstId = static_cast<int64_t>(info[1].As<Napi::Number>().Int64Value());
        std::string edgeType = info[2].As<Napi::String>().Utf8Value();
        std::unordered_map<std::string, zyx::Value> props;
        if (info.Length() > 3 && info[3].IsObject()) {
            props = ObjectToParams(info[3].As<Napi::Object>());
        }

        auto db = db_;
        auto worker = new ValueWorker<int64_t>(
            env,
            [db, srcId, dstId, edgeType, props]() {
                return db->createEdge(srcId, dstId, edgeType, props);
            },
            [](Napi::Env e, int64_t id) -> Napi::Value {
                return Napi::Number::New(e, static_cast<double>(id));
            }
        );
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value CreateEdges(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsArray()) {
            Napi::TypeError::New(env, "Expected (edgeType, edgesArray)").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        std::string edgeType = info[0].As<Napi::String>().Utf8Value();
        Napi::Array arr = info[1].As<Napi::Array>();
        std::vector<std::tuple<int64_t, int64_t, std::unordered_map<std::string, zyx::Value>>> edges;
        edges.reserve(arr.Length());
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            Napi::Array tuple = arr.Get(i).As<Napi::Array>();
            int64_t src = static_cast<int64_t>(tuple.Get((uint32_t)0).As<Napi::Number>().Int64Value());
            int64_t dst = static_cast<int64_t>(tuple.Get((uint32_t)1).As<Napi::Number>().Int64Value());
            std::unordered_map<std::string, zyx::Value> props;
            if (tuple.Length() > 2 && !tuple.Get((uint32_t)2).IsNull() && !tuple.Get((uint32_t)2).IsUndefined()) {
                props = ObjectToParams(tuple.Get((uint32_t)2).As<Napi::Object>());
            }
            edges.emplace_back(src, dst, std::move(props));
        }

        auto db = db_;
        auto worker = new ValueWorker<std::vector<int64_t>>(
            env,
            [db, edgeType, edges]() { return db->createEdges(edgeType, edges); },
            [](Napi::Env e, std::vector<int64_t> ids) -> Napi::Value {
                Napi::Array result = Napi::Array::New(e, ids.size());
                for (size_t i = 0; i < ids.size(); ++i) {
                    result[i] = Napi::Number::New(e, static_cast<double>(ids[i]));
                }
                return result;
            }
        );
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value GetShortestPath(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (info.Length() < 2) {
            Napi::TypeError::New(env, "Expected (startId, endId, [maxDepth])").ThrowAsJavaScriptException();
            return env.Undefined();
        }

        int64_t startId = static_cast<int64_t>(info[0].As<Napi::Number>().Int64Value());
        int64_t endId = static_cast<int64_t>(info[1].As<Napi::Number>().Int64Value());
        int maxDepth = 15;
        if (info.Length() > 2 && info[2].IsNumber()) {
            maxDepth = info[2].As<Napi::Number>().Int32Value();
        }

        auto db = db_;
        auto worker = new ValueWorker<std::vector<zyx::Node>>(
            env,
            [db, startId, endId, maxDepth]() {
                return db->getShortestPath(startId, endId, maxDepth);
            },
            [](Napi::Env e, std::vector<zyx::Node> nodes) -> Napi::Value {
                Napi::Array result = Napi::Array::New(e, nodes.size());
                for (size_t i = 0; i < nodes.size(); ++i) {
                    Napi::Object obj = Napi::Object::New(e);
                    obj.Set("id", Napi::Number::New(e, static_cast<double>(nodes[i].id)));
                    obj.Set("label", Napi::String::New(e, nodes[i].label));
                    Napi::Array labels = Napi::Array::New(e, nodes[i].labels.size());
                    for (size_t j = 0; j < nodes[i].labels.size(); ++j) {
                        labels[j] = Napi::String::New(e, nodes[i].labels[j]);
                    }
                    obj.Set("labels", labels);
                    obj.Set("properties", PropsToObject(e, nodes[i].properties));
                    result[i] = obj;
                }
                return result;
            }
        );
        worker->Queue();
        return worker->GetPromise();
    }

    Napi::Value HasActiveTransaction(const Napi::CallbackInfo &info) {
        return Napi::Boolean::New(info.Env(), db_->hasActiveTransaction());
    }

    std::shared_ptr<zyx::Database> db_;
    static Napi::FunctionReference constructor_;
};

Napi::FunctionReference DatabaseWrap::constructor_;

// =============================================================================
// Module initialization
// =============================================================================

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    DatabaseWrap::Init(env, exports);
    TransactionWrap::Init(env, exports);
    return exports;
}

NODE_API_MODULE(zyxdb, Init)
