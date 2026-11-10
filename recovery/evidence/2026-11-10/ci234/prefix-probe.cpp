#include "query/api/LogicalVectorProcedureBatchFixture.hpp"
#include <cstdio>
using namespace graph;using namespace graph::query;
struct Runner : LogicalVectorPropertyBatchTest {
 void TestBody() override {}
 void run(){SetUp();backend->state->enable_property_admission=true;
  for(auto &[id,node]:backend->state->nodes)node.properties.back().value=PropertyValue(std::string(1000U,'x'));
  std::vector<std::unique_ptr<expressions::Expression>> args;args.push_back(std::make_unique<expressions::VariableReferenceExpression>("node","body"));
  auto pred=std::make_shared<expressions::BinaryOpExpression>(std::make_unique<expressions::FunctionCallExpression>("size",std::move(args)),expressions::BinaryOperatorType::BOP_GREATER,std::make_unique<expressions::LiteralExpression>(int64_t{0}));
  auto score=std::make_shared<expressions::VariableReferenceExpression>("score");auto rank=std::make_shared<expressions::VariableReferenceExpression>("node","rank");
  logical::LogicalProject project(std::make_unique<logical::LogicalFilter>(std::make_unique<logical::LogicalSort>(search(),std::vector<logical::LogicalSortItem>{{score,true}}),pred),{{rank,"rank"}});
  for(size_t limit=2500;limit<=8500;limit+=4){options.bounds.maximum_materialized_bytes=limit;
   try{auto r=execute(project);if(r.rowCount()!=3)std::abort();std::printf("PASS %zu\n",limit);}catch(const std::runtime_error&){}
   backend->state->property_column_ids.clear();backend->state->node_batch_sizes.clear();
  }
 }
};
int main(int argc,char **argv){::testing::InitGoogleTest(&argc,argv);Runner r;r.run();}
