
    #include "antlr4-runtime.h"


// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"


namespace graph::parser::cypher {


class  CypherLexer : public antlr4::Lexer {
public:
  enum {
    T__0 = 1, T__1 = 2, T__2 = 3, T__3 = 4, K_INDEX = 5, K_SHOW = 6, K_DROP = 7, 
    K_INDEXES = 8, K_ON = 9, K_MATCH = 10, K_CREATE = 11, K_WHERE = 12, 
    K_RETURN = 13, K_AS = 14, K_CALL = 15, NULL_LITERAL = 16, BOOLEAN = 17, 
    ID = 18, STRING = 19, INTEGER = 20, DECIMAL = 21, WS = 22, LPAREN = 23, 
    RPAREN = 24, LBRACE = 25, RBRACE = 26, COLON = 27, COMMA = 28, DOT = 29, 
    LT = 30, GT = 31, EQ = 32, NEQ = 33, GTE = 34, LTE = 35
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
