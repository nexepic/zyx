
    #include "antlr4-runtime.h"


// Generated from CypherParser.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"
#include "CypherParser.h"


namespace graph::parser::cypher {

/**
 * This class defines an abstract visitor for a parse tree
 * produced by CypherParser.
 */
class  CypherParserVisitor : public antlr4::tree::AbstractParseTreeVisitor {
public:

  /**
   * Visit parse trees produced by CypherParser.
   */
    virtual std::any visitCypher(CypherParser::CypherContext *context) = 0;

    virtual std::any visitStatement(CypherParser::StatementContext *context) = 0;

    virtual std::any visitAdministrationStatement(CypherParser::AdministrationStatementContext *context) = 0;

    virtual std::any visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *context) = 0;

    virtual std::any visitDropIndexByName(CypherParser::DropIndexByNameContext *context) = 0;

    virtual std::any visitDropIndexByLabel(CypherParser::DropIndexByLabelContext *context) = 0;

    virtual std::any visitCreateIndexByPattern(CypherParser::CreateIndexByPatternContext *context) = 0;

    virtual std::any visitCreateIndexByLabel(CypherParser::CreateIndexByLabelContext *context) = 0;

    virtual std::any visitCreateVectorIndex(CypherParser::CreateVectorIndexContext *context) = 0;

    virtual std::any visitQuery(CypherParser::QueryContext *context) = 0;

    virtual std::any visitRegularQuery(CypherParser::RegularQueryContext *context) = 0;

    virtual std::any visitSingleQuery(CypherParser::SingleQueryContext *context) = 0;

    virtual std::any visitReadingClause(CypherParser::ReadingClauseContext *context) = 0;

    virtual std::any visitUpdatingClause(CypherParser::UpdatingClauseContext *context) = 0;

    virtual std::any visitMatchStatement(CypherParser::MatchStatementContext *context) = 0;

    virtual std::any visitUnwindStatement(CypherParser::UnwindStatementContext *context) = 0;

    virtual std::any visitInQueryCallStatement(CypherParser::InQueryCallStatementContext *context) = 0;

    virtual std::any visitCreateStatement(CypherParser::CreateStatementContext *context) = 0;

    virtual std::any visitMergeStatement(CypherParser::MergeStatementContext *context) = 0;

    virtual std::any visitSetStatement(CypherParser::SetStatementContext *context) = 0;

    virtual std::any visitSetItem(CypherParser::SetItemContext *context) = 0;

    virtual std::any visitDeleteStatement(CypherParser::DeleteStatementContext *context) = 0;

    virtual std::any visitRemoveStatement(CypherParser::RemoveStatementContext *context) = 0;

    virtual std::any visitRemoveItem(CypherParser::RemoveItemContext *context) = 0;

    virtual std::any visitReturnStatement(CypherParser::ReturnStatementContext *context) = 0;

    virtual std::any visitStandaloneCallStatement(CypherParser::StandaloneCallStatementContext *context) = 0;

    virtual std::any visitYieldItems(CypherParser::YieldItemsContext *context) = 0;

    virtual std::any visitYieldItem(CypherParser::YieldItemContext *context) = 0;

    virtual std::any visitProjectionBody(CypherParser::ProjectionBodyContext *context) = 0;

    virtual std::any visitProjectionItems(CypherParser::ProjectionItemsContext *context) = 0;

    virtual std::any visitProjectionItem(CypherParser::ProjectionItemContext *context) = 0;

    virtual std::any visitOrderStatement(CypherParser::OrderStatementContext *context) = 0;

    virtual std::any visitSkipStatement(CypherParser::SkipStatementContext *context) = 0;

    virtual std::any visitLimitStatement(CypherParser::LimitStatementContext *context) = 0;

    virtual std::any visitSortItem(CypherParser::SortItemContext *context) = 0;

    virtual std::any visitWhere(CypherParser::WhereContext *context) = 0;

    virtual std::any visitPattern(CypherParser::PatternContext *context) = 0;

    virtual std::any visitPatternPart(CypherParser::PatternPartContext *context) = 0;

    virtual std::any visitPatternElement(CypherParser::PatternElementContext *context) = 0;

    virtual std::any visitPatternElementChain(CypherParser::PatternElementChainContext *context) = 0;

    virtual std::any visitNodePattern(CypherParser::NodePatternContext *context) = 0;

    virtual std::any visitRelationshipPattern(CypherParser::RelationshipPatternContext *context) = 0;

    virtual std::any visitRelationshipDetail(CypherParser::RelationshipDetailContext *context) = 0;

    virtual std::any visitProperties(CypherParser::PropertiesContext *context) = 0;

    virtual std::any visitNodeLabels(CypherParser::NodeLabelsContext *context) = 0;

    virtual std::any visitNodeLabel(CypherParser::NodeLabelContext *context) = 0;

    virtual std::any visitRelationshipTypes(CypherParser::RelationshipTypesContext *context) = 0;

    virtual std::any visitRangeLiteral(CypherParser::RangeLiteralContext *context) = 0;

    virtual std::any visitExpression(CypherParser::ExpressionContext *context) = 0;

    virtual std::any visitOrExpression(CypherParser::OrExpressionContext *context) = 0;

    virtual std::any visitXorExpression(CypherParser::XorExpressionContext *context) = 0;

    virtual std::any visitAndExpression(CypherParser::AndExpressionContext *context) = 0;

    virtual std::any visitNotExpression(CypherParser::NotExpressionContext *context) = 0;

    virtual std::any visitComparisonExpression(CypherParser::ComparisonExpressionContext *context) = 0;

    virtual std::any visitArithmeticExpression(CypherParser::ArithmeticExpressionContext *context) = 0;

    virtual std::any visitUnaryExpression(CypherParser::UnaryExpressionContext *context) = 0;

    virtual std::any visitAccessor(CypherParser::AccessorContext *context) = 0;

    virtual std::any visitAtom(CypherParser::AtomContext *context) = 0;

    virtual std::any visitPropertyExpression(CypherParser::PropertyExpressionContext *context) = 0;

    virtual std::any visitFunctionInvocation(CypherParser::FunctionInvocationContext *context) = 0;

    virtual std::any visitExplicitProcedureInvocation(CypherParser::ExplicitProcedureInvocationContext *context) = 0;

    virtual std::any visitImplicitProcedureInvocation(CypherParser::ImplicitProcedureInvocationContext *context) = 0;

    virtual std::any visitVariable(CypherParser::VariableContext *context) = 0;

    virtual std::any visitLabelName(CypherParser::LabelNameContext *context) = 0;

    virtual std::any visitRelTypeName(CypherParser::RelTypeNameContext *context) = 0;

    virtual std::any visitPropertyKeyName(CypherParser::PropertyKeyNameContext *context) = 0;

    virtual std::any visitProcedureName(CypherParser::ProcedureNameContext *context) = 0;

    virtual std::any visitProcedureResultField(CypherParser::ProcedureResultFieldContext *context) = 0;

    virtual std::any visitFunctionName(CypherParser::FunctionNameContext *context) = 0;

    virtual std::any visitNamespace(CypherParser::NamespaceContext *context) = 0;

    virtual std::any visitSchemaName(CypherParser::SchemaNameContext *context) = 0;

    virtual std::any visitSymbolicName(CypherParser::SymbolicNameContext *context) = 0;

    virtual std::any visitLiteral(CypherParser::LiteralContext *context) = 0;

    virtual std::any visitBooleanLiteral(CypherParser::BooleanLiteralContext *context) = 0;

    virtual std::any visitNumberLiteral(CypherParser::NumberLiteralContext *context) = 0;

    virtual std::any visitIntegerLiteral(CypherParser::IntegerLiteralContext *context) = 0;

    virtual std::any visitMapLiteral(CypherParser::MapLiteralContext *context) = 0;

    virtual std::any visitListLiteral(CypherParser::ListLiteralContext *context) = 0;

    virtual std::any visitParameter(CypherParser::ParameterContext *context) = 0;


};

}  // namespace graph::parser::cypher
