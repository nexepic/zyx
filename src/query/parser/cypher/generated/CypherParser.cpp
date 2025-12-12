
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
      "query", "statement", "administrationStatement", "showIndexesStatement", 
      "dropIndexStatement", "matchStatement", "returnClause", "returnBody", 
      "returnItem", "expression", "createStatement", "indexDefinition", 
      "pattern", "patternPart", "patternElementChain", "relationshipPattern", 
      "relationshipDetail", "nodePattern", "whereClause", "propertyExpression", 
      "mapLiteral", "mapEntry", "literal"
    },
    std::vector<std::string>{
      "", "'*'", "'-'", "'['", "']'", "", "", "", "", "", "", "", "", "", 
      "", "", "", "", "", "", "", "", "'('", "')'", "'{'", "'}'", "':'", 
      "','", "'.'", "'<'", "'>'", "'='", "'<>'", "'>='", "'<='"
    },
    std::vector<std::string>{
      "", "", "", "", "", "K_INDEX", "K_SHOW", "K_DROP", "K_INDEXES", "K_ON", 
      "K_MATCH", "K_CREATE", "K_WHERE", "K_RETURN", "K_AS", "NULL_LITERAL", 
      "BOOLEAN", "ID", "STRING", "INTEGER", "DECIMAL", "WS", "LPAREN", "RPAREN", 
      "LBRACE", "RBRACE", "COLON", "COMMA", "DOT", "LT", "GT", "EQ", "NEQ", 
      "GTE", "LTE"
    }
  );
  static const int32_t serializedATNSegment[] = {
  	4,1,34,198,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,6,2,
  	7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,7,
  	14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,2,20,7,20,2,21,7,
  	21,2,22,7,22,1,0,1,0,1,0,1,1,1,1,1,1,3,1,53,8,1,1,2,1,2,3,2,57,8,2,1,
  	3,1,3,1,3,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,5,1,5,1,5,1,5,3,5,75,
  	8,5,1,5,3,5,78,8,5,1,6,1,6,1,6,1,7,1,7,1,7,5,7,86,8,7,10,7,12,7,89,9,
  	7,1,7,3,7,92,8,7,1,8,1,8,1,8,3,8,97,8,8,1,9,1,9,1,9,3,9,102,8,9,1,10,
  	1,10,1,10,3,10,107,8,10,1,11,1,11,1,11,1,11,1,11,1,11,1,11,1,11,1,12,
  	1,12,1,12,5,12,120,8,12,10,12,12,12,123,9,12,1,13,1,13,5,13,127,8,13,
  	10,13,12,13,130,9,13,1,14,1,14,1,14,1,15,3,15,136,8,15,1,15,1,15,1,15,
  	1,15,1,15,3,15,143,8,15,1,16,1,16,3,16,147,8,16,1,16,1,16,3,16,151,8,
  	16,1,16,3,16,154,8,16,1,16,1,16,1,17,1,17,3,17,160,8,17,1,17,1,17,3,17,
  	164,8,17,1,17,3,17,167,8,17,1,17,1,17,1,18,1,18,1,18,1,18,1,19,1,19,1,
  	19,1,19,1,20,1,20,1,20,1,20,5,20,183,8,20,10,20,12,20,186,9,20,3,20,188,
  	8,20,1,20,1,20,1,21,1,21,1,21,1,21,1,22,1,22,1,22,0,0,23,0,2,4,6,8,10,
  	12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,0,3,2,0,5,5,8,8,1,
  	0,29,34,2,0,15,16,18,20,197,0,46,1,0,0,0,2,52,1,0,0,0,4,56,1,0,0,0,6,
  	58,1,0,0,0,8,61,1,0,0,0,10,70,1,0,0,0,12,79,1,0,0,0,14,91,1,0,0,0,16,
  	93,1,0,0,0,18,101,1,0,0,0,20,103,1,0,0,0,22,108,1,0,0,0,24,116,1,0,0,
  	0,26,124,1,0,0,0,28,131,1,0,0,0,30,135,1,0,0,0,32,144,1,0,0,0,34,157,
  	1,0,0,0,36,170,1,0,0,0,38,174,1,0,0,0,40,178,1,0,0,0,42,191,1,0,0,0,44,
  	195,1,0,0,0,46,47,3,2,1,0,47,48,5,0,0,1,48,1,1,0,0,0,49,53,3,10,5,0,50,
  	53,3,20,10,0,51,53,3,4,2,0,52,49,1,0,0,0,52,50,1,0,0,0,52,51,1,0,0,0,
  	53,3,1,0,0,0,54,57,3,6,3,0,55,57,3,8,4,0,56,54,1,0,0,0,56,55,1,0,0,0,
  	57,5,1,0,0,0,58,59,5,6,0,0,59,60,7,0,0,0,60,7,1,0,0,0,61,62,5,7,0,0,62,
  	63,5,5,0,0,63,64,5,9,0,0,64,65,5,26,0,0,65,66,5,17,0,0,66,67,5,22,0,0,
  	67,68,5,17,0,0,68,69,5,23,0,0,69,9,1,0,0,0,70,71,5,10,0,0,71,74,3,24,
  	12,0,72,73,5,12,0,0,73,75,3,36,18,0,74,72,1,0,0,0,74,75,1,0,0,0,75,77,
  	1,0,0,0,76,78,3,12,6,0,77,76,1,0,0,0,77,78,1,0,0,0,78,11,1,0,0,0,79,80,
  	5,13,0,0,80,81,3,14,7,0,81,13,1,0,0,0,82,87,3,16,8,0,83,84,5,27,0,0,84,
  	86,3,16,8,0,85,83,1,0,0,0,86,89,1,0,0,0,87,85,1,0,0,0,87,88,1,0,0,0,88,
  	92,1,0,0,0,89,87,1,0,0,0,90,92,5,1,0,0,91,82,1,0,0,0,91,90,1,0,0,0,92,
  	15,1,0,0,0,93,96,3,18,9,0,94,95,5,14,0,0,95,97,5,17,0,0,96,94,1,0,0,0,
  	96,97,1,0,0,0,97,17,1,0,0,0,98,102,3,38,19,0,99,102,5,17,0,0,100,102,
  	3,44,22,0,101,98,1,0,0,0,101,99,1,0,0,0,101,100,1,0,0,0,102,19,1,0,0,
  	0,103,106,5,11,0,0,104,107,3,22,11,0,105,107,3,24,12,0,106,104,1,0,0,
  	0,106,105,1,0,0,0,107,21,1,0,0,0,108,109,5,5,0,0,109,110,5,9,0,0,110,
  	111,5,26,0,0,111,112,5,17,0,0,112,113,5,22,0,0,113,114,5,17,0,0,114,115,
  	5,23,0,0,115,23,1,0,0,0,116,121,3,26,13,0,117,118,5,27,0,0,118,120,3,
  	26,13,0,119,117,1,0,0,0,120,123,1,0,0,0,121,119,1,0,0,0,121,122,1,0,0,
  	0,122,25,1,0,0,0,123,121,1,0,0,0,124,128,3,34,17,0,125,127,3,28,14,0,
  	126,125,1,0,0,0,127,130,1,0,0,0,128,126,1,0,0,0,128,129,1,0,0,0,129,27,
  	1,0,0,0,130,128,1,0,0,0,131,132,3,30,15,0,132,133,3,34,17,0,133,29,1,
  	0,0,0,134,136,5,29,0,0,135,134,1,0,0,0,135,136,1,0,0,0,136,137,1,0,0,
  	0,137,138,5,2,0,0,138,139,1,0,0,0,139,140,3,32,16,0,140,142,5,2,0,0,141,
  	143,5,30,0,0,142,141,1,0,0,0,142,143,1,0,0,0,143,31,1,0,0,0,144,146,5,
  	3,0,0,145,147,5,17,0,0,146,145,1,0,0,0,146,147,1,0,0,0,147,150,1,0,0,
  	0,148,149,5,26,0,0,149,151,5,17,0,0,150,148,1,0,0,0,150,151,1,0,0,0,151,
  	153,1,0,0,0,152,154,3,40,20,0,153,152,1,0,0,0,153,154,1,0,0,0,154,155,
  	1,0,0,0,155,156,5,4,0,0,156,33,1,0,0,0,157,159,5,22,0,0,158,160,5,17,
  	0,0,159,158,1,0,0,0,159,160,1,0,0,0,160,163,1,0,0,0,161,162,5,26,0,0,
  	162,164,5,17,0,0,163,161,1,0,0,0,163,164,1,0,0,0,164,166,1,0,0,0,165,
  	167,3,40,20,0,166,165,1,0,0,0,166,167,1,0,0,0,167,168,1,0,0,0,168,169,
  	5,23,0,0,169,35,1,0,0,0,170,171,3,38,19,0,171,172,7,1,0,0,172,173,3,44,
  	22,0,173,37,1,0,0,0,174,175,5,17,0,0,175,176,5,28,0,0,176,177,5,17,0,
  	0,177,39,1,0,0,0,178,187,5,24,0,0,179,184,3,42,21,0,180,181,5,27,0,0,
  	181,183,3,42,21,0,182,180,1,0,0,0,183,186,1,0,0,0,184,182,1,0,0,0,184,
  	185,1,0,0,0,185,188,1,0,0,0,186,184,1,0,0,0,187,179,1,0,0,0,187,188,1,
  	0,0,0,188,189,1,0,0,0,189,190,5,25,0,0,190,41,1,0,0,0,191,192,5,17,0,
  	0,192,193,5,26,0,0,193,194,3,44,22,0,194,43,1,0,0,0,195,196,7,2,0,0,196,
  	45,1,0,0,0,21,52,56,74,77,87,91,96,101,106,121,128,135,142,146,150,153,
  	159,163,166,184,187
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
    setState(46);
    statement();
    setState(47);
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

CypherParser::AdministrationStatementContext* CypherParser::StatementContext::administrationStatement() {
  return getRuleContext<CypherParser::AdministrationStatementContext>(0);
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
    setState(52);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_MATCH: {
        enterOuterAlt(_localctx, 1);
        setState(49);
        matchStatement();
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 2);
        setState(50);
        createStatement();
        break;
      }

      case CypherParser::K_SHOW:
      case CypherParser::K_DROP: {
        enterOuterAlt(_localctx, 3);
        setState(51);
        administrationStatement();
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

//----------------- AdministrationStatementContext ------------------------------------------------------------------

CypherParser::AdministrationStatementContext::AdministrationStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ShowIndexesStatementContext* CypherParser::AdministrationStatementContext::showIndexesStatement() {
  return getRuleContext<CypherParser::ShowIndexesStatementContext>(0);
}

CypherParser::DropIndexStatementContext* CypherParser::AdministrationStatementContext::dropIndexStatement() {
  return getRuleContext<CypherParser::DropIndexStatementContext>(0);
}


size_t CypherParser::AdministrationStatementContext::getRuleIndex() const {
  return CypherParser::RuleAdministrationStatement;
}


std::any CypherParser::AdministrationStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitAdministrationStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AdministrationStatementContext* CypherParser::administrationStatement() {
  AdministrationStatementContext *_localctx = _tracker.createInstance<AdministrationStatementContext>(_ctx, getState());
  enterRule(_localctx, 4, CypherParser::RuleAdministrationStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(56);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_SHOW: {
        enterOuterAlt(_localctx, 1);
        setState(54);
        showIndexesStatement();
        break;
      }

      case CypherParser::K_DROP: {
        enterOuterAlt(_localctx, 2);
        setState(55);
        dropIndexStatement();
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

//----------------- ShowIndexesStatementContext ------------------------------------------------------------------

CypherParser::ShowIndexesStatementContext::ShowIndexesStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ShowIndexesStatementContext::K_SHOW() {
  return getToken(CypherParser::K_SHOW, 0);
}

tree::TerminalNode* CypherParser::ShowIndexesStatementContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::ShowIndexesStatementContext::K_INDEXES() {
  return getToken(CypherParser::K_INDEXES, 0);
}


size_t CypherParser::ShowIndexesStatementContext::getRuleIndex() const {
  return CypherParser::RuleShowIndexesStatement;
}


std::any CypherParser::ShowIndexesStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitShowIndexesStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ShowIndexesStatementContext* CypherParser::showIndexesStatement() {
  ShowIndexesStatementContext *_localctx = _tracker.createInstance<ShowIndexesStatementContext>(_ctx, getState());
  enterRule(_localctx, 6, CypherParser::RuleShowIndexesStatement);
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
    match(CypherParser::K_SHOW);
    setState(59);
    _la = _input->LA(1);
    if (!(_la == CypherParser::K_INDEX

    || _la == CypherParser::K_INDEXES)) {
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

//----------------- DropIndexStatementContext ------------------------------------------------------------------

CypherParser::DropIndexStatementContext::DropIndexStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

std::vector<tree::TerminalNode *> CypherParser::DropIndexStatementContext::ID() {
  return getTokens(CypherParser::ID);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::ID(size_t i) {
  return getToken(CypherParser::ID, i);
}


size_t CypherParser::DropIndexStatementContext::getRuleIndex() const {
  return CypherParser::RuleDropIndexStatement;
}


std::any CypherParser::DropIndexStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitDropIndexStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::DropIndexStatementContext* CypherParser::dropIndexStatement() {
  DropIndexStatementContext *_localctx = _tracker.createInstance<DropIndexStatementContext>(_ctx, getState());
  enterRule(_localctx, 8, CypherParser::RuleDropIndexStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(61);
    match(CypherParser::K_DROP);
    setState(62);
    match(CypherParser::K_INDEX);
    setState(63);
    match(CypherParser::K_ON);
    setState(64);
    match(CypherParser::COLON);
    setState(65);
    antlrcpp::downCast<DropIndexStatementContext *>(_localctx)->label = match(CypherParser::ID);
    setState(66);
    match(CypherParser::LPAREN);
    setState(67);
    antlrcpp::downCast<DropIndexStatementContext *>(_localctx)->property = match(CypherParser::ID);
    setState(68);
    match(CypherParser::RPAREN);
   
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
  enterRule(_localctx, 10, CypherParser::RuleMatchStatement);
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
    match(CypherParser::K_MATCH);
    setState(71);
    pattern();
    setState(74);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(72);
      match(CypherParser::K_WHERE);
      setState(73);
      whereClause();
    }
    setState(77);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_RETURN) {
      setState(76);
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
  enterRule(_localctx, 12, CypherParser::RuleReturnClause);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(79);
    match(CypherParser::K_RETURN);
    setState(80);
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
  enterRule(_localctx, 14, CypherParser::RuleReturnBody);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(91);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::NULL_LITERAL:
      case CypherParser::BOOLEAN:
      case CypherParser::ID:
      case CypherParser::STRING:
      case CypherParser::INTEGER:
      case CypherParser::DECIMAL: {
        enterOuterAlt(_localctx, 1);
        setState(82);
        returnItem();
        setState(87);
        _errHandler->sync(this);
        _la = _input->LA(1);
        while (_la == CypherParser::COMMA) {
          setState(83);
          match(CypherParser::COMMA);
          setState(84);
          returnItem();
          setState(89);
          _errHandler->sync(this);
          _la = _input->LA(1);
        }
        break;
      }

      case CypherParser::T__0: {
        enterOuterAlt(_localctx, 2);
        setState(90);
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
  enterRule(_localctx, 16, CypherParser::RuleReturnItem);
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
    expression();
    setState(96);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_AS) {
      setState(94);
      match(CypherParser::K_AS);
      setState(95);
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
  enterRule(_localctx, 18, CypherParser::RuleExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(101);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 7, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(98);
      propertyExpression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(99);
      match(CypherParser::ID);
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(100);
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
  enterRule(_localctx, 20, CypherParser::RuleCreateStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(103);
    match(CypherParser::K_CREATE);
    setState(106);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_INDEX: {
        setState(104);
        indexDefinition();
        break;
      }

      case CypherParser::LPAREN: {
        setState(105);
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
  enterRule(_localctx, 22, CypherParser::RuleIndexDefinition);

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
    match(CypherParser::K_INDEX);
    setState(109);
    match(CypherParser::K_ON);
    setState(110);
    match(CypherParser::COLON);
    setState(111);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->label = match(CypherParser::ID);
    setState(112);
    match(CypherParser::LPAREN);
    setState(113);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->property = match(CypherParser::ID);
    setState(114);
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
  enterRule(_localctx, 24, CypherParser::RulePattern);
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
    setState(116);
    patternPart();
    setState(121);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(117);
      match(CypherParser::COMMA);
      setState(118);
      patternPart();
      setState(123);
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
  enterRule(_localctx, 26, CypherParser::RulePatternPart);
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
    setState(124);
    nodePattern();
    setState(128);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::T__1

    || _la == CypherParser::LT) {
      setState(125);
      patternElementChain();
      setState(130);
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
  enterRule(_localctx, 28, CypherParser::RulePatternElementChain);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(131);
    relationshipPattern();
    setState(132);
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
  enterRule(_localctx, 30, CypherParser::RuleRelationshipPattern);
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
    setState(135);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LT) {
      setState(134);
      match(CypherParser::LT);
    }
    setState(137);
    match(CypherParser::T__1);
    setState(139);
    relationshipDetail();

    setState(140);
    match(CypherParser::T__1);
    setState(142);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::GT) {
      setState(141);
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
  enterRule(_localctx, 32, CypherParser::RuleRelationshipDetail);
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
    setState(144);
    match(CypherParser::T__2);
    setState(146);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(145);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(150);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(148);
      match(CypherParser::COLON);
      setState(149);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(153);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(152);
      mapLiteral();
    }
    setState(155);
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
  enterRule(_localctx, 34, CypherParser::RuleNodePattern);
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
    setState(157);
    match(CypherParser::LPAREN);
    setState(159);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(158);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(163);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(161);
      match(CypherParser::COLON);
      setState(162);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(166);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(165);
      mapLiteral();
    }
    setState(168);
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
  enterRule(_localctx, 36, CypherParser::RuleWhereClause);
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
    setState(170);
    propertyExpression();
    setState(171);
    antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _input->LT(1);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 33822867456) != 0))) {
      antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
    setState(172);
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
  enterRule(_localctx, 38, CypherParser::RulePropertyExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(174);
    match(CypherParser::ID);
    setState(175);
    match(CypherParser::DOT);
    setState(176);
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
  enterRule(_localctx, 40, CypherParser::RuleMapLiteral);
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
    setState(178);
    match(CypherParser::LBRACE);
    setState(187);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(179);
      mapEntry();
      setState(184);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(180);
        match(CypherParser::COMMA);
        setState(181);
        mapEntry();
        setState(186);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(189);
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
  enterRule(_localctx, 42, CypherParser::RuleMapEntry);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(191);
    antlrcpp::downCast<MapEntryContext *>(_localctx)->key = match(CypherParser::ID);
    setState(192);
    match(CypherParser::COLON);
    setState(193);
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
  enterRule(_localctx, 44, CypherParser::RuleLiteral);
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
    setState(195);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 1933312) != 0))) {
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
