
    #include "antlr4-runtime.h"


// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"


namespace graph::parser::cypher {


class  CypherParser : public antlr4::Parser {
public:
  enum {
    T__0 = 1, T__1 = 2, T__2 = 3, T__3 = 4, K_INDEX = 5, K_ON = 6, K_MATCH = 7, 
    K_CREATE = 8, K_WHERE = 9, K_RETURN = 10, K_AS = 11, NULL_LITERAL = 12, 
    BOOLEAN = 13, ID = 14, STRING = 15, INTEGER = 16, DECIMAL = 17, WS = 18, 
    LPAREN = 19, RPAREN = 20, LBRACE = 21, RBRACE = 22, COLON = 23, COMMA = 24, 
    DOT = 25, LT = 26, GT = 27, EQ = 28, NEQ = 29, GTE = 30, LTE = 31
  };

  enum {
    RuleQuery = 0, RuleStatement = 1, RuleMatchStatement = 2, RuleReturnClause = 3, 
    RuleReturnBody = 4, RuleReturnItem = 5, RuleExpression = 6, RuleCreateStatement = 7, 
    RuleIndexDefinition = 8, RulePattern = 9, RulePatternPart = 10, RulePatternElementChain = 11, 
    RuleRelationshipPattern = 12, RuleRelationshipDetail = 13, RuleNodePattern = 14, 
    RuleWhereClause = 15, RulePropertyExpression = 16, RuleMapLiteral = 17, 
    RuleMapEntry = 18, RuleLiteral = 19
  };

  explicit CypherParser(antlr4::TokenStream *input);

  CypherParser(antlr4::TokenStream *input, const antlr4::atn::ParserATNSimulatorOptions &options);

  ~CypherParser() override;

  std::string getGrammarFileName() const override;

  const antlr4::atn::ATN& getATN() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;


  class QueryContext;
  class StatementContext;
  class MatchStatementContext;
  class ReturnClauseContext;
  class ReturnBodyContext;
  class ReturnItemContext;
  class ExpressionContext;
  class CreateStatementContext;
  class IndexDefinitionContext;
  class PatternContext;
  class PatternPartContext;
  class PatternElementChainContext;
  class RelationshipPatternContext;
  class RelationshipDetailContext;
  class NodePatternContext;
  class WhereClauseContext;
  class PropertyExpressionContext;
  class MapLiteralContext;
  class MapEntryContext;
  class LiteralContext; 

  class  QueryContext : public antlr4::ParserRuleContext {
  public:
    QueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    StatementContext *statement();
    antlr4::tree::TerminalNode *EOF();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  QueryContext* query();

  class  StatementContext : public antlr4::ParserRuleContext {
  public:
    StatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    MatchStatementContext *matchStatement();
    CreateStatementContext *createStatement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  StatementContext* statement();

  class  MatchStatementContext : public antlr4::ParserRuleContext {
  public:
    MatchStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_MATCH();
    PatternContext *pattern();
    antlr4::tree::TerminalNode *K_WHERE();
    WhereClauseContext *whereClause();
    ReturnClauseContext *returnClause();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  MatchStatementContext* matchStatement();

  class  ReturnClauseContext : public antlr4::ParserRuleContext {
  public:
    ReturnClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_RETURN();
    ReturnBodyContext *returnBody();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReturnClauseContext* returnClause();

  class  ReturnBodyContext : public antlr4::ParserRuleContext {
  public:
    ReturnBodyContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<ReturnItemContext *> returnItem();
    ReturnItemContext* returnItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReturnBodyContext* returnBody();

  class  ReturnItemContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *variable = nullptr;
    ReturnItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExpressionContext *expression();
    antlr4::tree::TerminalNode *K_AS();
    antlr4::tree::TerminalNode *ID();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReturnItemContext* returnItem();

  class  ExpressionContext : public antlr4::ParserRuleContext {
  public:
    ExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    PropertyExpressionContext *propertyExpression();
    antlr4::tree::TerminalNode *ID();
    LiteralContext *literal();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ExpressionContext* expression();

  class  CreateStatementContext : public antlr4::ParserRuleContext {
  public:
    CreateStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_CREATE();
    IndexDefinitionContext *indexDefinition();
    PatternContext *pattern();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  CreateStatementContext* createStatement();

  class  IndexDefinitionContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *label = nullptr;
    antlr4::Token *property = nullptr;
    IndexDefinitionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_INDEX();
    antlr4::tree::TerminalNode *K_ON();
    antlr4::tree::TerminalNode *COLON();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    std::vector<antlr4::tree::TerminalNode *> ID();
    antlr4::tree::TerminalNode* ID(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  IndexDefinitionContext* indexDefinition();

  class  PatternContext : public antlr4::ParserRuleContext {
  public:
    PatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<PatternPartContext *> patternPart();
    PatternPartContext* patternPart(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PatternContext* pattern();

  class  PatternPartContext : public antlr4::ParserRuleContext {
  public:
    PatternPartContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    NodePatternContext *nodePattern();
    std::vector<PatternElementChainContext *> patternElementChain();
    PatternElementChainContext* patternElementChain(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PatternPartContext* patternPart();

  class  PatternElementChainContext : public antlr4::ParserRuleContext {
  public:
    PatternElementChainContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    RelationshipPatternContext *relationshipPattern();
    NodePatternContext *nodePattern();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PatternElementChainContext* patternElementChain();

  class  RelationshipPatternContext : public antlr4::ParserRuleContext {
  public:
    RelationshipPatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    RelationshipDetailContext *relationshipDetail();
    antlr4::tree::TerminalNode *LT();
    antlr4::tree::TerminalNode *GT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RelationshipPatternContext* relationshipPattern();

  class  RelationshipDetailContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *variable = nullptr;
    antlr4::Token *label = nullptr;
    RelationshipDetailContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COLON();
    MapLiteralContext *mapLiteral();
    std::vector<antlr4::tree::TerminalNode *> ID();
    antlr4::tree::TerminalNode* ID(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RelationshipDetailContext* relationshipDetail();

  class  NodePatternContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *variable = nullptr;
    antlr4::Token *label = nullptr;
    NodePatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    antlr4::tree::TerminalNode *COLON();
    MapLiteralContext *mapLiteral();
    std::vector<antlr4::tree::TerminalNode *> ID();
    antlr4::tree::TerminalNode* ID(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NodePatternContext* nodePattern();

  class  WhereClauseContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *op = nullptr;
    WhereClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    PropertyExpressionContext *propertyExpression();
    LiteralContext *literal();
    antlr4::tree::TerminalNode *EQ();
    antlr4::tree::TerminalNode *NEQ();
    antlr4::tree::TerminalNode *GT();
    antlr4::tree::TerminalNode *LT();
    antlr4::tree::TerminalNode *GTE();
    antlr4::tree::TerminalNode *LTE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WhereClauseContext* whereClause();

  class  PropertyExpressionContext : public antlr4::ParserRuleContext {
  public:
    PropertyExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> ID();
    antlr4::tree::TerminalNode* ID(size_t i);
    antlr4::tree::TerminalNode *DOT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PropertyExpressionContext* propertyExpression();

  class  MapLiteralContext : public antlr4::ParserRuleContext {
  public:
    MapLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<MapEntryContext *> mapEntry();
    MapEntryContext* mapEntry(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  MapLiteralContext* mapLiteral();

  class  MapEntryContext : public antlr4::ParserRuleContext {
  public:
    antlr4::Token *key = nullptr;
    MapEntryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COLON();
    LiteralContext *literal();
    antlr4::tree::TerminalNode *ID();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  MapEntryContext* mapEntry();

  class  LiteralContext : public antlr4::ParserRuleContext {
  public:
    LiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *STRING();
    antlr4::tree::TerminalNode *INTEGER();
    antlr4::tree::TerminalNode *DECIMAL();
    antlr4::tree::TerminalNode *BOOLEAN();
    antlr4::tree::TerminalNode *NULL_LITERAL();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LiteralContext* literal();


  // By default the static state used to implement the parser is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:
};

}  // namespace graph::parser::cypher
