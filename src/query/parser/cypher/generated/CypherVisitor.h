
    #include "antlr4-runtime.h"


// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"
#include "CypherParser.h"


namespace graph::parser::cypher {

/**
 * This class defines an abstract visitor for a parse tree
 * produced by CypherParser.
 */
class  CypherVisitor : public antlr4::tree::AbstractParseTreeVisitor {
public:

  /**
   * Visit parse trees produced by CypherParser.
   */
    virtual std::any visitQuery(CypherParser::QueryContext *context) = 0;

    virtual std::any visitStatement(CypherParser::StatementContext *context) = 0;

    virtual std::any visitAdministrationStatement(CypherParser::AdministrationStatementContext *context) = 0;

    virtual std::any visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *context) = 0;

    virtual std::any visitDropIndexStatement(CypherParser::DropIndexStatementContext *context) = 0;

    virtual std::any visitMatchStatement(CypherParser::MatchStatementContext *context) = 0;

    virtual std::any visitReturnClause(CypherParser::ReturnClauseContext *context) = 0;

    virtual std::any visitReturnBody(CypherParser::ReturnBodyContext *context) = 0;

    virtual std::any visitReturnItem(CypherParser::ReturnItemContext *context) = 0;

    virtual std::any visitExpression(CypherParser::ExpressionContext *context) = 0;

    virtual std::any visitCreateStatement(CypherParser::CreateStatementContext *context) = 0;

    virtual std::any visitIndexDefinition(CypherParser::IndexDefinitionContext *context) = 0;

    virtual std::any visitPattern(CypherParser::PatternContext *context) = 0;

    virtual std::any visitPatternPart(CypherParser::PatternPartContext *context) = 0;

    virtual std::any visitPatternElementChain(CypherParser::PatternElementChainContext *context) = 0;

    virtual std::any visitRelationshipPattern(CypherParser::RelationshipPatternContext *context) = 0;

    virtual std::any visitRelationshipDetail(CypherParser::RelationshipDetailContext *context) = 0;

    virtual std::any visitNodePattern(CypherParser::NodePatternContext *context) = 0;

    virtual std::any visitWhereClause(CypherParser::WhereClauseContext *context) = 0;

    virtual std::any visitPropertyExpression(CypherParser::PropertyExpressionContext *context) = 0;

    virtual std::any visitMapLiteral(CypherParser::MapLiteralContext *context) = 0;

    virtual std::any visitMapEntry(CypherParser::MapEntryContext *context) = 0;

    virtual std::any visitLiteral(CypherParser::LiteralContext *context) = 0;


};

}  // namespace graph::parser::cypher
