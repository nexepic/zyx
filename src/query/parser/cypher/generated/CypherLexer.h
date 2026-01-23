
// Generated from CypherLexer.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"


namespace graph::parser::cypher {


class  CypherLexer : public antlr4::Lexer {
public:
  enum {
    K_CALL = 1, K_YIELD = 2, K_CREATE = 3, K_DELETE = 4, K_DETACH = 5, K_EXISTS = 6, 
    K_LIMIT = 7, K_MATCH = 8, K_MERGE = 9, K_OPTIONAL = 10, K_ORDER = 11, 
    K_BY = 12, K_SKIP = 13, K_ASCENDING = 14, K_ASC = 15, K_DESCENDING = 16, 
    K_DESC = 17, K_REMOVE = 18, K_RETURN = 19, K_SET = 20, K_WHERE = 21, 
    K_WITH = 22, K_UNION = 23, K_UNWIND = 24, K_AND = 25, K_AS = 26, K_CONTAINS = 27, 
    K_DISTINCT = 28, K_ENDS = 29, K_IN = 30, K_IS = 31, K_NOT = 32, K_OR = 33, 
    K_STARTS = 34, K_XOR = 35, K_FALSE = 36, K_TRUE = 37, K_NULL = 38, K_CASE = 39, 
    K_WHEN = 40, K_THEN = 41, K_ELSE = 42, K_END = 43, K_COUNT = 44, K_FILTER = 45, 
    K_EXTRACT = 46, K_ANY = 47, K_NONE = 48, K_SINGLE = 49, K_ALL = 50, 
    K_INDEX = 51, K_ON = 52, K_SHOW = 53, K_DROP = 54, K_FOR = 55, K_CONSTRAINT = 56, 
    K_DO = 57, K_REQUIRE = 58, K_UNIQUE = 59, K_MANDATORY = 60, K_SCALAR = 61, 
    K_OF = 62, K_ADD = 63, K_VECTOR = 64, K_OPTIONS = 65, EQ = 66, NEQ = 67, 
    LT = 68, GT = 69, LTE = 70, GTE = 71, PLUS = 72, MINUS = 73, MULTIPLY = 74, 
    DIVIDE = 75, MODULO = 76, POWER = 77, LPAREN = 78, RPAREN = 79, LBRACE = 80, 
    RBRACE = 81, LBRACK = 82, RBRACK = 83, COMMA = 84, DOT = 85, COLON = 86, 
    PIPE = 87, DOLLAR = 88, RANGE = 89, SEMI = 90, HexInteger = 91, OctalInteger = 92, 
    DecimalInteger = 93, DoubleLiteral = 94, ID = 95, StringLiteral = 96, 
    WS = 97, COMMENT = 98, LINE_COMMENT = 99
  };

  enum {
    COMMENTS = 2
  };

  explicit CypherLexer(antlr4::CharStream *input);

  ~CypherLexer() override;


  std::string getGrammarFileName() const override;

  const std::vector<std::string>& getRuleNames() const override;

  const std::vector<std::string>& getChannelNames() const override;

  const std::vector<std::string>& getModeNames() const override;

  const antlr4::dfa::Vocabulary& getVocabulary() const override;

  antlr4::atn::SerializedATNView getSerializedATN() const override;

  const antlr4::atn::ATN& getATN() const override;

  // By default the static state used to implement the lexer is lazily initialized during the first
  // call to the constructor. You can call this function if you wish to initialize the static state
  // ahead of time.
  static void initialize();

private:

  // Individual action functions triggered by action() above.

  // Individual semantic predicate functions triggered by sempred() above.

};

}  // namespace graph::parser::cypher
