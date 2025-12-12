
    #include "antlr4-runtime.h"


// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"
#include "CypherVisitor.h"


namespace graph::parser::cypher {

/**
 * This class provides an empty implementation of CypherVisitor, which can be
 * extended to create a visitor which only needs to handle a subset of the available methods.
 */
class  CypherBaseVisitor : public CypherVisitor {
public:

  virtual std::any visitQuery(CypherParser::QueryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitStatement(CypherParser::StatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitAdministrationStatement(CypherParser::AdministrationStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitDropIndexStatement(CypherParser::DropIndexStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMatchStatement(CypherParser::MatchStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnClause(CypherParser::ReturnClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnBody(CypherParser::ReturnBodyContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitReturnItem(CypherParser::ReturnItemContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitExpression(CypherParser::ExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitCreateStatement(CypherParser::CreateStatementContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitIndexDefinition(CypherParser::IndexDefinitionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPattern(CypherParser::PatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPatternPart(CypherParser::PatternPartContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPatternElementChain(CypherParser::PatternElementChainContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelationshipPattern(CypherParser::RelationshipPatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitRelationshipDetail(CypherParser::RelationshipDetailContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitNodePattern(CypherParser::NodePatternContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitWhereClause(CypherParser::WhereClauseContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitPropertyExpression(CypherParser::PropertyExpressionContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMapLiteral(CypherParser::MapLiteralContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitMapEntry(CypherParser::MapEntryContext *ctx) override {
    return visitChildren(ctx);
  }

  virtual std::any visitLiteral(CypherParser::LiteralContext *ctx) override {
    return visitChildren(ctx);
  }


};

}  // namespace graph::parser::cypher
