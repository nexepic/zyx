// Optional development bridge. Decode supported native logical operators before
// any storage operation; unsupported operators fail without a partial execution.
#include <algorithm>
#include <cstdint>
#include <cctype>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "query/parser/cypher/CypherParserImpl.hpp"
#include "graph/query/expressions/ParameterExpression.hpp"
#include "graph/query/logical/operators/LogicalCreateNode.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalSet.hpp"
#include "graph/query/logical/operators/LogicalDelete.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalUnwind.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"

namespace py = pybind11;
using namespace graph::query::logical;
using namespace graph::query::expressions;

namespace {
py::object scalar(const graph::PropertyValue &v) {
    const auto &value = v.getVariant();
    if (std::holds_alternative<std::monostate>(value)) return py::none();
    if (auto i=std::get_if<int64_t>(&value)) return py::int_(*i);
    throw std::invalid_argument("experimental backend supports int64 and NULL");
}

py::tuple expression(const Expression &e) {
    if (auto p=dynamic_cast<const ParameterExpression*>(&e))
        return py::make_tuple("parameter",p->getParameterName());
    if (auto p=dynamic_cast<const VariableReferenceExpression*>(&e))
        return py::make_tuple("reference",p->getVariableName(),p->getPropertyName());
    if (auto p=dynamic_cast<const LiteralExpression*>(&e)) {
        if (p->isNull()) return py::make_tuple("literal",py::none());
        if (p->isInteger()) return py::make_tuple("literal",p->getIntegerValue());
        if (p->isPropertyValue()) return py::make_tuple("literal",scalar(p->getPropertyValue()));
    }
    if (auto call=dynamic_cast<const FunctionCallExpression*>(&e)) {
        auto name=call->getFunctionName();
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return std::tolower(c); });
        if (name=="id" && !call->isDistinct() && call->getArguments().size()==1) {
            const auto *entity=dynamic_cast<const VariableReferenceExpression*>(call->getArguments()[0].get());
            if (entity && !entity->hasProperty()) return py::make_tuple("entity_id",entity->getVariableName());
        }
    }
    if (auto p=dynamic_cast<const BinaryOpExpression*>(&e)) {
        if (p->getOperator()==BinaryOperatorType::BOP_EQUAL)
            return py::make_tuple("equal",expression(*p->getLeft()),expression(*p->getRight()));
    }
    throw std::invalid_argument("unsupported experimental expression");
}

py::tuple operation(const LogicalOperator &op) {
    if (op.getType()==LogicalOpType::LOP_SINGLE_ROW) return py::make_tuple("one");
    if (auto p=dynamic_cast<const LogicalUnwind*>(&op)) {
        if (!p->getListExpr()) throw std::invalid_argument("UNWIND requires a bound list");
        return py::make_tuple("unwind",p->getAlias(),expression(*p->getListExpr()));
    }
    if (auto p=dynamic_cast<const LogicalCreateNode*>(&op)) {
        py::dict props;
        for (auto &[k,v]:p->getProperties()) props[py::str(k)]=py::make_tuple("literal",scalar(v));
        for (auto &[k,v]:p->getPropertyExprs()) props[py::str(k)]=expression(*v);
        return py::make_tuple("create",p->getVariable(),p->getLabels(),props);
    }
    if (auto p=dynamic_cast<const LogicalNodeScan*>(&op)) {
        if (!p->getRangePredicates().empty() || p->getCompositeEquality())
            throw std::invalid_argument("unsupported experimental scan predicates");
        py::list predicates;
        for (auto &[k,v]:p->getPropertyPredicates())
            predicates.append(py::make_tuple(k,py::make_tuple("literal",scalar(v))));
        for (auto &v:p->getParameterPropertyPredicates())
            predicates.append(py::make_tuple(v.key,py::make_tuple("parameter",v.parameter_name)));
        return py::make_tuple("scan",p->getVariable(),p->getLabels(),predicates);
    }
    if (auto p=dynamic_cast<const LogicalTraversal*>(&op)) {
        if (!p->getTargetProperties().empty() || !p->getEdgeProperties().empty())
            throw std::invalid_argument("unsupported traversal property map");
        return py::make_tuple("expand",p->getSourceVar(),p->getEdgeVar(),p->getTargetVar(),
                              p->getEdgeType(),p->getDirection(),p->getTargetLabels());
    }
    if (auto p=dynamic_cast<const LogicalSet*>(&op)) {
        const auto &items=p->getItems();
        if (items.size()!=1 || items.front().type!=SetActionType::LSET_PROPERTY)
            throw std::invalid_argument("only one property assignment is admitted");
        const auto &item=items.front();
        return py::make_tuple("set",std::string(item.variable),std::string(item.key),expression(*item.expression));
    }
    if (auto p=dynamic_cast<const LogicalDelete*>(&op)) {
        if (!p->isDetach() || p->getVariables().size()!=1)
            throw std::invalid_argument("only single-target DETACH DELETE is admitted");
        return py::make_tuple("erase",p->getVariables().front());
    }
    if (auto p=dynamic_cast<const LogicalFilter*>(&op)) return py::make_tuple("filter",expression(*p->getPredicateRaw()));
    if (auto p=dynamic_cast<const LogicalProject*>(&op)) {
        if (p->isDistinct()) throw std::invalid_argument("unsupported DISTINCT");
        py::list items;
        for (auto &v:p->getItems()) items.append(py::make_tuple(std::string(v.alias),expression(*v.expression)));
        return py::make_tuple("project",items);
    }
    if (auto p=dynamic_cast<const LogicalLimit*>(&op)) return py::make_tuple("limit",p->getLimit());
    throw std::invalid_argument("unsupported experimental operator: "+toString(op.getType()));
}
}

PYBIND11_MODULE(_wal_plan, module) {
    module.def("compile", [](const std::string &query) {
        if (query.size()>8192) throw std::length_error("experimental statement length bound");
        static graph::parser::cypher::CypherParserImpl parser;
        auto plan=parser.parseToLogical(query);
        if (!plan.root || plan.mutatesSchema) throw std::invalid_argument("unsupported statement");
        std::vector<py::tuple> reversed;
        for (auto *node=plan.root.get(); node; ) {
            if (reversed.size()==32) throw std::length_error("operator count bound");
            reversed.push_back(operation(*node));
            auto children=node->getChildren();
            if (children.size()>1) throw std::invalid_argument("unsupported branching plan");
            node=children.empty()?nullptr:children.front();
        }
        std::reverse(reversed.begin(),reversed.end());
        return py::make_tuple(plan.mutatesData,reversed);
    });
}
