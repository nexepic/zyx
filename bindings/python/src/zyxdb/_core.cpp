/**
 * @file _core.cpp
 * @brief pybind11 bindings for ZYX graph database engine.
 *
 * Wraps the C++ API (zyx::Database, zyx::Transaction, zyx::Result)
 * into a Python extension module `_core`.
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "zyx/zyx.hpp"
#include "zyx/value.hpp"

namespace py = pybind11;

// =============================================================================
// Value conversion helpers
// =============================================================================

static py::object valueToPython(const zyx::Value &val);
static zyx::Value pythonToValue(const py::handle &obj);

static py::dict propsToDict(const std::unordered_map<std::string, zyx::Value> &props) {
	py::dict d;
	for (const auto &[k, v] : props) {
		d[py::cast(k)] = valueToPython(v);
	}
	return d;
}

static py::object valueToPython(const zyx::Value &val) {
	return std::visit(
		[](auto &&arg) -> py::object {
			using T = std::decay_t<decltype(arg)>;
			if constexpr (std::is_same_v<T, std::monostate>) {
				return py::none();
			} else if constexpr (std::is_same_v<T, bool>) {
				return py::bool_(arg);
			} else if constexpr (std::is_same_v<T, int64_t>) {
				return py::int_(arg);
			} else if constexpr (std::is_same_v<T, double>) {
				return py::float_(arg);
			} else if constexpr (std::is_same_v<T, std::string>) {
				return py::str(arg);
			} else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Node>>) {
				if (!arg) return py::none();
				py::dict d;
				d["id"] = py::int_(arg->id);
				d["label"] = py::str(arg->label);
				d["labels"] = py::cast(arg->labels);
				d["properties"] = propsToDict(arg->properties);
				return d;
			} else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::Edge>>) {
				if (!arg) return py::none();
				py::dict d;
				d["id"] = py::int_(arg->id);
				d["source_id"] = py::int_(arg->sourceId);
				d["target_id"] = py::int_(arg->targetId);
				d["type"] = py::str(arg->type);
				d["properties"] = propsToDict(arg->properties);
				return d;
			} else if constexpr (std::is_same_v<T, std::vector<float>>) {
				py::list lst;
				for (const auto &f : arg) lst.append(f);
				return lst;
			} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
				py::list lst;
				for (const auto &s : arg) lst.append(py::str(s));
				return lst;
			} else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::ValueList>>) {
				if (!arg) return py::none();
				py::list lst;
				for (const auto &elem : arg->elements) {
					lst.append(valueToPython(elem));
				}
				return lst;
			} else if constexpr (std::is_same_v<T, std::shared_ptr<zyx::ValueMap>>) {
				if (!arg) return py::none();
				py::dict d;
				for (const auto &[k, v] : arg->entries) {
					d[py::cast(k)] = valueToPython(v);
				}
				return d;
			} else {
				return py::none();
			}
		},
		val);
}

static zyx::Value pythonToValue(const py::handle &obj) {
	if (obj.is_none()) {
		return std::monostate{};
	}
	// bool check must come before int (Python bool is subclass of int)
	if (py::isinstance<py::bool_>(obj)) {
		return obj.cast<bool>();
	}
	if (py::isinstance<py::int_>(obj)) {
		return obj.cast<int64_t>();
	}
	if (py::isinstance<py::float_>(obj)) {
		return obj.cast<double>();
	}
	if (py::isinstance<py::str>(obj)) {
		return obj.cast<std::string>();
	}
	if (py::isinstance<py::list>(obj)) {
		auto lst = obj.cast<py::list>();
		if (lst.empty()) {
			// Empty list → empty ValueList
			auto vl = std::make_shared<zyx::ValueList>();
			return vl;
		}
		// Check if all elements are float → vector<float>
		bool all_float = true;
		bool all_str = true;
		for (const auto &item : lst) {
			if (!py::isinstance<py::float_>(item) && !py::isinstance<py::int_>(item)) {
				all_float = false;
			}
			if (!py::isinstance<py::str>(item)) {
				all_str = false;
			}
		}
		if (all_float) {
			std::vector<float> vec;
			vec.reserve(lst.size());
			for (const auto &item : lst) {
				vec.push_back(item.cast<float>());
			}
			return vec;
		}
		if (all_str) {
			std::vector<std::string> vec;
			vec.reserve(lst.size());
			for (const auto &item : lst) {
				vec.push_back(item.cast<std::string>());
			}
			return vec;
		}
		// Mixed list → ValueList
		auto vl = std::make_shared<zyx::ValueList>();
		for (const auto &item : lst) {
			vl->elements.push_back(pythonToValue(item));
		}
		return vl;
	}
	if (py::isinstance<py::dict>(obj)) {
		auto dct = obj.cast<py::dict>();
		auto vm = std::make_shared<zyx::ValueMap>();
		for (const auto &[k, v] : dct) {
			vm->entries[k.cast<std::string>()] = pythonToValue(v);
		}
		return vm;
	}
	throw py::type_error("Cannot convert Python object to zyx::Value");
}

static std::unordered_map<std::string, zyx::Value> dictToParams(const py::dict &d) {
	std::unordered_map<std::string, zyx::Value> params;
	for (const auto &[k, v] : d) {
		params[k.cast<std::string>()] = pythonToValue(v);
	}
	return params;
}

static std::unordered_map<std::string, zyx::Value> kwargsToParams(const py::kwargs &kw) {
	std::unordered_map<std::string, zyx::Value> params;
	for (const auto &[k, v] : kw) {
		params[k.cast<std::string>()] = pythonToValue(v);
	}
	return params;
}

// =============================================================================
// Result row helper: convert current row to dict
// =============================================================================

static py::dict resultRowToDict(const zyx::Result &result) {
	py::dict row;
	int ncols = result.getColumnCount();
	for (int i = 0; i < ncols; ++i) {
		std::string colName = result.getColumnName(i);
		zyx::Value val = result.get(i);
		row[py::cast(colName)] = valueToPython(val);
	}
	return row;
}

// =============================================================================
// Module definition
// =============================================================================

PYBIND11_MODULE(_core, m) {
	m.doc() = "ZYX graph database engine - core bindings";

	// Custom exception type
	static py::exception<std::runtime_error> DatabaseError(m, "DatabaseError");

	// Register exception translator
	// Note: pybind11 exceptions (stop_iteration, etc.) inherit from std::runtime_error,
	// so we must let them pass through by re-throwing pybind11 exceptions.
	py::register_exception_translator([](std::exception_ptr p) {
		try {
			if (p) std::rethrow_exception(p);
		} catch (const py::builtin_exception &) {
			// Let pybind11's built-in exceptions (stop_iteration, etc.) pass through
			throw;
		} catch (const std::runtime_error &e) {
			DatabaseError(e.what());
		}
	});

	// =========================================================================
	// Result class
	// =========================================================================
	py::class_<zyx::Result>(m, "Result")
		.def("has_next", &zyx::Result::hasNext)
		.def("next", &zyx::Result::next)
		.def("get", [](const zyx::Result &r, const std::string &key) {
			return valueToPython(r.get(key));
		})
		.def("get_by_index", [](const zyx::Result &r, int index) {
			return valueToPython(r.get(index));
		})
		.def_property_readonly("column_count", &zyx::Result::getColumnCount)
		.def("column_name", &zyx::Result::getColumnName)
		.def_property_readonly("column_names", [](const zyx::Result &r) {
			py::list names;
			for (int i = 0; i < r.getColumnCount(); ++i) {
				names.append(py::str(r.getColumnName(i)));
			}
			return names;
		})
		.def_property_readonly("duration", &zyx::Result::getDuration)
		.def_property_readonly("is_success", &zyx::Result::isSuccess)
		.def_property_readonly("error", [](const zyx::Result &r) -> py::object {
			if (r.isSuccess()) return py::none();
			return py::str(r.getError());
		})
		.def("row_dict", [](const zyx::Result &r) {
			return resultRowToDict(r);
		})
		.def("__iter__", [](zyx::Result &r) -> zyx::Result& {
			return r;
		}, py::return_value_policy::reference_internal)
		.def("__next__", [](zyx::Result &r) -> py::dict {
			if (!r.hasNext()) {
				throw py::stop_iteration();
			}
			r.next();
			py::dict row = resultRowToDict(r);
			return row;
		});

	// =========================================================================
	// Transaction class
	// =========================================================================
	py::class_<zyx::Transaction>(m, "Transaction")
		.def("execute", [](zyx::Transaction &tx, const std::string &cypher, py::kwargs kw) {
			if (kw.empty()) {
				return tx.execute(cypher);
			}
			return tx.execute(cypher, kwargsToParams(kw));
		}, py::arg("cypher"))
		.def("commit", &zyx::Transaction::commit)
		.def("rollback", &zyx::Transaction::rollback)
		.def_property_readonly("is_active", &zyx::Transaction::isActive)
		.def_property_readonly("is_read_only", &zyx::Transaction::isReadOnly)
		.def("__enter__", [](zyx::Transaction &tx) -> zyx::Transaction& {
			return tx;
		}, py::return_value_policy::reference_internal)
		.def("__exit__", [](zyx::Transaction &tx,
							const py::object &exc_type,
							const py::object & /*exc_val*/,
							const py::object & /*exc_tb*/) {
			if (tx.isActive()) {
				if (!exc_type.is_none()) {
					tx.rollback();
				}
			}
			return false; // Don't suppress exceptions
		});

	// =========================================================================
	// Database class
	// =========================================================================
	py::class_<zyx::Database>(m, "Database")
		.def(py::init<const std::string &>(), py::arg("path"))
		.def("open", &zyx::Database::open)
		.def("open_if_exists", &zyx::Database::openIfExists)
		.def("close", &zyx::Database::close)
		.def("save", &zyx::Database::save)
		.def("begin_transaction", &zyx::Database::beginTransaction)
		.def("begin_read_only_transaction", &zyx::Database::beginReadOnlyTransaction)
		.def_property_readonly("has_active_transaction", &zyx::Database::hasActiveTransaction)
		.def("set_thread_pool_size", &zyx::Database::setThreadPoolSize, py::arg("size"))
		.def("execute", [](zyx::Database &db, const std::string &cypher, py::kwargs kw) {
			if (kw.empty()) {
				return db.execute(cypher);
			}
			return db.execute(cypher, kwargsToParams(kw));
		}, py::arg("cypher"))
		.def("create_node",
			[](zyx::Database &db, const py::object &label, const py::dict &props) {
				auto propMap = dictToParams(props);
				if (py::isinstance<py::list>(label)) {
					db.createNode(label.cast<std::vector<std::string>>(), propMap);
				} else {
					db.createNode(label.cast<std::string>(), propMap);
				}
			},
			py::arg("label"), py::arg("props") = py::dict())
		.def("create_node_ret_id",
			[](zyx::Database &db, const std::string &label, const py::dict &props) {
				return db.createNodeRetId(label, dictToParams(props));
			},
			py::arg("label"), py::arg("props") = py::dict())
		.def("create_nodes",
			[](zyx::Database &db, const std::string &label, const py::list &propsList) {
				std::vector<std::unordered_map<std::string, zyx::Value>> vec;
				vec.reserve(propsList.size());
				for (const auto &item : propsList) {
					vec.push_back(dictToParams(item.cast<py::dict>()));
				}
				db.createNodes(label, vec);
			},
			py::arg("label"), py::arg("props_list"))
		.def("create_edge",
			[](zyx::Database &db,
			   const std::string &srcLabel, const std::string &srcKey, const py::object &srcVal,
			   const std::string &dstLabel, const std::string &dstKey, const py::object &dstVal,
			   const std::string &edgeType, const py::dict &props) {
				db.createEdge(srcLabel, srcKey, pythonToValue(srcVal),
							  dstLabel, dstKey, pythonToValue(dstVal),
							  edgeType, dictToParams(props));
			},
			py::arg("src_label"), py::arg("src_key"), py::arg("src_val"),
			py::arg("dst_label"), py::arg("dst_key"), py::arg("dst_val"),
			py::arg("edge_type"), py::arg("props") = py::dict())
		.def("create_edge_by_id",
			[](zyx::Database &db, int64_t srcId, int64_t dstId,
			   const std::string &edgeType, const py::dict &props) {
				db.createEdgeById(srcId, dstId, edgeType, dictToParams(props));
			},
			py::arg("src_id"), py::arg("dst_id"), py::arg("edge_type"),
			py::arg("props") = py::dict())
		.def("get_shortest_path",
			[](zyx::Database &db, int64_t startId, int64_t endId, int maxDepth) {
				auto nodes = db.getShortestPath(startId, endId, maxDepth);
				py::list result;
				for (const auto &node : nodes) {
					py::dict d;
					d["id"] = py::int_(node.id);
					d["label"] = py::str(node.label);
					d["labels"] = py::cast(node.labels);
					d["properties"] = propsToDict(node.properties);
					result.append(d);
				}
				return result;
			},
			py::arg("start_id"), py::arg("end_id"), py::arg("max_depth") = 15)
		.def("__enter__", [](zyx::Database &db) -> zyx::Database& {
			return db;
		}, py::return_value_policy::reference_internal)
		.def("__exit__", [](zyx::Database &db,
							const py::object &, const py::object &, const py::object &) {
			db.close();
			return false;
		});
}
