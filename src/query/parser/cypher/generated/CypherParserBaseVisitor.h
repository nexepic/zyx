
    #include "antlr4-runtime.h"


// Generated from CypherParser.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"
#include "CypherParserVisitor.h"


/**
 * This class provides an empty implementation of CypherParserVisitor, which can be
 * extended to create a visitor which only needs to handle a subset of the available methods.
 */
class  CypherParserBaseVisitor : public CypherParserVisitor {
public:

  virtual std::any visitCypher(CypherParser::CypherContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExplainStatement(CypherParser::ExplainStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProfileStatement(CypherParser::ProfileStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRegularStatement(CypherParser::RegularStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAdminStatement(CypherParser::AdminStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAdministrationStatement(CypherParser::AdministrationStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTxnBegin(CypherParser::TxnBeginContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTxnCommit(CypherParser::TxnCommitContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitTxnRollback(CypherParser::TxnRollbackContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropIndexByName(CypherParser::DropIndexByNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropIndexByLabel(CypherParser::DropIndexByLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateIndexByPattern(CypherParser::CreateIndexByPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateIndexByLabel(CypherParser::CreateIndexByLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateVectorIndex(CypherParser::CreateVectorIndexContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateNodeConstraint(CypherParser::CreateNodeConstraintContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateEdgeConstraint(CypherParser::CreateEdgeConstraintContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConstraintNodePattern(CypherParser::ConstraintNodePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConstraintEdgePattern(CypherParser::ConstraintEdgePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUniqueConstraintBody(CypherParser::UniqueConstraintBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNotNullConstraintBody(CypherParser::NotNullConstraintBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyTypeConstraintBody(CypherParser::PropertyTypeConstraintBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeKeyConstraintBody(CypherParser::NodeKeyConstraintBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitConstraintTypeName(CypherParser::ConstraintTypeNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropConstraintByName(CypherParser::DropConstraintByNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropConstraintIfExists(CypherParser::DropConstraintIfExistsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitShowConstraintsStatement(CypherParser::ShowConstraintsStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitQuery(CypherParser::QueryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRegularQuery(CypherParser::RegularQueryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSingleQuery(CypherParser::SingleQueryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReadingClause(CypherParser::ReadingClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUpdatingClause(CypherParser::UpdatingClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWithClause(CypherParser::WithClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMatchStatement(CypherParser::MatchStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnwindStatement(CypherParser::UnwindStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInQueryCallStatement(CypherParser::InQueryCallStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateStatement(CypherParser::CreateStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMergeStatement(CypherParser::MergeStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetStatement(CypherParser::SetStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSetItem(CypherParser::SetItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDeleteStatement(CypherParser::DeleteStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRemoveStatement(CypherParser::RemoveStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRemoveItem(CypherParser::RemoveItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitForeachStatement(CypherParser::ForeachStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCallSubquery(CypherParser::CallSubqueryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitInTransactionsClause(CypherParser::InTransactionsClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLoadCsvStatement(CypherParser::LoadCsvStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnStatement(CypherParser::ReturnStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStandaloneCallStatement(CypherParser::StandaloneCallStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitYieldItems(CypherParser::YieldItemsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitYieldItem(CypherParser::YieldItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProjectionBody(CypherParser::ProjectionBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProjectionItems(CypherParser::ProjectionItemsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProjectionItem(CypherParser::ProjectionItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOrderStatement(CypherParser::OrderStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSkipStatement(CypherParser::SkipStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLimitStatement(CypherParser::LimitStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSortItem(CypherParser::SortItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhere(CypherParser::WhereContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPattern(CypherParser::PatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPatternPart(CypherParser::PatternPartContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPatternElement(CypherParser::PatternElementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPatternElementChain(CypherParser::PatternElementChainContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodePattern(CypherParser::NodePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelationshipPattern(CypherParser::RelationshipPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelationshipDetail(CypherParser::RelationshipDetailContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProperties(CypherParser::PropertiesContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeLabels(CypherParser::NodeLabelsContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodeLabel(CypherParser::NodeLabelContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelationshipTypes(CypherParser::RelationshipTypesContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRangeLiteral(CypherParser::RangeLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExpression(CypherParser::ExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitOrExpression(CypherParser::OrExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitXorExpression(CypherParser::XorExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAndExpression(CypherParser::AndExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNotExpression(CypherParser::NotExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitComparisonExpression(CypherParser::ComparisonExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitArithmeticExpression(CypherParser::ArithmeticExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPowerExpression(CypherParser::PowerExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitUnaryExpression(CypherParser::UnaryExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAccessor(CypherParser::AccessorContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAtom(CypherParser::AtomContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitQuantifierExpression(CypherParser::QuantifierExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExistsExpression(CypherParser::ExistsExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCaseExpression(CypherParser::CaseExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyExpression(CypherParser::PropertyExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFunctionInvocation(CypherParser::FunctionInvocationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExplicitProcedureInvocation(CypherParser::ExplicitProcedureInvocationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitImplicitProcedureInvocation(CypherParser::ImplicitProcedureInvocationContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitVariable(CypherParser::VariableContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLabelName(CypherParser::LabelNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelTypeName(CypherParser::RelTypeNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyKeyName(CypherParser::PropertyKeyNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureName(CypherParser::ProcedureNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitProcedureResultField(CypherParser::ProcedureResultFieldContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitFunctionName(CypherParser::FunctionNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNamespace(CypherParser::NamespaceContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSchemaName(CypherParser::SchemaNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitSymbolicName(CypherParser::SymbolicNameContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLiteral(CypherParser::LiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitBooleanLiteral(CypherParser::BooleanLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNumberLiteral(CypherParser::NumberLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIntegerLiteral(CypherParser::IntegerLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMapLiteral(CypherParser::MapLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListLiteral(CypherParser::ListLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitParameter(CypherParser::ParameterContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitListComprehension(CypherParser::ListComprehensionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReduceExpression(CypherParser::ReduceExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitShortestPathExpression(CypherParser::ShortestPathExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMapProjection(CypherParser::MapProjectionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMapProjectionElement(CypherParser::MapProjectionElementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPatternComprehension(CypherParser::PatternComprehensionContext *ctx) override {
    return visitChildren(ctx);
  }


};

