
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
      "query", "statement", "callStatement", "procedureName", "symbolicName", 
      "argumentList", "administrationStatement", "showIndexesStatement", 
      "dropIndexStatement", "matchStatement", "returnClause", "returnBody", 
      "returnItem", "expression", "createStatement", "indexDefinition", 
      "pattern", "patternPart", "patternElementChain", "relationshipPattern", 
      "relationshipDetail", "nodePattern", "whereClause", "propertyExpression", 
      "mapLiteral", "mapEntry", "literal"
    },
    std::vector<std::string>{
      "", "'*'", "'-'", "'['", "']'", "", "", "", "", "", "", "", "", "", 
      "", "", "", "", "", "", "", "", "", "'('", "')'", "'{'", "'}'", "':'", 
      "','", "'.'", "'<'", "'>'", "'='", "'<>'", "'>='", "'<='"
    },
    std::vector<std::string>{
      "", "", "", "", "", "K_INDEX", "K_SHOW", "K_DROP", "K_INDEXES", "K_ON", 
      "K_MATCH", "K_CREATE", "K_WHERE", "K_RETURN", "K_AS", "K_CALL", "NULL_LITERAL", 
      "BOOLEAN", "ID", "STRING", "INTEGER", "DECIMAL", "WS", "LPAREN", "RPAREN", 
      "LBRACE", "RBRACE", "COLON", "COMMA", "DOT", "LT", "GT", "EQ", "NEQ", 
      "GTE", "LTE"
    }
  );
  static const int32_t serializedATNSegment[] = {
  	4,1,35,233,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,6,2,
  	7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,7,
  	14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,2,20,7,20,2,21,7,
  	21,2,22,7,22,2,23,7,23,2,24,7,24,2,25,7,25,2,26,7,26,1,0,1,0,1,0,1,1,
  	1,1,1,1,1,1,3,1,62,8,1,1,2,1,2,1,2,1,2,3,2,68,8,2,1,2,1,2,1,3,1,3,1,3,
  	5,3,75,8,3,10,3,12,3,78,9,3,1,4,1,4,1,5,1,5,1,5,5,5,85,8,5,10,5,12,5,
  	88,9,5,1,6,1,6,3,6,92,8,6,1,7,1,7,1,7,1,8,1,8,1,8,1,8,1,8,1,8,1,8,1,8,
  	1,8,1,9,1,9,1,9,1,9,3,9,110,8,9,1,9,3,9,113,8,9,1,10,1,10,1,10,1,11,1,
  	11,1,11,5,11,121,8,11,10,11,12,11,124,9,11,1,11,3,11,127,8,11,1,12,1,
  	12,1,12,3,12,132,8,12,1,13,1,13,1,13,3,13,137,8,13,1,14,1,14,1,14,3,14,
  	142,8,14,1,15,1,15,1,15,1,15,1,15,1,15,1,15,1,15,1,16,1,16,1,16,5,16,
  	155,8,16,10,16,12,16,158,9,16,1,17,1,17,5,17,162,8,17,10,17,12,17,165,
  	9,17,1,18,1,18,1,18,1,19,3,19,171,8,19,1,19,1,19,1,19,1,19,1,19,3,19,
  	178,8,19,1,20,1,20,3,20,182,8,20,1,20,1,20,3,20,186,8,20,1,20,3,20,189,
  	8,20,1,20,1,20,1,21,1,21,3,21,195,8,21,1,21,1,21,3,21,199,8,21,1,21,3,
  	21,202,8,21,1,21,1,21,1,22,1,22,1,22,1,22,1,23,1,23,1,23,1,23,1,24,1,
  	24,1,24,1,24,5,24,218,8,24,10,24,12,24,221,9,24,3,24,223,8,24,1,24,1,
  	24,1,25,1,25,1,25,1,25,1,26,1,26,1,26,0,0,27,0,2,4,6,8,10,12,14,16,18,
  	20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,0,4,3,0,5,7,9,15,18,
  	18,2,0,5,5,8,8,1,0,30,35,2,0,16,17,19,21,232,0,54,1,0,0,0,2,61,1,0,0,
  	0,4,63,1,0,0,0,6,71,1,0,0,0,8,79,1,0,0,0,10,81,1,0,0,0,12,91,1,0,0,0,
  	14,93,1,0,0,0,16,96,1,0,0,0,18,105,1,0,0,0,20,114,1,0,0,0,22,126,1,0,
  	0,0,24,128,1,0,0,0,26,136,1,0,0,0,28,138,1,0,0,0,30,143,1,0,0,0,32,151,
  	1,0,0,0,34,159,1,0,0,0,36,166,1,0,0,0,38,170,1,0,0,0,40,179,1,0,0,0,42,
  	192,1,0,0,0,44,205,1,0,0,0,46,209,1,0,0,0,48,213,1,0,0,0,50,226,1,0,0,
  	0,52,230,1,0,0,0,54,55,3,2,1,0,55,56,5,0,0,1,56,1,1,0,0,0,57,62,3,18,
  	9,0,58,62,3,28,14,0,59,62,3,12,6,0,60,62,3,4,2,0,61,57,1,0,0,0,61,58,
  	1,0,0,0,61,59,1,0,0,0,61,60,1,0,0,0,62,3,1,0,0,0,63,64,5,15,0,0,64,65,
  	3,6,3,0,65,67,5,23,0,0,66,68,3,10,5,0,67,66,1,0,0,0,67,68,1,0,0,0,68,
  	69,1,0,0,0,69,70,5,24,0,0,70,5,1,0,0,0,71,76,3,8,4,0,72,73,5,29,0,0,73,
  	75,3,8,4,0,74,72,1,0,0,0,75,78,1,0,0,0,76,74,1,0,0,0,76,77,1,0,0,0,77,
  	7,1,0,0,0,78,76,1,0,0,0,79,80,7,0,0,0,80,9,1,0,0,0,81,86,3,26,13,0,82,
  	83,5,28,0,0,83,85,3,26,13,0,84,82,1,0,0,0,85,88,1,0,0,0,86,84,1,0,0,0,
  	86,87,1,0,0,0,87,11,1,0,0,0,88,86,1,0,0,0,89,92,3,14,7,0,90,92,3,16,8,
  	0,91,89,1,0,0,0,91,90,1,0,0,0,92,13,1,0,0,0,93,94,5,6,0,0,94,95,7,1,0,
  	0,95,15,1,0,0,0,96,97,5,7,0,0,97,98,5,5,0,0,98,99,5,9,0,0,99,100,5,27,
  	0,0,100,101,5,18,0,0,101,102,5,23,0,0,102,103,5,18,0,0,103,104,5,24,0,
  	0,104,17,1,0,0,0,105,106,5,10,0,0,106,109,3,32,16,0,107,108,5,12,0,0,
  	108,110,3,44,22,0,109,107,1,0,0,0,109,110,1,0,0,0,110,112,1,0,0,0,111,
  	113,3,20,10,0,112,111,1,0,0,0,112,113,1,0,0,0,113,19,1,0,0,0,114,115,
  	5,13,0,0,115,116,3,22,11,0,116,21,1,0,0,0,117,122,3,24,12,0,118,119,5,
  	28,0,0,119,121,3,24,12,0,120,118,1,0,0,0,121,124,1,0,0,0,122,120,1,0,
  	0,0,122,123,1,0,0,0,123,127,1,0,0,0,124,122,1,0,0,0,125,127,5,1,0,0,126,
  	117,1,0,0,0,126,125,1,0,0,0,127,23,1,0,0,0,128,131,3,26,13,0,129,130,
  	5,14,0,0,130,132,5,18,0,0,131,129,1,0,0,0,131,132,1,0,0,0,132,25,1,0,
  	0,0,133,137,3,46,23,0,134,137,5,18,0,0,135,137,3,52,26,0,136,133,1,0,
  	0,0,136,134,1,0,0,0,136,135,1,0,0,0,137,27,1,0,0,0,138,141,5,11,0,0,139,
  	142,3,30,15,0,140,142,3,32,16,0,141,139,1,0,0,0,141,140,1,0,0,0,142,29,
  	1,0,0,0,143,144,5,5,0,0,144,145,5,9,0,0,145,146,5,27,0,0,146,147,5,18,
  	0,0,147,148,5,23,0,0,148,149,5,18,0,0,149,150,5,24,0,0,150,31,1,0,0,0,
  	151,156,3,34,17,0,152,153,5,28,0,0,153,155,3,34,17,0,154,152,1,0,0,0,
  	155,158,1,0,0,0,156,154,1,0,0,0,156,157,1,0,0,0,157,33,1,0,0,0,158,156,
  	1,0,0,0,159,163,3,42,21,0,160,162,3,36,18,0,161,160,1,0,0,0,162,165,1,
  	0,0,0,163,161,1,0,0,0,163,164,1,0,0,0,164,35,1,0,0,0,165,163,1,0,0,0,
  	166,167,3,38,19,0,167,168,3,42,21,0,168,37,1,0,0,0,169,171,5,30,0,0,170,
  	169,1,0,0,0,170,171,1,0,0,0,171,172,1,0,0,0,172,173,5,2,0,0,173,174,1,
  	0,0,0,174,175,3,40,20,0,175,177,5,2,0,0,176,178,5,31,0,0,177,176,1,0,
  	0,0,177,178,1,0,0,0,178,39,1,0,0,0,179,181,5,3,0,0,180,182,5,18,0,0,181,
  	180,1,0,0,0,181,182,1,0,0,0,182,185,1,0,0,0,183,184,5,27,0,0,184,186,
  	5,18,0,0,185,183,1,0,0,0,185,186,1,0,0,0,186,188,1,0,0,0,187,189,3,48,
  	24,0,188,187,1,0,0,0,188,189,1,0,0,0,189,190,1,0,0,0,190,191,5,4,0,0,
  	191,41,1,0,0,0,192,194,5,23,0,0,193,195,5,18,0,0,194,193,1,0,0,0,194,
  	195,1,0,0,0,195,198,1,0,0,0,196,197,5,27,0,0,197,199,5,18,0,0,198,196,
  	1,0,0,0,198,199,1,0,0,0,199,201,1,0,0,0,200,202,3,48,24,0,201,200,1,0,
  	0,0,201,202,1,0,0,0,202,203,1,0,0,0,203,204,5,24,0,0,204,43,1,0,0,0,205,
  	206,3,46,23,0,206,207,7,2,0,0,207,208,3,52,26,0,208,45,1,0,0,0,209,210,
  	5,18,0,0,210,211,5,29,0,0,211,212,5,18,0,0,212,47,1,0,0,0,213,222,5,25,
  	0,0,214,219,3,50,25,0,215,216,5,28,0,0,216,218,3,50,25,0,217,215,1,0,
  	0,0,218,221,1,0,0,0,219,217,1,0,0,0,219,220,1,0,0,0,220,223,1,0,0,0,221,
  	219,1,0,0,0,222,214,1,0,0,0,222,223,1,0,0,0,223,224,1,0,0,0,224,225,5,
  	26,0,0,225,49,1,0,0,0,226,227,5,18,0,0,227,228,5,27,0,0,228,229,3,52,
  	26,0,229,51,1,0,0,0,230,231,7,3,0,0,231,53,1,0,0,0,24,61,67,76,86,91,
  	109,112,122,126,131,136,141,156,163,170,177,181,185,188,194,198,201,219,
  	222
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
    setState(54);
    statement();
    setState(55);
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

CypherParser::CallStatementContext* CypherParser::StatementContext::callStatement() {
  return getRuleContext<CypherParser::CallStatementContext>(0);
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
    setState(61);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_MATCH: {
        enterOuterAlt(_localctx, 1);
        setState(57);
        matchStatement();
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 2);
        setState(58);
        createStatement();
        break;
      }

      case CypherParser::K_SHOW:
      case CypherParser::K_DROP: {
        enterOuterAlt(_localctx, 3);
        setState(59);
        administrationStatement();
        break;
      }

      case CypherParser::K_CALL: {
        enterOuterAlt(_localctx, 4);
        setState(60);
        callStatement();
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

//----------------- CallStatementContext ------------------------------------------------------------------

CypherParser::CallStatementContext::CallStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::CallStatementContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

CypherParser::ProcedureNameContext* CypherParser::CallStatementContext::procedureName() {
  return getRuleContext<CypherParser::ProcedureNameContext>(0);
}

tree::TerminalNode* CypherParser::CallStatementContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::CallStatementContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::ArgumentListContext* CypherParser::CallStatementContext::argumentList() {
  return getRuleContext<CypherParser::ArgumentListContext>(0);
}


size_t CypherParser::CallStatementContext::getRuleIndex() const {
  return CypherParser::RuleCallStatement;
}


std::any CypherParser::CallStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitCallStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CallStatementContext* CypherParser::callStatement() {
  CallStatementContext *_localctx = _tracker.createInstance<CallStatementContext>(_ctx, getState());
  enterRule(_localctx, 4, CypherParser::RuleCallStatement);
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
    setState(63);
    match(CypherParser::K_CALL);
    setState(64);
    procedureName();
    setState(65);
    match(CypherParser::LPAREN);
    setState(67);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 4128768) != 0)) {
      setState(66);
      argumentList();
    }
    setState(69);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ProcedureNameContext ------------------------------------------------------------------

CypherParser::ProcedureNameContext::ProcedureNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::SymbolicNameContext *> CypherParser::ProcedureNameContext::symbolicName() {
  return getRuleContexts<CypherParser::SymbolicNameContext>();
}

CypherParser::SymbolicNameContext* CypherParser::ProcedureNameContext::symbolicName(size_t i) {
  return getRuleContext<CypherParser::SymbolicNameContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ProcedureNameContext::DOT() {
  return getTokens(CypherParser::DOT);
}

tree::TerminalNode* CypherParser::ProcedureNameContext::DOT(size_t i) {
  return getToken(CypherParser::DOT, i);
}


size_t CypherParser::ProcedureNameContext::getRuleIndex() const {
  return CypherParser::RuleProcedureName;
}


std::any CypherParser::ProcedureNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitProcedureName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProcedureNameContext* CypherParser::procedureName() {
  ProcedureNameContext *_localctx = _tracker.createInstance<ProcedureNameContext>(_ctx, getState());
  enterRule(_localctx, 6, CypherParser::RuleProcedureName);
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
    setState(71);
    symbolicName();
    setState(76);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::DOT) {
      setState(72);
      match(CypherParser::DOT);
      setState(73);
      symbolicName();
      setState(78);
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

//----------------- SymbolicNameContext ------------------------------------------------------------------

CypherParser::SymbolicNameContext::SymbolicNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SymbolicNameContext::ID() {
  return getToken(CypherParser::ID, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MATCH() {
  return getToken(CypherParser::K_MATCH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SHOW() {
  return getToken(CypherParser::K_SHOW, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}


size_t CypherParser::SymbolicNameContext::getRuleIndex() const {
  return CypherParser::RuleSymbolicName;
}


std::any CypherParser::SymbolicNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitSymbolicName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SymbolicNameContext* CypherParser::symbolicName() {
  SymbolicNameContext *_localctx = _tracker.createInstance<SymbolicNameContext>(_ctx, getState());
  enterRule(_localctx, 8, CypherParser::RuleSymbolicName);
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
    setState(79);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 327392) != 0))) {
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

//----------------- ArgumentListContext ------------------------------------------------------------------

CypherParser::ArgumentListContext::ArgumentListContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ArgumentListContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ArgumentListContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ArgumentListContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ArgumentListContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ArgumentListContext::getRuleIndex() const {
  return CypherParser::RuleArgumentList;
}


std::any CypherParser::ArgumentListContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherVisitor*>(visitor))
    return parserVisitor->visitArgumentList(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ArgumentListContext* CypherParser::argumentList() {
  ArgumentListContext *_localctx = _tracker.createInstance<ArgumentListContext>(_ctx, getState());
  enterRule(_localctx, 10, CypherParser::RuleArgumentList);
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
    setState(81);
    expression();
    setState(86);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(82);
      match(CypherParser::COMMA);
      setState(83);
      expression();
      setState(88);
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
  enterRule(_localctx, 12, CypherParser::RuleAdministrationStatement);

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
      case CypherParser::K_SHOW: {
        enterOuterAlt(_localctx, 1);
        setState(89);
        showIndexesStatement();
        break;
      }

      case CypherParser::K_DROP: {
        enterOuterAlt(_localctx, 2);
        setState(90);
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
  enterRule(_localctx, 14, CypherParser::RuleShowIndexesStatement);
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
    match(CypherParser::K_SHOW);
    setState(94);
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
  enterRule(_localctx, 16, CypherParser::RuleDropIndexStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(96);
    match(CypherParser::K_DROP);
    setState(97);
    match(CypherParser::K_INDEX);
    setState(98);
    match(CypherParser::K_ON);
    setState(99);
    match(CypherParser::COLON);
    setState(100);
    antlrcpp::downCast<DropIndexStatementContext *>(_localctx)->label = match(CypherParser::ID);
    setState(101);
    match(CypherParser::LPAREN);
    setState(102);
    antlrcpp::downCast<DropIndexStatementContext *>(_localctx)->property = match(CypherParser::ID);
    setState(103);
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
  enterRule(_localctx, 18, CypherParser::RuleMatchStatement);
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
    setState(105);
    match(CypherParser::K_MATCH);
    setState(106);
    pattern();
    setState(109);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(107);
      match(CypherParser::K_WHERE);
      setState(108);
      whereClause();
    }
    setState(112);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_RETURN) {
      setState(111);
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
  enterRule(_localctx, 20, CypherParser::RuleReturnClause);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(114);
    match(CypherParser::K_RETURN);
    setState(115);
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
  enterRule(_localctx, 22, CypherParser::RuleReturnBody);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(126);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::NULL_LITERAL:
      case CypherParser::BOOLEAN:
      case CypherParser::ID:
      case CypherParser::STRING:
      case CypherParser::INTEGER:
      case CypherParser::DECIMAL: {
        enterOuterAlt(_localctx, 1);
        setState(117);
        returnItem();
        setState(122);
        _errHandler->sync(this);
        _la = _input->LA(1);
        while (_la == CypherParser::COMMA) {
          setState(118);
          match(CypherParser::COMMA);
          setState(119);
          returnItem();
          setState(124);
          _errHandler->sync(this);
          _la = _input->LA(1);
        }
        break;
      }

      case CypherParser::T__0: {
        enterOuterAlt(_localctx, 2);
        setState(125);
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
  enterRule(_localctx, 24, CypherParser::RuleReturnItem);
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
    setState(128);
    expression();
    setState(131);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_AS) {
      setState(129);
      match(CypherParser::K_AS);
      setState(130);
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
  enterRule(_localctx, 26, CypherParser::RuleExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(136);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 10, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(133);
      propertyExpression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(134);
      match(CypherParser::ID);
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(135);
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
  enterRule(_localctx, 28, CypherParser::RuleCreateStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(138);
    match(CypherParser::K_CREATE);
    setState(141);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_INDEX: {
        setState(139);
        indexDefinition();
        break;
      }

      case CypherParser::LPAREN: {
        setState(140);
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
  enterRule(_localctx, 30, CypherParser::RuleIndexDefinition);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(143);
    match(CypherParser::K_INDEX);
    setState(144);
    match(CypherParser::K_ON);
    setState(145);
    match(CypherParser::COLON);
    setState(146);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->label = match(CypherParser::ID);
    setState(147);
    match(CypherParser::LPAREN);
    setState(148);
    antlrcpp::downCast<IndexDefinitionContext *>(_localctx)->property = match(CypherParser::ID);
    setState(149);
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
  enterRule(_localctx, 32, CypherParser::RulePattern);
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
    setState(151);
    patternPart();
    setState(156);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(152);
      match(CypherParser::COMMA);
      setState(153);
      patternPart();
      setState(158);
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
  enterRule(_localctx, 34, CypherParser::RulePatternPart);
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
    setState(159);
    nodePattern();
    setState(163);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::T__1

    || _la == CypherParser::LT) {
      setState(160);
      patternElementChain();
      setState(165);
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
  enterRule(_localctx, 36, CypherParser::RulePatternElementChain);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(166);
    relationshipPattern();
    setState(167);
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
  enterRule(_localctx, 38, CypherParser::RuleRelationshipPattern);
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
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LT) {
      setState(169);
      match(CypherParser::LT);
    }
    setState(172);
    match(CypherParser::T__1);
    setState(174);
    relationshipDetail();

    setState(175);
    match(CypherParser::T__1);
    setState(177);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::GT) {
      setState(176);
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
  enterRule(_localctx, 40, CypherParser::RuleRelationshipDetail);
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
    setState(179);
    match(CypherParser::T__2);
    setState(181);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(180);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(185);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(183);
      match(CypherParser::COLON);
      setState(184);
      antlrcpp::downCast<RelationshipDetailContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(188);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(187);
      mapLiteral();
    }
    setState(190);
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
  enterRule(_localctx, 42, CypherParser::RuleNodePattern);
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
    setState(192);
    match(CypherParser::LPAREN);
    setState(194);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(193);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->variable = match(CypherParser::ID);
    }
    setState(198);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(196);
      match(CypherParser::COLON);
      setState(197);
      antlrcpp::downCast<NodePatternContext *>(_localctx)->label = match(CypherParser::ID);
    }
    setState(201);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE) {
      setState(200);
      mapLiteral();
    }
    setState(203);
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
  enterRule(_localctx, 44, CypherParser::RuleWhereClause);
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
    setState(205);
    propertyExpression();
    setState(206);
    antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _input->LT(1);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 67645734912) != 0))) {
      antlrcpp::downCast<WhereClauseContext *>(_localctx)->op = _errHandler->recoverInline(this);
    }
    else {
      _errHandler->reportMatch(this);
      consume();
    }
    setState(207);
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
  enterRule(_localctx, 46, CypherParser::RulePropertyExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(209);
    match(CypherParser::ID);
    setState(210);
    match(CypherParser::DOT);
    setState(211);
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
  enterRule(_localctx, 48, CypherParser::RuleMapLiteral);
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
    setState(213);
    match(CypherParser::LBRACE);
    setState(222);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::ID) {
      setState(214);
      mapEntry();
      setState(219);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(215);
        match(CypherParser::COMMA);
        setState(216);
        mapEntry();
        setState(221);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(224);
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
  enterRule(_localctx, 50, CypherParser::RuleMapEntry);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(226);
    antlrcpp::downCast<MapEntryContext *>(_localctx)->key = match(CypherParser::ID);
    setState(227);
    match(CypherParser::COLON);
    setState(228);
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
  enterRule(_localctx, 52, CypherParser::RuleLiteral);
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
    setState(230);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 3866624) != 0))) {
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
