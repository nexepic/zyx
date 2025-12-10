
    #include "antlr4-runtime.h"


// Generated from Cypher.g4 by ANTLR 4.13.1


#include "CypherLexer.h"


using namespace antlr4;

using namespace graph::parser::cypher;


using namespace antlr4;

namespace {

struct CypherLexerStaticData final {
  CypherLexerStaticData(std::vector<std::string> ruleNames,
                          std::vector<std::string> channelNames,
                          std::vector<std::string> modeNames,
                          std::vector<std::string> literalNames,
                          std::vector<std::string> symbolicNames)
      : ruleNames(std::move(ruleNames)), channelNames(std::move(channelNames)),
        modeNames(std::move(modeNames)), literalNames(std::move(literalNames)),
        symbolicNames(std::move(symbolicNames)),
        vocabulary(this->literalNames, this->symbolicNames) {}

  CypherLexerStaticData(const CypherLexerStaticData&) = delete;
  CypherLexerStaticData(CypherLexerStaticData&&) = delete;
  CypherLexerStaticData& operator=(const CypherLexerStaticData&) = delete;
  CypherLexerStaticData& operator=(CypherLexerStaticData&&) = delete;

  std::vector<antlr4::dfa::DFA> decisionToDFA;
  antlr4::atn::PredictionContextCache sharedContextCache;
  const std::vector<std::string> ruleNames;
  const std::vector<std::string> channelNames;
  const std::vector<std::string> modeNames;
  const std::vector<std::string> literalNames;
  const std::vector<std::string> symbolicNames;
  const antlr4::dfa::Vocabulary vocabulary;
  antlr4::atn::SerializedATNView serializedATN;
  std::unique_ptr<antlr4::atn::ATN> atn;
};

::antlr4::internal::OnceFlag cypherlexerLexerOnceFlag;
#if ANTLR4_USE_THREAD_LOCAL_CACHE
static thread_local
#endif
CypherLexerStaticData *cypherlexerLexerStaticData = nullptr;

void cypherlexerLexerInitialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  if (cypherlexerLexerStaticData != nullptr) {
    return;
  }
#else
  assert(cypherlexerLexerStaticData == nullptr);
#endif
  auto staticData = std::make_unique<CypherLexerStaticData>(
    std::vector<std::string>{
      "T__0", "T__1", "T__2", "K_INDEX", "K_ON", "K_MATCH", "K_CREATE", 
      "K_WHERE", "NULL_LITERAL", "BOOLEAN", "ID", "STRING", "INTEGER", "DECIMAL", 
      "WS", "LPAREN", "RPAREN", "LBRACE", "RBRACE", "COLON", "COMMA", "DOT", 
      "LT", "GT", "EQ", "NEQ", "GTE", "LTE"
    },
    std::vector<std::string>{
      "DEFAULT_TOKEN_CHANNEL", "HIDDEN"
    },
    std::vector<std::string>{
      "DEFAULT_MODE"
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
  	4,0,28,230,6,-1,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,
  	6,2,7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,
  	7,14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,2,20,7,20,2,21,
  	7,21,2,22,7,22,2,23,7,23,2,24,7,24,2,25,7,25,2,26,7,26,2,27,7,27,1,0,
  	1,0,1,1,1,1,1,2,1,2,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1,3,3,3,74,8,
  	3,1,4,1,4,1,4,1,4,3,4,80,8,4,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,
  	3,5,92,8,5,1,6,1,6,1,6,1,6,1,6,1,6,1,6,1,6,1,6,1,6,1,6,1,6,3,6,106,8,
  	6,1,7,1,7,1,7,1,7,1,7,1,7,1,7,1,7,1,7,1,7,3,7,118,8,7,1,8,1,8,1,8,1,8,
  	1,8,1,8,1,8,1,8,3,8,128,8,8,1,9,1,9,1,9,1,9,1,9,1,9,1,9,1,9,1,9,1,9,1,
  	9,1,9,1,9,1,9,1,9,1,9,1,9,1,9,3,9,148,8,9,1,10,1,10,5,10,152,8,10,10,
  	10,12,10,155,9,10,1,11,1,11,1,11,1,11,5,11,161,8,11,10,11,12,11,164,9,
  	11,1,11,1,11,1,11,1,11,1,11,5,11,171,8,11,10,11,12,11,174,9,11,1,11,3,
  	11,177,8,11,1,12,4,12,180,8,12,11,12,12,12,181,1,13,4,13,185,8,13,11,
  	13,12,13,186,1,13,1,13,4,13,191,8,13,11,13,12,13,192,1,14,4,14,196,8,
  	14,11,14,12,14,197,1,14,1,14,1,15,1,15,1,16,1,16,1,17,1,17,1,18,1,18,
  	1,19,1,19,1,20,1,20,1,21,1,21,1,22,1,22,1,23,1,23,1,24,1,24,1,25,1,25,
  	1,25,1,26,1,26,1,26,1,27,1,27,1,27,0,0,28,1,1,3,2,5,3,7,4,9,5,11,6,13,
  	7,15,8,17,9,19,10,21,11,23,12,25,13,27,14,29,15,31,16,33,17,35,18,37,
  	19,39,20,41,21,43,22,45,23,47,24,49,25,51,26,53,27,55,28,1,0,6,3,0,65,
  	90,95,95,97,122,4,0,48,57,65,90,95,95,97,122,1,0,34,34,1,0,39,39,1,0,
  	48,57,3,0,9,10,13,13,32,32,248,0,1,1,0,0,0,0,3,1,0,0,0,0,5,1,0,0,0,0,
  	7,1,0,0,0,0,9,1,0,0,0,0,11,1,0,0,0,0,13,1,0,0,0,0,15,1,0,0,0,0,17,1,0,
  	0,0,0,19,1,0,0,0,0,21,1,0,0,0,0,23,1,0,0,0,0,25,1,0,0,0,0,27,1,0,0,0,
  	0,29,1,0,0,0,0,31,1,0,0,0,0,33,1,0,0,0,0,35,1,0,0,0,0,37,1,0,0,0,0,39,
  	1,0,0,0,0,41,1,0,0,0,0,43,1,0,0,0,0,45,1,0,0,0,0,47,1,0,0,0,0,49,1,0,
  	0,0,0,51,1,0,0,0,0,53,1,0,0,0,0,55,1,0,0,0,1,57,1,0,0,0,3,59,1,0,0,0,
  	5,61,1,0,0,0,7,73,1,0,0,0,9,79,1,0,0,0,11,91,1,0,0,0,13,105,1,0,0,0,15,
  	117,1,0,0,0,17,127,1,0,0,0,19,147,1,0,0,0,21,149,1,0,0,0,23,176,1,0,0,
  	0,25,179,1,0,0,0,27,184,1,0,0,0,29,195,1,0,0,0,31,201,1,0,0,0,33,203,
  	1,0,0,0,35,205,1,0,0,0,37,207,1,0,0,0,39,209,1,0,0,0,41,211,1,0,0,0,43,
  	213,1,0,0,0,45,215,1,0,0,0,47,217,1,0,0,0,49,219,1,0,0,0,51,221,1,0,0,
  	0,53,224,1,0,0,0,55,227,1,0,0,0,57,58,5,45,0,0,58,2,1,0,0,0,59,60,5,91,
  	0,0,60,4,1,0,0,0,61,62,5,93,0,0,62,6,1,0,0,0,63,64,5,73,0,0,64,65,5,78,
  	0,0,65,66,5,68,0,0,66,67,5,69,0,0,67,74,5,88,0,0,68,69,5,105,0,0,69,70,
  	5,110,0,0,70,71,5,100,0,0,71,72,5,101,0,0,72,74,5,120,0,0,73,63,1,0,0,
  	0,73,68,1,0,0,0,74,8,1,0,0,0,75,76,5,79,0,0,76,80,5,78,0,0,77,78,5,111,
  	0,0,78,80,5,110,0,0,79,75,1,0,0,0,79,77,1,0,0,0,80,10,1,0,0,0,81,82,5,
  	77,0,0,82,83,5,65,0,0,83,84,5,84,0,0,84,85,5,67,0,0,85,92,5,72,0,0,86,
  	87,5,109,0,0,87,88,5,97,0,0,88,89,5,116,0,0,89,90,5,99,0,0,90,92,5,104,
  	0,0,91,81,1,0,0,0,91,86,1,0,0,0,92,12,1,0,0,0,93,94,5,67,0,0,94,95,5,
  	82,0,0,95,96,5,69,0,0,96,97,5,65,0,0,97,98,5,84,0,0,98,106,5,69,0,0,99,
  	100,5,99,0,0,100,101,5,114,0,0,101,102,5,101,0,0,102,103,5,97,0,0,103,
  	104,5,116,0,0,104,106,5,101,0,0,105,93,1,0,0,0,105,99,1,0,0,0,106,14,
  	1,0,0,0,107,108,5,87,0,0,108,109,5,72,0,0,109,110,5,69,0,0,110,111,5,
  	82,0,0,111,118,5,69,0,0,112,113,5,119,0,0,113,114,5,104,0,0,114,115,5,
  	101,0,0,115,116,5,114,0,0,116,118,5,101,0,0,117,107,1,0,0,0,117,112,1,
  	0,0,0,118,16,1,0,0,0,119,120,5,78,0,0,120,121,5,85,0,0,121,122,5,76,0,
  	0,122,128,5,76,0,0,123,124,5,110,0,0,124,125,5,117,0,0,125,126,5,108,
  	0,0,126,128,5,108,0,0,127,119,1,0,0,0,127,123,1,0,0,0,128,18,1,0,0,0,
  	129,130,5,84,0,0,130,131,5,82,0,0,131,132,5,85,0,0,132,148,5,69,0,0,133,
  	134,5,116,0,0,134,135,5,114,0,0,135,136,5,117,0,0,136,148,5,101,0,0,137,
  	138,5,70,0,0,138,139,5,65,0,0,139,140,5,76,0,0,140,141,5,83,0,0,141,148,
  	5,69,0,0,142,143,5,102,0,0,143,144,5,97,0,0,144,145,5,108,0,0,145,146,
  	5,115,0,0,146,148,5,101,0,0,147,129,1,0,0,0,147,133,1,0,0,0,147,137,1,
  	0,0,0,147,142,1,0,0,0,148,20,1,0,0,0,149,153,7,0,0,0,150,152,7,1,0,0,
  	151,150,1,0,0,0,152,155,1,0,0,0,153,151,1,0,0,0,153,154,1,0,0,0,154,22,
  	1,0,0,0,155,153,1,0,0,0,156,162,5,34,0,0,157,161,8,2,0,0,158,159,5,92,
  	0,0,159,161,5,34,0,0,160,157,1,0,0,0,160,158,1,0,0,0,161,164,1,0,0,0,
  	162,160,1,0,0,0,162,163,1,0,0,0,163,165,1,0,0,0,164,162,1,0,0,0,165,177,
  	5,34,0,0,166,172,5,39,0,0,167,171,8,3,0,0,168,169,5,92,0,0,169,171,5,
  	39,0,0,170,167,1,0,0,0,170,168,1,0,0,0,171,174,1,0,0,0,172,170,1,0,0,
  	0,172,173,1,0,0,0,173,175,1,0,0,0,174,172,1,0,0,0,175,177,5,39,0,0,176,
  	156,1,0,0,0,176,166,1,0,0,0,177,24,1,0,0,0,178,180,7,4,0,0,179,178,1,
  	0,0,0,180,181,1,0,0,0,181,179,1,0,0,0,181,182,1,0,0,0,182,26,1,0,0,0,
  	183,185,7,4,0,0,184,183,1,0,0,0,185,186,1,0,0,0,186,184,1,0,0,0,186,187,
  	1,0,0,0,187,188,1,0,0,0,188,190,5,46,0,0,189,191,7,4,0,0,190,189,1,0,
  	0,0,191,192,1,0,0,0,192,190,1,0,0,0,192,193,1,0,0,0,193,28,1,0,0,0,194,
  	196,7,5,0,0,195,194,1,0,0,0,196,197,1,0,0,0,197,195,1,0,0,0,197,198,1,
  	0,0,0,198,199,1,0,0,0,199,200,6,14,0,0,200,30,1,0,0,0,201,202,5,40,0,
  	0,202,32,1,0,0,0,203,204,5,41,0,0,204,34,1,0,0,0,205,206,5,123,0,0,206,
  	36,1,0,0,0,207,208,5,125,0,0,208,38,1,0,0,0,209,210,5,58,0,0,210,40,1,
  	0,0,0,211,212,5,44,0,0,212,42,1,0,0,0,213,214,5,46,0,0,214,44,1,0,0,0,
  	215,216,5,60,0,0,216,46,1,0,0,0,217,218,5,62,0,0,218,48,1,0,0,0,219,220,
  	5,61,0,0,220,50,1,0,0,0,221,222,5,60,0,0,222,223,5,62,0,0,223,52,1,0,
  	0,0,224,225,5,62,0,0,225,226,5,61,0,0,226,54,1,0,0,0,227,228,5,60,0,0,
  	228,229,5,61,0,0,229,56,1,0,0,0,18,0,73,79,91,105,117,127,147,153,160,
  	162,170,172,176,181,186,192,197,1,6,0,0
  };
  staticData->serializedATN = antlr4::atn::SerializedATNView(serializedATNSegment, sizeof(serializedATNSegment) / sizeof(serializedATNSegment[0]));

  antlr4::atn::ATNDeserializer deserializer;
  staticData->atn = deserializer.deserialize(staticData->serializedATN);

  const size_t count = staticData->atn->getNumberOfDecisions();
  staticData->decisionToDFA.reserve(count);
  for (size_t i = 0; i < count; i++) { 
    staticData->decisionToDFA.emplace_back(staticData->atn->getDecisionState(i), i);
  }
  cypherlexerLexerStaticData = staticData.release();
}

}

CypherLexer::CypherLexer(CharStream *input) : Lexer(input) {
  CypherLexer::initialize();
  _interpreter = new atn::LexerATNSimulator(this, *cypherlexerLexerStaticData->atn, cypherlexerLexerStaticData->decisionToDFA, cypherlexerLexerStaticData->sharedContextCache);
}

CypherLexer::~CypherLexer() {
  delete _interpreter;
}

std::string CypherLexer::getGrammarFileName() const {
  return "Cypher.g4";
}

const std::vector<std::string>& CypherLexer::getRuleNames() const {
  return cypherlexerLexerStaticData->ruleNames;
}

const std::vector<std::string>& CypherLexer::getChannelNames() const {
  return cypherlexerLexerStaticData->channelNames;
}

const std::vector<std::string>& CypherLexer::getModeNames() const {
  return cypherlexerLexerStaticData->modeNames;
}

const dfa::Vocabulary& CypherLexer::getVocabulary() const {
  return cypherlexerLexerStaticData->vocabulary;
}

antlr4::atn::SerializedATNView CypherLexer::getSerializedATN() const {
  return cypherlexerLexerStaticData->serializedATN;
}

const atn::ATN& CypherLexer::getATN() const {
  return *cypherlexerLexerStaticData->atn;
}




void CypherLexer::initialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  cypherlexerLexerInitialize();
#else
  ::antlr4::internal::call_once(cypherlexerLexerOnceFlag, cypherlexerLexerInitialize);
#endif
}
