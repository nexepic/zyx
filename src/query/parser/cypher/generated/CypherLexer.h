
// Generated from CypherLexer.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"




class  CypherLexer : public antlr4::Lexer {
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
    K_LIST = 77, K_MAP = 78, K_VECTOR = 79, K_OPTIONS = 80, K_EXPLAIN = 81, 
    K_PROFILE = 82, EQ = 83, NEQ = 84, LT = 85, GT = 86, LTE = 87, GTE = 88, 
    PLUS = 89, MINUS = 90, MULTIPLY = 91, DIVIDE = 92, MODULO = 93, POWER = 94, 
    LPAREN = 95, RPAREN = 96, LBRACE = 97, RBRACE = 98, LBRACK = 99, RBRACK = 100, 
    COMMA = 101, DOT = 102, COLON = 103, PIPE = 104, DOLLAR = 105, RANGE = 106, 
    SEMI = 107, HexInteger = 108, OctalInteger = 109, DecimalInteger = 110, 
    DoubleLiteral = 111, ID = 112, StringLiteral = 113, WS = 114, COMMENT = 115, 
    LINE_COMMENT = 116
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

