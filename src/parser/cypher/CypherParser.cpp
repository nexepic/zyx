
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
      "query", "statement", "matchStatement", "createStatement", "indexDefinition", 
      "pattern", "patternPart", "patternElementChain", "relationshipPattern", 
      "relationshipDetail", "nodePattern", "whereClause", "propertyExpression", 
      "mapLiteral", "mapEntry", "literal"
    },
    std::vector<std::string>{
      "", "'-'", "'['", "']'", "", "", "", "", "", "", "", "", "", "", "", 
      "", "'('", "')'", "'{'", "'}'", "':'", "','", "'.'", "'<'", "'>'", 
      "'='", "'<>'", "'>='", "'<='"
    },
    std::vector<std::string>{
      "", "", "", "", "K_INDEX", "K_ON", "K_MATCH", "K_CREATE", "K_WHERE", 
      "NULL_LITERAL", "BOOLEAN", "ID", "STRING", "INTEGER", "DECIMAL", "WS", 
      "LPAREN", "RPAREN", "LBRACE", "RBRACE", "COLON", "COMMA", "DOT", "LT", 
      "GT", "EQ", "NEQ", "GTE", "LTE"
    }
  );
  static const int32_t serializedATNSegment[] = {
  	4,1,28,140,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,6,2,
  	7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,7,
  	14,2,15,7,15,1,0,1,0,1,0,1,1,1,1,3,1,38,8,1,1,2,1,2,1,2,1,2,3,2,44,8,
  	2,1,3,1,3,1,3,3,3,49,8,3,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,5,1,5,1,5,
  	5,5,62,8,5,10,5,12,5,65,9,5,1,6,1,6,5,6,69,8,6,10,6,12,6,72,9,6,1,7,1,
  	7,1,7,1,8,3,8,78,8,8,1,8,1,8,1,8,1,8,1,8,3,8,85,8,8,1,9,1,9,3,9,89,8,
  	9,1,9,1,9,3,9,93,8,9,1,9,3,9,96,8,9,1,9,1,9,1,10,1,10,3,10,102,8,10,1,
  	10,1,10,3,10,106,8,10,1,10,3,10,109,8,10,1,10,1,10,1,11,1,11,1,11,1,11,
  	1,12,1,12,1,12,1,12,1,13,1,13,1,13,1,13,5,13,125,8,13,10,13,12,13,128,
  	9,13,3,13,130,8,13,1,13,1,13,1,14,1,14,1,14,1,14,1,15,1,15,1,15,0,0,16,
  	0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,0,2,1,0,23,28,2,0,9,10,12,
  	14,138,0,32,1,0,0,0,2,37,1,0,0,0,4,39,1,0,0,0,6,45,1,0,0,0,8,50,1,0,0,
  	0,10,58,1,0,0,0,12,66,1,0,0,0,14,73,1,0,0,0,16,77,1,0,0,0,18,86,1,0,0,
  	0,20,99,1,0,0,0,22,112,1,0,0,0,24,116,1,0,0,0,26,120,1,0,0,0,28,133,1,
  	0,0,0,30,137,1,0,0,0,32,33,3,2,1,0,33,34,5,0,0,1,34,1,1,0,0,0,35,38,3,
  	4,2,0,36,38,3,6,3,0,37,35,1,0,0,0,37,36,1,0,0,0,38,3,1,0,0,0,39,40,5,
  	6,0,0,40,43,3,10,5,0,41,42,5,8,0,0,42,44,3,22,11,0,43,41,1,0,0,0,43,44,
  	1,0,0,0,44,5,1,0,0,0,45,48,5,7,0,0,46,49,3,8,4,0,47,49,3,10,5,0,48,46,
  	1,0,0,0,48,47,1,0,0,0,49,7,1,0,0,0,50,51,5,4,0,0,51,52,5,5,0,0,52,53,
  	5,20,0,0,53,54,5,11,0,0,54,55,5,16,0,0,55,56,5,11,0,0,56,57,5,17,0,0,
  	57,9,1,0,0,0,58,63,3,12,6,0,59,60,5,21,0,0,60,62,3,12,6,0,61,59,1,0,0,
  	0,62,65,1,0,0,0,63,61,1,0,0,0,63,64,1,0,0,0,64,11,1,0,0,0,65,63,1,0,0,
  	0,66,70,3,20,10,0,67,69,3,14,7,0,68,67,1,0,0,0,69,72,1,0,0,0,70,68,1,
  	0,0,0,70,71,1,0,0,0,71,13,1,0,0,0,72,70,1,0,0,0,73,74,3,16,8,0,74,75,
  	3,20,10,0,75,15,1,0,0,0,76,78,5,23,0,0,77,76,1,0,0,0,77,78,1,0,0,0,78,
  	79,1,0,0,0,79,80,5,1,0,0,80,81,1,0,0,0,81,82,3,18,9,0,82,84,5,1,0,0,83,
  	85,5,24,0,0,84,83,1,0,0,0,84,85,1,0,0,0,85,17,1,0,0,0,86,88,5,2,0,0,87,
  	89,5,11,0,0,88,87,1,0,0,0,88,89,1,0,0,0,89,92,1,0,0,0,90,91,5,20,0,0,
  	91,93,5,11,0,0,92,90,1,0,0,0,92,93,1,0,0,0,93,95,1,0,0,0,94,96,3,26,13,
  	0,95,94,1,0,0,0,95,96,1,0,0,0,96,97,1,0,0,0,97,98,5,3,0,0,98,19,1,0,0,
  	0,99,101,5,16,0,0,100,102,5,11,0,0,101,100,1,0,0,0,101,102,1,0,0,0,102,
  	105,1,0,0,0,103,104,5,20,0,0,104,106,5,11,0,0,105,103,1,0,0,0,105,106,
  	1,0,0,0,106,108,1,0,0,0,107,109,3,26,13,0,108,107,1,0,0,0,108,109,1,0,
  	0,0,109,110,1,0,0,0,110,111,5,17,0,0,111,21,1,0,0,0,112,113,3,24,12,0,
  	113,114,7,0,0,0,114,115,3,30,15,0,115,23,1,0,0,0,116,117,5,11,0,0,117,
  	118,5,22,0,0,118,119,5,11,0,0,119,25,1,0,0,0,120,129,5,18,0,0,121,126,
  	3,28,14,0,122,123,5,21,0,0,123,125,3,28,14,0,124,122,1,0,0,0,125,128,
  	1,0,0,0,126,124,1,0,0,0,126,127,1,0,0,0,127,130,1,0,0,0,128,126,1,0,0,
  	0,129,121,1,0,0,0,129,130,1,0,0,0,130,131,1,0,0,0,131,132,5,19,0,0,132,
  	27,1,0,0,0,133,134,5,11,0,0,134,135,5,20,0,0,135,136,3,30,15,0,136,29,
  	1,0,0,0,137,138,7,1,0,0,138,31,1,0,0,0,15,37,43,48,63,70,77,84,88,92,
  	95,101,105,108,126,129
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
    setState(32);
    statement();
    setState(33);
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
    setState(37);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_MATCH: {
        enterOuterAlt(_localctx, 1);
        setState(35);
        matchStatement();
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 2);
        setState(36);
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
    setState(39);
    match(CypherParser::K_MATCH);
    setState(40);
    pattern();
    setState(43);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(41);
      match(CypherParser::K_WHERE);
      setState(42);
      whereClause();
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
  enterRule(_localctx, 6, CypherParser::RuleCreateStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(45);
    match(CypherParser::K_CREATE);
    setState(48);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_INDEX: {
        setState(46);
        indexDefinition();
        break;
      }

      case CypherParser::LPAREN: {
        setState(47);
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
  enterRule(_localctx, 8, CypherParser::RuleIndexDefinition);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(50);
    match(CypherParser::K_INDEX);
    setState(51);
    match(CypherParser::K_ON);
    setState(52);
    match(CypherParser::COLON);
    setState(53);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->label = match(CypherParser::ID);
    setState(54);
    match(CypherParser::LPAREN);
    setState(55);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->property = match(CypherParser::ID);
    setState(56);
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
  enterRule(_localctx, 10, CypherParser::RulePattern);
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
    setState(58);
    patternPart();
    setState(63);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(59);
      match(CypherParser::COMMA);
      setState(60);
      patternPart();
      setState(65);
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
  enterRule(_localctx, 12, CypherParser::RulePatternPart);
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
    setState(66);
    nodePattern();
    setState(70);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::T__0

    || _la == CypherParser::LT) {
      setState(67);
      patternElementChain();
      setState(72);
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
  enterRule(_localctx, 14, CypherParser::RulePatternElementChain);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(73);
    relationshipPattern();
    setState(74);
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
  enterRule(_localctx, 16, CypherParser::RuleRelationshipPattern);
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
    setState(77);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LT) {
      setState(76);
      match(CypherParser::LT);
    }
    setState(79);
    match(CypherParser::T__0);
    setState(81);
    relationshipDetail();

    setState(82);
    match(CypherParser::T__0);
    setState(84);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::GT) {
      setState(83);
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
  enterRule(_localctx, 18, CypherParser::RuleRelationshipDetail);
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
    setState(86);
    match(CypherParser::T__1);
    setState(88);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(87);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(92);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(90);
      match(CypherParser::COLON);
      setState(91);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(95);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(94);
      mapLiteral();
    }
    setState(97);
    match(CypherParser::T__2);
   
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
  enterRule(_localctx, 20, CypherParser::RuleNodePattern);
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
    setState(99);
    match(CypherParser::LPAREN);
    setState(101);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(100);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(105);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(103);
      match(CypherParser::COLON);
      setState(104);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(108);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(107);
      mapLiteral();
    }
    setState(110);
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
  enterRule(_localctx, 22, CypherParser::RuleWhereClause);
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
    propertyExpression();
    setState(113);
    antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _input->LT(1);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 528482304) != 0))) {
      antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
    setState(114);
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
  enterRule(_localctx, 24, CypherParser::RulePropertyExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(116);
    match(CypherParser::ID);
    setState(117);
    match(CypherParser::DOT);
    setState(118);
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
  enterRule(_localctx, 26, CypherParser::RuleMapLiteral);
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
    setState(120);
    match(CypherParser::LBRACE);
    setState(129);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(121);
      mapEntry();
      setState(126);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(122);
        match(CypherParser::COMMA);
        setState(123);
        mapEntry();
        setState(128);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(131);
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
  enterRule(_localctx, 28, CypherParser::RuleMapEntry);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(133);
    antlrcpp::downCast<MapEntryContext *>(_localctx)->key = match(CypherParser::ID);
    setState(134);
    match(CypherParser::COLON);
    setState(135);
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
  enterRule(_localctx, 30, CypherParser::RuleLiteral);
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
    setState(137);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 30208) != 0))) {
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
