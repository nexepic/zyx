
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
    K_SINGLE = 50, K_ALL = 51, K_REDUCE = 52, K_SHORTESTPATH = 53, K_ALLSHORTESTPATHS = 54, 
    K_INDEX = 55, K_ON = 56, K_SHOW = 57, K_DROP = 58, K_IF = 59, K_FOR = 60, 
    K_CONSTRAINT = 61, K_DO = 62, K_REQUIRE = 63, K_UNIQUE = 64, K_MANDATORY = 65, 
    K_SCALAR = 66, K_OF = 67, K_ADD = 68, K_BEGIN = 69, K_COMMIT = 70, K_ROLLBACK = 71, 
    K_TRANSACTION = 72, K_KEY = 73, K_NODE = 74, K_BOOLEAN = 75, K_INTEGER = 76, 
    K_FLOAT = 77, K_STRING = 78, K_LIST = 79, K_MAP = 80, K_VECTOR = 81, 
    K_OPTIONS = 82, K_FOREACH = 83, K_LOAD = 84, K_CSV = 85, K_HEADERS = 86, 
    K_FROM = 87, K_FIELDTERMINATOR = 88, K_TRANSACTIONS = 89, K_ROWS = 90, 
    K_EXPLAIN = 91, K_PROFILE = 92, REGEX_MATCH = 93, EQ = 94, NEQ = 95, 
    LT = 96, GT = 97, LTE = 98, GTE = 99, PLUS = 100, MINUS = 101, MULTIPLY = 102, 
    DIVIDE = 103, MODULO = 104, POWER = 105, LPAREN = 106, RPAREN = 107, 
    LBRACE = 108, RBRACE = 109, LBRACK = 110, RBRACK = 111, COMMA = 112, 
    DOT = 113, COLON = 114, PIPE = 115, DOLLAR = 116, RANGE = 117, SEMI = 118, 
    HexInteger = 119, OctalInteger = 120, DecimalInteger = 121, DoubleLiteral = 122, 
    ID = 123, StringLiteral = 124, WS = 125, COMMENT = 126, LINE_COMMENT = 127
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

