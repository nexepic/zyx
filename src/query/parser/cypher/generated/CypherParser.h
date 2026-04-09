
    #include "antlr4-runtime.h"


// Generated from CypherParser.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"




class  CypherParser : public antlr4::Parser {
public:
  enum {
    K_CALL = 1, K_YIELD = 2, K_CREATE = 3, K_DELETE = 4, K_DETACH = 5, K_EXISTS = 6, 
    K_LIMIT = 7, K_MATCH = 8, K_MERGE = 9, K_OPTIONAL = 10, K_ORDER = 11, 
    K_BY = 12, K_SKIP = 13, K_ASCENDING = 14, K_ASC = 15, K_DESCENDING = 16, 
    K_DESC = 17, K_REMOVE = 18, K_RETURN = 19, K_SET = 20, K_WHERE = 21, 
    K_WITH = 22, K_UNION = 23, K_UNWIND = 24, K_AND = 25, K_AS = 26, K_CONTAINS = 27, 
    K_DISTINCT = 28, K_ENDS = 29, K_IN = 30, K_IS = 31, K_NOT = 32, K_OR = 33, 
    K_STARTS = 34, K_XOR = 35, K_BETWEEN = 36, K_FALSE = 37, K_TRUE = 38, 
    K_NULL = 39, K_CASE = 40, K_WHEN = 41, K_THEN = 42, K_ELSE = 43, K_END = 44, 
    K_COUNT = 45, K_FILTER = 46, K_EXTRACT = 47, K_ANY = 48, K_NONE = 49, 
    K_SINGLE = 50, K_ALL = 51, K_REDUCE = 52, K_INDEX = 53, K_ON = 54, K_SHOW = 55, 
    K_DROP = 56, K_IF = 57, K_FOR = 58, K_CONSTRAINT = 59, K_DO = 60, K_REQUIRE = 61, 
    K_UNIQUE = 62, K_MANDATORY = 63, K_SCALAR = 64, K_OF = 65, K_ADD = 66, 
    K_BEGIN = 67, K_COMMIT = 68, K_ROLLBACK = 69, K_TRANSACTION = 70, K_KEY = 71, 
    K_NODE = 72, K_BOOLEAN = 73, K_INTEGER = 74, K_FLOAT = 75, K_STRING = 76, 
    K_LIST = 77, K_MAP = 78, K_VECTOR = 79, K_OPTIONS = 80, K_FOREACH = 81, 
    K_LOAD = 82, K_CSV = 83, K_HEADERS = 84, K_FROM = 85, K_FIELDTERMINATOR = 86, 
    K_TRANSACTIONS = 87, K_ROWS = 88, K_EXPLAIN = 89, K_PROFILE = 90, EQ = 91, 
    NEQ = 92, LT = 93, GT = 94, LTE = 95, GTE = 96, PLUS = 97, MINUS = 98, 
    MULTIPLY = 99, DIVIDE = 100, MODULO = 101, POWER = 102, LPAREN = 103, 
    RPAREN = 104, LBRACE = 105, RBRACE = 106, LBRACK = 107, RBRACK = 108, 
    COMMA = 109, DOT = 110, COLON = 111, PIPE = 112, DOLLAR = 113, RANGE = 114, 
    SEMI = 115, HexInteger = 116, OctalInteger = 117, DecimalInteger = 118, 
    DoubleLiteral = 119, ID = 120, StringLiteral = 121, WS = 122, COMMENT = 123, 
    LINE_COMMENT = 124
  };

  enum {
    RuleCypher = 0, RuleStatement = 1, RuleAdministrationStatement = 2, 
    RuleTransactionStatement = 3, RuleShowIndexesStatement = 4, RuleDropIndexStatement = 5, 
    RuleCreateIndexStatement = 6, RuleCreateConstraintStatement = 7, RuleConstraintNodePattern = 8, 
    RuleConstraintEdgePattern = 9, RuleConstraintBody = 10, RuleConstraintTypeName = 11, 
    RuleDropConstraintStatement = 12, RuleShowConstraintsStatement = 13, 
    RuleQuery = 14, RuleRegularQuery = 15, RuleSingleQuery = 16, RuleReadingClause = 17, 
    RuleUpdatingClause = 18, RuleWithClause = 19, RuleMatchStatement = 20, 
    RuleUnwindStatement = 21, RuleInQueryCallStatement = 22, RuleCreateStatement = 23, 
    RuleMergeStatement = 24, RuleSetStatement = 25, RuleSetItem = 26, RuleDeleteStatement = 27, 
    RuleRemoveStatement = 28, RuleRemoveItem = 29, RuleForeachStatement = 30, 
    RuleCallSubquery = 31, RuleInTransactionsClause = 32, RuleLoadCsvStatement = 33, 
    RuleReturnStatement = 34, RuleStandaloneCallStatement = 35, RuleYieldItems = 36, 
    RuleYieldItem = 37, RuleProjectionBody = 38, RuleProjectionItems = 39, 
    RuleProjectionItem = 40, RuleOrderStatement = 41, RuleSkipStatement = 42, 
    RuleLimitStatement = 43, RuleSortItem = 44, RuleWhere = 45, RulePattern = 46, 
    RulePatternPart = 47, RulePatternElement = 48, RulePatternElementChain = 49, 
    RuleNodePattern = 50, RuleRelationshipPattern = 51, RuleRelationshipDetail = 52, 
    RuleProperties = 53, RuleNodeLabels = 54, RuleNodeLabel = 55, RuleRelationshipTypes = 56, 
    RuleRangeLiteral = 57, RuleExpression = 58, RuleOrExpression = 59, RuleXorExpression = 60, 
    RuleAndExpression = 61, RuleNotExpression = 62, RuleComparisonExpression = 63, 
    RuleArithmeticExpression = 64, RulePowerExpression = 65, RuleUnaryExpression = 66, 
    RuleAccessor = 67, RuleAtom = 68, RuleQuantifierExpression = 69, RuleExistsExpression = 70, 
    RuleCaseExpression = 71, RulePropertyExpression = 72, RuleFunctionInvocation = 73, 
    RuleExplicitProcedureInvocation = 74, RuleImplicitProcedureInvocation = 75, 
    RuleVariable = 76, RuleLabelName = 77, RuleRelTypeName = 78, RulePropertyKeyName = 79, 
    RuleProcedureName = 80, RuleProcedureResultField = 81, RuleFunctionName = 82, 
    RuleNamespace = 83, RuleSchemaName = 84, RuleSymbolicName = 85, RuleLiteral = 86, 
    RuleBooleanLiteral = 87, RuleNumberLiteral = 88, RuleIntegerLiteral = 89, 
    RuleMapLiteral = 90, RuleListLiteral = 91, RuleParameter = 92, RuleListComprehension = 93, 
    RuleReduceExpression = 94, RulePatternComprehension = 95
  };

  explicit CypherParser(antlr4::TokenStream *input);

  CypherParser(antlr4::TokenStream *input, const antlr4::atn::ParserATNSimulatorOptions &options);

  ~CypherParser() override;

  std::string getGrammarFileName() const override;

  const antlr4::atn::ATN& getATN() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;


  class CypherContext;
  class StatementContext;
  class AdministrationStatementContext;
  class TransactionStatementContext;
  class ShowIndexesStatementContext;
  class DropIndexStatementContext;
  class CreateIndexStatementContext;
  class CreateConstraintStatementContext;
  class ConstraintNodePatternContext;
  class ConstraintEdgePatternContext;
  class ConstraintBodyContext;
  class ConstraintTypeNameContext;
  class DropConstraintStatementContext;
  class ShowConstraintsStatementContext;
  class QueryContext;
  class RegularQueryContext;
  class SingleQueryContext;
  class ReadingClauseContext;
  class UpdatingClauseContext;
  class WithClauseContext;
  class MatchStatementContext;
  class UnwindStatementContext;
  class InQueryCallStatementContext;
  class CreateStatementContext;
  class MergeStatementContext;
  class SetStatementContext;
  class SetItemContext;
  class DeleteStatementContext;
  class RemoveStatementContext;
  class RemoveItemContext;
  class ForeachStatementContext;
  class CallSubqueryContext;
  class InTransactionsClauseContext;
  class LoadCsvStatementContext;
  class ReturnStatementContext;
  class StandaloneCallStatementContext;
  class YieldItemsContext;
  class YieldItemContext;
  class ProjectionBodyContext;
  class ProjectionItemsContext;
  class ProjectionItemContext;
  class OrderStatementContext;
  class SkipStatementContext;
  class LimitStatementContext;
  class SortItemContext;
  class WhereContext;
  class PatternContext;
  class PatternPartContext;
  class PatternElementContext;
  class PatternElementChainContext;
  class NodePatternContext;
  class RelationshipPatternContext;
  class RelationshipDetailContext;
  class PropertiesContext;
  class NodeLabelsContext;
  class NodeLabelContext;
  class RelationshipTypesContext;
  class RangeLiteralContext;
  class ExpressionContext;
  class OrExpressionContext;
  class XorExpressionContext;
  class AndExpressionContext;
  class NotExpressionContext;
  class ComparisonExpressionContext;
  class ArithmeticExpressionContext;
  class PowerExpressionContext;
  class UnaryExpressionContext;
  class AccessorContext;
  class AtomContext;
  class QuantifierExpressionContext;
  class ExistsExpressionContext;
  class CaseExpressionContext;
  class PropertyExpressionContext;
  class FunctionInvocationContext;
  class ExplicitProcedureInvocationContext;
  class ImplicitProcedureInvocationContext;
  class VariableContext;
  class LabelNameContext;
  class RelTypeNameContext;
  class PropertyKeyNameContext;
  class ProcedureNameContext;
  class ProcedureResultFieldContext;
  class FunctionNameContext;
  class NamespaceContext;
  class SchemaNameContext;
  class SymbolicNameContext;
  class LiteralContext;
  class BooleanLiteralContext;
  class NumberLiteralContext;
  class IntegerLiteralContext;
  class MapLiteralContext;
  class ListLiteralContext;
  class ParameterContext;
  class ListComprehensionContext;
  class ReduceExpressionContext;
  class PatternComprehensionContext; 

  class  CypherContext : public antlr4::ParserRuleContext {
  public:
    CypherContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    StatementContext *statement();
    antlr4::tree::TerminalNode *EOF();
    antlr4::tree::TerminalNode *SEMI();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  CypherContext* cypher();

  class  StatementContext : public antlr4::ParserRuleContext {
  public:
    StatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    StatementContext() = default;
    void copyFrom(StatementContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  ProfileStatementContext : public StatementContext {
  public:
    ProfileStatementContext(StatementContext *ctx);

    antlr4::tree::TerminalNode *K_PROFILE();
    QueryContext *query();
    AdministrationStatementContext *administrationStatement();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  ExplainStatementContext : public StatementContext {
  public:
    ExplainStatementContext(StatementContext *ctx);

    antlr4::tree::TerminalNode *K_EXPLAIN();
    QueryContext *query();
    AdministrationStatementContext *administrationStatement();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  AdminStatementContext : public StatementContext {
  public:
    AdminStatementContext(StatementContext *ctx);

    AdministrationStatementContext *administrationStatement();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  RegularStatementContext : public StatementContext {
  public:
    RegularStatementContext(StatementContext *ctx);

    QueryContext *query();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  StatementContext* statement();

  class  AdministrationStatementContext : public antlr4::ParserRuleContext {
  public:
    AdministrationStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ShowIndexesStatementContext *showIndexesStatement();
    DropIndexStatementContext *dropIndexStatement();
    CreateIndexStatementContext *createIndexStatement();
    CreateConstraintStatementContext *createConstraintStatement();
    DropConstraintStatementContext *dropConstraintStatement();
    ShowConstraintsStatementContext *showConstraintsStatement();
    TransactionStatementContext *transactionStatement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AdministrationStatementContext* administrationStatement();

  class  TransactionStatementContext : public antlr4::ParserRuleContext {
  public:
    TransactionStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    TransactionStatementContext() = default;
    void copyFrom(TransactionStatementContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  TxnRollbackContext : public TransactionStatementContext {
  public:
    TxnRollbackContext(TransactionStatementContext *ctx);

    antlr4::tree::TerminalNode *K_ROLLBACK();
    antlr4::tree::TerminalNode *K_TRANSACTION();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  TxnCommitContext : public TransactionStatementContext {
  public:
    TxnCommitContext(TransactionStatementContext *ctx);

    antlr4::tree::TerminalNode *K_COMMIT();
    antlr4::tree::TerminalNode *K_TRANSACTION();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  TxnBeginContext : public TransactionStatementContext {
  public:
    TxnBeginContext(TransactionStatementContext *ctx);

    antlr4::tree::TerminalNode *K_BEGIN();
    antlr4::tree::TerminalNode *K_TRANSACTION();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  TransactionStatementContext* transactionStatement();

  class  ShowIndexesStatementContext : public antlr4::ParserRuleContext {
  public:
    ShowIndexesStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_SHOW();
    antlr4::tree::TerminalNode *K_INDEX();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ShowIndexesStatementContext* showIndexesStatement();

  class  DropIndexStatementContext : public antlr4::ParserRuleContext {
  public:
    DropIndexStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    DropIndexStatementContext() = default;
    void copyFrom(DropIndexStatementContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  DropIndexByNameContext : public DropIndexStatementContext {
  public:
    DropIndexByNameContext(DropIndexStatementContext *ctx);

    antlr4::tree::TerminalNode *K_DROP();
    antlr4::tree::TerminalNode *K_INDEX();
    SymbolicNameContext *symbolicName();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  DropIndexByLabelContext : public DropIndexStatementContext {
  public:
    DropIndexByLabelContext(DropIndexStatementContext *ctx);

    antlr4::tree::TerminalNode *K_DROP();
    antlr4::tree::TerminalNode *K_INDEX();
    antlr4::tree::TerminalNode *K_ON();
    NodeLabelContext *nodeLabel();
    antlr4::tree::TerminalNode *LPAREN();
    PropertyKeyNameContext *propertyKeyName();
    antlr4::tree::TerminalNode *RPAREN();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  DropIndexStatementContext* dropIndexStatement();

  class  CreateIndexStatementContext : public antlr4::ParserRuleContext {
  public:
    CreateIndexStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    CreateIndexStatementContext() = default;
    void copyFrom(CreateIndexStatementContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  CreateIndexByPatternContext : public CreateIndexStatementContext {
  public:
    CreateIndexByPatternContext(CreateIndexStatementContext *ctx);

    antlr4::tree::TerminalNode *K_CREATE();
    antlr4::tree::TerminalNode *K_INDEX();
    antlr4::tree::TerminalNode *K_FOR();
    NodePatternContext *nodePattern();
    antlr4::tree::TerminalNode *K_ON();
    antlr4::tree::TerminalNode *LPAREN();
    std::vector<PropertyExpressionContext *> propertyExpression();
    PropertyExpressionContext* propertyExpression(size_t i);
    antlr4::tree::TerminalNode *RPAREN();
    SymbolicNameContext *symbolicName();
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  CreateIndexByLabelContext : public CreateIndexStatementContext {
  public:
    CreateIndexByLabelContext(CreateIndexStatementContext *ctx);

    antlr4::tree::TerminalNode *K_CREATE();
    antlr4::tree::TerminalNode *K_INDEX();
    antlr4::tree::TerminalNode *K_ON();
    NodeLabelContext *nodeLabel();
    antlr4::tree::TerminalNode *LPAREN();
    PropertyKeyNameContext *propertyKeyName();
    antlr4::tree::TerminalNode *RPAREN();
    SymbolicNameContext *symbolicName();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  CreateVectorIndexContext : public CreateIndexStatementContext {
  public:
    CreateVectorIndexContext(CreateIndexStatementContext *ctx);

    antlr4::tree::TerminalNode *K_CREATE();
    antlr4::tree::TerminalNode *K_VECTOR();
    antlr4::tree::TerminalNode *K_INDEX();
    antlr4::tree::TerminalNode *K_ON();
    NodeLabelContext *nodeLabel();
    antlr4::tree::TerminalNode *LPAREN();
    PropertyKeyNameContext *propertyKeyName();
    antlr4::tree::TerminalNode *RPAREN();
    SymbolicNameContext *symbolicName();
    antlr4::tree::TerminalNode *K_OPTIONS();
    MapLiteralContext *mapLiteral();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  CreateIndexStatementContext* createIndexStatement();

  class  CreateConstraintStatementContext : public antlr4::ParserRuleContext {
  public:
    CreateConstraintStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    CreateConstraintStatementContext() = default;
    void copyFrom(CreateConstraintStatementContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  CreateNodeConstraintContext : public CreateConstraintStatementContext {
  public:
    CreateNodeConstraintContext(CreateConstraintStatementContext *ctx);

    antlr4::tree::TerminalNode *K_CREATE();
    antlr4::tree::TerminalNode *K_CONSTRAINT();
    SymbolicNameContext *symbolicName();
    antlr4::tree::TerminalNode *K_FOR();
    ConstraintNodePatternContext *constraintNodePattern();
    antlr4::tree::TerminalNode *K_REQUIRE();
    ConstraintBodyContext *constraintBody();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  CreateEdgeConstraintContext : public CreateConstraintStatementContext {
  public:
    CreateEdgeConstraintContext(CreateConstraintStatementContext *ctx);

    antlr4::tree::TerminalNode *K_CREATE();
    antlr4::tree::TerminalNode *K_CONSTRAINT();
    SymbolicNameContext *symbolicName();
    antlr4::tree::TerminalNode *K_FOR();
    ConstraintEdgePatternContext *constraintEdgePattern();
    antlr4::tree::TerminalNode *K_REQUIRE();
    ConstraintBodyContext *constraintBody();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  CreateConstraintStatementContext* createConstraintStatement();

  class  ConstraintNodePatternContext : public antlr4::ParserRuleContext {
  public:
    ConstraintNodePatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LPAREN();
    VariableContext *variable();
    antlr4::tree::TerminalNode *COLON();
    LabelNameContext *labelName();
    antlr4::tree::TerminalNode *RPAREN();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ConstraintNodePatternContext* constraintNodePattern();

  class  ConstraintEdgePatternContext : public antlr4::ParserRuleContext {
  public:
    ConstraintEdgePatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> LPAREN();
    antlr4::tree::TerminalNode* LPAREN(size_t i);
    std::vector<antlr4::tree::TerminalNode *> RPAREN();
    antlr4::tree::TerminalNode* RPAREN(size_t i);
    std::vector<antlr4::tree::TerminalNode *> MINUS();
    antlr4::tree::TerminalNode* MINUS(size_t i);
    antlr4::tree::TerminalNode *LBRACK();
    VariableContext *variable();
    antlr4::tree::TerminalNode *COLON();
    LabelNameContext *labelName();
    antlr4::tree::TerminalNode *RBRACK();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ConstraintEdgePatternContext* constraintEdgePattern();

  class  ConstraintBodyContext : public antlr4::ParserRuleContext {
  public:
    ConstraintBodyContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    ConstraintBodyContext() = default;
    void copyFrom(ConstraintBodyContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  NodeKeyConstraintBodyContext : public ConstraintBodyContext {
  public:
    NodeKeyConstraintBodyContext(ConstraintBodyContext *ctx);

    antlr4::tree::TerminalNode *LPAREN();
    std::vector<PropertyExpressionContext *> propertyExpression();
    PropertyExpressionContext* propertyExpression(size_t i);
    antlr4::tree::TerminalNode *RPAREN();
    antlr4::tree::TerminalNode *K_IS();
    antlr4::tree::TerminalNode *K_NODE();
    antlr4::tree::TerminalNode *K_KEY();
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  UniqueConstraintBodyContext : public ConstraintBodyContext {
  public:
    UniqueConstraintBodyContext(ConstraintBodyContext *ctx);

    PropertyExpressionContext *propertyExpression();
    antlr4::tree::TerminalNode *K_IS();
    antlr4::tree::TerminalNode *K_UNIQUE();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  PropertyTypeConstraintBodyContext : public ConstraintBodyContext {
  public:
    PropertyTypeConstraintBodyContext(ConstraintBodyContext *ctx);

    PropertyExpressionContext *propertyExpression();
    antlr4::tree::TerminalNode *K_IS();
    std::vector<antlr4::tree::TerminalNode *> COLON();
    antlr4::tree::TerminalNode* COLON(size_t i);
    ConstraintTypeNameContext *constraintTypeName();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  NotNullConstraintBodyContext : public ConstraintBodyContext {
  public:
    NotNullConstraintBodyContext(ConstraintBodyContext *ctx);

    PropertyExpressionContext *propertyExpression();
    antlr4::tree::TerminalNode *K_IS();
    antlr4::tree::TerminalNode *K_NOT();
    antlr4::tree::TerminalNode *K_NULL();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  ConstraintBodyContext* constraintBody();

  class  ConstraintTypeNameContext : public antlr4::ParserRuleContext {
  public:
    ConstraintTypeNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_BOOLEAN();
    antlr4::tree::TerminalNode *K_INTEGER();
    antlr4::tree::TerminalNode *K_FLOAT();
    antlr4::tree::TerminalNode *K_STRING();
    antlr4::tree::TerminalNode *K_LIST();
    antlr4::tree::TerminalNode *K_MAP();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ConstraintTypeNameContext* constraintTypeName();

  class  DropConstraintStatementContext : public antlr4::ParserRuleContext {
  public:
    DropConstraintStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
   
    DropConstraintStatementContext() = default;
    void copyFrom(DropConstraintStatementContext *context);
    using antlr4::ParserRuleContext::copyFrom;

    virtual size_t getRuleIndex() const override;

   
  };

  class  DropConstraintIfExistsContext : public DropConstraintStatementContext {
  public:
    DropConstraintIfExistsContext(DropConstraintStatementContext *ctx);

    antlr4::tree::TerminalNode *K_DROP();
    antlr4::tree::TerminalNode *K_CONSTRAINT();
    SymbolicNameContext *symbolicName();
    antlr4::tree::TerminalNode *K_IF();
    antlr4::tree::TerminalNode *K_EXISTS();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  class  DropConstraintByNameContext : public DropConstraintStatementContext {
  public:
    DropConstraintByNameContext(DropConstraintStatementContext *ctx);

    antlr4::tree::TerminalNode *K_DROP();
    antlr4::tree::TerminalNode *K_CONSTRAINT();
    SymbolicNameContext *symbolicName();

    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
  };

  DropConstraintStatementContext* dropConstraintStatement();

  class  ShowConstraintsStatementContext : public antlr4::ParserRuleContext {
  public:
    ShowConstraintsStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_SHOW();
    antlr4::tree::TerminalNode *K_CONSTRAINT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ShowConstraintsStatementContext* showConstraintsStatement();

  class  QueryContext : public antlr4::ParserRuleContext {
  public:
    QueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    RegularQueryContext *regularQuery();
    StandaloneCallStatementContext *standaloneCallStatement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  QueryContext* query();

  class  RegularQueryContext : public antlr4::ParserRuleContext {
  public:
    RegularQueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<SingleQueryContext *> singleQuery();
    SingleQueryContext* singleQuery(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_UNION();
    antlr4::tree::TerminalNode* K_UNION(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_ALL();
    antlr4::tree::TerminalNode* K_ALL(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RegularQueryContext* regularQuery();

  class  SingleQueryContext : public antlr4::ParserRuleContext {
  public:
    SingleQueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ReturnStatementContext *returnStatement();
    std::vector<WithClauseContext *> withClause();
    WithClauseContext* withClause(size_t i);
    std::vector<ReadingClauseContext *> readingClause();
    ReadingClauseContext* readingClause(size_t i);
    std::vector<UpdatingClauseContext *> updatingClause();
    UpdatingClauseContext* updatingClause(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SingleQueryContext* singleQuery();

  class  ReadingClauseContext : public antlr4::ParserRuleContext {
  public:
    ReadingClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    MatchStatementContext *matchStatement();
    UnwindStatementContext *unwindStatement();
    CallSubqueryContext *callSubquery();
    InQueryCallStatementContext *inQueryCallStatement();
    LoadCsvStatementContext *loadCsvStatement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReadingClauseContext* readingClause();

  class  UpdatingClauseContext : public antlr4::ParserRuleContext {
  public:
    UpdatingClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    CreateStatementContext *createStatement();
    MergeStatementContext *mergeStatement();
    DeleteStatementContext *deleteStatement();
    SetStatementContext *setStatement();
    RemoveStatementContext *removeStatement();
    ForeachStatementContext *foreachStatement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  UpdatingClauseContext* updatingClause();

  class  WithClauseContext : public antlr4::ParserRuleContext {
  public:
    WithClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_WITH();
    ProjectionBodyContext *projectionBody();
    antlr4::tree::TerminalNode *K_WHERE();
    WhereContext *where();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WithClauseContext* withClause();

  class  MatchStatementContext : public antlr4::ParserRuleContext {
  public:
    MatchStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_MATCH();
    PatternContext *pattern();
    antlr4::tree::TerminalNode *K_OPTIONAL();
    antlr4::tree::TerminalNode *K_WHERE();
    WhereContext *where();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  MatchStatementContext* matchStatement();

  class  UnwindStatementContext : public antlr4::ParserRuleContext {
  public:
    UnwindStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_UNWIND();
    ExpressionContext *expression();
    antlr4::tree::TerminalNode *K_AS();
    VariableContext *variable();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  UnwindStatementContext* unwindStatement();

  class  InQueryCallStatementContext : public antlr4::ParserRuleContext {
  public:
    InQueryCallStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_CALL();
    ExplicitProcedureInvocationContext *explicitProcedureInvocation();
    antlr4::tree::TerminalNode *K_YIELD();
    YieldItemsContext *yieldItems();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  InQueryCallStatementContext* inQueryCallStatement();

  class  CreateStatementContext : public antlr4::ParserRuleContext {
  public:
    CreateStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_CREATE();
    PatternContext *pattern();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  CreateStatementContext* createStatement();

  class  MergeStatementContext : public antlr4::ParserRuleContext {
  public:
    MergeStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_MERGE();
    PatternPartContext *patternPart();
    std::vector<antlr4::tree::TerminalNode *> K_ON();
    antlr4::tree::TerminalNode* K_ON(size_t i);
    std::vector<SetStatementContext *> setStatement();
    SetStatementContext* setStatement(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_MATCH();
    antlr4::tree::TerminalNode* K_MATCH(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_CREATE();
    antlr4::tree::TerminalNode* K_CREATE(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  MergeStatementContext* mergeStatement();

  class  SetStatementContext : public antlr4::ParserRuleContext {
  public:
    SetStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_SET();
    std::vector<SetItemContext *> setItem();
    SetItemContext* setItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SetStatementContext* setStatement();

  class  SetItemContext : public antlr4::ParserRuleContext {
  public:
    SetItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    PropertyExpressionContext *propertyExpression();
    antlr4::tree::TerminalNode *EQ();
    ExpressionContext *expression();
    VariableContext *variable();
    antlr4::tree::TerminalNode *PLUS();
    NodeLabelsContext *nodeLabels();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SetItemContext* setItem();

  class  DeleteStatementContext : public antlr4::ParserRuleContext {
  public:
    DeleteStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_DELETE();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    antlr4::tree::TerminalNode *K_DETACH();
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  DeleteStatementContext* deleteStatement();

  class  RemoveStatementContext : public antlr4::ParserRuleContext {
  public:
    RemoveStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_REMOVE();
    std::vector<RemoveItemContext *> removeItem();
    RemoveItemContext* removeItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RemoveStatementContext* removeStatement();

  class  RemoveItemContext : public antlr4::ParserRuleContext {
  public:
    RemoveItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    VariableContext *variable();
    NodeLabelsContext *nodeLabels();
    PropertyExpressionContext *propertyExpression();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RemoveItemContext* removeItem();

  class  ForeachStatementContext : public antlr4::ParserRuleContext {
  public:
    ForeachStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_FOREACH();
    antlr4::tree::TerminalNode *LPAREN();
    VariableContext *variable();
    antlr4::tree::TerminalNode *K_IN();
    ExpressionContext *expression();
    antlr4::tree::TerminalNode *PIPE();
    antlr4::tree::TerminalNode *RPAREN();
    std::vector<UpdatingClauseContext *> updatingClause();
    UpdatingClauseContext* updatingClause(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ForeachStatementContext* foreachStatement();

  class  CallSubqueryContext : public antlr4::ParserRuleContext {
  public:
    CallSubqueryContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_CALL();
    antlr4::tree::TerminalNode *LBRACE();
    SingleQueryContext *singleQuery();
    antlr4::tree::TerminalNode *RBRACE();
    InTransactionsClauseContext *inTransactionsClause();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  CallSubqueryContext* callSubquery();

  class  InTransactionsClauseContext : public antlr4::ParserRuleContext {
  public:
    InTransactionsClauseContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_IN();
    antlr4::tree::TerminalNode *K_TRANSACTIONS();
    antlr4::tree::TerminalNode *K_OF();
    ExpressionContext *expression();
    antlr4::tree::TerminalNode *K_ROWS();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  InTransactionsClauseContext* inTransactionsClause();

  class  LoadCsvStatementContext : public antlr4::ParserRuleContext {
  public:
    LoadCsvStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_LOAD();
    antlr4::tree::TerminalNode *K_CSV();
    antlr4::tree::TerminalNode *K_FROM();
    ExpressionContext *expression();
    antlr4::tree::TerminalNode *K_AS();
    VariableContext *variable();
    antlr4::tree::TerminalNode *K_WITH();
    antlr4::tree::TerminalNode *K_HEADERS();
    antlr4::tree::TerminalNode *K_FIELDTERMINATOR();
    antlr4::tree::TerminalNode *StringLiteral();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LoadCsvStatementContext* loadCsvStatement();

  class  ReturnStatementContext : public antlr4::ParserRuleContext {
  public:
    ReturnStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_RETURN();
    ProjectionBodyContext *projectionBody();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReturnStatementContext* returnStatement();

  class  StandaloneCallStatementContext : public antlr4::ParserRuleContext {
  public:
    StandaloneCallStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_CALL();
    ExplicitProcedureInvocationContext *explicitProcedureInvocation();
    ImplicitProcedureInvocationContext *implicitProcedureInvocation();
    antlr4::tree::TerminalNode *K_YIELD();
    antlr4::tree::TerminalNode *MULTIPLY();
    YieldItemsContext *yieldItems();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  StandaloneCallStatementContext* standaloneCallStatement();

  class  YieldItemsContext : public antlr4::ParserRuleContext {
  public:
    YieldItemsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<YieldItemContext *> yieldItem();
    YieldItemContext* yieldItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);
    antlr4::tree::TerminalNode *K_WHERE();
    WhereContext *where();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  YieldItemsContext* yieldItems();

  class  YieldItemContext : public antlr4::ParserRuleContext {
  public:
    YieldItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    VariableContext *variable();
    ProcedureResultFieldContext *procedureResultField();
    antlr4::tree::TerminalNode *K_AS();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  YieldItemContext* yieldItem();

  class  ProjectionBodyContext : public antlr4::ParserRuleContext {
  public:
    ProjectionBodyContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ProjectionItemsContext *projectionItems();
    antlr4::tree::TerminalNode *K_DISTINCT();
    OrderStatementContext *orderStatement();
    SkipStatementContext *skipStatement();
    LimitStatementContext *limitStatement();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ProjectionBodyContext* projectionBody();

  class  ProjectionItemsContext : public antlr4::ParserRuleContext {
  public:
    ProjectionItemsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MULTIPLY();
    std::vector<ProjectionItemContext *> projectionItem();
    ProjectionItemContext* projectionItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ProjectionItemsContext* projectionItems();

  class  ProjectionItemContext : public antlr4::ParserRuleContext {
  public:
    ProjectionItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExpressionContext *expression();
    antlr4::tree::TerminalNode *K_AS();
    VariableContext *variable();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ProjectionItemContext* projectionItem();

  class  OrderStatementContext : public antlr4::ParserRuleContext {
  public:
    OrderStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_ORDER();
    antlr4::tree::TerminalNode *K_BY();
    std::vector<SortItemContext *> sortItem();
    SortItemContext* sortItem(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  OrderStatementContext* orderStatement();

  class  SkipStatementContext : public antlr4::ParserRuleContext {
  public:
    SkipStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_SKIP();
    ExpressionContext *expression();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SkipStatementContext* skipStatement();

  class  LimitStatementContext : public antlr4::ParserRuleContext {
  public:
    LimitStatementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_LIMIT();
    ExpressionContext *expression();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LimitStatementContext* limitStatement();

  class  SortItemContext : public antlr4::ParserRuleContext {
  public:
    SortItemContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExpressionContext *expression();
    antlr4::tree::TerminalNode *K_ASCENDING();
    antlr4::tree::TerminalNode *K_ASC();
    antlr4::tree::TerminalNode *K_DESCENDING();
    antlr4::tree::TerminalNode *K_DESC();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SortItemContext* sortItem();

  class  WhereContext : public antlr4::ParserRuleContext {
  public:
    WhereContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ExpressionContext *expression();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  WhereContext* where();

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
    PatternElementContext *patternElement();
    VariableContext *variable();
    antlr4::tree::TerminalNode *EQ();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PatternPartContext* patternPart();

  class  PatternElementContext : public antlr4::ParserRuleContext {
  public:
    PatternElementContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    NodePatternContext *nodePattern();
    std::vector<PatternElementChainContext *> patternElementChain();
    PatternElementChainContext* patternElementChain(size_t i);
    antlr4::tree::TerminalNode *LPAREN();
    PatternElementContext *patternElement();
    antlr4::tree::TerminalNode *RPAREN();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PatternElementContext* patternElement();

  class  PatternElementChainContext : public antlr4::ParserRuleContext {
  public:
    PatternElementChainContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    RelationshipPatternContext *relationshipPattern();
    NodePatternContext *nodePattern();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PatternElementChainContext* patternElementChain();

  class  NodePatternContext : public antlr4::ParserRuleContext {
  public:
    NodePatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    VariableContext *variable();
    NodeLabelsContext *nodeLabels();
    PropertiesContext *properties();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NodePatternContext* nodePattern();

  class  RelationshipPatternContext : public antlr4::ParserRuleContext {
  public:
    RelationshipPatternContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> MINUS();
    antlr4::tree::TerminalNode* MINUS(size_t i);
    antlr4::tree::TerminalNode *LT();
    RelationshipDetailContext *relationshipDetail();
    antlr4::tree::TerminalNode *GT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RelationshipPatternContext* relationshipPattern();

  class  RelationshipDetailContext : public antlr4::ParserRuleContext {
  public:
    RelationshipDetailContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACK();
    antlr4::tree::TerminalNode *RBRACK();
    VariableContext *variable();
    RelationshipTypesContext *relationshipTypes();
    RangeLiteralContext *rangeLiteral();
    PropertiesContext *properties();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RelationshipDetailContext* relationshipDetail();

  class  PropertiesContext : public antlr4::ParserRuleContext {
  public:
    PropertiesContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    MapLiteralContext *mapLiteral();
    ParameterContext *parameter();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PropertiesContext* properties();

  class  NodeLabelsContext : public antlr4::ParserRuleContext {
  public:
    NodeLabelsContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<NodeLabelContext *> nodeLabel();
    NodeLabelContext* nodeLabel(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NodeLabelsContext* nodeLabels();

  class  NodeLabelContext : public antlr4::ParserRuleContext {
  public:
    NodeLabelContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *COLON();
    LabelNameContext *labelName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NodeLabelContext* nodeLabel();

  class  RelationshipTypesContext : public antlr4::ParserRuleContext {
  public:
    RelationshipTypesContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<antlr4::tree::TerminalNode *> COLON();
    antlr4::tree::TerminalNode* COLON(size_t i);
    std::vector<RelTypeNameContext *> relTypeName();
    RelTypeNameContext* relTypeName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> PIPE();
    antlr4::tree::TerminalNode* PIPE(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RelationshipTypesContext* relationshipTypes();

  class  RangeLiteralContext : public antlr4::ParserRuleContext {
  public:
    RangeLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *MULTIPLY();
    std::vector<IntegerLiteralContext *> integerLiteral();
    IntegerLiteralContext* integerLiteral(size_t i);
    antlr4::tree::TerminalNode *RANGE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RangeLiteralContext* rangeLiteral();

  class  ExpressionContext : public antlr4::ParserRuleContext {
  public:
    ExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    OrExpressionContext *orExpression();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ExpressionContext* expression();

  class  OrExpressionContext : public antlr4::ParserRuleContext {
  public:
    OrExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<XorExpressionContext *> xorExpression();
    XorExpressionContext* xorExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_OR();
    antlr4::tree::TerminalNode* K_OR(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  OrExpressionContext* orExpression();

  class  XorExpressionContext : public antlr4::ParserRuleContext {
  public:
    XorExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<AndExpressionContext *> andExpression();
    AndExpressionContext* andExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_XOR();
    antlr4::tree::TerminalNode* K_XOR(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  XorExpressionContext* xorExpression();

  class  AndExpressionContext : public antlr4::ParserRuleContext {
  public:
    AndExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<NotExpressionContext *> notExpression();
    NotExpressionContext* notExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_AND();
    antlr4::tree::TerminalNode* K_AND(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AndExpressionContext* andExpression();

  class  NotExpressionContext : public antlr4::ParserRuleContext {
  public:
    NotExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ComparisonExpressionContext *comparisonExpression();
    antlr4::tree::TerminalNode *K_NOT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NotExpressionContext* notExpression();

  class  ComparisonExpressionContext : public antlr4::ParserRuleContext {
  public:
    ComparisonExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<ArithmeticExpressionContext *> arithmeticExpression();
    ArithmeticExpressionContext* arithmeticExpression(size_t i);
    antlr4::tree::TerminalNode *K_BETWEEN();
    antlr4::tree::TerminalNode *K_AND();
    antlr4::tree::TerminalNode *K_IS();
    antlr4::tree::TerminalNode *K_NULL();
    std::vector<antlr4::tree::TerminalNode *> EQ();
    antlr4::tree::TerminalNode* EQ(size_t i);
    std::vector<antlr4::tree::TerminalNode *> NEQ();
    antlr4::tree::TerminalNode* NEQ(size_t i);
    std::vector<antlr4::tree::TerminalNode *> LT();
    antlr4::tree::TerminalNode* LT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> GT();
    antlr4::tree::TerminalNode* GT(size_t i);
    std::vector<antlr4::tree::TerminalNode *> LTE();
    antlr4::tree::TerminalNode* LTE(size_t i);
    std::vector<antlr4::tree::TerminalNode *> GTE();
    antlr4::tree::TerminalNode* GTE(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_IN();
    antlr4::tree::TerminalNode* K_IN(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_STARTS();
    antlr4::tree::TerminalNode* K_STARTS(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_WITH();
    antlr4::tree::TerminalNode* K_WITH(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_ENDS();
    antlr4::tree::TerminalNode* K_ENDS(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_CONTAINS();
    antlr4::tree::TerminalNode* K_CONTAINS(size_t i);
    antlr4::tree::TerminalNode *K_NOT();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ComparisonExpressionContext* comparisonExpression();

  class  ArithmeticExpressionContext : public antlr4::ParserRuleContext {
  public:
    ArithmeticExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<PowerExpressionContext *> powerExpression();
    PowerExpressionContext* powerExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> PLUS();
    antlr4::tree::TerminalNode* PLUS(size_t i);
    std::vector<antlr4::tree::TerminalNode *> MINUS();
    antlr4::tree::TerminalNode* MINUS(size_t i);
    std::vector<antlr4::tree::TerminalNode *> MULTIPLY();
    antlr4::tree::TerminalNode* MULTIPLY(size_t i);
    std::vector<antlr4::tree::TerminalNode *> DIVIDE();
    antlr4::tree::TerminalNode* DIVIDE(size_t i);
    std::vector<antlr4::tree::TerminalNode *> MODULO();
    antlr4::tree::TerminalNode* MODULO(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ArithmeticExpressionContext* arithmeticExpression();

  class  PowerExpressionContext : public antlr4::ParserRuleContext {
  public:
    PowerExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<UnaryExpressionContext *> unaryExpression();
    UnaryExpressionContext* unaryExpression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> POWER();
    antlr4::tree::TerminalNode* POWER(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PowerExpressionContext* powerExpression();

  class  UnaryExpressionContext : public antlr4::ParserRuleContext {
  public:
    UnaryExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    AtomContext *atom();
    std::vector<AccessorContext *> accessor();
    AccessorContext* accessor(size_t i);
    antlr4::tree::TerminalNode *PLUS();
    antlr4::tree::TerminalNode *MINUS();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  UnaryExpressionContext* unaryExpression();

  class  AccessorContext : public antlr4::ParserRuleContext {
  public:
    AccessorContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DOT();
    PropertyKeyNameContext *propertyKeyName();
    antlr4::tree::TerminalNode *LBRACK();
    antlr4::tree::TerminalNode *RBRACK();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    antlr4::tree::TerminalNode *RANGE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AccessorContext* accessor();

  class  AtomContext : public antlr4::ParserRuleContext {
  public:
    AtomContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    LiteralContext *literal();
    ParameterContext *parameter();
    antlr4::tree::TerminalNode *K_COUNT();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *MULTIPLY();
    antlr4::tree::TerminalNode *RPAREN();
    QuantifierExpressionContext *quantifierExpression();
    ReduceExpressionContext *reduceExpression();
    ExistsExpressionContext *existsExpression();
    FunctionInvocationContext *functionInvocation();
    CaseExpressionContext *caseExpression();
    VariableContext *variable();
    ExpressionContext *expression();
    ListLiteralContext *listLiteral();
    ListComprehensionContext *listComprehension();
    PatternComprehensionContext *patternComprehension();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  AtomContext* atom();

  class  QuantifierExpressionContext : public antlr4::ParserRuleContext {
  public:
    QuantifierExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LPAREN();
    VariableContext *variable();
    antlr4::tree::TerminalNode *K_IN();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    antlr4::tree::TerminalNode *K_WHERE();
    antlr4::tree::TerminalNode *RPAREN();
    antlr4::tree::TerminalNode *K_ALL();
    antlr4::tree::TerminalNode *K_ANY();
    antlr4::tree::TerminalNode *K_NONE();
    antlr4::tree::TerminalNode *K_SINGLE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  QuantifierExpressionContext* quantifierExpression();

  class  ExistsExpressionContext : public antlr4::ParserRuleContext {
  public:
    ExistsExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_EXISTS();
    antlr4::tree::TerminalNode *LPAREN();
    PatternElementContext *patternElement();
    antlr4::tree::TerminalNode *RPAREN();
    antlr4::tree::TerminalNode *K_WHERE();
    ExpressionContext *expression();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ExistsExpressionContext* existsExpression();

  class  CaseExpressionContext : public antlr4::ParserRuleContext {
  public:
    CaseExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_CASE();
    antlr4::tree::TerminalNode *K_END();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_WHEN();
    antlr4::tree::TerminalNode* K_WHEN(size_t i);
    std::vector<antlr4::tree::TerminalNode *> K_THEN();
    antlr4::tree::TerminalNode* K_THEN(size_t i);
    antlr4::tree::TerminalNode *K_ELSE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  CaseExpressionContext* caseExpression();

  class  PropertyExpressionContext : public antlr4::ParserRuleContext {
  public:
    PropertyExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    AtomContext *atom();
    std::vector<antlr4::tree::TerminalNode *> DOT();
    antlr4::tree::TerminalNode* DOT(size_t i);
    std::vector<PropertyKeyNameContext *> propertyKeyName();
    PropertyKeyNameContext* propertyKeyName(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PropertyExpressionContext* propertyExpression();

  class  FunctionInvocationContext : public antlr4::ParserRuleContext {
  public:
    FunctionInvocationContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    FunctionNameContext *functionName();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    antlr4::tree::TerminalNode *K_DISTINCT();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  FunctionInvocationContext* functionInvocation();

  class  ExplicitProcedureInvocationContext : public antlr4::ParserRuleContext {
  public:
    ExplicitProcedureInvocationContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ProcedureNameContext *procedureName();
    antlr4::tree::TerminalNode *LPAREN();
    antlr4::tree::TerminalNode *RPAREN();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ExplicitProcedureInvocationContext* explicitProcedureInvocation();

  class  ImplicitProcedureInvocationContext : public antlr4::ParserRuleContext {
  public:
    ImplicitProcedureInvocationContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    ProcedureNameContext *procedureName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ImplicitProcedureInvocationContext* implicitProcedureInvocation();

  class  VariableContext : public antlr4::ParserRuleContext {
  public:
    VariableContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    SymbolicNameContext *symbolicName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  VariableContext* variable();

  class  LabelNameContext : public antlr4::ParserRuleContext {
  public:
    LabelNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    SchemaNameContext *schemaName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LabelNameContext* labelName();

  class  RelTypeNameContext : public antlr4::ParserRuleContext {
  public:
    RelTypeNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    SchemaNameContext *schemaName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  RelTypeNameContext* relTypeName();

  class  PropertyKeyNameContext : public antlr4::ParserRuleContext {
  public:
    PropertyKeyNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    SchemaNameContext *schemaName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PropertyKeyNameContext* propertyKeyName();

  class  ProcedureNameContext : public antlr4::ParserRuleContext {
  public:
    ProcedureNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    NamespaceContext *namespace_();
    SymbolicNameContext *symbolicName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ProcedureNameContext* procedureName();

  class  ProcedureResultFieldContext : public antlr4::ParserRuleContext {
  public:
    ProcedureResultFieldContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    SymbolicNameContext *symbolicName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ProcedureResultFieldContext* procedureResultField();

  class  FunctionNameContext : public antlr4::ParserRuleContext {
  public:
    FunctionNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    NamespaceContext *namespace_();
    SymbolicNameContext *symbolicName();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  FunctionNameContext* functionName();

  class  NamespaceContext : public antlr4::ParserRuleContext {
  public:
    NamespaceContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    std::vector<SymbolicNameContext *> symbolicName();
    SymbolicNameContext* symbolicName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> DOT();
    antlr4::tree::TerminalNode* DOT(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NamespaceContext* namespace_();

  class  SchemaNameContext : public antlr4::ParserRuleContext {
  public:
    SchemaNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    SymbolicNameContext *symbolicName();
    antlr4::tree::TerminalNode *K_INDEX();
    antlr4::tree::TerminalNode *K_ON();
    antlr4::tree::TerminalNode *K_MATCH();
    antlr4::tree::TerminalNode *K_CREATE();
    antlr4::tree::TerminalNode *K_WHERE();
    antlr4::tree::TerminalNode *K_RETURN();
    antlr4::tree::TerminalNode *K_BEGIN();
    antlr4::tree::TerminalNode *K_COMMIT();
    antlr4::tree::TerminalNode *K_ROLLBACK();
    antlr4::tree::TerminalNode *K_TRANSACTION();
    antlr4::tree::TerminalNode *K_KEY();
    antlr4::tree::TerminalNode *K_NODE();
    antlr4::tree::TerminalNode *K_CONSTRAINT();
    antlr4::tree::TerminalNode *K_BOOLEAN();
    antlr4::tree::TerminalNode *K_INTEGER();
    antlr4::tree::TerminalNode *K_FLOAT();
    antlr4::tree::TerminalNode *K_STRING();
    antlr4::tree::TerminalNode *K_LIST();
    antlr4::tree::TerminalNode *K_MAP();
    antlr4::tree::TerminalNode *K_FOREACH();
    antlr4::tree::TerminalNode *K_LOAD();
    antlr4::tree::TerminalNode *K_CSV();
    antlr4::tree::TerminalNode *K_HEADERS();
    antlr4::tree::TerminalNode *K_FROM();
    antlr4::tree::TerminalNode *K_FIELDTERMINATOR();
    antlr4::tree::TerminalNode *K_TRANSACTIONS();
    antlr4::tree::TerminalNode *K_ROWS();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SchemaNameContext* schemaName();

  class  SymbolicNameContext : public antlr4::ParserRuleContext {
  public:
    SymbolicNameContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *ID();
    antlr4::tree::TerminalNode *K_CALL();
    antlr4::tree::TerminalNode *K_YIELD();
    antlr4::tree::TerminalNode *K_CREATE();
    antlr4::tree::TerminalNode *K_DELETE();
    antlr4::tree::TerminalNode *K_DETACH();
    antlr4::tree::TerminalNode *K_EXISTS();
    antlr4::tree::TerminalNode *K_LIMIT();
    antlr4::tree::TerminalNode *K_MATCH();
    antlr4::tree::TerminalNode *K_MERGE();
    antlr4::tree::TerminalNode *K_OPTIONAL();
    antlr4::tree::TerminalNode *K_ORDER();
    antlr4::tree::TerminalNode *K_BY();
    antlr4::tree::TerminalNode *K_SKIP();
    antlr4::tree::TerminalNode *K_ASCENDING();
    antlr4::tree::TerminalNode *K_ASC();
    antlr4::tree::TerminalNode *K_DESCENDING();
    antlr4::tree::TerminalNode *K_DESC();
    antlr4::tree::TerminalNode *K_REMOVE();
    antlr4::tree::TerminalNode *K_RETURN();
    antlr4::tree::TerminalNode *K_SET();
    antlr4::tree::TerminalNode *K_WHERE();
    antlr4::tree::TerminalNode *K_WITH();
    antlr4::tree::TerminalNode *K_UNION();
    antlr4::tree::TerminalNode *K_UNWIND();
    antlr4::tree::TerminalNode *K_AND();
    antlr4::tree::TerminalNode *K_AS();
    antlr4::tree::TerminalNode *K_CONTAINS();
    antlr4::tree::TerminalNode *K_DISTINCT();
    antlr4::tree::TerminalNode *K_ENDS();
    antlr4::tree::TerminalNode *K_IN();
    antlr4::tree::TerminalNode *K_IS();
    antlr4::tree::TerminalNode *K_NOT();
    antlr4::tree::TerminalNode *K_OR();
    antlr4::tree::TerminalNode *K_STARTS();
    antlr4::tree::TerminalNode *K_XOR();
    antlr4::tree::TerminalNode *K_BETWEEN();
    antlr4::tree::TerminalNode *K_FALSE();
    antlr4::tree::TerminalNode *K_TRUE();
    antlr4::tree::TerminalNode *K_NULL();
    antlr4::tree::TerminalNode *K_CASE();
    antlr4::tree::TerminalNode *K_WHEN();
    antlr4::tree::TerminalNode *K_THEN();
    antlr4::tree::TerminalNode *K_ELSE();
    antlr4::tree::TerminalNode *K_END();
    antlr4::tree::TerminalNode *K_COUNT();
    antlr4::tree::TerminalNode *K_FILTER();
    antlr4::tree::TerminalNode *K_EXTRACT();
    antlr4::tree::TerminalNode *K_ANY();
    antlr4::tree::TerminalNode *K_NONE();
    antlr4::tree::TerminalNode *K_SINGLE();
    antlr4::tree::TerminalNode *K_ALL();
    antlr4::tree::TerminalNode *K_REDUCE();
    antlr4::tree::TerminalNode *K_INDEX();
    antlr4::tree::TerminalNode *K_ON();
    antlr4::tree::TerminalNode *K_SHOW();
    antlr4::tree::TerminalNode *K_DROP();
    antlr4::tree::TerminalNode *K_FOR();
    antlr4::tree::TerminalNode *K_CONSTRAINT();
    antlr4::tree::TerminalNode *K_DO();
    antlr4::tree::TerminalNode *K_REQUIRE();
    antlr4::tree::TerminalNode *K_UNIQUE();
    antlr4::tree::TerminalNode *K_MANDATORY();
    antlr4::tree::TerminalNode *K_SCALAR();
    antlr4::tree::TerminalNode *K_OF();
    antlr4::tree::TerminalNode *K_ADD();
    antlr4::tree::TerminalNode *K_KEY();
    antlr4::tree::TerminalNode *K_NODE();
    antlr4::tree::TerminalNode *K_BOOLEAN();
    antlr4::tree::TerminalNode *K_INTEGER();
    antlr4::tree::TerminalNode *K_FLOAT();
    antlr4::tree::TerminalNode *K_STRING();
    antlr4::tree::TerminalNode *K_LIST();
    antlr4::tree::TerminalNode *K_MAP();
    antlr4::tree::TerminalNode *K_IF();
    antlr4::tree::TerminalNode *K_VECTOR();
    antlr4::tree::TerminalNode *K_OPTIONS();
    antlr4::tree::TerminalNode *K_BEGIN();
    antlr4::tree::TerminalNode *K_COMMIT();
    antlr4::tree::TerminalNode *K_ROLLBACK();
    antlr4::tree::TerminalNode *K_TRANSACTION();
    antlr4::tree::TerminalNode *K_EXPLAIN();
    antlr4::tree::TerminalNode *K_PROFILE();
    antlr4::tree::TerminalNode *K_FOREACH();
    antlr4::tree::TerminalNode *K_LOAD();
    antlr4::tree::TerminalNode *K_CSV();
    antlr4::tree::TerminalNode *K_HEADERS();
    antlr4::tree::TerminalNode *K_FROM();
    antlr4::tree::TerminalNode *K_FIELDTERMINATOR();
    antlr4::tree::TerminalNode *K_TRANSACTIONS();
    antlr4::tree::TerminalNode *K_ROWS();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  SymbolicNameContext* symbolicName();

  class  LiteralContext : public antlr4::ParserRuleContext {
  public:
    LiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    BooleanLiteralContext *booleanLiteral();
    antlr4::tree::TerminalNode *K_NULL();
    NumberLiteralContext *numberLiteral();
    antlr4::tree::TerminalNode *StringLiteral();
    MapLiteralContext *mapLiteral();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  LiteralContext* literal();

  class  BooleanLiteralContext : public antlr4::ParserRuleContext {
  public:
    BooleanLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_TRUE();
    antlr4::tree::TerminalNode *K_FALSE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  BooleanLiteralContext* booleanLiteral();

  class  NumberLiteralContext : public antlr4::ParserRuleContext {
  public:
    NumberLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DoubleLiteral();
    IntegerLiteralContext *integerLiteral();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  NumberLiteralContext* numberLiteral();

  class  IntegerLiteralContext : public antlr4::ParserRuleContext {
  public:
    IntegerLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DecimalInteger();
    antlr4::tree::TerminalNode *HexInteger();
    antlr4::tree::TerminalNode *OctalInteger();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  IntegerLiteralContext* integerLiteral();

  class  MapLiteralContext : public antlr4::ParserRuleContext {
  public:
    MapLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACE();
    antlr4::tree::TerminalNode *RBRACE();
    std::vector<PropertyKeyNameContext *> propertyKeyName();
    PropertyKeyNameContext* propertyKeyName(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COLON();
    antlr4::tree::TerminalNode* COLON(size_t i);
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  MapLiteralContext* mapLiteral();

  class  ListLiteralContext : public antlr4::ParserRuleContext {
  public:
    ListLiteralContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACK();
    antlr4::tree::TerminalNode *RBRACK();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    std::vector<antlr4::tree::TerminalNode *> COMMA();
    antlr4::tree::TerminalNode* COMMA(size_t i);


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ListLiteralContext* listLiteral();

  class  ParameterContext : public antlr4::ParserRuleContext {
  public:
    ParameterContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *DOLLAR();
    SymbolicNameContext *symbolicName();
    antlr4::tree::TerminalNode *DecimalInteger();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ParameterContext* parameter();

  class  ListComprehensionContext : public antlr4::ParserRuleContext {
  public:
    CypherParser::ExpressionContext *whereExpression = nullptr;
    CypherParser::ExpressionContext *mapExpression = nullptr;
    ListComprehensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACK();
    VariableContext *variable();
    antlr4::tree::TerminalNode *K_IN();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    antlr4::tree::TerminalNode *RBRACK();
    antlr4::tree::TerminalNode *K_WHERE();
    antlr4::tree::TerminalNode *PIPE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ListComprehensionContext* listComprehension();

  class  ReduceExpressionContext : public antlr4::ParserRuleContext {
  public:
    ReduceExpressionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *K_REDUCE();
    antlr4::tree::TerminalNode *LPAREN();
    std::vector<VariableContext *> variable();
    VariableContext* variable(size_t i);
    antlr4::tree::TerminalNode *EQ();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    antlr4::tree::TerminalNode *COMMA();
    antlr4::tree::TerminalNode *K_IN();
    antlr4::tree::TerminalNode *PIPE();
    antlr4::tree::TerminalNode *RPAREN();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  ReduceExpressionContext* reduceExpression();

  class  PatternComprehensionContext : public antlr4::ParserRuleContext {
  public:
    PatternComprehensionContext(antlr4::ParserRuleContext *parent, size_t invokingState);
    virtual size_t getRuleIndex() const override;
    antlr4::tree::TerminalNode *LBRACK();
    PatternElementContext *patternElement();
    antlr4::tree::TerminalNode *PIPE();
    std::vector<ExpressionContext *> expression();
    ExpressionContext* expression(size_t i);
    antlr4::tree::TerminalNode *RBRACK();
    antlr4::tree::TerminalNode *K_WHERE();


    virtual std::any accept(antlr4::tree::ParseTreeVisitor *visitor) override;
   
  };

  PatternComprehensionContext* patternComprehension();


  // By default the static state used to implement the parser is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:
};

