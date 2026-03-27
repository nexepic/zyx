
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
    K_STARTS = 34, K_XOR = 35, K_FALSE = 36, K_TRUE = 37, K_NULL = 38, K_CASE = 39, 
    K_WHEN = 40, K_THEN = 41, K_ELSE = 42, K_END = 43, K_COUNT = 44, K_FILTER = 45, 
    K_EXTRACT = 46, K_ANY = 47, K_NONE = 48, K_SINGLE = 49, K_ALL = 50, 
    K_REDUCE = 51, K_INDEX = 52, K_ON = 53, K_SHOW = 54, K_DROP = 55, K_IF = 56, 
    K_FOR = 57, K_CONSTRAINT = 58, K_DO = 59, K_REQUIRE = 60, K_UNIQUE = 61, 
    K_MANDATORY = 62, K_SCALAR = 63, K_OF = 64, K_ADD = 65, K_BEGIN = 66, 
    K_COMMIT = 67, K_ROLLBACK = 68, K_TRANSACTION = 69, K_KEY = 70, K_NODE = 71, 
    K_BOOLEAN = 72, K_INTEGER = 73, K_FLOAT = 74, K_STRING = 75, K_LIST = 76, 
    K_MAP = 77, K_VECTOR = 78, K_OPTIONS = 79, EQ = 80, NEQ = 81, LT = 82, 
    GT = 83, LTE = 84, GTE = 85, PLUS = 86, MINUS = 87, MULTIPLY = 88, DIVIDE = 89, 
    MODULO = 90, POWER = 91, LPAREN = 92, RPAREN = 93, LBRACE = 94, RBRACE = 95, 
    LBRACK = 96, RBRACK = 97, COMMA = 98, DOT = 99, COLON = 100, PIPE = 101, 
    DOLLAR = 102, RANGE = 103, SEMI = 104, HexInteger = 105, OctalInteger = 106, 
    DecimalInteger = 107, DoubleLiteral = 108, ID = 109, StringLiteral = 110, 
    WS = 111, COMMENT = 112, LINE_COMMENT = 113
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

