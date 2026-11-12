#include "graph/query/api/ResultValue.hpp"
#include <cstdio>
#include <type_traits>
static_assert(std::is_same_v<graph::query::ResultValue::VariantType,std::variant<graph::PropertyValue,graph::Node,graph::Edge,graph::query::LogicalNodeResult,graph::query::LogicalEdgeResult>>);
int main(){std::printf("node=%zu edge=%zu result_value=%zu result_pair=%zu node_slot=%zu edge_slot=%zu\n",sizeof(graph::Node),sizeof(graph::Edge),sizeof(graph::query::ResultValue),sizeof(std::pair<const std::string,graph::query::ResultValue>),graph::Node::getTotalSize(),graph::Edge::getTotalSize());}
