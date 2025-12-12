
    #include "antlr4-runtime.h"


// Generated from Cypher.g4 by ANTLR 4.13.1


#include "CypherVisitor.h"

#include "CypherParser.h"


using namespace antlrcpp;
using namespace graph::parser::cypher;

using namespace antlr4;

namespace {

struct CypherParserStaticData final {
  CypherParserStaticData(std::vector<std::string> ruleNames,
                        std::vector<std::string> literalNames,
                        std::vector<std::string> symbolicNames)
      : ruleNames(std::move(ruleNames)), literalNames(std::move(literalNames)),
        symbolicNames(std::move(symbolicNames)),
        vocabulary(this->literalNames, this->symbolicNames) {}

  CypherParserStaticData(const CypherParserStaticData&) = delete;
  CypherParserStaticData(CypherParserStaticData&&) = delete;
  CypherParserStaticData& operator=(const CypherParserStaticData&) = delete;
  CypherParserStaticData& operator=(CypherParserStaticData&&) = delete;

  std::vector<antlr4::dfa::DFA> decisionToDFA;
  antlr4::atn::PredictionContextCache sharedContextCache;
  const std::vector<std::string> ruleNames;
  const std::vector<std::string> literalNames;
  const std::vector<std::string> symbolicNames;
  const antlr4::dfa::Vocabulary vocabulary;
  antlr4::atn::SerializedATNView serializedATN;
  std::unique_ptr<antlr4::atn::ATN> atn;
};

::antlr4::internal::OnceFlag cypherParserOnceFlag;
#if ANTLR4_USE_THREAD_LOCAL_CACHE
static thread_local
#endif
CypherParserStaticData *cypherParserStaticData = nullptr;

void cypherParserInitialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  if (cypherParserStaticData != nullptr) {
    return;
  }
#else
  assert(cypherParserStaticData == nullptr);
#endif
  auto staticData = std::make_unique<CypherParserStaticData>(
    std::vector<std::string>{
      "query", "statement", "matchStatement", "returnClause", "returnBody", 
      "returnItem", "expression", "createStatement", "indexDefinition", 
      "pattern", "patternPart", "patternElementChain", "relationshipPattern", 
      "relationshipDetail", "nodePattern", "whereClause", "propertyExpression", 
      "mapLiteral", "mapEntry", "literal"
    },
    std::vector<std::string>{
      "", "'*'", "'-'", "'['", "']'", "", "", "", "", "", "", "", "", "", 
      "", "", "", "", "", "'('", "')'", "'{'", "'}'", "':'", "','", "'.'", 
      "'<'", "'>'", "'='", "'<>'", "'>='", "'<='"
    },
    std::vector<std::string>{
      "", "", "", "", "", "K_INDEX", "K_ON", "K_MATCH", "K_CREATE", "K_WHERE", 
      "K_RETURN", "K_AS", "NULL_LITERAL", "BOOLEAN", "ID", "STRING", "INTEGER", 
      "DECIMAL", "WS", "LPAREN", "RPAREN", "LBRACE", "RBRACE", "COLON", 
      "COMMA", "DOT", "LT", "GT", "EQ", "NEQ", "GTE", "LTE"
    }
  );
  static const int32_t serializedATNSegment[] = {
  	4,1,31,175,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,6,2,
  	7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,7,
  	14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,1,0,1,0,1,0,1,1,
  	1,1,3,1,46,8,1,1,2,1,2,1,2,1,2,3,2,52,8,2,1,2,3,2,55,8,2,1,3,1,3,1,3,
  	1,4,1,4,1,4,5,4,63,8,4,10,4,12,4,66,9,4,1,4,3,4,69,8,4,1,5,1,5,1,5,3,
  	5,74,8,5,1,6,1,6,1,6,3,6,79,8,6,1,7,1,7,1,7,3,7,84,8,7,1,8,1,8,1,8,1,
  	8,1,8,1,8,1,8,1,8,1,9,1,9,1,9,5,9,97,8,9,10,9,12,9,100,9,9,1,10,1,10,
  	5,10,104,8,10,10,10,12,10,107,9,10,1,11,1,11,1,11,1,12,3,12,113,8,12,
  	1,12,1,12,1,12,1,12,1,12,3,12,120,8,12,1,13,1,13,3,13,124,8,13,1,13,1,
  	13,3,13,128,8,13,1,13,3,13,131,8,13,1,13,1,13,1,14,1,14,3,14,137,8,14,
  	1,14,1,14,3,14,141,8,14,1,14,3,14,144,8,14,1,14,1,14,1,15,1,15,1,15,1,
  	15,1,16,1,16,1,16,1,16,1,17,1,17,1,17,1,17,5,17,160,8,17,10,17,12,17,
  	163,9,17,3,17,165,8,17,1,17,1,17,1,18,1,18,1,18,1,18,1,19,1,19,1,19,0,
  	0,20,0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,36,38,0,2,1,0,26,
  	31,2,0,12,13,15,17,175,0,40,1,0,0,0,2,45,1,0,0,0,4,47,1,0,0,0,6,56,1,
  	0,0,0,8,68,1,0,0,0,10,70,1,0,0,0,12,78,1,0,0,0,14,80,1,0,0,0,16,85,1,
  	0,0,0,18,93,1,0,0,0,20,101,1,0,0,0,22,108,1,0,0,0,24,112,1,0,0,0,26,121,
  	1,0,0,0,28,134,1,0,0,0,30,147,1,0,0,0,32,151,1,0,0,0,34,155,1,0,0,0,36,
  	168,1,0,0,0,38,172,1,0,0,0,40,41,3,2,1,0,41,42,5,0,0,1,42,1,1,0,0,0,43,
  	46,3,4,2,0,44,46,3,14,7,0,45,43,1,0,0,0,45,44,1,0,0,0,46,3,1,0,0,0,47,
  	48,5,7,0,0,48,51,3,18,9,0,49,50,5,9,0,0,50,52,3,30,15,0,51,49,1,0,0,0,
  	51,52,1,0,0,0,52,54,1,0,0,0,53,55,3,6,3,0,54,53,1,0,0,0,54,55,1,0,0,0,
  	55,5,1,0,0,0,56,57,5,10,0,0,57,58,3,8,4,0,58,7,1,0,0,0,59,64,3,10,5,0,
  	60,61,5,24,0,0,61,63,3,10,5,0,62,60,1,0,0,0,63,66,1,0,0,0,64,62,1,0,0,
  	0,64,65,1,0,0,0,65,69,1,0,0,0,66,64,1,0,0,0,67,69,5,1,0,0,68,59,1,0,0,
  	0,68,67,1,0,0,0,69,9,1,0,0,0,70,73,3,12,6,0,71,72,5,11,0,0,72,74,5,14,
  	0,0,73,71,1,0,0,0,73,74,1,0,0,0,74,11,1,0,0,0,75,79,3,32,16,0,76,79,5,
  	14,0,0,77,79,3,38,19,0,78,75,1,0,0,0,78,76,1,0,0,0,78,77,1,0,0,0,79,13,
  	1,0,0,0,80,83,5,8,0,0,81,84,3,16,8,0,82,84,3,18,9,0,83,81,1,0,0,0,83,
  	82,1,0,0,0,84,15,1,0,0,0,85,86,5,5,0,0,86,87,5,6,0,0,87,88,5,23,0,0,88,
  	89,5,14,0,0,89,90,5,19,0,0,90,91,5,14,0,0,91,92,5,20,0,0,92,17,1,0,0,
  	0,93,98,3,20,10,0,94,95,5,24,0,0,95,97,3,20,10,0,96,94,1,0,0,0,97,100,
  	1,0,0,0,98,96,1,0,0,0,98,99,1,0,0,0,99,19,1,0,0,0,100,98,1,0,0,0,101,
  	105,3,28,14,0,102,104,3,22,11,0,103,102,1,0,0,0,104,107,1,0,0,0,105,103,
  	1,0,0,0,105,106,1,0,0,0,106,21,1,0,0,0,107,105,1,0,0,0,108,109,3,24,12,
  	0,109,110,3,28,14,0,110,23,1,0,0,0,111,113,5,26,0,0,112,111,1,0,0,0,112,
  	113,1,0,0,0,113,114,1,0,0,0,114,115,5,2,0,0,115,116,1,0,0,0,116,117,3,
  	26,13,0,117,119,5,2,0,0,118,120,5,27,0,0,119,118,1,0,0,0,119,120,1,0,
  	0,0,120,25,1,0,0,0,121,123,5,3,0,0,122,124,5,14,0,0,123,122,1,0,0,0,123,
  	124,1,0,0,0,124,127,1,0,0,0,125,126,5,23,0,0,126,128,5,14,0,0,127,125,
  	1,0,0,0,127,128,1,0,0,0,128,130,1,0,0,0,129,131,3,34,17,0,130,129,1,0,
  	0,0,130,131,1,0,0,0,131,132,1,0,0,0,132,133,5,4,0,0,133,27,1,0,0,0,134,
  	136,5,19,0,0,135,137,5,14,0,0,136,135,1,0,0,0,136,137,1,0,0,0,137,140,
  	1,0,0,0,138,139,5,23,0,0,139,141,5,14,0,0,140,138,1,0,0,0,140,141,1,0,
  	0,0,141,143,1,0,0,0,142,144,3,34,17,0,143,142,1,0,0,0,143,144,1,0,0,0,
  	144,145,1,0,0,0,145,146,5,20,0,0,146,29,1,0,0,0,147,148,3,32,16,0,148,
  	149,7,0,0,0,149,150,3,38,19,0,150,31,1,0,0,0,151,152,5,14,0,0,152,153,
  	5,25,0,0,153,154,5,14,0,0,154,33,1,0,0,0,155,164,5,21,0,0,156,161,3,36,
  	18,0,157,158,5,24,0,0,158,160,3,36,18,0,159,157,1,0,0,0,160,163,1,0,0,
  	0,161,159,1,0,0,0,161,162,1,0,0,0,162,165,1,0,0,0,163,161,1,0,0,0,164,
  	156,1,0,0,0,164,165,1,0,0,0,165,166,1,0,0,0,166,167,5,22,0,0,167,35,1,
  	0,0,0,168,169,5,14,0,0,169,170,5,23,0,0,170,171,3,38,19,0,171,37,1,0,
  	0,0,172,173,7,1,0,0,173,39,1,0,0,0,20,45,51,54,64,68,73,78,83,98,105,
  	112,119,123,127,130,136,140,143,161,164
  };
  staticData->serializedATN = antlr4::atn::SerializedATNView(serializedATNSegment, sizeof(serializedATNSegment) / sizeof(serializedATNSegment[0]));

  antlr4::atn::ATNDeserializer deserializer;
  staticData->atn = deserializer.deserialize(staticData->serializedATN);

  const size_t count = staticData->atn->getNumberOfDecisions();
  staticData->decisionToDFA.reserve(count);
  for (size_t i = 0; i < count; i++) { 
    staticData->decisionToDFA.emplace_back(staticData->atn->getDecisionState(i), i);
  }
  cypherParserStaticData = staticData.release();
}

}

CypherParser::CypherParser(TokenStream *input) : CypherParser(input, antlr4::atn::ParserATNSimulatorOptions()) {}

CypherParser::CypherParser(TokenStream *input, const antlr4::atn::ParserATNSimulatorOptions &options) : Parser(input) {
  CypherParser::initialize();
  _interpreter = new atn::ParserATNSimulator(this, *cypherParserStaticData->atn, cypherParserStaticData->decisionToDFA, cypherParserStaticData->sharedContextCache, options);
}

CypherParser::~CypherParser() {
  delete _interpreter;
}

const atn::ATN& CypherParser::getATN() const {
  return *cypherParserStaticData->atn;
}

std::string CypherParser::getGrammarFileName() const {
  return "Cypher.g4";
}

const std::vector<std::string>& CypherParser::getRuleNames() const {
  return cypherParserStaticData->ruleNames;
}

const dfa::Vocabulary& CypherParser::getVocabulary() const {
  return cypherParserStaticData->vocabulary;
}

antlr4::atn::SerializedATNView CypherParser::getSerializedATN() const {
  return cypherParserStaticData->serializedATN;
}


//----------------- QueryContext ------------------------------------------------------------------

CypherParser::QueryContext::QueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::StatementContext* CypherParser::QueryContext::statement() {
  return getRuleContext<CypherParser::StatementContext>(0);
}

tree::TerminalNode* CypherParser::QueryContext::EOF() {
  return getToken(CypherParser::EOF, 0);
}


size_t CypherParser::QueryContext::getRuleIndex() const {
  return CypherParser::RuleQuery;
}


std::any CypherParser::QueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitQuery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::QueryContext* CypherParser::query() {
  QueryContext *_localctx = _tracker.createInstance<QueryContext>(_ctx, getState());
  enterRule(_localctx, 0, CypherParser::RuleQuery);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(40);
    statement();
    setState(41);
    match(CypherParser::EOF);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- StatementContext ------------------------------------------------------------------

CypherParser::StatementContext::StatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::MatchStatementContext* CypherParser::StatementContext::matchStatement() {
  return getRuleContext<CypherParser::MatchStatementContext>(0);
}

CypherParser::CreateStatementContext* CypherParser::StatementContext::createStatement() {
  return getRuleContext<CypherParser::CreateStatementContext>(0);
}


size_t CypherParser::StatementContext::getRuleIndex() const {
  return CypherParser::RuleStatement;
}


std::any CypherParser::StatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::StatementContext* CypherParser::statement() {
  StatementContext *_localctx = _tracker.createInstance<StatementContext>(_ctx, getState());
  enterRule(_localctx, 2, CypherParser::RuleStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(45);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_MATCH: {
        enterOuterAlt(_localctx, 1);
        setState(43);
        matchStatement();
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 2);
        setState(44);
        createStatement();
        break;
      }

    default:
      throw NoViableAltException(this);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MatchStatementContext ------------------------------------------------------------------

CypherParser::MatchStatementContext::MatchStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MatchStatementContext::K_MATCH() {
  return getToken(CypherParser::K_MATCH, 0);
}

CypherParser::PatternContext* CypherParser::MatchStatementContext::pattern() {
  return getRuleContext<CypherParser::PatternContext>(0);
}

tree::TerminalNode* CypherParser::MatchStatementContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::WhereClauseContext* CypherParser::MatchStatementContext::whereClause() {
  return getRuleContext<CypherParser::WhereClauseContext>(0);
}

CypherParser::ReturnClauseContext* CypherParser::MatchStatementContext::returnClause() {
  return getRuleContext<CypherParser::ReturnClauseContext>(0);
}


size_t CypherParser::MatchStatementContext::getRuleIndex() const {
  return CypherParser::RuleMatchStatement;
}


std::any CypherParser::MatchStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitMatchStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MatchStatementContext* CypherParser::matchStatement() {
  MatchStatementContext *_localctx = _tracker.createInstance<MatchStatementContext>(_ctx, getState());
  enterRule(_localctx, 4, CypherParser::RuleMatchStatement);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(47);
    match(CypherParser::K_MATCH);
    setState(48);
    pattern();
    setState(51);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(49);
      match(CypherParser::K_WHERE);
      setState(50);
      whereClause();
    }
    setState(54);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_RETURN) {
      setState(53);
      returnClause();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ReturnClauseContext ------------------------------------------------------------------

CypherParser::ReturnClauseContext::ReturnClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ReturnClauseContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}

CypherParser::ReturnBodyContext* CypherParser::ReturnClauseContext::returnBody() {
  return getRuleContext<CypherParser::ReturnBodyContext>(0);
}


size_t CypherParser::ReturnClauseContext::getRuleIndex() const {
  return CypherParser::RuleReturnClause;
}


std::any CypherParser::ReturnClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitReturnClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReturnClauseContext* CypherParser::returnClause() {
  ReturnClauseContext *_localctx = _tracker.createInstance<ReturnClauseContext>(_ctx, getState());
  enterRule(_localctx, 6, CypherParser::RuleReturnClause);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(56);
    match(CypherParser::K_RETURN);
    setState(57);
    returnBody();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ReturnBodyContext ------------------------------------------------------------------

CypherParser::ReturnBodyContext::ReturnBodyContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::ReturnItemContext *> CypherParser::ReturnBodyContext::returnItem() {
  return getRuleContexts<CypherParser::ReturnItemContext>();
}

CypherParser::ReturnItemContext* CypherParser::ReturnBodyContext::returnItem(size_t i) {
  return getRuleContext<CypherParser::ReturnItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ReturnBodyContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ReturnBodyContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ReturnBodyContext::getRuleIndex() const {
  return CypherParser::RuleReturnBody;
}


std::any CypherParser::ReturnBodyContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitReturnBody(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReturnBodyContext* CypherParser::returnBody() {
  ReturnBodyContext *_localctx = _tracker.createInstance<ReturnBodyContext>(_ctx, getState());
  enterRule(_localctx, 8, CypherParser::RuleReturnBody);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(68);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::NULL_LITERAL:
      case CypherParser::BOOLEAN:
      case CypherParser::ID:
      case CypherParser::STRING:
      case CypherParser::INTEGER:
      case CypherParser::DECIMAL: {
        enterOuterAlt(_localctx, 1);
        setState(59);
        returnItem();
        setState(64);
        _errHandler->sync(this);
        _la = _input->LA(1);
        while (_la == CypherParser::COMMA) {
          setState(60);
          match(CypherParser::COMMA);
          setState(61);
          returnItem();
          setState(66);
          _errHandler->sync(this);
          _la = _input->LA(1);
        }
        break;
      }

      case CypherParser::T__0: {
        enterOuterAlt(_localctx, 2);
        setState(67);
        match(CypherParser::T__0);
        break;
      }

    default:
      throw NoViableAltException(this);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ReturnItemContext ------------------------------------------------------------------

CypherParser::ReturnItemContext::ReturnItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ExpressionContext* CypherParser::ReturnItemContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::ReturnItemContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

tree::TerminalNode* CypherParser::ReturnItemContext::ID() {
  return getToken(CypherParser::ID, 0);
}


size_t CypherParser::ReturnItemContext::getRuleIndex() const {
  return CypherParser::RuleReturnItem;
}


std::any CypherParser::ReturnItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitReturnItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReturnItemContext* CypherParser::returnItem() {
  ReturnItemContext *_localctx = _tracker.createInstance<ReturnItemContext>(_ctx, getState());
  enterRule(_localctx, 10, CypherParser::RuleReturnItem);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(70);
    expression();
    setState(73);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_AS) {
      setState(71);
      match(CypherParser::K_AS);
      setState(72);
      antlrcpp::downCast<ReturnItemContext *>(_localctx)->variable = match(CypherParser::ID);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ExpressionContext ------------------------------------------------------------------

CypherParser::ExpressionContext::ExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::PropertyExpressionContext* CypherParser::ExpressionContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

tree::TerminalNode* CypherParser::ExpressionContext::ID() {
  return getToken(CypherParser::ID, 0);
}

CypherParser::LiteralContext* CypherParser::ExpressionContext::literal() {
  return getRuleContext<CypherParser::LiteralContext>(0);
}


size_t CypherParser::ExpressionContext::getRuleIndex() const {
  return CypherParser::RuleExpression;
}


std::any CypherParser::ExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ExpressionContext* CypherParser::expression() {
  ExpressionContext *_localctx = _tracker.createInstance<ExpressionContext>(_ctx, getState());
  enterRule(_localctx, 12, CypherParser::RuleExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(78);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 6, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(75);
      propertyExpression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(76);
      match(CypherParser::ID);
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(77);
      literal();
      break;
    }

    default:
      break;
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- CreateStatementContext ------------------------------------------------------------------

CypherParser::CreateStatementContext::CreateStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::CreateStatementContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

CypherParser::IndexDefinitionContext* CypherParser::CreateStatementContext::indexDefinition() {
  return getRuleContext<CypherParser::IndexDefinitionContext>(0);
}

CypherParser::PatternContext* CypherParser::CreateStatementContext::pattern() {
  return getRuleContext<CypherParser::PatternContext>(0);
}


size_t CypherParser::CreateStatementContext::getRuleIndex() const {
  return CypherParser::RuleCreateStatement;
}


std::any CypherParser::CreateStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitCreateStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CreateStatementContext* CypherParser::createStatement() {
  CreateStatementContext *_localctx = _tracker.createInstance<CreateStatementContext>(_ctx, getState());
  enterRule(_localctx, 14, CypherParser::RuleCreateStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(80);
    match(CypherParser::K_CREATE);
    setState(83);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_INDEX: {
        setState(81);
        indexDefinition();
        break;
      }

      case CypherParser::LPAREN: {
        setState(82);
        pattern();
        break;
      }

    default:
      throw NoViableAltException(this);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- IndexDefinitionContext ------------------------------------------------------------------

CypherParser::IndexDefinitionContext::IndexDefinitionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::IndexDefinitionContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::IndexDefinitionContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::IndexDefinitionContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

tree::TerminalNode* CypherParser::IndexDefinitionContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::IndexDefinitionContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

std::vector<tree::TerminalNode *> CypherParser::IndexDefinitionContext::ID() {
  return getTokens(CypherParser::ID);
}

tree::TerminalNode* CypherParser::IndexDefinitionContext::ID(size_t i) {
  return getToken(CypherParser::ID, i);
}


size_t CypherParser::IndexDefinitionContext::getRuleIndex() const {
  return CypherParser::RuleIndexDefinition;
}


std::any CypherParser::IndexDefinitionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitIndexDefinition(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::IndexDefinitionContext* CypherParser::indexDefinition() {
  IndexDefinitionContext *_localctx = _tracker.createInstance<IndexDefinitionContext>(_ctx, getState());
  enterRule(_localctx, 16, CypherParser::RuleIndexDefinition);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(85);
    match(CypherParser::K_INDEX);
    setState(86);
    match(CypherParser::K_ON);
    setState(87);
    match(CypherParser::COLON);
    setState(88);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->label = match(CypherParser::ID);
    setState(89);
    match(CypherParser::LPAREN);
    setState(90);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->property = match(CypherParser::ID);
    setState(91);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PatternContext ------------------------------------------------------------------

CypherParser::PatternContext::PatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::PatternPartContext *> CypherParser::PatternContext::patternPart() {
  return getRuleContexts<CypherParser::PatternPartContext>();
}

CypherParser::PatternPartContext* CypherParser::PatternContext::patternPart(size_t i) {
  return getRuleContext<CypherParser::PatternPartContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::PatternContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::PatternContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::PatternContext::getRuleIndex() const {
  return CypherParser::RulePattern;
}


std::any CypherParser::PatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitPattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternContext* CypherParser::pattern() {
  PatternContext *_localctx = _tracker.createInstance<PatternContext>(_ctx, getState());
  enterRule(_localctx, 18, CypherParser::RulePattern);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(93);
    patternPart();
    setState(98);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(94);
      match(CypherParser::COMMA);
      setState(95);
      patternPart();
      setState(100);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PatternPartContext ------------------------------------------------------------------

CypherParser::PatternPartContext::PatternPartContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::NodePatternContext* CypherParser::PatternPartContext::nodePattern() {
  return getRuleContext<CypherParser::NodePatternContext>(0);
}

std::vector<CypherParser::PatternElementChainContext *> CypherParser::PatternPartContext::patternElementChain() {
  return getRuleContexts<CypherParser::PatternElementChainContext>();
}

CypherParser::PatternElementChainContext* CypherParser::PatternPartContext::patternElementChain(size_t i) {
  return getRuleContext<CypherParser::PatternElementChainContext>(i);
}


size_t CypherParser::PatternPartContext::getRuleIndex() const {
  return CypherParser::RulePatternPart;
}


std::any CypherParser::PatternPartContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitPatternPart(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternPartContext* CypherParser::patternPart() {
  PatternPartContext *_localctx = _tracker.createInstance<PatternPartContext>(_ctx, getState());
  enterRule(_localctx, 20, CypherParser::RulePatternPart);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(101);
    nodePattern();
    setState(105);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::T__1

    || _la == CypherParser::LT) {
      setState(102);
      patternElementChain();
      setState(107);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PatternElementChainContext ------------------------------------------------------------------

CypherParser::PatternElementChainContext::PatternElementChainContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::RelationshipPatternContext* CypherParser::PatternElementChainContext::relationshipPattern() {
  return getRuleContext<CypherParser::RelationshipPatternContext>(0);
}

CypherParser::NodePatternContext* CypherParser::PatternElementChainContext::nodePattern() {
  return getRuleContext<CypherParser::NodePatternContext>(0);
}


size_t CypherParser::PatternElementChainContext::getRuleIndex() const {
  return CypherParser::RulePatternElementChain;
}


std::any CypherParser::PatternElementChainContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitPatternElementChain(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternElementChainContext* CypherParser::patternElementChain() {
  PatternElementChainContext *_localctx = _tracker.createInstance<PatternElementChainContext>(_ctx, getState());
  enterRule(_localctx, 22, CypherParser::RulePatternElementChain);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(108);
    relationshipPattern();
    setState(109);
    nodePattern();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RelationshipPatternContext ------------------------------------------------------------------

CypherParser::RelationshipPatternContext::RelationshipPatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::RelationshipDetailContext* CypherParser::RelationshipPatternContext::relationshipDetail() {
  return getRuleContext<CypherParser::RelationshipDetailContext>(0);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::LT() {
  return getToken(CypherParser::LT, 0);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::GT() {
  return getToken(CypherParser::GT, 0);
}


size_t CypherParser::RelationshipPatternContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipPattern;
}


std::any CypherParser::RelationshipPatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitRelationshipPattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipPatternContext* CypherParser::relationshipPattern() {
  RelationshipPatternContext *_localctx = _tracker.createInstance<RelationshipPatternContext>(_ctx, getState());
  enterRule(_localctx, 24, CypherParser::RuleRelationshipPattern);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(112);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LT) {
      setState(111);
      match(CypherParser::LT);
    }
    setState(114);
    match(CypherParser::T__1);
    setState(116);
    relationshipDetail();

    setState(117);
    match(CypherParser::T__1);
    setState(119);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::GT) {
      setState(118);
      match(CypherParser::GT);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RelationshipDetailContext ------------------------------------------------------------------

CypherParser::RelationshipDetailContext::RelationshipDetailContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::RelationshipDetailContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::MapLiteralContext* CypherParser::RelationshipDetailContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}

std::vector<tree::TerminalNode *> CypherParser::RelationshipDetailContext::ID() {
  return getTokens(CypherParser::ID);
}

tree::TerminalNode* CypherParser::RelationshipDetailContext::ID(size_t i) {
  return getToken(CypherParser::ID, i);
}


size_t CypherParser::RelationshipDetailContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipDetail;
}


std::any CypherParser::RelationshipDetailContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitRelationshipDetail(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipDetailContext* CypherParser::relationshipDetail() {
  RelationshipDetailContext *_localctx = _tracker.createInstance<RelationshipDetailContext>(_ctx, getState());
  enterRule(_localctx, 26, CypherParser::RuleRelationshipDetail);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(121);
    match(CypherParser::T__2);
    setState(123);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(122);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(127);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(125);
      match(CypherParser::COLON);
      setState(126);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(130);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(129);
      mapLiteral();
    }
    setState(132);
    match(CypherParser::T__3);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- NodePatternContext ------------------------------------------------------------------

CypherParser::NodePatternContext::NodePatternContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::NodePatternContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::NodePatternContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

tree::TerminalNode* CypherParser::NodePatternContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::MapLiteralContext* CypherParser::NodePatternContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}

std::vector<tree::TerminalNode *> CypherParser::NodePatternContext::ID() {
  return getTokens(CypherParser::ID);
}

tree::TerminalNode* CypherParser::NodePatternContext::ID(size_t i) {
  return getToken(CypherParser::ID, i);
}


size_t CypherParser::NodePatternContext::getRuleIndex() const {
  return CypherParser::RuleNodePattern;
}


std::any CypherParser::NodePatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitNodePattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NodePatternContext* CypherParser::nodePattern() {
  NodePatternContext *_localctx = _tracker.createInstance<NodePatternContext>(_ctx, getState());
  enterRule(_localctx, 28, CypherParser::RuleNodePattern);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(134);
    match(CypherParser::LPAREN);
    setState(136);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(135);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(140);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(138);
      match(CypherParser::COLON);
      setState(139);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(143);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(142);
      mapLiteral();
    }
    setState(145);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- WhereClauseContext ------------------------------------------------------------------

CypherParser::WhereClauseContext::WhereClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::PropertyExpressionContext* CypherParser::WhereClauseContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

CypherParser::LiteralContext* CypherParser::WhereClauseContext::literal() {
  return getRuleContext<CypherParser::LiteralContext>(0);
}

tree::TerminalNode* CypherParser::WhereClauseContext::EQ() {
  return getToken(CypherParser::EQ, 0);
}

tree::TerminalNode* CypherParser::WhereClauseContext::NEQ() {
  return getToken(CypherParser::NEQ, 0);
}

tree::TerminalNode* CypherParser::WhereClauseContext::GT() {
  return getToken(CypherParser::GT, 0);
}

tree::TerminalNode* CypherParser::WhereClauseContext::LT() {
  return getToken(CypherParser::LT, 0);
}

tree::TerminalNode* CypherParser::WhereClauseContext::GTE() {
  return getToken(CypherParser::GTE, 0);
}

tree::TerminalNode* CypherParser::WhereClauseContext::LTE() {
  return getToken(CypherParser::LTE, 0);
}


size_t CypherParser::WhereClauseContext::getRuleIndex() const {
  return CypherParser::RuleWhereClause;
}


std::any CypherParser::WhereClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitWhereClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::WhereClauseContext* CypherParser::whereClause() {
  WhereClauseContext *_localctx = _tracker.createInstance<WhereClauseContext>(_ctx, getState());
  enterRule(_localctx, 30, CypherParser::RuleWhereClause);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(147);
    propertyExpression();
    setState(148);
    antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _input->LT(1);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 4227858432) != 0))) {
      antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
    setState(149);
    literal();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PropertyExpressionContext ------------------------------------------------------------------

CypherParser::PropertyExpressionContext::PropertyExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<tree::TerminalNode *> CypherParser::PropertyExpressionContext::ID() {
  return getTokens(CypherParser::ID);
}

tree::TerminalNode* CypherParser::PropertyExpressionContext::ID(size_t i) {
  return getToken(CypherParser::ID, i);
}

tree::TerminalNode* CypherParser::PropertyExpressionContext::DOT() {
  return getToken(CypherParser::DOT, 0);
}


size_t CypherParser::PropertyExpressionContext::getRuleIndex() const {
  return CypherParser::RulePropertyExpression;
}


std::any CypherParser::PropertyExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitPropertyExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PropertyExpressionContext* CypherParser::propertyExpression() {
  PropertyExpressionContext *_localctx = _tracker.createInstance<PropertyExpressionContext>(_ctx, getState());
  enterRule(_localctx, 32, CypherParser::RulePropertyExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(151);
    match(CypherParser::ID);
    setState(152);
    match(CypherParser::DOT);
    setState(153);
    match(CypherParser::ID);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MapLiteralContext ------------------------------------------------------------------

CypherParser::MapLiteralContext::MapLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MapLiteralContext::LBRACE() {
  return getToken(CypherParser::LBRACE, 0);
}

tree::TerminalNode* CypherParser::MapLiteralContext::RBRACE() {
  return getToken(CypherParser::RBRACE, 0);
}

std::vector<CypherParser::MapEntryContext *> CypherParser::MapLiteralContext::mapEntry() {
  return getRuleContexts<CypherParser::MapEntryContext>();
}

CypherParser::MapEntryContext* CypherParser::MapLiteralContext::mapEntry(size_t i) {
  return getRuleContext<CypherParser::MapEntryContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::MapLiteralContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::MapLiteralContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::MapLiteralContext::getRuleIndex() const {
  return CypherParser::RuleMapLiteral;
}


std::any CypherParser::MapLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitMapLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MapLiteralContext* CypherParser::mapLiteral() {
  MapLiteralContext *_localctx = _tracker.createInstance<MapLiteralContext>(_ctx, getState());
  enterRule(_localctx, 34, CypherParser::RuleMapLiteral);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(155);
    match(CypherParser::LBRACE);
    setState(164);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(156);
      mapEntry();
      setState(161);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(157);
        match(CypherParser::COMMA);
        setState(158);
        mapEntry();
        setState(163);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(166);
    match(CypherParser::RBRACE);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MapEntryContext ------------------------------------------------------------------

CypherParser::MapEntryContext::MapEntryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MapEntryContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::LiteralContext* CypherParser::MapEntryContext::literal() {
  return getRuleContext<CypherParser::LiteralContext>(0);
}

tree::TerminalNode* CypherParser::MapEntryContext::ID() {
  return getToken(CypherParser::ID, 0);
}


size_t CypherParser::MapEntryContext::getRuleIndex() const {
  return CypherParser::RuleMapEntry;
}


std::any CypherParser::MapEntryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitMapEntry(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MapEntryContext* CypherParser::mapEntry() {
  MapEntryContext *_localctx = _tracker.createInstance<MapEntryContext>(_ctx, getState());
  enterRule(_localctx, 36, CypherParser::RuleMapEntry);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(168);
    antlrcpp::downCast<MapEntryContext *>(_localctx)->key = match(CypherParser::ID);
    setState(169);
    match(CypherParser::COLON);
    setState(170);
    literal();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- LiteralContext ------------------------------------------------------------------

CypherParser::LiteralContext::LiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::LiteralContext::STRING() {
  return getToken(CypherParser::STRING, 0);
}

tree::TerminalNode* CypherParser::LiteralContext::INTEGER() {
  return getToken(CypherParser::INTEGER, 0);
}

tree::TerminalNode* CypherParser::LiteralContext::DECIMAL() {
  return getToken(CypherParser::DECIMAL, 0);
}

tree::TerminalNode* CypherParser::LiteralContext::BOOLEAN() {
  return getToken(CypherParser::BOOLEAN, 0);
}

tree::TerminalNode* CypherParser::LiteralContext::NULL_LITERAL() {
  return getToken(CypherParser::NULL_LITERAL, 0);
}


size_t CypherParser::LiteralContext::getRuleIndex() const {
  return CypherParser::RuleLiteral;
}


std::any CypherParser::LiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LiteralContext* CypherParser::literal() {
  LiteralContext *_localctx = _tracker.createInstance<LiteralContext>(_ctx, getState());
  enterRule(_localctx, 38, CypherParser::RuleLiteral);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(172);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 241664) != 0))) {
    _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

void CypherParser::initialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  cypherParserInitialize();
#else
  ::antlr4::internal::call_once(cypherParserOnceFlag, cypherParserInitialize);
#endif
}
