
    #include "antlr4-runtime.h"


// Generated from Cypher.g4 by ANTLR 4.13.1

#pragma once


#include "antlr4-runtime.h"


namespace graph::parser::cypher {


class  CypherLexer : public antlr4::Lexer {
public:
  enum {
    T__0 = 1, T__1 = 2, T__2 = 3, K_INDEX = 4, K_ON = 5, K_MATCH = 6, K_CREATE = 7, 
    K_WHERE = 8, NULL_LITERAL = 9, BOOLEAN = 10, ID = 11, STRING = 12, INTEGER = 13, 
    DECIMAL = 14, WS = 15, LPAREN = 16, RPAREN = 17, LBRACE = 18, RBRACE = 19, 
    COLON = 20, COMMA = 21, DOT = 22, LT = 23, GT = 24, EQ = 25, NEQ = 26, 
    GTE = 27, LTE = 28
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
