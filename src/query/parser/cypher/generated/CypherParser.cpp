
    #include "antlr4-runtime.h"


// Generated from CypherParser.g4 by ANTLR 4.13.1


#include "CypherParserVisitor.h"

#include "CypherParser.h"


using namespace antlrcpp;

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

::antlr4::internal::OnceFlag cypherparserParserOnceFlag;
#if ANTLR4_USE_THREAD_LOCAL_CACHE
static thread_local
#endif
CypherParserStaticData *cypherparserParserStaticData = nullptr;

void cypherparserParserInitialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  if (cypherparserParserStaticData != nullptr) {
    return;
  }
#else
  assert(cypherparserParserStaticData == nullptr);
#endif
  auto staticData = std::make_unique<CypherParserStaticData>(
    std::vector<std::string>{
      "cypher", "statement", "administrationStatement", "showIndexesStatement", 
      "dropIndexStatement", "createIndexStatement", "query", "regularQuery", 
      "singleQuery", "readingClause", "updatingClause", "withClause", "matchStatement", 
      "unwindStatement", "inQueryCallStatement", "createStatement", "mergeStatement", 
      "setStatement", "setItem", "deleteStatement", "removeStatement", "removeItem", 
      "returnStatement", "standaloneCallStatement", "yieldItems", "yieldItem", 
      "projectionBody", "projectionItems", "projectionItem", "orderStatement", 
      "skipStatement", "limitStatement", "sortItem", "where", "pattern", 
      "patternPart", "patternElement", "patternElementChain", "nodePattern", 
      "relationshipPattern", "relationshipDetail", "properties", "nodeLabels", 
      "nodeLabel", "relationshipTypes", "rangeLiteral", "expression", "orExpression", 
      "xorExpression", "andExpression", "notExpression", "comparisonExpression", 
      "arithmeticExpression", "unaryExpression", "accessor", "atom", "propertyExpression", 
      "functionInvocation", "explicitProcedureInvocation", "implicitProcedureInvocation", 
      "variable", "labelName", "relTypeName", "propertyKeyName", "procedureName", 
      "procedureResultField", "functionName", "namespace", "schemaName", 
      "symbolicName", "literal", "booleanLiteral", "numberLiteral", "integerLiteral", 
      "mapLiteral", "listLiteral", "parameter"
    },
    std::vector<std::string>{
      "", "'CALL'", "'YIELD'", "'CREATE'", "'DELETE'", "'DETACH'", "'EXISTS'", 
      "'LIMIT'", "'MATCH'", "'MERGE'", "'OPTIONAL'", "'ORDER'", "'BY'", 
      "'SKIP'", "'ASCENDING'", "'ASC'", "'DESCENDING'", "'DESC'", "'REMOVE'", 
      "'RETURN'", "'SET'", "'WHERE'", "'WITH'", "'UNION'", "'UNWIND'", "'AND'", 
      "'AS'", "'CONTAINS'", "'DISTINCT'", "'ENDS'", "'IN'", "'IS'", "'NOT'", 
      "'OR'", "'STARTS'", "'XOR'", "'FALSE'", "'TRUE'", "'NULL'", "'CASE'", 
      "'WHEN'", "'THEN'", "'ELSE'", "'END'", "'COUNT'", "'FILTER'", "'EXTRACT'", 
      "'ANY'", "'NONE'", "'SINGLE'", "'ALL'", "", "'ON'", "'SHOW'", "'DROP'", 
      "'FOR'", "'CONSTRAINT'", "'DO'", "'REQUIRE'", "'UNIQUE'", "'MANDATORY'", 
      "'SCALAR'", "'OF'", "'ADD'", "'VECTOR'", "'OPTIONS'", "'='", "'<>'", 
      "'<'", "'>'", "'<='", "'>='", "'+'", "'-'", "'*'", "'/'", "'%'", "'^'", 
      "'('", "')'", "'{'", "'}'", "'['", "']'", "','", "'.'", "':'", "'|'", 
      "'$'", "'..'", "';'"
    },
    std::vector<std::string>{
      "", "K_CALL", "K_YIELD", "K_CREATE", "K_DELETE", "K_DETACH", "K_EXISTS", 
      "K_LIMIT", "K_MATCH", "K_MERGE", "K_OPTIONAL", "K_ORDER", "K_BY", 
      "K_SKIP", "K_ASCENDING", "K_ASC", "K_DESCENDING", "K_DESC", "K_REMOVE", 
      "K_RETURN", "K_SET", "K_WHERE", "K_WITH", "K_UNION", "K_UNWIND", "K_AND", 
      "K_AS", "K_CONTAINS", "K_DISTINCT", "K_ENDS", "K_IN", "K_IS", "K_NOT", 
      "K_OR", "K_STARTS", "K_XOR", "K_FALSE", "K_TRUE", "K_NULL", "K_CASE", 
      "K_WHEN", "K_THEN", "K_ELSE", "K_END", "K_COUNT", "K_FILTER", "K_EXTRACT", 
      "K_ANY", "K_NONE", "K_SINGLE", "K_ALL", "K_INDEX", "K_ON", "K_SHOW", 
      "K_DROP", "K_FOR", "K_CONSTRAINT", "K_DO", "K_REQUIRE", "K_UNIQUE", 
      "K_MANDATORY", "K_SCALAR", "K_OF", "K_ADD", "K_VECTOR", "K_OPTIONS", 
      "EQ", "NEQ", "LT", "GT", "LTE", "GTE", "PLUS", "MINUS", "MULTIPLY", 
      "DIVIDE", "MODULO", "POWER", "LPAREN", "RPAREN", "LBRACE", "RBRACE", 
      "LBRACK", "RBRACK", "COMMA", "DOT", "COLON", "PIPE", "DOLLAR", "RANGE", 
      "SEMI", "HexInteger", "OctalInteger", "DecimalInteger", "DoubleLiteral", 
      "ID", "StringLiteral", "WS", "COMMENT", "LINE_COMMENT"
    }
  );
  static const int32_t serializedATNSegment[] = {
  	4,1,99,846,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,6,2,
  	7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,7,
  	14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,2,20,7,20,2,21,7,
  	21,2,22,7,22,2,23,7,23,2,24,7,24,2,25,7,25,2,26,7,26,2,27,7,27,2,28,7,
  	28,2,29,7,29,2,30,7,30,2,31,7,31,2,32,7,32,2,33,7,33,2,34,7,34,2,35,7,
  	35,2,36,7,36,2,37,7,37,2,38,7,38,2,39,7,39,2,40,7,40,2,41,7,41,2,42,7,
  	42,2,43,7,43,2,44,7,44,2,45,7,45,2,46,7,46,2,47,7,47,2,48,7,48,2,49,7,
  	49,2,50,7,50,2,51,7,51,2,52,7,52,2,53,7,53,2,54,7,54,2,55,7,55,2,56,7,
  	56,2,57,7,57,2,58,7,58,2,59,7,59,2,60,7,60,2,61,7,61,2,62,7,62,2,63,7,
  	63,2,64,7,64,2,65,7,65,2,66,7,66,2,67,7,67,2,68,7,68,2,69,7,69,2,70,7,
  	70,2,71,7,71,2,72,7,72,2,73,7,73,2,74,7,74,2,75,7,75,2,76,7,76,1,0,1,
  	0,3,0,157,8,0,1,0,1,0,1,1,1,1,3,1,163,8,1,1,2,1,2,1,2,3,2,168,8,2,1,3,
  	1,3,1,3,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,3,4,184,8,4,1,5,1,
  	5,1,5,3,5,189,8,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,3,5,201,8,5,
  	1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,3,5,213,8,5,1,5,1,5,1,5,1,5,1,
  	5,1,5,1,5,3,5,222,8,5,3,5,224,8,5,1,6,1,6,3,6,228,8,6,1,7,1,7,1,7,3,7,
  	233,8,7,1,7,5,7,236,8,7,10,7,12,7,239,9,7,1,8,4,8,242,8,8,11,8,12,8,243,
  	1,8,5,8,247,8,8,10,8,12,8,250,9,8,1,8,1,8,1,8,4,8,255,8,8,11,8,12,8,256,
  	1,8,5,8,260,8,8,10,8,12,8,263,9,8,1,8,4,8,266,8,8,11,8,12,8,267,1,8,3,
  	8,271,8,8,1,8,5,8,274,8,8,10,8,12,8,277,9,8,1,8,4,8,280,8,8,11,8,12,8,
  	281,1,8,5,8,285,8,8,10,8,12,8,288,9,8,1,8,1,8,1,8,5,8,293,8,8,10,8,12,
  	8,296,9,8,1,8,4,8,299,8,8,11,8,12,8,300,1,8,5,8,304,8,8,10,8,12,8,307,
  	9,8,1,8,4,8,310,8,8,11,8,12,8,311,1,8,3,8,315,8,8,1,8,5,8,318,8,8,10,
  	8,12,8,321,9,8,1,8,1,8,5,8,325,8,8,10,8,12,8,328,9,8,1,8,4,8,331,8,8,
  	11,8,12,8,332,1,8,3,8,336,8,8,3,8,338,8,8,1,9,1,9,1,9,3,9,343,8,9,1,10,
  	1,10,1,10,1,10,1,10,3,10,350,8,10,1,11,1,11,1,11,1,11,3,11,356,8,11,1,
  	12,3,12,359,8,12,1,12,1,12,1,12,1,12,3,12,365,8,12,1,13,1,13,1,13,1,13,
  	1,13,1,14,1,14,1,14,1,14,3,14,376,8,14,1,15,1,15,1,15,1,16,1,16,1,16,
  	1,16,1,16,5,16,386,8,16,10,16,12,16,389,9,16,1,17,1,17,1,17,1,17,5,17,
  	395,8,17,10,17,12,17,398,9,17,1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,
  	1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,3,18,416,8,18,1,19,3,19,419,8,
  	19,1,19,1,19,1,19,1,19,5,19,425,8,19,10,19,12,19,428,9,19,1,20,1,20,1,
  	20,1,20,5,20,434,8,20,10,20,12,20,437,9,20,1,21,1,21,1,21,1,21,3,21,443,
  	8,21,1,22,1,22,1,22,1,23,1,23,1,23,3,23,451,8,23,1,23,1,23,1,23,3,23,
  	456,8,23,3,23,458,8,23,1,24,1,24,1,24,5,24,463,8,24,10,24,12,24,466,9,
  	24,1,24,1,24,3,24,470,8,24,1,25,1,25,1,25,3,25,475,8,25,1,25,1,25,1,26,
  	3,26,480,8,26,1,26,1,26,3,26,484,8,26,1,26,3,26,487,8,26,1,26,3,26,490,
  	8,26,1,27,1,27,1,27,1,27,5,27,496,8,27,10,27,12,27,499,9,27,3,27,501,
  	8,27,1,28,1,28,1,28,3,28,506,8,28,1,29,1,29,1,29,1,29,1,29,5,29,513,8,
  	29,10,29,12,29,516,9,29,1,30,1,30,1,30,1,31,1,31,1,31,1,32,1,32,3,32,
  	526,8,32,1,33,1,33,1,34,1,34,1,34,5,34,533,8,34,10,34,12,34,536,9,34,
  	1,35,1,35,1,35,3,35,541,8,35,1,35,1,35,1,36,1,36,5,36,547,8,36,10,36,
  	12,36,550,9,36,1,36,1,36,1,36,1,36,3,36,556,8,36,1,37,1,37,1,37,1,38,
  	1,38,3,38,563,8,38,1,38,3,38,566,8,38,1,38,3,38,569,8,38,1,38,1,38,1,
  	39,3,39,574,8,39,1,39,1,39,3,39,578,8,39,1,39,1,39,3,39,582,8,39,1,39,
  	1,39,3,39,586,8,39,1,39,3,39,589,8,39,1,40,1,40,3,40,593,8,40,1,40,3,
  	40,596,8,40,1,40,3,40,599,8,40,1,40,3,40,602,8,40,1,40,1,40,1,41,1,41,
  	3,41,608,8,41,1,42,4,42,611,8,42,11,42,12,42,612,1,43,1,43,1,43,1,44,
  	1,44,1,44,1,44,3,44,622,8,44,1,44,5,44,625,8,44,10,44,12,44,628,9,44,
  	1,45,1,45,3,45,632,8,45,1,45,1,45,3,45,636,8,45,3,45,638,8,45,1,46,1,
  	46,1,47,1,47,1,47,5,47,645,8,47,10,47,12,47,648,9,47,1,48,1,48,1,48,5,
  	48,653,8,48,10,48,12,48,656,9,48,1,49,1,49,1,49,5,49,661,8,49,10,49,12,
  	49,664,9,49,1,50,3,50,667,8,50,1,50,1,50,1,51,1,51,1,51,5,51,674,8,51,
  	10,51,12,51,677,9,51,1,52,1,52,1,52,5,52,682,8,52,10,52,12,52,685,9,52,
  	1,53,3,53,688,8,53,1,53,1,53,5,53,692,8,53,10,53,12,53,695,9,53,1,54,
  	1,54,1,54,1,54,1,54,1,54,3,54,703,8,54,1,55,1,55,1,55,1,55,1,55,1,55,
  	1,55,1,55,1,55,1,55,1,55,1,55,1,55,3,55,718,8,55,1,56,1,56,1,56,4,56,
  	723,8,56,11,56,12,56,724,1,57,1,57,1,57,3,57,730,8,57,1,57,1,57,1,57,
  	5,57,735,8,57,10,57,12,57,738,9,57,3,57,740,8,57,1,57,1,57,1,58,1,58,
  	1,58,1,58,1,58,5,58,749,8,58,10,58,12,58,752,9,58,3,58,754,8,58,1,58,
  	1,58,1,59,1,59,1,60,1,60,1,61,1,61,1,62,1,62,1,63,1,63,1,64,1,64,1,64,
  	1,65,1,65,1,66,1,66,1,66,1,67,1,67,1,67,5,67,779,8,67,10,67,12,67,782,
  	9,67,1,68,1,68,1,68,1,68,1,68,1,68,1,68,3,68,791,8,68,1,69,1,69,1,70,
  	1,70,1,70,1,70,1,70,3,70,800,8,70,1,71,1,71,1,72,1,72,3,72,806,8,72,1,
  	73,1,73,1,74,1,74,1,74,1,74,1,74,1,74,1,74,1,74,1,74,5,74,819,8,74,10,
  	74,12,74,822,9,74,3,74,824,8,74,1,74,1,74,1,75,1,75,1,75,1,75,5,75,832,
  	8,75,10,75,12,75,835,9,75,3,75,837,8,75,1,75,1,75,1,76,1,76,1,76,3,76,
  	844,8,76,1,76,0,0,77,0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32,34,
  	36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,66,68,70,72,74,76,78,80,
  	82,84,86,88,90,92,94,96,98,100,102,104,106,108,110,112,114,116,118,120,
  	122,124,126,128,130,132,134,136,138,140,142,144,146,148,150,152,0,8,2,
  	0,3,3,8,8,1,0,14,17,2,0,30,30,66,71,1,0,72,76,1,0,72,73,2,0,1,65,95,95,
  	1,0,36,37,1,0,91,93,900,0,154,1,0,0,0,2,162,1,0,0,0,4,167,1,0,0,0,6,169,
  	1,0,0,0,8,183,1,0,0,0,10,223,1,0,0,0,12,227,1,0,0,0,14,229,1,0,0,0,16,
  	337,1,0,0,0,18,342,1,0,0,0,20,349,1,0,0,0,22,351,1,0,0,0,24,358,1,0,0,
  	0,26,366,1,0,0,0,28,371,1,0,0,0,30,377,1,0,0,0,32,380,1,0,0,0,34,390,
  	1,0,0,0,36,415,1,0,0,0,38,418,1,0,0,0,40,429,1,0,0,0,42,442,1,0,0,0,44,
  	444,1,0,0,0,46,447,1,0,0,0,48,459,1,0,0,0,50,474,1,0,0,0,52,479,1,0,0,
  	0,54,500,1,0,0,0,56,502,1,0,0,0,58,507,1,0,0,0,60,517,1,0,0,0,62,520,
  	1,0,0,0,64,523,1,0,0,0,66,527,1,0,0,0,68,529,1,0,0,0,70,540,1,0,0,0,72,
  	555,1,0,0,0,74,557,1,0,0,0,76,560,1,0,0,0,78,588,1,0,0,0,80,590,1,0,0,
  	0,82,607,1,0,0,0,84,610,1,0,0,0,86,614,1,0,0,0,88,617,1,0,0,0,90,629,
  	1,0,0,0,92,639,1,0,0,0,94,641,1,0,0,0,96,649,1,0,0,0,98,657,1,0,0,0,100,
  	666,1,0,0,0,102,670,1,0,0,0,104,678,1,0,0,0,106,687,1,0,0,0,108,702,1,
  	0,0,0,110,717,1,0,0,0,112,719,1,0,0,0,114,726,1,0,0,0,116,743,1,0,0,0,
  	118,757,1,0,0,0,120,759,1,0,0,0,122,761,1,0,0,0,124,763,1,0,0,0,126,765,
  	1,0,0,0,128,767,1,0,0,0,130,770,1,0,0,0,132,772,1,0,0,0,134,780,1,0,0,
  	0,136,790,1,0,0,0,138,792,1,0,0,0,140,799,1,0,0,0,142,801,1,0,0,0,144,
  	805,1,0,0,0,146,807,1,0,0,0,148,809,1,0,0,0,150,827,1,0,0,0,152,840,1,
  	0,0,0,154,156,3,2,1,0,155,157,5,90,0,0,156,155,1,0,0,0,156,157,1,0,0,
  	0,157,158,1,0,0,0,158,159,5,0,0,1,159,1,1,0,0,0,160,163,3,12,6,0,161,
  	163,3,4,2,0,162,160,1,0,0,0,162,161,1,0,0,0,163,3,1,0,0,0,164,168,3,6,
  	3,0,165,168,3,8,4,0,166,168,3,10,5,0,167,164,1,0,0,0,167,165,1,0,0,0,
  	167,166,1,0,0,0,168,5,1,0,0,0,169,170,5,53,0,0,170,171,5,51,0,0,171,7,
  	1,0,0,0,172,173,5,54,0,0,173,174,5,51,0,0,174,184,3,138,69,0,175,176,
  	5,54,0,0,176,177,5,51,0,0,177,178,5,52,0,0,178,179,3,86,43,0,179,180,
  	5,78,0,0,180,181,3,126,63,0,181,182,5,79,0,0,182,184,1,0,0,0,183,172,
  	1,0,0,0,183,175,1,0,0,0,184,9,1,0,0,0,185,186,5,3,0,0,186,188,5,51,0,
  	0,187,189,3,138,69,0,188,187,1,0,0,0,188,189,1,0,0,0,189,190,1,0,0,0,
  	190,191,5,55,0,0,191,192,3,76,38,0,192,193,5,52,0,0,193,194,5,78,0,0,
  	194,195,3,112,56,0,195,196,5,79,0,0,196,224,1,0,0,0,197,198,5,3,0,0,198,
  	200,5,51,0,0,199,201,3,138,69,0,200,199,1,0,0,0,200,201,1,0,0,0,201,202,
  	1,0,0,0,202,203,5,52,0,0,203,204,3,86,43,0,204,205,5,78,0,0,205,206,3,
  	126,63,0,206,207,5,79,0,0,207,224,1,0,0,0,208,209,5,3,0,0,209,210,5,64,
  	0,0,210,212,5,51,0,0,211,213,3,138,69,0,212,211,1,0,0,0,212,213,1,0,0,
  	0,213,214,1,0,0,0,214,215,5,52,0,0,215,216,3,86,43,0,216,217,5,78,0,0,
  	217,218,3,126,63,0,218,221,5,79,0,0,219,220,5,65,0,0,220,222,3,148,74,
  	0,221,219,1,0,0,0,221,222,1,0,0,0,222,224,1,0,0,0,223,185,1,0,0,0,223,
  	197,1,0,0,0,223,208,1,0,0,0,224,11,1,0,0,0,225,228,3,14,7,0,226,228,3,
  	46,23,0,227,225,1,0,0,0,227,226,1,0,0,0,228,13,1,0,0,0,229,237,3,16,8,
  	0,230,232,5,23,0,0,231,233,5,50,0,0,232,231,1,0,0,0,232,233,1,0,0,0,233,
  	234,1,0,0,0,234,236,3,16,8,0,235,230,1,0,0,0,236,239,1,0,0,0,237,235,
  	1,0,0,0,237,238,1,0,0,0,238,15,1,0,0,0,239,237,1,0,0,0,240,242,3,22,11,
  	0,241,240,1,0,0,0,242,243,1,0,0,0,243,241,1,0,0,0,243,244,1,0,0,0,244,
  	248,1,0,0,0,245,247,3,18,9,0,246,245,1,0,0,0,247,250,1,0,0,0,248,246,
  	1,0,0,0,248,249,1,0,0,0,249,251,1,0,0,0,250,248,1,0,0,0,251,252,3,44,
  	22,0,252,338,1,0,0,0,253,255,3,22,11,0,254,253,1,0,0,0,255,256,1,0,0,
  	0,256,254,1,0,0,0,256,257,1,0,0,0,257,261,1,0,0,0,258,260,3,18,9,0,259,
  	258,1,0,0,0,260,263,1,0,0,0,261,259,1,0,0,0,261,262,1,0,0,0,262,265,1,
  	0,0,0,263,261,1,0,0,0,264,266,3,20,10,0,265,264,1,0,0,0,266,267,1,0,0,
  	0,267,265,1,0,0,0,267,268,1,0,0,0,268,270,1,0,0,0,269,271,3,44,22,0,270,
  	269,1,0,0,0,270,271,1,0,0,0,271,338,1,0,0,0,272,274,3,18,9,0,273,272,
  	1,0,0,0,274,277,1,0,0,0,275,273,1,0,0,0,275,276,1,0,0,0,276,279,1,0,0,
  	0,277,275,1,0,0,0,278,280,3,22,11,0,279,278,1,0,0,0,280,281,1,0,0,0,281,
  	279,1,0,0,0,281,282,1,0,0,0,282,286,1,0,0,0,283,285,3,18,9,0,284,283,
  	1,0,0,0,285,288,1,0,0,0,286,284,1,0,0,0,286,287,1,0,0,0,287,289,1,0,0,
  	0,288,286,1,0,0,0,289,290,3,44,22,0,290,338,1,0,0,0,291,293,3,18,9,0,
  	292,291,1,0,0,0,293,296,1,0,0,0,294,292,1,0,0,0,294,295,1,0,0,0,295,298,
  	1,0,0,0,296,294,1,0,0,0,297,299,3,22,11,0,298,297,1,0,0,0,299,300,1,0,
  	0,0,300,298,1,0,0,0,300,301,1,0,0,0,301,305,1,0,0,0,302,304,3,18,9,0,
  	303,302,1,0,0,0,304,307,1,0,0,0,305,303,1,0,0,0,305,306,1,0,0,0,306,309,
  	1,0,0,0,307,305,1,0,0,0,308,310,3,20,10,0,309,308,1,0,0,0,310,311,1,0,
  	0,0,311,309,1,0,0,0,311,312,1,0,0,0,312,314,1,0,0,0,313,315,3,44,22,0,
  	314,313,1,0,0,0,314,315,1,0,0,0,315,338,1,0,0,0,316,318,3,18,9,0,317,
  	316,1,0,0,0,318,321,1,0,0,0,319,317,1,0,0,0,319,320,1,0,0,0,320,322,1,
  	0,0,0,321,319,1,0,0,0,322,338,3,44,22,0,323,325,3,18,9,0,324,323,1,0,
  	0,0,325,328,1,0,0,0,326,324,1,0,0,0,326,327,1,0,0,0,327,330,1,0,0,0,328,
  	326,1,0,0,0,329,331,3,20,10,0,330,329,1,0,0,0,331,332,1,0,0,0,332,330,
  	1,0,0,0,332,333,1,0,0,0,333,335,1,0,0,0,334,336,3,44,22,0,335,334,1,0,
  	0,0,335,336,1,0,0,0,336,338,1,0,0,0,337,241,1,0,0,0,337,254,1,0,0,0,337,
  	275,1,0,0,0,337,294,1,0,0,0,337,319,1,0,0,0,337,326,1,0,0,0,338,17,1,
  	0,0,0,339,343,3,24,12,0,340,343,3,26,13,0,341,343,3,28,14,0,342,339,1,
  	0,0,0,342,340,1,0,0,0,342,341,1,0,0,0,343,19,1,0,0,0,344,350,3,30,15,
  	0,345,350,3,32,16,0,346,350,3,38,19,0,347,350,3,34,17,0,348,350,3,40,
  	20,0,349,344,1,0,0,0,349,345,1,0,0,0,349,346,1,0,0,0,349,347,1,0,0,0,
  	349,348,1,0,0,0,350,21,1,0,0,0,351,352,5,22,0,0,352,355,3,52,26,0,353,
  	354,5,21,0,0,354,356,3,66,33,0,355,353,1,0,0,0,355,356,1,0,0,0,356,23,
  	1,0,0,0,357,359,5,10,0,0,358,357,1,0,0,0,358,359,1,0,0,0,359,360,1,0,
  	0,0,360,361,5,8,0,0,361,364,3,68,34,0,362,363,5,21,0,0,363,365,3,66,33,
  	0,364,362,1,0,0,0,364,365,1,0,0,0,365,25,1,0,0,0,366,367,5,24,0,0,367,
  	368,3,92,46,0,368,369,5,26,0,0,369,370,3,120,60,0,370,27,1,0,0,0,371,
  	372,5,1,0,0,372,375,3,116,58,0,373,374,5,2,0,0,374,376,3,48,24,0,375,
  	373,1,0,0,0,375,376,1,0,0,0,376,29,1,0,0,0,377,378,5,3,0,0,378,379,3,
  	68,34,0,379,31,1,0,0,0,380,381,5,9,0,0,381,387,3,70,35,0,382,383,5,52,
  	0,0,383,384,7,0,0,0,384,386,3,34,17,0,385,382,1,0,0,0,386,389,1,0,0,0,
  	387,385,1,0,0,0,387,388,1,0,0,0,388,33,1,0,0,0,389,387,1,0,0,0,390,391,
  	5,20,0,0,391,396,3,36,18,0,392,393,5,84,0,0,393,395,3,36,18,0,394,392,
  	1,0,0,0,395,398,1,0,0,0,396,394,1,0,0,0,396,397,1,0,0,0,397,35,1,0,0,
  	0,398,396,1,0,0,0,399,400,3,112,56,0,400,401,5,66,0,0,401,402,3,92,46,
  	0,402,416,1,0,0,0,403,404,3,120,60,0,404,405,5,66,0,0,405,406,3,92,46,
  	0,406,416,1,0,0,0,407,408,3,120,60,0,408,409,5,72,0,0,409,410,5,66,0,
  	0,410,411,3,92,46,0,411,416,1,0,0,0,412,413,3,120,60,0,413,414,3,84,42,
  	0,414,416,1,0,0,0,415,399,1,0,0,0,415,403,1,0,0,0,415,407,1,0,0,0,415,
  	412,1,0,0,0,416,37,1,0,0,0,417,419,5,5,0,0,418,417,1,0,0,0,418,419,1,
  	0,0,0,419,420,1,0,0,0,420,421,5,4,0,0,421,426,3,92,46,0,422,423,5,84,
  	0,0,423,425,3,92,46,0,424,422,1,0,0,0,425,428,1,0,0,0,426,424,1,0,0,0,
  	426,427,1,0,0,0,427,39,1,0,0,0,428,426,1,0,0,0,429,430,5,18,0,0,430,435,
  	3,42,21,0,431,432,5,84,0,0,432,434,3,42,21,0,433,431,1,0,0,0,434,437,
  	1,0,0,0,435,433,1,0,0,0,435,436,1,0,0,0,436,41,1,0,0,0,437,435,1,0,0,
  	0,438,439,3,120,60,0,439,440,3,84,42,0,440,443,1,0,0,0,441,443,3,112,
  	56,0,442,438,1,0,0,0,442,441,1,0,0,0,443,43,1,0,0,0,444,445,5,19,0,0,
  	445,446,3,52,26,0,446,45,1,0,0,0,447,450,5,1,0,0,448,451,3,116,58,0,449,
  	451,3,118,59,0,450,448,1,0,0,0,450,449,1,0,0,0,451,457,1,0,0,0,452,455,
  	5,2,0,0,453,456,5,74,0,0,454,456,3,48,24,0,455,453,1,0,0,0,455,454,1,
  	0,0,0,456,458,1,0,0,0,457,452,1,0,0,0,457,458,1,0,0,0,458,47,1,0,0,0,
  	459,464,3,50,25,0,460,461,5,84,0,0,461,463,3,50,25,0,462,460,1,0,0,0,
  	463,466,1,0,0,0,464,462,1,0,0,0,464,465,1,0,0,0,465,469,1,0,0,0,466,464,
  	1,0,0,0,467,468,5,21,0,0,468,470,3,66,33,0,469,467,1,0,0,0,469,470,1,
  	0,0,0,470,49,1,0,0,0,471,472,3,130,65,0,472,473,5,26,0,0,473,475,1,0,
  	0,0,474,471,1,0,0,0,474,475,1,0,0,0,475,476,1,0,0,0,476,477,3,120,60,
  	0,477,51,1,0,0,0,478,480,5,28,0,0,479,478,1,0,0,0,479,480,1,0,0,0,480,
  	481,1,0,0,0,481,483,3,54,27,0,482,484,3,58,29,0,483,482,1,0,0,0,483,484,
  	1,0,0,0,484,486,1,0,0,0,485,487,3,60,30,0,486,485,1,0,0,0,486,487,1,0,
  	0,0,487,489,1,0,0,0,488,490,3,62,31,0,489,488,1,0,0,0,489,490,1,0,0,0,
  	490,53,1,0,0,0,491,501,5,74,0,0,492,497,3,56,28,0,493,494,5,84,0,0,494,
  	496,3,56,28,0,495,493,1,0,0,0,496,499,1,0,0,0,497,495,1,0,0,0,497,498,
  	1,0,0,0,498,501,1,0,0,0,499,497,1,0,0,0,500,491,1,0,0,0,500,492,1,0,0,
  	0,501,55,1,0,0,0,502,505,3,92,46,0,503,504,5,26,0,0,504,506,3,120,60,
  	0,505,503,1,0,0,0,505,506,1,0,0,0,506,57,1,0,0,0,507,508,5,11,0,0,508,
  	509,5,12,0,0,509,514,3,64,32,0,510,511,5,84,0,0,511,513,3,64,32,0,512,
  	510,1,0,0,0,513,516,1,0,0,0,514,512,1,0,0,0,514,515,1,0,0,0,515,59,1,
  	0,0,0,516,514,1,0,0,0,517,518,5,13,0,0,518,519,3,92,46,0,519,61,1,0,0,
  	0,520,521,5,7,0,0,521,522,3,92,46,0,522,63,1,0,0,0,523,525,3,92,46,0,
  	524,526,7,1,0,0,525,524,1,0,0,0,525,526,1,0,0,0,526,65,1,0,0,0,527,528,
  	3,92,46,0,528,67,1,0,0,0,529,534,3,70,35,0,530,531,5,84,0,0,531,533,3,
  	70,35,0,532,530,1,0,0,0,533,536,1,0,0,0,534,532,1,0,0,0,534,535,1,0,0,
  	0,535,69,1,0,0,0,536,534,1,0,0,0,537,538,3,120,60,0,538,539,5,66,0,0,
  	539,541,1,0,0,0,540,537,1,0,0,0,540,541,1,0,0,0,541,542,1,0,0,0,542,543,
  	3,72,36,0,543,71,1,0,0,0,544,548,3,76,38,0,545,547,3,74,37,0,546,545,
  	1,0,0,0,547,550,1,0,0,0,548,546,1,0,0,0,548,549,1,0,0,0,549,556,1,0,0,
  	0,550,548,1,0,0,0,551,552,5,78,0,0,552,553,3,72,36,0,553,554,5,79,0,0,
  	554,556,1,0,0,0,555,544,1,0,0,0,555,551,1,0,0,0,556,73,1,0,0,0,557,558,
  	3,78,39,0,558,559,3,76,38,0,559,75,1,0,0,0,560,562,5,78,0,0,561,563,3,
  	120,60,0,562,561,1,0,0,0,562,563,1,0,0,0,563,565,1,0,0,0,564,566,3,84,
  	42,0,565,564,1,0,0,0,565,566,1,0,0,0,566,568,1,0,0,0,567,569,3,82,41,
  	0,568,567,1,0,0,0,568,569,1,0,0,0,569,570,1,0,0,0,570,571,5,79,0,0,571,
  	77,1,0,0,0,572,574,5,68,0,0,573,572,1,0,0,0,573,574,1,0,0,0,574,575,1,
  	0,0,0,575,577,5,73,0,0,576,578,3,80,40,0,577,576,1,0,0,0,577,578,1,0,
  	0,0,578,579,1,0,0,0,579,581,5,73,0,0,580,582,5,69,0,0,581,580,1,0,0,0,
  	581,582,1,0,0,0,582,589,1,0,0,0,583,585,5,73,0,0,584,586,3,80,40,0,585,
  	584,1,0,0,0,585,586,1,0,0,0,586,587,1,0,0,0,587,589,5,73,0,0,588,573,
  	1,0,0,0,588,583,1,0,0,0,589,79,1,0,0,0,590,592,5,82,0,0,591,593,3,120,
  	60,0,592,591,1,0,0,0,592,593,1,0,0,0,593,595,1,0,0,0,594,596,3,88,44,
  	0,595,594,1,0,0,0,595,596,1,0,0,0,596,598,1,0,0,0,597,599,3,90,45,0,598,
  	597,1,0,0,0,598,599,1,0,0,0,599,601,1,0,0,0,600,602,3,82,41,0,601,600,
  	1,0,0,0,601,602,1,0,0,0,602,603,1,0,0,0,603,604,5,83,0,0,604,81,1,0,0,
  	0,605,608,3,148,74,0,606,608,3,152,76,0,607,605,1,0,0,0,607,606,1,0,0,
  	0,608,83,1,0,0,0,609,611,3,86,43,0,610,609,1,0,0,0,611,612,1,0,0,0,612,
  	610,1,0,0,0,612,613,1,0,0,0,613,85,1,0,0,0,614,615,5,86,0,0,615,616,3,
  	122,61,0,616,87,1,0,0,0,617,618,5,86,0,0,618,626,3,124,62,0,619,621,5,
  	87,0,0,620,622,5,86,0,0,621,620,1,0,0,0,621,622,1,0,0,0,622,623,1,0,0,
  	0,623,625,3,124,62,0,624,619,1,0,0,0,625,628,1,0,0,0,626,624,1,0,0,0,
  	626,627,1,0,0,0,627,89,1,0,0,0,628,626,1,0,0,0,629,631,5,74,0,0,630,632,
  	3,146,73,0,631,630,1,0,0,0,631,632,1,0,0,0,632,637,1,0,0,0,633,635,5,
  	89,0,0,634,636,3,146,73,0,635,634,1,0,0,0,635,636,1,0,0,0,636,638,1,0,
  	0,0,637,633,1,0,0,0,637,638,1,0,0,0,638,91,1,0,0,0,639,640,3,94,47,0,
  	640,93,1,0,0,0,641,646,3,96,48,0,642,643,5,33,0,0,643,645,3,96,48,0,644,
  	642,1,0,0,0,645,648,1,0,0,0,646,644,1,0,0,0,646,647,1,0,0,0,647,95,1,
  	0,0,0,648,646,1,0,0,0,649,654,3,98,49,0,650,651,5,35,0,0,651,653,3,98,
  	49,0,652,650,1,0,0,0,653,656,1,0,0,0,654,652,1,0,0,0,654,655,1,0,0,0,
  	655,97,1,0,0,0,656,654,1,0,0,0,657,662,3,100,50,0,658,659,5,25,0,0,659,
  	661,3,100,50,0,660,658,1,0,0,0,661,664,1,0,0,0,662,660,1,0,0,0,662,663,
  	1,0,0,0,663,99,1,0,0,0,664,662,1,0,0,0,665,667,5,32,0,0,666,665,1,0,0,
  	0,666,667,1,0,0,0,667,668,1,0,0,0,668,669,3,102,51,0,669,101,1,0,0,0,
  	670,675,3,104,52,0,671,672,7,2,0,0,672,674,3,104,52,0,673,671,1,0,0,0,
  	674,677,1,0,0,0,675,673,1,0,0,0,675,676,1,0,0,0,676,103,1,0,0,0,677,675,
  	1,0,0,0,678,683,3,106,53,0,679,680,7,3,0,0,680,682,3,106,53,0,681,679,
  	1,0,0,0,682,685,1,0,0,0,683,681,1,0,0,0,683,684,1,0,0,0,684,105,1,0,0,
  	0,685,683,1,0,0,0,686,688,7,4,0,0,687,686,1,0,0,0,687,688,1,0,0,0,688,
  	689,1,0,0,0,689,693,3,110,55,0,690,692,3,108,54,0,691,690,1,0,0,0,692,
  	695,1,0,0,0,693,691,1,0,0,0,693,694,1,0,0,0,694,107,1,0,0,0,695,693,1,
  	0,0,0,696,697,5,85,0,0,697,703,3,126,63,0,698,699,5,82,0,0,699,700,3,
  	92,46,0,700,701,5,83,0,0,701,703,1,0,0,0,702,696,1,0,0,0,702,698,1,0,
  	0,0,703,109,1,0,0,0,704,718,3,140,70,0,705,718,3,152,76,0,706,707,5,44,
  	0,0,707,708,5,78,0,0,708,709,5,74,0,0,709,718,5,79,0,0,710,718,3,114,
  	57,0,711,718,3,120,60,0,712,713,5,78,0,0,713,714,3,92,46,0,714,715,5,
  	79,0,0,715,718,1,0,0,0,716,718,3,150,75,0,717,704,1,0,0,0,717,705,1,0,
  	0,0,717,706,1,0,0,0,717,710,1,0,0,0,717,711,1,0,0,0,717,712,1,0,0,0,717,
  	716,1,0,0,0,718,111,1,0,0,0,719,722,3,110,55,0,720,721,5,85,0,0,721,723,
  	3,126,63,0,722,720,1,0,0,0,723,724,1,0,0,0,724,722,1,0,0,0,724,725,1,
  	0,0,0,725,113,1,0,0,0,726,727,3,132,66,0,727,729,5,78,0,0,728,730,5,28,
  	0,0,729,728,1,0,0,0,729,730,1,0,0,0,730,739,1,0,0,0,731,736,3,92,46,0,
  	732,733,5,84,0,0,733,735,3,92,46,0,734,732,1,0,0,0,735,738,1,0,0,0,736,
  	734,1,0,0,0,736,737,1,0,0,0,737,740,1,0,0,0,738,736,1,0,0,0,739,731,1,
  	0,0,0,739,740,1,0,0,0,740,741,1,0,0,0,741,742,5,79,0,0,742,115,1,0,0,
  	0,743,744,3,128,64,0,744,753,5,78,0,0,745,750,3,92,46,0,746,747,5,84,
  	0,0,747,749,3,92,46,0,748,746,1,0,0,0,749,752,1,0,0,0,750,748,1,0,0,0,
  	750,751,1,0,0,0,751,754,1,0,0,0,752,750,1,0,0,0,753,745,1,0,0,0,753,754,
  	1,0,0,0,754,755,1,0,0,0,755,756,5,79,0,0,756,117,1,0,0,0,757,758,3,128,
  	64,0,758,119,1,0,0,0,759,760,3,138,69,0,760,121,1,0,0,0,761,762,3,136,
  	68,0,762,123,1,0,0,0,763,764,3,136,68,0,764,125,1,0,0,0,765,766,3,136,
  	68,0,766,127,1,0,0,0,767,768,3,134,67,0,768,769,3,138,69,0,769,129,1,
  	0,0,0,770,771,3,138,69,0,771,131,1,0,0,0,772,773,3,134,67,0,773,774,3,
  	138,69,0,774,133,1,0,0,0,775,776,3,138,69,0,776,777,5,85,0,0,777,779,
  	1,0,0,0,778,775,1,0,0,0,779,782,1,0,0,0,780,778,1,0,0,0,780,781,1,0,0,
  	0,781,135,1,0,0,0,782,780,1,0,0,0,783,791,3,138,69,0,784,791,5,51,0,0,
  	785,791,5,52,0,0,786,791,5,8,0,0,787,791,5,3,0,0,788,791,5,21,0,0,789,
  	791,5,19,0,0,790,783,1,0,0,0,790,784,1,0,0,0,790,785,1,0,0,0,790,786,
  	1,0,0,0,790,787,1,0,0,0,790,788,1,0,0,0,790,789,1,0,0,0,791,137,1,0,0,
  	0,792,793,7,5,0,0,793,139,1,0,0,0,794,800,3,142,71,0,795,800,5,38,0,0,
  	796,800,3,144,72,0,797,800,5,96,0,0,798,800,3,148,74,0,799,794,1,0,0,
  	0,799,795,1,0,0,0,799,796,1,0,0,0,799,797,1,0,0,0,799,798,1,0,0,0,800,
  	141,1,0,0,0,801,802,7,6,0,0,802,143,1,0,0,0,803,806,5,94,0,0,804,806,
  	3,146,73,0,805,803,1,0,0,0,805,804,1,0,0,0,806,145,1,0,0,0,807,808,7,
  	7,0,0,808,147,1,0,0,0,809,823,5,80,0,0,810,811,3,126,63,0,811,812,5,86,
  	0,0,812,820,3,92,46,0,813,814,5,84,0,0,814,815,3,126,63,0,815,816,5,86,
  	0,0,816,817,3,92,46,0,817,819,1,0,0,0,818,813,1,0,0,0,819,822,1,0,0,0,
  	820,818,1,0,0,0,820,821,1,0,0,0,821,824,1,0,0,0,822,820,1,0,0,0,823,810,
  	1,0,0,0,823,824,1,0,0,0,824,825,1,0,0,0,825,826,5,81,0,0,826,149,1,0,
  	0,0,827,836,5,82,0,0,828,833,3,92,46,0,829,830,5,84,0,0,830,832,3,92,
  	46,0,831,829,1,0,0,0,832,835,1,0,0,0,833,831,1,0,0,0,833,834,1,0,0,0,
  	834,837,1,0,0,0,835,833,1,0,0,0,836,828,1,0,0,0,836,837,1,0,0,0,837,838,
  	1,0,0,0,838,839,5,83,0,0,839,151,1,0,0,0,840,843,5,88,0,0,841,844,3,138,
  	69,0,842,844,5,93,0,0,843,841,1,0,0,0,843,842,1,0,0,0,844,153,1,0,0,0,
  	107,156,162,167,183,188,200,212,221,223,227,232,237,243,248,256,261,267,
  	270,275,281,286,294,300,305,311,314,319,326,332,335,337,342,349,355,358,
  	364,375,387,396,415,418,426,435,442,450,455,457,464,469,474,479,483,486,
  	489,497,500,505,514,525,534,540,548,555,562,565,568,573,577,581,585,588,
  	592,595,598,601,607,612,621,626,631,635,637,646,654,662,666,675,683,687,
  	693,702,717,724,729,736,739,750,753,780,790,799,805,820,823,833,836,843
  };
  staticData->serializedATN = antlr4::atn::SerializedATNView(serializedATNSegment, sizeof(serializedATNSegment) / sizeof(serializedATNSegment[0]));

  antlr4::atn::ATNDeserializer deserializer;
  staticData->atn = deserializer.deserialize(staticData->serializedATN);

  const size_t count = staticData->atn->getNumberOfDecisions();
  staticData->decisionToDFA.reserve(count);
  for (size_t i = 0; i < count; i++) { 
    staticData->decisionToDFA.emplace_back(staticData->atn->getDecisionState(i), i);
  }
  cypherparserParserStaticData = staticData.release();
}

}

CypherParser::CypherParser(TokenStream *input) : CypherParser(input, antlr4::atn::ParserATNSimulatorOptions()) {}

CypherParser::CypherParser(TokenStream *input, const antlr4::atn::ParserATNSimulatorOptions &options) : Parser(input) {
  CypherParser::initialize();
  _interpreter = new atn::ParserATNSimulator(this, *cypherparserParserStaticData->atn, cypherparserParserStaticData->decisionToDFA, cypherparserParserStaticData->sharedContextCache, options);
}

CypherParser::~CypherParser() {
  delete _interpreter;
}

const atn::ATN& CypherParser::getATN() const {
  return *cypherparserParserStaticData->atn;
}

std::string CypherParser::getGrammarFileName() const {
  return "CypherParser.g4";
}

const std::vector<std::string>& CypherParser::getRuleNames() const {
  return cypherparserParserStaticData->ruleNames;
}

const dfa::Vocabulary& CypherParser::getVocabulary() const {
  return cypherparserParserStaticData->vocabulary;
}

antlr4::atn::SerializedATNView CypherParser::getSerializedATN() const {
  return cypherparserParserStaticData->serializedATN;
}


//----------------- CypherContext ------------------------------------------------------------------

CypherParser::CypherContext::CypherContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::StatementContext* CypherParser::CypherContext::statement() {
  return getRuleContext<CypherParser::StatementContext>(0);
}

tree::TerminalNode* CypherParser::CypherContext::EOF() {
  return getToken(CypherParser::EOF, 0);
}

tree::TerminalNode* CypherParser::CypherContext::SEMI() {
  return getToken(CypherParser::SEMI, 0);
}


size_t CypherParser::CypherContext::getRuleIndex() const {
  return CypherParser::RuleCypher;
}


std::any CypherParser::CypherContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCypher(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CypherContext* CypherParser::cypher() {
  CypherContext *_localctx = _tracker.createInstance<CypherContext>(_ctx, getState());
  enterRule(_localctx, 0, CypherParser::RuleCypher);
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
    setState(154);
    statement();
    setState(156);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::SEMI) {
      setState(155);
      match(CypherParser::SEMI);
    }
    setState(158);
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

CypherParser::QueryContext* CypherParser::StatementContext::query() {
  return getRuleContext<CypherParser::QueryContext>(0);
}

CypherParser::AdministrationStatementContext* CypherParser::StatementContext::administrationStatement() {
  return getRuleContext<CypherParser::AdministrationStatementContext>(0);
}


size_t CypherParser::StatementContext::getRuleIndex() const {
  return CypherParser::RuleStatement;
}


std::any CypherParser::StatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
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
    setState(162);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 1, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(160);
      query();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(161);
      administrationStatement();
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

CypherParser::CreateIndexStatementContext* CypherParser::AdministrationStatementContext::createIndexStatement() {
  return getRuleContext<CypherParser::CreateIndexStatementContext>(0);
}


size_t CypherParser::AdministrationStatementContext::getRuleIndex() const {
  return CypherParser::RuleAdministrationStatement;
}


std::any CypherParser::AdministrationStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
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
    setState(167);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_SHOW: {
        enterOuterAlt(_localctx, 1);
        setState(164);
        showIndexesStatement();
        break;
      }

      case CypherParser::K_DROP: {
        enterOuterAlt(_localctx, 2);
        setState(165);
        dropIndexStatement();
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 3);
        setState(166);
        createIndexStatement();
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


size_t CypherParser::ShowIndexesStatementContext::getRuleIndex() const {
  return CypherParser::RuleShowIndexesStatement;
}


std::any CypherParser::ShowIndexesStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitShowIndexesStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ShowIndexesStatementContext* CypherParser::showIndexesStatement() {
  ShowIndexesStatementContext *_localctx = _tracker.createInstance<ShowIndexesStatementContext>(_ctx, getState());
  enterRule(_localctx, 6, CypherParser::RuleShowIndexesStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(169);
    match(CypherParser::K_SHOW);
    setState(170);
    match(CypherParser::K_INDEX);
   
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


size_t CypherParser::DropIndexStatementContext::getRuleIndex() const {
  return CypherParser::RuleDropIndexStatement;
}

void CypherParser::DropIndexStatementContext::copyFrom(DropIndexStatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- DropIndexByNameContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::DropIndexByNameContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropIndexByNameContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

CypherParser::SymbolicNameContext* CypherParser::DropIndexByNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

CypherParser::DropIndexByNameContext::DropIndexByNameContext(DropIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::DropIndexByNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDropIndexByName(this);
  else
    return visitor->visitChildren(this);
}
//----------------- DropIndexByLabelContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::DropIndexByLabelContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::DropIndexByLabelContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::DropIndexByLabelContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::DropIndexByLabelContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::DropIndexByLabelContext::DropIndexByLabelContext(DropIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::DropIndexByLabelContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDropIndexByLabel(this);
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
    setState(183);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 3, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::DropIndexByNameContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(172);
      match(CypherParser::K_DROP);
      setState(173);
      match(CypherParser::K_INDEX);
      setState(174);
      symbolicName();
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::DropIndexByLabelContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(175);
      match(CypherParser::K_DROP);
      setState(176);
      match(CypherParser::K_INDEX);
      setState(177);
      match(CypherParser::K_ON);
      setState(178);
      nodeLabel();
      setState(179);
      match(CypherParser::LPAREN);
      setState(180);
      propertyKeyName();
      setState(181);
      match(CypherParser::RPAREN);
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

//----------------- CreateIndexStatementContext ------------------------------------------------------------------

CypherParser::CreateIndexStatementContext::CreateIndexStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}


size_t CypherParser::CreateIndexStatementContext::getRuleIndex() const {
  return CypherParser::RuleCreateIndexStatement;
}

void CypherParser::CreateIndexStatementContext::copyFrom(CreateIndexStatementContext *ctx) {
  ParserRuleContext::copyFrom(ctx);
}

//----------------- CreateIndexByPatternContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_FOR() {
  return getToken(CypherParser::K_FOR, 0);
}

CypherParser::NodePatternContext* CypherParser::CreateIndexByPatternContext::nodePattern() {
  return getRuleContext<CypherParser::NodePatternContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyExpressionContext* CypherParser::CreateIndexByPatternContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexByPatternContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateIndexByPatternContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

CypherParser::CreateIndexByPatternContext::CreateIndexByPatternContext(CreateIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateIndexByPatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateIndexByPattern(this);
  else
    return visitor->visitChildren(this);
}
//----------------- CreateIndexByLabelContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::CreateIndexByLabelContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::CreateIndexByLabelContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexByLabelContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateIndexByLabelContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

CypherParser::CreateIndexByLabelContext::CreateIndexByLabelContext(CreateIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateIndexByLabelContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateIndexByLabel(this);
  else
    return visitor->visitChildren(this);
}
//----------------- CreateVectorIndexContext ------------------------------------------------------------------

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_VECTOR() {
  return getToken(CypherParser::K_VECTOR, 0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::CreateVectorIndexContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::CreateVectorIndexContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::SymbolicNameContext* CypherParser::CreateVectorIndexContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateVectorIndexContext::K_OPTIONS() {
  return getToken(CypherParser::K_OPTIONS, 0);
}

CypherParser::MapLiteralContext* CypherParser::CreateVectorIndexContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}

CypherParser::CreateVectorIndexContext::CreateVectorIndexContext(CreateIndexStatementContext *ctx) { copyFrom(ctx); }


std::any CypherParser::CreateVectorIndexContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateVectorIndex(this);
  else
    return visitor->visitChildren(this);
}
CypherParser::CreateIndexStatementContext* CypherParser::createIndexStatement() {
  CreateIndexStatementContext *_localctx = _tracker.createInstance<CreateIndexStatementContext>(_ctx, getState());
  enterRule(_localctx, 10, CypherParser::RuleCreateIndexStatement);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(223);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 8, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::CreateIndexByPatternContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(185);
      match(CypherParser::K_CREATE);
      setState(186);
      match(CypherParser::K_INDEX);
      setState(188);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 4, _ctx)) {
      case 1: {
        setState(187);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(190);
      match(CypherParser::K_FOR);
      setState(191);
      nodePattern();
      setState(192);
      match(CypherParser::K_ON);
      setState(193);
      match(CypherParser::LPAREN);
      setState(194);
      propertyExpression();
      setState(195);
      match(CypherParser::RPAREN);
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::CreateIndexByLabelContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(197);
      match(CypherParser::K_CREATE);
      setState(198);
      match(CypherParser::K_INDEX);
      setState(200);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 5, _ctx)) {
      case 1: {
        setState(199);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(202);
      match(CypherParser::K_ON);
      setState(203);
      nodeLabel();
      setState(204);
      match(CypherParser::LPAREN);
      setState(205);
      propertyKeyName();
      setState(206);
      match(CypherParser::RPAREN);
      break;
    }

    case 3: {
      _localctx = _tracker.createInstance<CypherParser::CreateVectorIndexContext>(_localctx);
      enterOuterAlt(_localctx, 3);
      setState(208);
      match(CypherParser::K_CREATE);
      setState(209);
      match(CypherParser::K_VECTOR);
      setState(210);
      match(CypherParser::K_INDEX);
      setState(212);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 6, _ctx)) {
      case 1: {
        setState(211);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(214);
      match(CypherParser::K_ON);
      setState(215);
      nodeLabel();
      setState(216);
      match(CypherParser::LPAREN);
      setState(217);
      propertyKeyName();
      setState(218);
      match(CypherParser::RPAREN);
      setState(221);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_OPTIONS) {
        setState(219);
        match(CypherParser::K_OPTIONS);
        setState(220);
        mapLiteral();
      }
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

//----------------- QueryContext ------------------------------------------------------------------

CypherParser::QueryContext::QueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::RegularQueryContext* CypherParser::QueryContext::regularQuery() {
  return getRuleContext<CypherParser::RegularQueryContext>(0);
}

CypherParser::StandaloneCallStatementContext* CypherParser::QueryContext::standaloneCallStatement() {
  return getRuleContext<CypherParser::StandaloneCallStatementContext>(0);
}


size_t CypherParser::QueryContext::getRuleIndex() const {
  return CypherParser::RuleQuery;
}


std::any CypherParser::QueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitQuery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::QueryContext* CypherParser::query() {
  QueryContext *_localctx = _tracker.createInstance<QueryContext>(_ctx, getState());
  enterRule(_localctx, 12, CypherParser::RuleQuery);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(227);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 9, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(225);
      regularQuery();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(226);
      standaloneCallStatement();
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

//----------------- RegularQueryContext ------------------------------------------------------------------

CypherParser::RegularQueryContext::RegularQueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::SingleQueryContext *> CypherParser::RegularQueryContext::singleQuery() {
  return getRuleContexts<CypherParser::SingleQueryContext>();
}

CypherParser::SingleQueryContext* CypherParser::RegularQueryContext::singleQuery(size_t i) {
  return getRuleContext<CypherParser::SingleQueryContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::RegularQueryContext::K_UNION() {
  return getTokens(CypherParser::K_UNION);
}

tree::TerminalNode* CypherParser::RegularQueryContext::K_UNION(size_t i) {
  return getToken(CypherParser::K_UNION, i);
}

std::vector<tree::TerminalNode *> CypherParser::RegularQueryContext::K_ALL() {
  return getTokens(CypherParser::K_ALL);
}

tree::TerminalNode* CypherParser::RegularQueryContext::K_ALL(size_t i) {
  return getToken(CypherParser::K_ALL, i);
}


size_t CypherParser::RegularQueryContext::getRuleIndex() const {
  return CypherParser::RuleRegularQuery;
}


std::any CypherParser::RegularQueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRegularQuery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RegularQueryContext* CypherParser::regularQuery() {
  RegularQueryContext *_localctx = _tracker.createInstance<RegularQueryContext>(_ctx, getState());
  enterRule(_localctx, 14, CypherParser::RuleRegularQuery);
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
    setState(229);
    singleQuery();
    setState(237);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_UNION) {
      setState(230);
      match(CypherParser::K_UNION);
      setState(232);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_ALL) {
        setState(231);
        match(CypherParser::K_ALL);
      }
      setState(234);
      singleQuery();
      setState(239);
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

//----------------- SingleQueryContext ------------------------------------------------------------------

CypherParser::SingleQueryContext::SingleQueryContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ReturnStatementContext* CypherParser::SingleQueryContext::returnStatement() {
  return getRuleContext<CypherParser::ReturnStatementContext>(0);
}

std::vector<CypherParser::WithClauseContext *> CypherParser::SingleQueryContext::withClause() {
  return getRuleContexts<CypherParser::WithClauseContext>();
}

CypherParser::WithClauseContext* CypherParser::SingleQueryContext::withClause(size_t i) {
  return getRuleContext<CypherParser::WithClauseContext>(i);
}

std::vector<CypherParser::ReadingClauseContext *> CypherParser::SingleQueryContext::readingClause() {
  return getRuleContexts<CypherParser::ReadingClauseContext>();
}

CypherParser::ReadingClauseContext* CypherParser::SingleQueryContext::readingClause(size_t i) {
  return getRuleContext<CypherParser::ReadingClauseContext>(i);
}

std::vector<CypherParser::UpdatingClauseContext *> CypherParser::SingleQueryContext::updatingClause() {
  return getRuleContexts<CypherParser::UpdatingClauseContext>();
}

CypherParser::UpdatingClauseContext* CypherParser::SingleQueryContext::updatingClause(size_t i) {
  return getRuleContext<CypherParser::UpdatingClauseContext>(i);
}


size_t CypherParser::SingleQueryContext::getRuleIndex() const {
  return CypherParser::RuleSingleQuery;
}


std::any CypherParser::SingleQueryContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSingleQuery(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SingleQueryContext* CypherParser::singleQuery() {
  SingleQueryContext *_localctx = _tracker.createInstance<SingleQueryContext>(_ctx, getState());
  enterRule(_localctx, 16, CypherParser::RuleSingleQuery);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(337);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 30, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(241); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(240);
        withClause();
        setState(243); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(248);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(245);
        readingClause();
        setState(250);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(251);
      returnStatement();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(254); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(253);
        withClause();
        setState(256); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(261);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(258);
        readingClause();
        setState(263);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(265); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(264);
        updatingClause();
        setState(267); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0));
      setState(270);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(269);
        returnStatement();
      }
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(275);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(272);
        readingClause();
        setState(277);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(279); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(278);
        withClause();
        setState(281); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(286);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(283);
        readingClause();
        setState(288);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(289);
      returnStatement();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(294);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(291);
        readingClause();
        setState(296);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(298); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(297);
        withClause();
        setState(300); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(305);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(302);
        readingClause();
        setState(307);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(309); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(308);
        updatingClause();
        setState(311); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0));
      setState(314);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(313);
        returnStatement();
      }
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(319);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(316);
        readingClause();
        setState(321);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(322);
      returnStatement();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(326);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(323);
        readingClause();
        setState(328);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(330); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(329);
        updatingClause();
        setState(332); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0));
      setState(335);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(334);
        returnStatement();
      }
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

//----------------- ReadingClauseContext ------------------------------------------------------------------

CypherParser::ReadingClauseContext::ReadingClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::MatchStatementContext* CypherParser::ReadingClauseContext::matchStatement() {
  return getRuleContext<CypherParser::MatchStatementContext>(0);
}

CypherParser::UnwindStatementContext* CypherParser::ReadingClauseContext::unwindStatement() {
  return getRuleContext<CypherParser::UnwindStatementContext>(0);
}

CypherParser::InQueryCallStatementContext* CypherParser::ReadingClauseContext::inQueryCallStatement() {
  return getRuleContext<CypherParser::InQueryCallStatementContext>(0);
}


size_t CypherParser::ReadingClauseContext::getRuleIndex() const {
  return CypherParser::RuleReadingClause;
}


std::any CypherParser::ReadingClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitReadingClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReadingClauseContext* CypherParser::readingClause() {
  ReadingClauseContext *_localctx = _tracker.createInstance<ReadingClauseContext>(_ctx, getState());
  enterRule(_localctx, 18, CypherParser::RuleReadingClause);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(342);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_MATCH:
      case CypherParser::K_OPTIONAL: {
        enterOuterAlt(_localctx, 1);
        setState(339);
        matchStatement();
        break;
      }

      case CypherParser::K_UNWIND: {
        enterOuterAlt(_localctx, 2);
        setState(340);
        unwindStatement();
        break;
      }

      case CypherParser::K_CALL: {
        enterOuterAlt(_localctx, 3);
        setState(341);
        inQueryCallStatement();
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

//----------------- UpdatingClauseContext ------------------------------------------------------------------

CypherParser::UpdatingClauseContext::UpdatingClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::CreateStatementContext* CypherParser::UpdatingClauseContext::createStatement() {
  return getRuleContext<CypherParser::CreateStatementContext>(0);
}

CypherParser::MergeStatementContext* CypherParser::UpdatingClauseContext::mergeStatement() {
  return getRuleContext<CypherParser::MergeStatementContext>(0);
}

CypherParser::DeleteStatementContext* CypherParser::UpdatingClauseContext::deleteStatement() {
  return getRuleContext<CypherParser::DeleteStatementContext>(0);
}

CypherParser::SetStatementContext* CypherParser::UpdatingClauseContext::setStatement() {
  return getRuleContext<CypherParser::SetStatementContext>(0);
}

CypherParser::RemoveStatementContext* CypherParser::UpdatingClauseContext::removeStatement() {
  return getRuleContext<CypherParser::RemoveStatementContext>(0);
}


size_t CypherParser::UpdatingClauseContext::getRuleIndex() const {
  return CypherParser::RuleUpdatingClause;
}


std::any CypherParser::UpdatingClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitUpdatingClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::UpdatingClauseContext* CypherParser::updatingClause() {
  UpdatingClauseContext *_localctx = _tracker.createInstance<UpdatingClauseContext>(_ctx, getState());
  enterRule(_localctx, 20, CypherParser::RuleUpdatingClause);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(349);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 1);
        setState(344);
        createStatement();
        break;
      }

      case CypherParser::K_MERGE: {
        enterOuterAlt(_localctx, 2);
        setState(345);
        mergeStatement();
        break;
      }

      case CypherParser::K_DELETE:
      case CypherParser::K_DETACH: {
        enterOuterAlt(_localctx, 3);
        setState(346);
        deleteStatement();
        break;
      }

      case CypherParser::K_SET: {
        enterOuterAlt(_localctx, 4);
        setState(347);
        setStatement();
        break;
      }

      case CypherParser::K_REMOVE: {
        enterOuterAlt(_localctx, 5);
        setState(348);
        removeStatement();
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

//----------------- WithClauseContext ------------------------------------------------------------------

CypherParser::WithClauseContext::WithClauseContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::WithClauseContext::K_WITH() {
  return getToken(CypherParser::K_WITH, 0);
}

CypherParser::ProjectionBodyContext* CypherParser::WithClauseContext::projectionBody() {
  return getRuleContext<CypherParser::ProjectionBodyContext>(0);
}

tree::TerminalNode* CypherParser::WithClauseContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::WhereContext* CypherParser::WithClauseContext::where() {
  return getRuleContext<CypherParser::WhereContext>(0);
}


size_t CypherParser::WithClauseContext::getRuleIndex() const {
  return CypherParser::RuleWithClause;
}


std::any CypherParser::WithClauseContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitWithClause(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::WithClauseContext* CypherParser::withClause() {
  WithClauseContext *_localctx = _tracker.createInstance<WithClauseContext>(_ctx, getState());
  enterRule(_localctx, 22, CypherParser::RuleWithClause);
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
    setState(351);
    match(CypherParser::K_WITH);
    setState(352);
    projectionBody();
    setState(355);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(353);
      match(CypherParser::K_WHERE);
      setState(354);
      where();
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

tree::TerminalNode* CypherParser::MatchStatementContext::K_OPTIONAL() {
  return getToken(CypherParser::K_OPTIONAL, 0);
}

tree::TerminalNode* CypherParser::MatchStatementContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::WhereContext* CypherParser::MatchStatementContext::where() {
  return getRuleContext<CypherParser::WhereContext>(0);
}


size_t CypherParser::MatchStatementContext::getRuleIndex() const {
  return CypherParser::RuleMatchStatement;
}


std::any CypherParser::MatchStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMatchStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MatchStatementContext* CypherParser::matchStatement() {
  MatchStatementContext *_localctx = _tracker.createInstance<MatchStatementContext>(_ctx, getState());
  enterRule(_localctx, 24, CypherParser::RuleMatchStatement);
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
    setState(358);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_OPTIONAL) {
      setState(357);
      match(CypherParser::K_OPTIONAL);
    }
    setState(360);
    match(CypherParser::K_MATCH);
    setState(361);
    pattern();
    setState(364);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(362);
      match(CypherParser::K_WHERE);
      setState(363);
      where();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- UnwindStatementContext ------------------------------------------------------------------

CypherParser::UnwindStatementContext::UnwindStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::UnwindStatementContext::K_UNWIND() {
  return getToken(CypherParser::K_UNWIND, 0);
}

CypherParser::ExpressionContext* CypherParser::UnwindStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::UnwindStatementContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

CypherParser::VariableContext* CypherParser::UnwindStatementContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}


size_t CypherParser::UnwindStatementContext::getRuleIndex() const {
  return CypherParser::RuleUnwindStatement;
}


std::any CypherParser::UnwindStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitUnwindStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::UnwindStatementContext* CypherParser::unwindStatement() {
  UnwindStatementContext *_localctx = _tracker.createInstance<UnwindStatementContext>(_ctx, getState());
  enterRule(_localctx, 26, CypherParser::RuleUnwindStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(366);
    match(CypherParser::K_UNWIND);
    setState(367);
    expression();
    setState(368);
    match(CypherParser::K_AS);
    setState(369);
    variable();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- InQueryCallStatementContext ------------------------------------------------------------------

CypherParser::InQueryCallStatementContext::InQueryCallStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::InQueryCallStatementContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

CypherParser::ExplicitProcedureInvocationContext* CypherParser::InQueryCallStatementContext::explicitProcedureInvocation() {
  return getRuleContext<CypherParser::ExplicitProcedureInvocationContext>(0);
}

tree::TerminalNode* CypherParser::InQueryCallStatementContext::K_YIELD() {
  return getToken(CypherParser::K_YIELD, 0);
}

CypherParser::YieldItemsContext* CypherParser::InQueryCallStatementContext::yieldItems() {
  return getRuleContext<CypherParser::YieldItemsContext>(0);
}


size_t CypherParser::InQueryCallStatementContext::getRuleIndex() const {
  return CypherParser::RuleInQueryCallStatement;
}


std::any CypherParser::InQueryCallStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitInQueryCallStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::InQueryCallStatementContext* CypherParser::inQueryCallStatement() {
  InQueryCallStatementContext *_localctx = _tracker.createInstance<InQueryCallStatementContext>(_ctx, getState());
  enterRule(_localctx, 28, CypherParser::RuleInQueryCallStatement);
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
    setState(371);
    match(CypherParser::K_CALL);
    setState(372);
    explicitProcedureInvocation();
    setState(375);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(373);
      match(CypherParser::K_YIELD);
      setState(374);
      yieldItems();
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

CypherParser::PatternContext* CypherParser::CreateStatementContext::pattern() {
  return getRuleContext<CypherParser::PatternContext>(0);
}


size_t CypherParser::CreateStatementContext::getRuleIndex() const {
  return CypherParser::RuleCreateStatement;
}


std::any CypherParser::CreateStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CreateStatementContext* CypherParser::createStatement() {
  CreateStatementContext *_localctx = _tracker.createInstance<CreateStatementContext>(_ctx, getState());
  enterRule(_localctx, 30, CypherParser::RuleCreateStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(377);
    match(CypherParser::K_CREATE);
    setState(378);
    pattern();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- MergeStatementContext ------------------------------------------------------------------

CypherParser::MergeStatementContext::MergeStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_MERGE() {
  return getToken(CypherParser::K_MERGE, 0);
}

CypherParser::PatternPartContext* CypherParser::MergeStatementContext::patternPart() {
  return getRuleContext<CypherParser::PatternPartContext>(0);
}

std::vector<tree::TerminalNode *> CypherParser::MergeStatementContext::K_ON() {
  return getTokens(CypherParser::K_ON);
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_ON(size_t i) {
  return getToken(CypherParser::K_ON, i);
}

std::vector<CypherParser::SetStatementContext *> CypherParser::MergeStatementContext::setStatement() {
  return getRuleContexts<CypherParser::SetStatementContext>();
}

CypherParser::SetStatementContext* CypherParser::MergeStatementContext::setStatement(size_t i) {
  return getRuleContext<CypherParser::SetStatementContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::MergeStatementContext::K_MATCH() {
  return getTokens(CypherParser::K_MATCH);
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_MATCH(size_t i) {
  return getToken(CypherParser::K_MATCH, i);
}

std::vector<tree::TerminalNode *> CypherParser::MergeStatementContext::K_CREATE() {
  return getTokens(CypherParser::K_CREATE);
}

tree::TerminalNode* CypherParser::MergeStatementContext::K_CREATE(size_t i) {
  return getToken(CypherParser::K_CREATE, i);
}


size_t CypherParser::MergeStatementContext::getRuleIndex() const {
  return CypherParser::RuleMergeStatement;
}


std::any CypherParser::MergeStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMergeStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MergeStatementContext* CypherParser::mergeStatement() {
  MergeStatementContext *_localctx = _tracker.createInstance<MergeStatementContext>(_ctx, getState());
  enterRule(_localctx, 32, CypherParser::RuleMergeStatement);
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
    setState(380);
    match(CypherParser::K_MERGE);
    setState(381);
    patternPart();
    setState(387);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_ON) {
      setState(382);
      match(CypherParser::K_ON);
      setState(383);
      _la = _input->LA(1);
      if (!(_la == CypherParser::K_CREATE

      || _la == CypherParser::K_MATCH)) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(384);
      setStatement();
      setState(389);
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

//----------------- SetStatementContext ------------------------------------------------------------------

CypherParser::SetStatementContext::SetStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SetStatementContext::K_SET() {
  return getToken(CypherParser::K_SET, 0);
}

std::vector<CypherParser::SetItemContext *> CypherParser::SetStatementContext::setItem() {
  return getRuleContexts<CypherParser::SetItemContext>();
}

CypherParser::SetItemContext* CypherParser::SetStatementContext::setItem(size_t i) {
  return getRuleContext<CypherParser::SetItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::SetStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::SetStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::SetStatementContext::getRuleIndex() const {
  return CypherParser::RuleSetStatement;
}


std::any CypherParser::SetStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSetStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SetStatementContext* CypherParser::setStatement() {
  SetStatementContext *_localctx = _tracker.createInstance<SetStatementContext>(_ctx, getState());
  enterRule(_localctx, 34, CypherParser::RuleSetStatement);
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
    setState(390);
    match(CypherParser::K_SET);
    setState(391);
    setItem();
    setState(396);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(392);
      match(CypherParser::COMMA);
      setState(393);
      setItem();
      setState(398);
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

//----------------- SetItemContext ------------------------------------------------------------------

CypherParser::SetItemContext::SetItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::PropertyExpressionContext* CypherParser::SetItemContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}

tree::TerminalNode* CypherParser::SetItemContext::EQ() {
  return getToken(CypherParser::EQ, 0);
}

CypherParser::ExpressionContext* CypherParser::SetItemContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

CypherParser::VariableContext* CypherParser::SetItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::SetItemContext::PLUS() {
  return getToken(CypherParser::PLUS, 0);
}

CypherParser::NodeLabelsContext* CypherParser::SetItemContext::nodeLabels() {
  return getRuleContext<CypherParser::NodeLabelsContext>(0);
}


size_t CypherParser::SetItemContext::getRuleIndex() const {
  return CypherParser::RuleSetItem;
}


std::any CypherParser::SetItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSetItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SetItemContext* CypherParser::setItem() {
  SetItemContext *_localctx = _tracker.createInstance<SetItemContext>(_ctx, getState());
  enterRule(_localctx, 36, CypherParser::RuleSetItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(415);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 39, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(399);
      propertyExpression();
      setState(400);
      match(CypherParser::EQ);
      setState(401);
      expression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(403);
      variable();
      setState(404);
      match(CypherParser::EQ);
      setState(405);
      expression();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(407);
      variable();
      setState(408);
      match(CypherParser::PLUS);
      setState(409);
      match(CypherParser::EQ);
      setState(410);
      expression();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(412);
      variable();
      setState(413);
      nodeLabels();
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

//----------------- DeleteStatementContext ------------------------------------------------------------------

CypherParser::DeleteStatementContext::DeleteStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::DeleteStatementContext::K_DELETE() {
  return getToken(CypherParser::K_DELETE, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::DeleteStatementContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::DeleteStatementContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::DeleteStatementContext::K_DETACH() {
  return getToken(CypherParser::K_DETACH, 0);
}

std::vector<tree::TerminalNode *> CypherParser::DeleteStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::DeleteStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::DeleteStatementContext::getRuleIndex() const {
  return CypherParser::RuleDeleteStatement;
}


std::any CypherParser::DeleteStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitDeleteStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::DeleteStatementContext* CypherParser::deleteStatement() {
  DeleteStatementContext *_localctx = _tracker.createInstance<DeleteStatementContext>(_ctx, getState());
  enterRule(_localctx, 38, CypherParser::RuleDeleteStatement);
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
    setState(418);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_DETACH) {
      setState(417);
      match(CypherParser::K_DETACH);
    }
    setState(420);
    match(CypherParser::K_DELETE);
    setState(421);
    expression();
    setState(426);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(422);
      match(CypherParser::COMMA);
      setState(423);
      expression();
      setState(428);
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

//----------------- RemoveStatementContext ------------------------------------------------------------------

CypherParser::RemoveStatementContext::RemoveStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::RemoveStatementContext::K_REMOVE() {
  return getToken(CypherParser::K_REMOVE, 0);
}

std::vector<CypherParser::RemoveItemContext *> CypherParser::RemoveStatementContext::removeItem() {
  return getRuleContexts<CypherParser::RemoveItemContext>();
}

CypherParser::RemoveItemContext* CypherParser::RemoveStatementContext::removeItem(size_t i) {
  return getRuleContext<CypherParser::RemoveItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::RemoveStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::RemoveStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::RemoveStatementContext::getRuleIndex() const {
  return CypherParser::RuleRemoveStatement;
}


std::any CypherParser::RemoveStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRemoveStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RemoveStatementContext* CypherParser::removeStatement() {
  RemoveStatementContext *_localctx = _tracker.createInstance<RemoveStatementContext>(_ctx, getState());
  enterRule(_localctx, 40, CypherParser::RuleRemoveStatement);
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
    setState(429);
    match(CypherParser::K_REMOVE);
    setState(430);
    removeItem();
    setState(435);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(431);
      match(CypherParser::COMMA);
      setState(432);
      removeItem();
      setState(437);
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

//----------------- RemoveItemContext ------------------------------------------------------------------

CypherParser::RemoveItemContext::RemoveItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::VariableContext* CypherParser::RemoveItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::NodeLabelsContext* CypherParser::RemoveItemContext::nodeLabels() {
  return getRuleContext<CypherParser::NodeLabelsContext>(0);
}

CypherParser::PropertyExpressionContext* CypherParser::RemoveItemContext::propertyExpression() {
  return getRuleContext<CypherParser::PropertyExpressionContext>(0);
}


size_t CypherParser::RemoveItemContext::getRuleIndex() const {
  return CypherParser::RuleRemoveItem;
}


std::any CypherParser::RemoveItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRemoveItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RemoveItemContext* CypherParser::removeItem() {
  RemoveItemContext *_localctx = _tracker.createInstance<RemoveItemContext>(_ctx, getState());
  enterRule(_localctx, 42, CypherParser::RuleRemoveItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(442);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 43, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(438);
      variable();
      setState(439);
      nodeLabels();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(441);
      propertyExpression();
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

//----------------- ReturnStatementContext ------------------------------------------------------------------

CypherParser::ReturnStatementContext::ReturnStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ReturnStatementContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}

CypherParser::ProjectionBodyContext* CypherParser::ReturnStatementContext::projectionBody() {
  return getRuleContext<CypherParser::ProjectionBodyContext>(0);
}


size_t CypherParser::ReturnStatementContext::getRuleIndex() const {
  return CypherParser::RuleReturnStatement;
}


std::any CypherParser::ReturnStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitReturnStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ReturnStatementContext* CypherParser::returnStatement() {
  ReturnStatementContext *_localctx = _tracker.createInstance<ReturnStatementContext>(_ctx, getState());
  enterRule(_localctx, 44, CypherParser::RuleReturnStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(444);
    match(CypherParser::K_RETURN);
    setState(445);
    projectionBody();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- StandaloneCallStatementContext ------------------------------------------------------------------

CypherParser::StandaloneCallStatementContext::StandaloneCallStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::StandaloneCallStatementContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

CypherParser::ExplicitProcedureInvocationContext* CypherParser::StandaloneCallStatementContext::explicitProcedureInvocation() {
  return getRuleContext<CypherParser::ExplicitProcedureInvocationContext>(0);
}

CypherParser::ImplicitProcedureInvocationContext* CypherParser::StandaloneCallStatementContext::implicitProcedureInvocation() {
  return getRuleContext<CypherParser::ImplicitProcedureInvocationContext>(0);
}

tree::TerminalNode* CypherParser::StandaloneCallStatementContext::K_YIELD() {
  return getToken(CypherParser::K_YIELD, 0);
}

tree::TerminalNode* CypherParser::StandaloneCallStatementContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

CypherParser::YieldItemsContext* CypherParser::StandaloneCallStatementContext::yieldItems() {
  return getRuleContext<CypherParser::YieldItemsContext>(0);
}


size_t CypherParser::StandaloneCallStatementContext::getRuleIndex() const {
  return CypherParser::RuleStandaloneCallStatement;
}


std::any CypherParser::StandaloneCallStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitStandaloneCallStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::StandaloneCallStatementContext* CypherParser::standaloneCallStatement() {
  StandaloneCallStatementContext *_localctx = _tracker.createInstance<StandaloneCallStatementContext>(_ctx, getState());
  enterRule(_localctx, 46, CypherParser::RuleStandaloneCallStatement);
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
    setState(447);
    match(CypherParser::K_CALL);
    setState(450);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 44, _ctx)) {
    case 1: {
      setState(448);
      explicitProcedureInvocation();
      break;
    }

    case 2: {
      setState(449);
      implicitProcedureInvocation();
      break;
    }

    default:
      break;
    }
    setState(457);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(452);
      match(CypherParser::K_YIELD);
      setState(455);
      _errHandler->sync(this);
      switch (_input->LA(1)) {
        case CypherParser::MULTIPLY: {
          setState(453);
          match(CypherParser::MULTIPLY);
          break;
        }

        case CypherParser::K_CALL:
        case CypherParser::K_YIELD:
        case CypherParser::K_CREATE:
        case CypherParser::K_DELETE:
        case CypherParser::K_DETACH:
        case CypherParser::K_EXISTS:
        case CypherParser::K_LIMIT:
        case CypherParser::K_MATCH:
        case CypherParser::K_MERGE:
        case CypherParser::K_OPTIONAL:
        case CypherParser::K_ORDER:
        case CypherParser::K_BY:
        case CypherParser::K_SKIP:
        case CypherParser::K_ASCENDING:
        case CypherParser::K_ASC:
        case CypherParser::K_DESCENDING:
        case CypherParser::K_DESC:
        case CypherParser::K_REMOVE:
        case CypherParser::K_RETURN:
        case CypherParser::K_SET:
        case CypherParser::K_WHERE:
        case CypherParser::K_WITH:
        case CypherParser::K_UNION:
        case CypherParser::K_UNWIND:
        case CypherParser::K_AND:
        case CypherParser::K_AS:
        case CypherParser::K_CONTAINS:
        case CypherParser::K_DISTINCT:
        case CypherParser::K_ENDS:
        case CypherParser::K_IN:
        case CypherParser::K_IS:
        case CypherParser::K_NOT:
        case CypherParser::K_OR:
        case CypherParser::K_STARTS:
        case CypherParser::K_XOR:
        case CypherParser::K_FALSE:
        case CypherParser::K_TRUE:
        case CypherParser::K_NULL:
        case CypherParser::K_CASE:
        case CypherParser::K_WHEN:
        case CypherParser::K_THEN:
        case CypherParser::K_ELSE:
        case CypherParser::K_END:
        case CypherParser::K_COUNT:
        case CypherParser::K_FILTER:
        case CypherParser::K_EXTRACT:
        case CypherParser::K_ANY:
        case CypherParser::K_NONE:
        case CypherParser::K_SINGLE:
        case CypherParser::K_ALL:
        case CypherParser::K_INDEX:
        case CypherParser::K_ON:
        case CypherParser::K_SHOW:
        case CypherParser::K_DROP:
        case CypherParser::K_FOR:
        case CypherParser::K_CONSTRAINT:
        case CypherParser::K_DO:
        case CypherParser::K_REQUIRE:
        case CypherParser::K_UNIQUE:
        case CypherParser::K_MANDATORY:
        case CypherParser::K_SCALAR:
        case CypherParser::K_OF:
        case CypherParser::K_ADD:
        case CypherParser::K_VECTOR:
        case CypherParser::K_OPTIONS:
        case CypherParser::ID: {
          setState(454);
          yieldItems();
          break;
        }

      default:
        throw NoViableAltException(this);
      }
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- YieldItemsContext ------------------------------------------------------------------

CypherParser::YieldItemsContext::YieldItemsContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::YieldItemContext *> CypherParser::YieldItemsContext::yieldItem() {
  return getRuleContexts<CypherParser::YieldItemContext>();
}

CypherParser::YieldItemContext* CypherParser::YieldItemsContext::yieldItem(size_t i) {
  return getRuleContext<CypherParser::YieldItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::YieldItemsContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::YieldItemsContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}

tree::TerminalNode* CypherParser::YieldItemsContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

CypherParser::WhereContext* CypherParser::YieldItemsContext::where() {
  return getRuleContext<CypherParser::WhereContext>(0);
}


size_t CypherParser::YieldItemsContext::getRuleIndex() const {
  return CypherParser::RuleYieldItems;
}


std::any CypherParser::YieldItemsContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitYieldItems(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::YieldItemsContext* CypherParser::yieldItems() {
  YieldItemsContext *_localctx = _tracker.createInstance<YieldItemsContext>(_ctx, getState());
  enterRule(_localctx, 48, CypherParser::RuleYieldItems);
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
    setState(459);
    yieldItem();
    setState(464);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(460);
      match(CypherParser::COMMA);
      setState(461);
      yieldItem();
      setState(466);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
    setState(469);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(467);
      match(CypherParser::K_WHERE);
      setState(468);
      where();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- YieldItemContext ------------------------------------------------------------------

CypherParser::YieldItemContext::YieldItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::VariableContext* CypherParser::YieldItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::ProcedureResultFieldContext* CypherParser::YieldItemContext::procedureResultField() {
  return getRuleContext<CypherParser::ProcedureResultFieldContext>(0);
}

tree::TerminalNode* CypherParser::YieldItemContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}


size_t CypherParser::YieldItemContext::getRuleIndex() const {
  return CypherParser::RuleYieldItem;
}


std::any CypherParser::YieldItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitYieldItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::YieldItemContext* CypherParser::yieldItem() {
  YieldItemContext *_localctx = _tracker.createInstance<YieldItemContext>(_ctx, getState());
  enterRule(_localctx, 50, CypherParser::RuleYieldItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(474);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 49, _ctx)) {
    case 1: {
      setState(471);
      procedureResultField();
      setState(472);
      match(CypherParser::K_AS);
      break;
    }

    default:
      break;
    }
    setState(476);
    variable();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ProjectionBodyContext ------------------------------------------------------------------

CypherParser::ProjectionBodyContext::ProjectionBodyContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ProjectionItemsContext* CypherParser::ProjectionBodyContext::projectionItems() {
  return getRuleContext<CypherParser::ProjectionItemsContext>(0);
}

tree::TerminalNode* CypherParser::ProjectionBodyContext::K_DISTINCT() {
  return getToken(CypherParser::K_DISTINCT, 0);
}

CypherParser::OrderStatementContext* CypherParser::ProjectionBodyContext::orderStatement() {
  return getRuleContext<CypherParser::OrderStatementContext>(0);
}

CypherParser::SkipStatementContext* CypherParser::ProjectionBodyContext::skipStatement() {
  return getRuleContext<CypherParser::SkipStatementContext>(0);
}

CypherParser::LimitStatementContext* CypherParser::ProjectionBodyContext::limitStatement() {
  return getRuleContext<CypherParser::LimitStatementContext>(0);
}


size_t CypherParser::ProjectionBodyContext::getRuleIndex() const {
  return CypherParser::RuleProjectionBody;
}


std::any CypherParser::ProjectionBodyContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProjectionBody(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProjectionBodyContext* CypherParser::projectionBody() {
  ProjectionBodyContext *_localctx = _tracker.createInstance<ProjectionBodyContext>(_ctx, getState());
  enterRule(_localctx, 52, CypherParser::RuleProjectionBody);
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
    setState(479);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 50, _ctx)) {
    case 1: {
      setState(478);
      match(CypherParser::K_DISTINCT);
      break;
    }

    default:
      break;
    }
    setState(481);
    projectionItems();
    setState(483);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_ORDER) {
      setState(482);
      orderStatement();
    }
    setState(486);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_SKIP) {
      setState(485);
      skipStatement();
    }
    setState(489);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_LIMIT) {
      setState(488);
      limitStatement();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ProjectionItemsContext ------------------------------------------------------------------

CypherParser::ProjectionItemsContext::ProjectionItemsContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ProjectionItemsContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

std::vector<CypherParser::ProjectionItemContext *> CypherParser::ProjectionItemsContext::projectionItem() {
  return getRuleContexts<CypherParser::ProjectionItemContext>();
}

CypherParser::ProjectionItemContext* CypherParser::ProjectionItemsContext::projectionItem(size_t i) {
  return getRuleContext<CypherParser::ProjectionItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ProjectionItemsContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ProjectionItemsContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ProjectionItemsContext::getRuleIndex() const {
  return CypherParser::RuleProjectionItems;
}


std::any CypherParser::ProjectionItemsContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProjectionItems(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProjectionItemsContext* CypherParser::projectionItems() {
  ProjectionItemsContext *_localctx = _tracker.createInstance<ProjectionItemsContext>(_ctx, getState());
  enterRule(_localctx, 54, CypherParser::RuleProjectionItems);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(500);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::MULTIPLY: {
        enterOuterAlt(_localctx, 1);
        setState(491);
        match(CypherParser::MULTIPLY);
        break;
      }

      case CypherParser::K_CALL:
      case CypherParser::K_YIELD:
      case CypherParser::K_CREATE:
      case CypherParser::K_DELETE:
      case CypherParser::K_DETACH:
      case CypherParser::K_EXISTS:
      case CypherParser::K_LIMIT:
      case CypherParser::K_MATCH:
      case CypherParser::K_MERGE:
      case CypherParser::K_OPTIONAL:
      case CypherParser::K_ORDER:
      case CypherParser::K_BY:
      case CypherParser::K_SKIP:
      case CypherParser::K_ASCENDING:
      case CypherParser::K_ASC:
      case CypherParser::K_DESCENDING:
      case CypherParser::K_DESC:
      case CypherParser::K_REMOVE:
      case CypherParser::K_RETURN:
      case CypherParser::K_SET:
      case CypherParser::K_WHERE:
      case CypherParser::K_WITH:
      case CypherParser::K_UNION:
      case CypherParser::K_UNWIND:
      case CypherParser::K_AND:
      case CypherParser::K_AS:
      case CypherParser::K_CONTAINS:
      case CypherParser::K_DISTINCT:
      case CypherParser::K_ENDS:
      case CypherParser::K_IN:
      case CypherParser::K_IS:
      case CypherParser::K_NOT:
      case CypherParser::K_OR:
      case CypherParser::K_STARTS:
      case CypherParser::K_XOR:
      case CypherParser::K_FALSE:
      case CypherParser::K_TRUE:
      case CypherParser::K_NULL:
      case CypherParser::K_CASE:
      case CypherParser::K_WHEN:
      case CypherParser::K_THEN:
      case CypherParser::K_ELSE:
      case CypherParser::K_END:
      case CypherParser::K_COUNT:
      case CypherParser::K_FILTER:
      case CypherParser::K_EXTRACT:
      case CypherParser::K_ANY:
      case CypherParser::K_NONE:
      case CypherParser::K_SINGLE:
      case CypherParser::K_ALL:
      case CypherParser::K_INDEX:
      case CypherParser::K_ON:
      case CypherParser::K_SHOW:
      case CypherParser::K_DROP:
      case CypherParser::K_FOR:
      case CypherParser::K_CONSTRAINT:
      case CypherParser::K_DO:
      case CypherParser::K_REQUIRE:
      case CypherParser::K_UNIQUE:
      case CypherParser::K_MANDATORY:
      case CypherParser::K_SCALAR:
      case CypherParser::K_OF:
      case CypherParser::K_ADD:
      case CypherParser::K_VECTOR:
      case CypherParser::K_OPTIONS:
      case CypherParser::PLUS:
      case CypherParser::MINUS:
      case CypherParser::LPAREN:
      case CypherParser::LBRACE:
      case CypherParser::LBRACK:
      case CypherParser::DOLLAR:
      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger:
      case CypherParser::DoubleLiteral:
      case CypherParser::ID:
      case CypherParser::StringLiteral: {
        enterOuterAlt(_localctx, 2);
        setState(492);
        projectionItem();
        setState(497);
        _errHandler->sync(this);
        _la = _input->LA(1);
        while (_la == CypherParser::COMMA) {
          setState(493);
          match(CypherParser::COMMA);
          setState(494);
          projectionItem();
          setState(499);
          _errHandler->sync(this);
          _la = _input->LA(1);
        }
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

//----------------- ProjectionItemContext ------------------------------------------------------------------

CypherParser::ProjectionItemContext::ProjectionItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ExpressionContext* CypherParser::ProjectionItemContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::ProjectionItemContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

CypherParser::VariableContext* CypherParser::ProjectionItemContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}


size_t CypherParser::ProjectionItemContext::getRuleIndex() const {
  return CypherParser::RuleProjectionItem;
}


std::any CypherParser::ProjectionItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProjectionItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProjectionItemContext* CypherParser::projectionItem() {
  ProjectionItemContext *_localctx = _tracker.createInstance<ProjectionItemContext>(_ctx, getState());
  enterRule(_localctx, 56, CypherParser::RuleProjectionItem);
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
    setState(502);
    expression();
    setState(505);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_AS) {
      setState(503);
      match(CypherParser::K_AS);
      setState(504);
      variable();
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- OrderStatementContext ------------------------------------------------------------------

CypherParser::OrderStatementContext::OrderStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::OrderStatementContext::K_ORDER() {
  return getToken(CypherParser::K_ORDER, 0);
}

tree::TerminalNode* CypherParser::OrderStatementContext::K_BY() {
  return getToken(CypherParser::K_BY, 0);
}

std::vector<CypherParser::SortItemContext *> CypherParser::OrderStatementContext::sortItem() {
  return getRuleContexts<CypherParser::SortItemContext>();
}

CypherParser::SortItemContext* CypherParser::OrderStatementContext::sortItem(size_t i) {
  return getRuleContext<CypherParser::SortItemContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::OrderStatementContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::OrderStatementContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::OrderStatementContext::getRuleIndex() const {
  return CypherParser::RuleOrderStatement;
}


std::any CypherParser::OrderStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitOrderStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::OrderStatementContext* CypherParser::orderStatement() {
  OrderStatementContext *_localctx = _tracker.createInstance<OrderStatementContext>(_ctx, getState());
  enterRule(_localctx, 58, CypherParser::RuleOrderStatement);
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
    setState(507);
    match(CypherParser::K_ORDER);
    setState(508);
    match(CypherParser::K_BY);
    setState(509);
    sortItem();
    setState(514);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(510);
      match(CypherParser::COMMA);
      setState(511);
      sortItem();
      setState(516);
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

//----------------- SkipStatementContext ------------------------------------------------------------------

CypherParser::SkipStatementContext::SkipStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SkipStatementContext::K_SKIP() {
  return getToken(CypherParser::K_SKIP, 0);
}

CypherParser::ExpressionContext* CypherParser::SkipStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::SkipStatementContext::getRuleIndex() const {
  return CypherParser::RuleSkipStatement;
}


std::any CypherParser::SkipStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSkipStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SkipStatementContext* CypherParser::skipStatement() {
  SkipStatementContext *_localctx = _tracker.createInstance<SkipStatementContext>(_ctx, getState());
  enterRule(_localctx, 60, CypherParser::RuleSkipStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(517);
    match(CypherParser::K_SKIP);
    setState(518);
    expression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- LimitStatementContext ------------------------------------------------------------------

CypherParser::LimitStatementContext::LimitStatementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::LimitStatementContext::K_LIMIT() {
  return getToken(CypherParser::K_LIMIT, 0);
}

CypherParser::ExpressionContext* CypherParser::LimitStatementContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::LimitStatementContext::getRuleIndex() const {
  return CypherParser::RuleLimitStatement;
}


std::any CypherParser::LimitStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitLimitStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LimitStatementContext* CypherParser::limitStatement() {
  LimitStatementContext *_localctx = _tracker.createInstance<LimitStatementContext>(_ctx, getState());
  enterRule(_localctx, 62, CypherParser::RuleLimitStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(520);
    match(CypherParser::K_LIMIT);
    setState(521);
    expression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- SortItemContext ------------------------------------------------------------------

CypherParser::SortItemContext::SortItemContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ExpressionContext* CypherParser::SortItemContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_ASCENDING() {
  return getToken(CypherParser::K_ASCENDING, 0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_ASC() {
  return getToken(CypherParser::K_ASC, 0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_DESCENDING() {
  return getToken(CypherParser::K_DESCENDING, 0);
}

tree::TerminalNode* CypherParser::SortItemContext::K_DESC() {
  return getToken(CypherParser::K_DESC, 0);
}


size_t CypherParser::SortItemContext::getRuleIndex() const {
  return CypherParser::RuleSortItem;
}


std::any CypherParser::SortItemContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSortItem(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SortItemContext* CypherParser::sortItem() {
  SortItemContext *_localctx = _tracker.createInstance<SortItemContext>(_ctx, getState());
  enterRule(_localctx, 64, CypherParser::RuleSortItem);
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
    setState(523);
    expression();
    setState(525);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 245760) != 0)) {
      setState(524);
      _la = _input->LA(1);
      if (!((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 245760) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- WhereContext ------------------------------------------------------------------

CypherParser::WhereContext::WhereContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ExpressionContext* CypherParser::WhereContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}


size_t CypherParser::WhereContext::getRuleIndex() const {
  return CypherParser::RuleWhere;
}


std::any CypherParser::WhereContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitWhere(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::WhereContext* CypherParser::where() {
  WhereContext *_localctx = _tracker.createInstance<WhereContext>(_ctx, getState());
  enterRule(_localctx, 66, CypherParser::RuleWhere);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(527);
    expression();
   
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
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternContext* CypherParser::pattern() {
  PatternContext *_localctx = _tracker.createInstance<PatternContext>(_ctx, getState());
  enterRule(_localctx, 68, CypherParser::RulePattern);
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
    setState(529);
    patternPart();
    setState(534);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(530);
      match(CypherParser::COMMA);
      setState(531);
      patternPart();
      setState(536);
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

CypherParser::PatternElementContext* CypherParser::PatternPartContext::patternElement() {
  return getRuleContext<CypherParser::PatternElementContext>(0);
}

CypherParser::VariableContext* CypherParser::PatternPartContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::PatternPartContext::EQ() {
  return getToken(CypherParser::EQ, 0);
}


size_t CypherParser::PatternPartContext::getRuleIndex() const {
  return CypherParser::RulePatternPart;
}


std::any CypherParser::PatternPartContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPatternPart(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternPartContext* CypherParser::patternPart() {
  PatternPartContext *_localctx = _tracker.createInstance<PatternPartContext>(_ctx, getState());
  enterRule(_localctx, 70, CypherParser::RulePatternPart);
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
    setState(540);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(537);
      variable();
      setState(538);
      match(CypherParser::EQ);
    }
    setState(542);
    patternElement();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PatternElementContext ------------------------------------------------------------------

CypherParser::PatternElementContext::PatternElementContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::NodePatternContext* CypherParser::PatternElementContext::nodePattern() {
  return getRuleContext<CypherParser::NodePatternContext>(0);
}

std::vector<CypherParser::PatternElementChainContext *> CypherParser::PatternElementContext::patternElementChain() {
  return getRuleContexts<CypherParser::PatternElementChainContext>();
}

CypherParser::PatternElementChainContext* CypherParser::PatternElementContext::patternElementChain(size_t i) {
  return getRuleContext<CypherParser::PatternElementChainContext>(i);
}

tree::TerminalNode* CypherParser::PatternElementContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PatternElementContext* CypherParser::PatternElementContext::patternElement() {
  return getRuleContext<CypherParser::PatternElementContext>(0);
}

tree::TerminalNode* CypherParser::PatternElementContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}


size_t CypherParser::PatternElementContext::getRuleIndex() const {
  return CypherParser::RulePatternElement;
}


std::any CypherParser::PatternElementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPatternElement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternElementContext* CypherParser::patternElement() {
  PatternElementContext *_localctx = _tracker.createInstance<PatternElementContext>(_ctx, getState());
  enterRule(_localctx, 72, CypherParser::RulePatternElement);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(555);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 62, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(544);
      nodePattern();
      setState(548);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::LT

      || _la == CypherParser::MINUS) {
        setState(545);
        patternElementChain();
        setState(550);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(551);
      match(CypherParser::LPAREN);
      setState(552);
      patternElement();
      setState(553);
      match(CypherParser::RPAREN);
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
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPatternElementChain(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PatternElementChainContext* CypherParser::patternElementChain() {
  PatternElementChainContext *_localctx = _tracker.createInstance<PatternElementChainContext>(_ctx, getState());
  enterRule(_localctx, 74, CypherParser::RulePatternElementChain);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(557);
    relationshipPattern();
    setState(558);
    nodePattern();
   
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

CypherParser::VariableContext* CypherParser::NodePatternContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::NodeLabelsContext* CypherParser::NodePatternContext::nodeLabels() {
  return getRuleContext<CypherParser::NodeLabelsContext>(0);
}

CypherParser::PropertiesContext* CypherParser::NodePatternContext::properties() {
  return getRuleContext<CypherParser::PropertiesContext>(0);
}


size_t CypherParser::NodePatternContext::getRuleIndex() const {
  return CypherParser::RuleNodePattern;
}


std::any CypherParser::NodePatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNodePattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NodePatternContext* CypherParser::nodePattern() {
  NodePatternContext *_localctx = _tracker.createInstance<NodePatternContext>(_ctx, getState());
  enterRule(_localctx, 76, CypherParser::RuleNodePattern);
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
    setState(560);
    match(CypherParser::LPAREN);
    setState(562);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(561);
      variable();
    }
    setState(565);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(564);
      nodeLabels();
    }
    setState(568);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(567);
      properties();
    }
    setState(570);
    match(CypherParser::RPAREN);
   
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

std::vector<tree::TerminalNode *> CypherParser::RelationshipPatternContext::MINUS() {
  return getTokens(CypherParser::MINUS);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::MINUS(size_t i) {
  return getToken(CypherParser::MINUS, i);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::LT() {
  return getToken(CypherParser::LT, 0);
}

CypherParser::RelationshipDetailContext* CypherParser::RelationshipPatternContext::relationshipDetail() {
  return getRuleContext<CypherParser::RelationshipDetailContext>(0);
}

tree::TerminalNode* CypherParser::RelationshipPatternContext::GT() {
  return getToken(CypherParser::GT, 0);
}


size_t CypherParser::RelationshipPatternContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipPattern;
}


std::any CypherParser::RelationshipPatternContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelationshipPattern(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipPatternContext* CypherParser::relationshipPattern() {
  RelationshipPatternContext *_localctx = _tracker.createInstance<RelationshipPatternContext>(_ctx, getState());
  enterRule(_localctx, 78, CypherParser::RuleRelationshipPattern);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(588);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 70, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(573);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LT) {
        setState(572);
        match(CypherParser::LT);
      }
      setState(575);
      match(CypherParser::MINUS);
      setState(577);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(576);
        relationshipDetail();
      }
      setState(579);
      match(CypherParser::MINUS);
      setState(581);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::GT) {
        setState(580);
        match(CypherParser::GT);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(583);
      match(CypherParser::MINUS);
      setState(585);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(584);
        relationshipDetail();
      }
      setState(587);
      match(CypherParser::MINUS);
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

//----------------- RelationshipDetailContext ------------------------------------------------------------------

CypherParser::RelationshipDetailContext::RelationshipDetailContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::RelationshipDetailContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

tree::TerminalNode* CypherParser::RelationshipDetailContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

CypherParser::VariableContext* CypherParser::RelationshipDetailContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::RelationshipTypesContext* CypherParser::RelationshipDetailContext::relationshipTypes() {
  return getRuleContext<CypherParser::RelationshipTypesContext>(0);
}

CypherParser::RangeLiteralContext* CypherParser::RelationshipDetailContext::rangeLiteral() {
  return getRuleContext<CypherParser::RangeLiteralContext>(0);
}

CypherParser::PropertiesContext* CypherParser::RelationshipDetailContext::properties() {
  return getRuleContext<CypherParser::PropertiesContext>(0);
}


size_t CypherParser::RelationshipDetailContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipDetail;
}


std::any CypherParser::RelationshipDetailContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelationshipDetail(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipDetailContext* CypherParser::relationshipDetail() {
  RelationshipDetailContext *_localctx = _tracker.createInstance<RelationshipDetailContext>(_ctx, getState());
  enterRule(_localctx, 80, CypherParser::RuleRelationshipDetail);
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
    setState(590);
    match(CypherParser::LBRACK);
    setState(592);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(591);
      variable();
    }
    setState(595);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(594);
      relationshipTypes();
    }
    setState(598);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::MULTIPLY) {
      setState(597);
      rangeLiteral();
    }
    setState(601);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(600);
      properties();
    }
    setState(603);
    match(CypherParser::RBRACK);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PropertiesContext ------------------------------------------------------------------

CypherParser::PropertiesContext::PropertiesContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::MapLiteralContext* CypherParser::PropertiesContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}

CypherParser::ParameterContext* CypherParser::PropertiesContext::parameter() {
  return getRuleContext<CypherParser::ParameterContext>(0);
}


size_t CypherParser::PropertiesContext::getRuleIndex() const {
  return CypherParser::RuleProperties;
}


std::any CypherParser::PropertiesContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProperties(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PropertiesContext* CypherParser::properties() {
  PropertiesContext *_localctx = _tracker.createInstance<PropertiesContext>(_ctx, getState());
  enterRule(_localctx, 82, CypherParser::RuleProperties);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(607);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 1);
        setState(605);
        mapLiteral();
        break;
      }

      case CypherParser::DOLLAR: {
        enterOuterAlt(_localctx, 2);
        setState(606);
        parameter();
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

//----------------- NodeLabelsContext ------------------------------------------------------------------

CypherParser::NodeLabelsContext::NodeLabelsContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::NodeLabelContext *> CypherParser::NodeLabelsContext::nodeLabel() {
  return getRuleContexts<CypherParser::NodeLabelContext>();
}

CypherParser::NodeLabelContext* CypherParser::NodeLabelsContext::nodeLabel(size_t i) {
  return getRuleContext<CypherParser::NodeLabelContext>(i);
}


size_t CypherParser::NodeLabelsContext::getRuleIndex() const {
  return CypherParser::RuleNodeLabels;
}


std::any CypherParser::NodeLabelsContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNodeLabels(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NodeLabelsContext* CypherParser::nodeLabels() {
  NodeLabelsContext *_localctx = _tracker.createInstance<NodeLabelsContext>(_ctx, getState());
  enterRule(_localctx, 84, CypherParser::RuleNodeLabels);
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
    setState(610); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(609);
      nodeLabel();
      setState(612); 
      _errHandler->sync(this);
      _la = _input->LA(1);
    } while (_la == CypherParser::COLON);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- NodeLabelContext ------------------------------------------------------------------

CypherParser::NodeLabelContext::NodeLabelContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::NodeLabelContext::COLON() {
  return getToken(CypherParser::COLON, 0);
}

CypherParser::LabelNameContext* CypherParser::NodeLabelContext::labelName() {
  return getRuleContext<CypherParser::LabelNameContext>(0);
}


size_t CypherParser::NodeLabelContext::getRuleIndex() const {
  return CypherParser::RuleNodeLabel;
}


std::any CypherParser::NodeLabelContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNodeLabel(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NodeLabelContext* CypherParser::nodeLabel() {
  NodeLabelContext *_localctx = _tracker.createInstance<NodeLabelContext>(_ctx, getState());
  enterRule(_localctx, 86, CypherParser::RuleNodeLabel);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(614);
    match(CypherParser::COLON);
    setState(615);
    labelName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RelationshipTypesContext ------------------------------------------------------------------

CypherParser::RelationshipTypesContext::RelationshipTypesContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<tree::TerminalNode *> CypherParser::RelationshipTypesContext::COLON() {
  return getTokens(CypherParser::COLON);
}

tree::TerminalNode* CypherParser::RelationshipTypesContext::COLON(size_t i) {
  return getToken(CypherParser::COLON, i);
}

std::vector<CypherParser::RelTypeNameContext *> CypherParser::RelationshipTypesContext::relTypeName() {
  return getRuleContexts<CypherParser::RelTypeNameContext>();
}

CypherParser::RelTypeNameContext* CypherParser::RelationshipTypesContext::relTypeName(size_t i) {
  return getRuleContext<CypherParser::RelTypeNameContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::RelationshipTypesContext::PIPE() {
  return getTokens(CypherParser::PIPE);
}

tree::TerminalNode* CypherParser::RelationshipTypesContext::PIPE(size_t i) {
  return getToken(CypherParser::PIPE, i);
}


size_t CypherParser::RelationshipTypesContext::getRuleIndex() const {
  return CypherParser::RuleRelationshipTypes;
}


std::any CypherParser::RelationshipTypesContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelationshipTypes(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelationshipTypesContext* CypherParser::relationshipTypes() {
  RelationshipTypesContext *_localctx = _tracker.createInstance<RelationshipTypesContext>(_ctx, getState());
  enterRule(_localctx, 88, CypherParser::RuleRelationshipTypes);
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
    setState(617);
    match(CypherParser::COLON);
    setState(618);
    relTypeName();
    setState(626);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::PIPE) {
      setState(619);
      match(CypherParser::PIPE);
      setState(621);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::COLON) {
        setState(620);
        match(CypherParser::COLON);
      }
      setState(623);
      relTypeName();
      setState(628);
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

//----------------- RangeLiteralContext ------------------------------------------------------------------

CypherParser::RangeLiteralContext::RangeLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::RangeLiteralContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

std::vector<CypherParser::IntegerLiteralContext *> CypherParser::RangeLiteralContext::integerLiteral() {
  return getRuleContexts<CypherParser::IntegerLiteralContext>();
}

CypherParser::IntegerLiteralContext* CypherParser::RangeLiteralContext::integerLiteral(size_t i) {
  return getRuleContext<CypherParser::IntegerLiteralContext>(i);
}

tree::TerminalNode* CypherParser::RangeLiteralContext::RANGE() {
  return getToken(CypherParser::RANGE, 0);
}


size_t CypherParser::RangeLiteralContext::getRuleIndex() const {
  return CypherParser::RuleRangeLiteral;
}


std::any CypherParser::RangeLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRangeLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RangeLiteralContext* CypherParser::rangeLiteral() {
  RangeLiteralContext *_localctx = _tracker.createInstance<RangeLiteralContext>(_ctx, getState());
  enterRule(_localctx, 90, CypherParser::RuleRangeLiteral);
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
    setState(629);
    match(CypherParser::MULTIPLY);
    setState(631);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 91) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 91)) & 7) != 0)) {
      setState(630);
      integerLiteral();
    }
    setState(637);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::RANGE) {
      setState(633);
      match(CypherParser::RANGE);
      setState(635);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (((((_la - 91) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 91)) & 7) != 0)) {
        setState(634);
        integerLiteral();
      }
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

CypherParser::OrExpressionContext* CypherParser::ExpressionContext::orExpression() {
  return getRuleContext<CypherParser::OrExpressionContext>(0);
}


size_t CypherParser::ExpressionContext::getRuleIndex() const {
  return CypherParser::RuleExpression;
}


std::any CypherParser::ExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ExpressionContext* CypherParser::expression() {
  ExpressionContext *_localctx = _tracker.createInstance<ExpressionContext>(_ctx, getState());
  enterRule(_localctx, 92, CypherParser::RuleExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(639);
    orExpression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- OrExpressionContext ------------------------------------------------------------------

CypherParser::OrExpressionContext::OrExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::XorExpressionContext *> CypherParser::OrExpressionContext::xorExpression() {
  return getRuleContexts<CypherParser::XorExpressionContext>();
}

CypherParser::XorExpressionContext* CypherParser::OrExpressionContext::xorExpression(size_t i) {
  return getRuleContext<CypherParser::XorExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::OrExpressionContext::K_OR() {
  return getTokens(CypherParser::K_OR);
}

tree::TerminalNode* CypherParser::OrExpressionContext::K_OR(size_t i) {
  return getToken(CypherParser::K_OR, i);
}


size_t CypherParser::OrExpressionContext::getRuleIndex() const {
  return CypherParser::RuleOrExpression;
}


std::any CypherParser::OrExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitOrExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::OrExpressionContext* CypherParser::orExpression() {
  OrExpressionContext *_localctx = _tracker.createInstance<OrExpressionContext>(_ctx, getState());
  enterRule(_localctx, 94, CypherParser::RuleOrExpression);
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
    setState(641);
    xorExpression();
    setState(646);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_OR) {
      setState(642);
      match(CypherParser::K_OR);
      setState(643);
      xorExpression();
      setState(648);
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

//----------------- XorExpressionContext ------------------------------------------------------------------

CypherParser::XorExpressionContext::XorExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::AndExpressionContext *> CypherParser::XorExpressionContext::andExpression() {
  return getRuleContexts<CypherParser::AndExpressionContext>();
}

CypherParser::AndExpressionContext* CypherParser::XorExpressionContext::andExpression(size_t i) {
  return getRuleContext<CypherParser::AndExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::XorExpressionContext::K_XOR() {
  return getTokens(CypherParser::K_XOR);
}

tree::TerminalNode* CypherParser::XorExpressionContext::K_XOR(size_t i) {
  return getToken(CypherParser::K_XOR, i);
}


size_t CypherParser::XorExpressionContext::getRuleIndex() const {
  return CypherParser::RuleXorExpression;
}


std::any CypherParser::XorExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitXorExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::XorExpressionContext* CypherParser::xorExpression() {
  XorExpressionContext *_localctx = _tracker.createInstance<XorExpressionContext>(_ctx, getState());
  enterRule(_localctx, 96, CypherParser::RuleXorExpression);
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
    setState(649);
    andExpression();
    setState(654);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_XOR) {
      setState(650);
      match(CypherParser::K_XOR);
      setState(651);
      andExpression();
      setState(656);
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

//----------------- AndExpressionContext ------------------------------------------------------------------

CypherParser::AndExpressionContext::AndExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::NotExpressionContext *> CypherParser::AndExpressionContext::notExpression() {
  return getRuleContexts<CypherParser::NotExpressionContext>();
}

CypherParser::NotExpressionContext* CypherParser::AndExpressionContext::notExpression(size_t i) {
  return getRuleContext<CypherParser::NotExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::AndExpressionContext::K_AND() {
  return getTokens(CypherParser::K_AND);
}

tree::TerminalNode* CypherParser::AndExpressionContext::K_AND(size_t i) {
  return getToken(CypherParser::K_AND, i);
}


size_t CypherParser::AndExpressionContext::getRuleIndex() const {
  return CypherParser::RuleAndExpression;
}


std::any CypherParser::AndExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAndExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AndExpressionContext* CypherParser::andExpression() {
  AndExpressionContext *_localctx = _tracker.createInstance<AndExpressionContext>(_ctx, getState());
  enterRule(_localctx, 98, CypherParser::RuleAndExpression);
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
    setState(657);
    notExpression();
    setState(662);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_AND) {
      setState(658);
      match(CypherParser::K_AND);
      setState(659);
      notExpression();
      setState(664);
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

//----------------- NotExpressionContext ------------------------------------------------------------------

CypherParser::NotExpressionContext::NotExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ComparisonExpressionContext* CypherParser::NotExpressionContext::comparisonExpression() {
  return getRuleContext<CypherParser::ComparisonExpressionContext>(0);
}

tree::TerminalNode* CypherParser::NotExpressionContext::K_NOT() {
  return getToken(CypherParser::K_NOT, 0);
}


size_t CypherParser::NotExpressionContext::getRuleIndex() const {
  return CypherParser::RuleNotExpression;
}


std::any CypherParser::NotExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNotExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NotExpressionContext* CypherParser::notExpression() {
  NotExpressionContext *_localctx = _tracker.createInstance<NotExpressionContext>(_ctx, getState());
  enterRule(_localctx, 100, CypherParser::RuleNotExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(666);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 85, _ctx)) {
    case 1: {
      setState(665);
      match(CypherParser::K_NOT);
      break;
    }

    default:
      break;
    }
    setState(668);
    comparisonExpression();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ComparisonExpressionContext ------------------------------------------------------------------

CypherParser::ComparisonExpressionContext::ComparisonExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::ArithmeticExpressionContext *> CypherParser::ComparisonExpressionContext::arithmeticExpression() {
  return getRuleContexts<CypherParser::ArithmeticExpressionContext>();
}

CypherParser::ArithmeticExpressionContext* CypherParser::ComparisonExpressionContext::arithmeticExpression(size_t i) {
  return getRuleContext<CypherParser::ArithmeticExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::EQ() {
  return getTokens(CypherParser::EQ);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::EQ(size_t i) {
  return getToken(CypherParser::EQ, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::NEQ() {
  return getTokens(CypherParser::NEQ);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::NEQ(size_t i) {
  return getToken(CypherParser::NEQ, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::LT() {
  return getTokens(CypherParser::LT);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::LT(size_t i) {
  return getToken(CypherParser::LT, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::GT() {
  return getTokens(CypherParser::GT);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::GT(size_t i) {
  return getToken(CypherParser::GT, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::LTE() {
  return getTokens(CypherParser::LTE);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::LTE(size_t i) {
  return getToken(CypherParser::LTE, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::GTE() {
  return getTokens(CypherParser::GTE);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::GTE(size_t i) {
  return getToken(CypherParser::GTE, i);
}

std::vector<tree::TerminalNode *> CypherParser::ComparisonExpressionContext::K_IN() {
  return getTokens(CypherParser::K_IN);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_IN(size_t i) {
  return getToken(CypherParser::K_IN, i);
}


size_t CypherParser::ComparisonExpressionContext::getRuleIndex() const {
  return CypherParser::RuleComparisonExpression;
}


std::any CypherParser::ComparisonExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitComparisonExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ComparisonExpressionContext* CypherParser::comparisonExpression() {
  ComparisonExpressionContext *_localctx = _tracker.createInstance<ComparisonExpressionContext>(_ctx, getState());
  enterRule(_localctx, 102, CypherParser::RuleComparisonExpression);
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
    setState(670);
    arithmeticExpression();
    setState(675);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (((((_la - 30) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 30)) & 4329327034369) != 0)) {
      setState(671);
      _la = _input->LA(1);
      if (!(((((_la - 30) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 30)) & 4329327034369) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(672);
      arithmeticExpression();
      setState(677);
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

//----------------- ArithmeticExpressionContext ------------------------------------------------------------------

CypherParser::ArithmeticExpressionContext::ArithmeticExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::UnaryExpressionContext *> CypherParser::ArithmeticExpressionContext::unaryExpression() {
  return getRuleContexts<CypherParser::UnaryExpressionContext>();
}

CypherParser::UnaryExpressionContext* CypherParser::ArithmeticExpressionContext::unaryExpression(size_t i) {
  return getRuleContext<CypherParser::UnaryExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::PLUS() {
  return getTokens(CypherParser::PLUS);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::PLUS(size_t i) {
  return getToken(CypherParser::PLUS, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::MINUS() {
  return getTokens(CypherParser::MINUS);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::MINUS(size_t i) {
  return getToken(CypherParser::MINUS, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::MULTIPLY() {
  return getTokens(CypherParser::MULTIPLY);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::MULTIPLY(size_t i) {
  return getToken(CypherParser::MULTIPLY, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::DIVIDE() {
  return getTokens(CypherParser::DIVIDE);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::DIVIDE(size_t i) {
  return getToken(CypherParser::DIVIDE, i);
}

std::vector<tree::TerminalNode *> CypherParser::ArithmeticExpressionContext::MODULO() {
  return getTokens(CypherParser::MODULO);
}

tree::TerminalNode* CypherParser::ArithmeticExpressionContext::MODULO(size_t i) {
  return getToken(CypherParser::MODULO, i);
}


size_t CypherParser::ArithmeticExpressionContext::getRuleIndex() const {
  return CypherParser::RuleArithmeticExpression;
}


std::any CypherParser::ArithmeticExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitArithmeticExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ArithmeticExpressionContext* CypherParser::arithmeticExpression() {
  ArithmeticExpressionContext *_localctx = _tracker.createInstance<ArithmeticExpressionContext>(_ctx, getState());
  enterRule(_localctx, 104, CypherParser::RuleArithmeticExpression);
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
    setState(678);
    unaryExpression();
    setState(683);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (((((_la - 72) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 72)) & 31) != 0)) {
      setState(679);
      _la = _input->LA(1);
      if (!(((((_la - 72) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 72)) & 31) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(680);
      unaryExpression();
      setState(685);
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

//----------------- UnaryExpressionContext ------------------------------------------------------------------

CypherParser::UnaryExpressionContext::UnaryExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::AtomContext* CypherParser::UnaryExpressionContext::atom() {
  return getRuleContext<CypherParser::AtomContext>(0);
}

std::vector<CypherParser::AccessorContext *> CypherParser::UnaryExpressionContext::accessor() {
  return getRuleContexts<CypherParser::AccessorContext>();
}

CypherParser::AccessorContext* CypherParser::UnaryExpressionContext::accessor(size_t i) {
  return getRuleContext<CypherParser::AccessorContext>(i);
}

tree::TerminalNode* CypherParser::UnaryExpressionContext::PLUS() {
  return getToken(CypherParser::PLUS, 0);
}

tree::TerminalNode* CypherParser::UnaryExpressionContext::MINUS() {
  return getToken(CypherParser::MINUS, 0);
}


size_t CypherParser::UnaryExpressionContext::getRuleIndex() const {
  return CypherParser::RuleUnaryExpression;
}


std::any CypherParser::UnaryExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitUnaryExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::UnaryExpressionContext* CypherParser::unaryExpression() {
  UnaryExpressionContext *_localctx = _tracker.createInstance<UnaryExpressionContext>(_ctx, getState());
  enterRule(_localctx, 106, CypherParser::RuleUnaryExpression);
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
    setState(687);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::PLUS

    || _la == CypherParser::MINUS) {
      setState(686);
      _la = _input->LA(1);
      if (!(_la == CypherParser::PLUS

      || _la == CypherParser::MINUS)) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
    }
    setState(689);
    atom();
    setState(693);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::LBRACK

    || _la == CypherParser::DOT) {
      setState(690);
      accessor();
      setState(695);
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

//----------------- AccessorContext ------------------------------------------------------------------

CypherParser::AccessorContext::AccessorContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::AccessorContext::DOT() {
  return getToken(CypherParser::DOT, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::AccessorContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::AccessorContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

CypherParser::ExpressionContext* CypherParser::AccessorContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

tree::TerminalNode* CypherParser::AccessorContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}


size_t CypherParser::AccessorContext::getRuleIndex() const {
  return CypherParser::RuleAccessor;
}


std::any CypherParser::AccessorContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAccessor(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AccessorContext* CypherParser::accessor() {
  AccessorContext *_localctx = _tracker.createInstance<AccessorContext>(_ctx, getState());
  enterRule(_localctx, 108, CypherParser::RuleAccessor);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(702);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::DOT: {
        enterOuterAlt(_localctx, 1);
        setState(696);
        match(CypherParser::DOT);
        setState(697);
        propertyKeyName();
        break;
      }

      case CypherParser::LBRACK: {
        enterOuterAlt(_localctx, 2);
        setState(698);
        match(CypherParser::LBRACK);
        setState(699);
        expression();
        setState(700);
        match(CypherParser::RBRACK);
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

//----------------- AtomContext ------------------------------------------------------------------

CypherParser::AtomContext::AtomContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::LiteralContext* CypherParser::AtomContext::literal() {
  return getRuleContext<CypherParser::LiteralContext>(0);
}

CypherParser::ParameterContext* CypherParser::AtomContext::parameter() {
  return getRuleContext<CypherParser::ParameterContext>(0);
}

tree::TerminalNode* CypherParser::AtomContext::K_COUNT() {
  return getToken(CypherParser::K_COUNT, 0);
}

tree::TerminalNode* CypherParser::AtomContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::AtomContext::MULTIPLY() {
  return getToken(CypherParser::MULTIPLY, 0);
}

tree::TerminalNode* CypherParser::AtomContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

CypherParser::FunctionInvocationContext* CypherParser::AtomContext::functionInvocation() {
  return getRuleContext<CypherParser::FunctionInvocationContext>(0);
}

CypherParser::VariableContext* CypherParser::AtomContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

CypherParser::ExpressionContext* CypherParser::AtomContext::expression() {
  return getRuleContext<CypherParser::ExpressionContext>(0);
}

CypherParser::ListLiteralContext* CypherParser::AtomContext::listLiteral() {
  return getRuleContext<CypherParser::ListLiteralContext>(0);
}


size_t CypherParser::AtomContext::getRuleIndex() const {
  return CypherParser::RuleAtom;
}


std::any CypherParser::AtomContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitAtom(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::AtomContext* CypherParser::atom() {
  AtomContext *_localctx = _tracker.createInstance<AtomContext>(_ctx, getState());
  enterRule(_localctx, 110, CypherParser::RuleAtom);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(717);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 91, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(704);
      literal();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(705);
      parameter();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(706);
      match(CypherParser::K_COUNT);
      setState(707);
      match(CypherParser::LPAREN);
      setState(708);
      match(CypherParser::MULTIPLY);
      setState(709);
      match(CypherParser::RPAREN);
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(710);
      functionInvocation();
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(711);
      variable();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(712);
      match(CypherParser::LPAREN);
      setState(713);
      expression();
      setState(714);
      match(CypherParser::RPAREN);
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(716);
      listLiteral();
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

//----------------- PropertyExpressionContext ------------------------------------------------------------------

CypherParser::PropertyExpressionContext::PropertyExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::AtomContext* CypherParser::PropertyExpressionContext::atom() {
  return getRuleContext<CypherParser::AtomContext>(0);
}

std::vector<tree::TerminalNode *> CypherParser::PropertyExpressionContext::DOT() {
  return getTokens(CypherParser::DOT);
}

tree::TerminalNode* CypherParser::PropertyExpressionContext::DOT(size_t i) {
  return getToken(CypherParser::DOT, i);
}

std::vector<CypherParser::PropertyKeyNameContext *> CypherParser::PropertyExpressionContext::propertyKeyName() {
  return getRuleContexts<CypherParser::PropertyKeyNameContext>();
}

CypherParser::PropertyKeyNameContext* CypherParser::PropertyExpressionContext::propertyKeyName(size_t i) {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(i);
}


size_t CypherParser::PropertyExpressionContext::getRuleIndex() const {
  return CypherParser::RulePropertyExpression;
}


std::any CypherParser::PropertyExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPropertyExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PropertyExpressionContext* CypherParser::propertyExpression() {
  PropertyExpressionContext *_localctx = _tracker.createInstance<PropertyExpressionContext>(_ctx, getState());
  enterRule(_localctx, 112, CypherParser::RulePropertyExpression);
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
    setState(719);
    atom();
    setState(722); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(720);
      match(CypherParser::DOT);
      setState(721);
      propertyKeyName();
      setState(724); 
      _errHandler->sync(this);
      _la = _input->LA(1);
    } while (_la == CypherParser::DOT);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- FunctionInvocationContext ------------------------------------------------------------------

CypherParser::FunctionInvocationContext::FunctionInvocationContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::FunctionNameContext* CypherParser::FunctionInvocationContext::functionName() {
  return getRuleContext<CypherParser::FunctionNameContext>(0);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::K_DISTINCT() {
  return getToken(CypherParser::K_DISTINCT, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::FunctionInvocationContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::FunctionInvocationContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::FunctionInvocationContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::FunctionInvocationContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::FunctionInvocationContext::getRuleIndex() const {
  return CypherParser::RuleFunctionInvocation;
}


std::any CypherParser::FunctionInvocationContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitFunctionInvocation(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::FunctionInvocationContext* CypherParser::functionInvocation() {
  FunctionInvocationContext *_localctx = _tracker.createInstance<FunctionInvocationContext>(_ctx, getState());
  enterRule(_localctx, 114, CypherParser::RuleFunctionInvocation);
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
    setState(726);
    functionName();
    setState(727);
    match(CypherParser::LPAREN);
    setState(729);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 93, _ctx)) {
    case 1: {
      setState(728);
      match(CypherParser::K_DISTINCT);
      break;
    }

    default:
      break;
    }
    setState(739);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 8472838915) != 0)) {
      setState(731);
      expression();
      setState(736);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(732);
        match(CypherParser::COMMA);
        setState(733);
        expression();
        setState(738);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(741);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ExplicitProcedureInvocationContext ------------------------------------------------------------------

CypherParser::ExplicitProcedureInvocationContext::ExplicitProcedureInvocationContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ProcedureNameContext* CypherParser::ExplicitProcedureInvocationContext::procedureName() {
  return getRuleContext<CypherParser::ProcedureNameContext>(0);
}

tree::TerminalNode* CypherParser::ExplicitProcedureInvocationContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

tree::TerminalNode* CypherParser::ExplicitProcedureInvocationContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ExplicitProcedureInvocationContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ExplicitProcedureInvocationContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ExplicitProcedureInvocationContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ExplicitProcedureInvocationContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ExplicitProcedureInvocationContext::getRuleIndex() const {
  return CypherParser::RuleExplicitProcedureInvocation;
}


std::any CypherParser::ExplicitProcedureInvocationContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitExplicitProcedureInvocation(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ExplicitProcedureInvocationContext* CypherParser::explicitProcedureInvocation() {
  ExplicitProcedureInvocationContext *_localctx = _tracker.createInstance<ExplicitProcedureInvocationContext>(_ctx, getState());
  enterRule(_localctx, 116, CypherParser::RuleExplicitProcedureInvocation);
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
    setState(743);
    procedureName();
    setState(744);
    match(CypherParser::LPAREN);
    setState(753);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 8472838915) != 0)) {
      setState(745);
      expression();
      setState(750);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(746);
        match(CypherParser::COMMA);
        setState(747);
        expression();
        setState(752);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(755);
    match(CypherParser::RPAREN);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ImplicitProcedureInvocationContext ------------------------------------------------------------------

CypherParser::ImplicitProcedureInvocationContext::ImplicitProcedureInvocationContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::ProcedureNameContext* CypherParser::ImplicitProcedureInvocationContext::procedureName() {
  return getRuleContext<CypherParser::ProcedureNameContext>(0);
}


size_t CypherParser::ImplicitProcedureInvocationContext::getRuleIndex() const {
  return CypherParser::RuleImplicitProcedureInvocation;
}


std::any CypherParser::ImplicitProcedureInvocationContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitImplicitProcedureInvocation(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ImplicitProcedureInvocationContext* CypherParser::implicitProcedureInvocation() {
  ImplicitProcedureInvocationContext *_localctx = _tracker.createInstance<ImplicitProcedureInvocationContext>(_ctx, getState());
  enterRule(_localctx, 118, CypherParser::RuleImplicitProcedureInvocation);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(757);
    procedureName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- VariableContext ------------------------------------------------------------------

CypherParser::VariableContext::VariableContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SymbolicNameContext* CypherParser::VariableContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::VariableContext::getRuleIndex() const {
  return CypherParser::RuleVariable;
}


std::any CypherParser::VariableContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitVariable(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::VariableContext* CypherParser::variable() {
  VariableContext *_localctx = _tracker.createInstance<VariableContext>(_ctx, getState());
  enterRule(_localctx, 120, CypherParser::RuleVariable);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(759);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- LabelNameContext ------------------------------------------------------------------

CypherParser::LabelNameContext::LabelNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SchemaNameContext* CypherParser::LabelNameContext::schemaName() {
  return getRuleContext<CypherParser::SchemaNameContext>(0);
}


size_t CypherParser::LabelNameContext::getRuleIndex() const {
  return CypherParser::RuleLabelName;
}


std::any CypherParser::LabelNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitLabelName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LabelNameContext* CypherParser::labelName() {
  LabelNameContext *_localctx = _tracker.createInstance<LabelNameContext>(_ctx, getState());
  enterRule(_localctx, 122, CypherParser::RuleLabelName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(761);
    schemaName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- RelTypeNameContext ------------------------------------------------------------------

CypherParser::RelTypeNameContext::RelTypeNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SchemaNameContext* CypherParser::RelTypeNameContext::schemaName() {
  return getRuleContext<CypherParser::SchemaNameContext>(0);
}


size_t CypherParser::RelTypeNameContext::getRuleIndex() const {
  return CypherParser::RuleRelTypeName;
}


std::any CypherParser::RelTypeNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitRelTypeName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::RelTypeNameContext* CypherParser::relTypeName() {
  RelTypeNameContext *_localctx = _tracker.createInstance<RelTypeNameContext>(_ctx, getState());
  enterRule(_localctx, 124, CypherParser::RuleRelTypeName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(763);
    schemaName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- PropertyKeyNameContext ------------------------------------------------------------------

CypherParser::PropertyKeyNameContext::PropertyKeyNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SchemaNameContext* CypherParser::PropertyKeyNameContext::schemaName() {
  return getRuleContext<CypherParser::SchemaNameContext>(0);
}


size_t CypherParser::PropertyKeyNameContext::getRuleIndex() const {
  return CypherParser::RulePropertyKeyName;
}


std::any CypherParser::PropertyKeyNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPropertyKeyName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PropertyKeyNameContext* CypherParser::propertyKeyName() {
  PropertyKeyNameContext *_localctx = _tracker.createInstance<PropertyKeyNameContext>(_ctx, getState());
  enterRule(_localctx, 126, CypherParser::RulePropertyKeyName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(765);
    schemaName();
   
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

CypherParser::NamespaceContext* CypherParser::ProcedureNameContext::namespace_() {
  return getRuleContext<CypherParser::NamespaceContext>(0);
}

CypherParser::SymbolicNameContext* CypherParser::ProcedureNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::ProcedureNameContext::getRuleIndex() const {
  return CypherParser::RuleProcedureName;
}


std::any CypherParser::ProcedureNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProcedureName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProcedureNameContext* CypherParser::procedureName() {
  ProcedureNameContext *_localctx = _tracker.createInstance<ProcedureNameContext>(_ctx, getState());
  enterRule(_localctx, 128, CypherParser::RuleProcedureName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(767);
    namespace_();
    setState(768);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ProcedureResultFieldContext ------------------------------------------------------------------

CypherParser::ProcedureResultFieldContext::ProcedureResultFieldContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SymbolicNameContext* CypherParser::ProcedureResultFieldContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::ProcedureResultFieldContext::getRuleIndex() const {
  return CypherParser::RuleProcedureResultField;
}


std::any CypherParser::ProcedureResultFieldContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitProcedureResultField(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ProcedureResultFieldContext* CypherParser::procedureResultField() {
  ProcedureResultFieldContext *_localctx = _tracker.createInstance<ProcedureResultFieldContext>(_ctx, getState());
  enterRule(_localctx, 130, CypherParser::RuleProcedureResultField);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(770);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- FunctionNameContext ------------------------------------------------------------------

CypherParser::FunctionNameContext::FunctionNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::NamespaceContext* CypherParser::FunctionNameContext::namespace_() {
  return getRuleContext<CypherParser::NamespaceContext>(0);
}

CypherParser::SymbolicNameContext* CypherParser::FunctionNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}


size_t CypherParser::FunctionNameContext::getRuleIndex() const {
  return CypherParser::RuleFunctionName;
}


std::any CypherParser::FunctionNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitFunctionName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::FunctionNameContext* CypherParser::functionName() {
  FunctionNameContext *_localctx = _tracker.createInstance<FunctionNameContext>(_ctx, getState());
  enterRule(_localctx, 132, CypherParser::RuleFunctionName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(772);
    namespace_();
    setState(773);
    symbolicName();
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- NamespaceContext ------------------------------------------------------------------

CypherParser::NamespaceContext::NamespaceContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::SymbolicNameContext *> CypherParser::NamespaceContext::symbolicName() {
  return getRuleContexts<CypherParser::SymbolicNameContext>();
}

CypherParser::SymbolicNameContext* CypherParser::NamespaceContext::symbolicName(size_t i) {
  return getRuleContext<CypherParser::SymbolicNameContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::NamespaceContext::DOT() {
  return getTokens(CypherParser::DOT);
}

tree::TerminalNode* CypherParser::NamespaceContext::DOT(size_t i) {
  return getToken(CypherParser::DOT, i);
}


size_t CypherParser::NamespaceContext::getRuleIndex() const {
  return CypherParser::RuleNamespace;
}


std::any CypherParser::NamespaceContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNamespace(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NamespaceContext* CypherParser::namespace_() {
  NamespaceContext *_localctx = _tracker.createInstance<NamespaceContext>(_ctx, getState());
  enterRule(_localctx, 134, CypherParser::RuleNamespace);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    size_t alt;
    enterOuterAlt(_localctx, 1);
    setState(780);
    _errHandler->sync(this);
    alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 98, _ctx);
    while (alt != 2 && alt != atn::ATN::INVALID_ALT_NUMBER) {
      if (alt == 1) {
        setState(775);
        symbolicName();
        setState(776);
        match(CypherParser::DOT); 
      }
      setState(782);
      _errHandler->sync(this);
      alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 98, _ctx);
    }
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- SchemaNameContext ------------------------------------------------------------------

CypherParser::SchemaNameContext::SchemaNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::SymbolicNameContext* CypherParser::SchemaNameContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_MATCH() {
  return getToken(CypherParser::K_MATCH, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::SchemaNameContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}


size_t CypherParser::SchemaNameContext::getRuleIndex() const {
  return CypherParser::RuleSchemaName;
}


std::any CypherParser::SchemaNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSchemaName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SchemaNameContext* CypherParser::schemaName() {
  SchemaNameContext *_localctx = _tracker.createInstance<SchemaNameContext>(_ctx, getState());
  enterRule(_localctx, 136, CypherParser::RuleSchemaName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(790);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 99, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(783);
      symbolicName();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(784);
      match(CypherParser::K_INDEX);
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(785);
      match(CypherParser::K_ON);
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(786);
      match(CypherParser::K_MATCH);
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(787);
      match(CypherParser::K_CREATE);
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(788);
      match(CypherParser::K_WHERE);
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(789);
      match(CypherParser::K_RETURN);
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

//----------------- SymbolicNameContext ------------------------------------------------------------------

CypherParser::SymbolicNameContext::SymbolicNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SymbolicNameContext::ID() {
  return getToken(CypherParser::ID, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CALL() {
  return getToken(CypherParser::K_CALL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_YIELD() {
  return getToken(CypherParser::K_YIELD, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DELETE() {
  return getToken(CypherParser::K_DELETE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DETACH() {
  return getToken(CypherParser::K_DETACH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_EXISTS() {
  return getToken(CypherParser::K_EXISTS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_LIMIT() {
  return getToken(CypherParser::K_LIMIT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MATCH() {
  return getToken(CypherParser::K_MATCH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MERGE() {
  return getToken(CypherParser::K_MERGE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OPTIONAL() {
  return getToken(CypherParser::K_OPTIONAL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ORDER() {
  return getToken(CypherParser::K_ORDER, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_BY() {
  return getToken(CypherParser::K_BY, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SKIP() {
  return getToken(CypherParser::K_SKIP, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ASCENDING() {
  return getToken(CypherParser::K_ASCENDING, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ASC() {
  return getToken(CypherParser::K_ASC, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DESCENDING() {
  return getToken(CypherParser::K_DESCENDING, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DESC() {
  return getToken(CypherParser::K_DESC, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_REMOVE() {
  return getToken(CypherParser::K_REMOVE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_RETURN() {
  return getToken(CypherParser::K_RETURN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SET() {
  return getToken(CypherParser::K_SET, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_WITH() {
  return getToken(CypherParser::K_WITH, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_UNION() {
  return getToken(CypherParser::K_UNION, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_UNWIND() {
  return getToken(CypherParser::K_UNWIND, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_AND() {
  return getToken(CypherParser::K_AND, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_AS() {
  return getToken(CypherParser::K_AS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CONTAINS() {
  return getToken(CypherParser::K_CONTAINS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DISTINCT() {
  return getToken(CypherParser::K_DISTINCT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ENDS() {
  return getToken(CypherParser::K_ENDS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_NOT() {
  return getToken(CypherParser::K_NOT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OR() {
  return getToken(CypherParser::K_OR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_STARTS() {
  return getToken(CypherParser::K_STARTS, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_XOR() {
  return getToken(CypherParser::K_XOR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FALSE() {
  return getToken(CypherParser::K_FALSE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_TRUE() {
  return getToken(CypherParser::K_TRUE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_NULL() {
  return getToken(CypherParser::K_NULL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CASE() {
  return getToken(CypherParser::K_CASE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_WHEN() {
  return getToken(CypherParser::K_WHEN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_THEN() {
  return getToken(CypherParser::K_THEN, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ELSE() {
  return getToken(CypherParser::K_ELSE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_END() {
  return getToken(CypherParser::K_END, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_COUNT() {
  return getToken(CypherParser::K_COUNT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FILTER() {
  return getToken(CypherParser::K_FILTER, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_EXTRACT() {
  return getToken(CypherParser::K_EXTRACT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ANY() {
  return getToken(CypherParser::K_ANY, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_NONE() {
  return getToken(CypherParser::K_NONE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SINGLE() {
  return getToken(CypherParser::K_SINGLE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ALL() {
  return getToken(CypherParser::K_ALL, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SHOW() {
  return getToken(CypherParser::K_SHOW, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_FOR() {
  return getToken(CypherParser::K_FOR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_CONSTRAINT() {
  return getToken(CypherParser::K_CONSTRAINT, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_DO() {
  return getToken(CypherParser::K_DO, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_REQUIRE() {
  return getToken(CypherParser::K_REQUIRE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_UNIQUE() {
  return getToken(CypherParser::K_UNIQUE, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_MANDATORY() {
  return getToken(CypherParser::K_MANDATORY, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_SCALAR() {
  return getToken(CypherParser::K_SCALAR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OF() {
  return getToken(CypherParser::K_OF, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_ADD() {
  return getToken(CypherParser::K_ADD, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_VECTOR() {
  return getToken(CypherParser::K_VECTOR, 0);
}

tree::TerminalNode* CypherParser::SymbolicNameContext::K_OPTIONS() {
  return getToken(CypherParser::K_OPTIONS, 0);
}


size_t CypherParser::SymbolicNameContext::getRuleIndex() const {
  return CypherParser::RuleSymbolicName;
}


std::any CypherParser::SymbolicNameContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitSymbolicName(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::SymbolicNameContext* CypherParser::symbolicName() {
  SymbolicNameContext *_localctx = _tracker.createInstance<SymbolicNameContext>(_ctx, getState());
  enterRule(_localctx, 138, CypherParser::RuleSymbolicName);
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
    setState(792);
    _la = _input->LA(1);
    if (!((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0))) {
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

//----------------- LiteralContext ------------------------------------------------------------------

CypherParser::LiteralContext::LiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

CypherParser::BooleanLiteralContext* CypherParser::LiteralContext::booleanLiteral() {
  return getRuleContext<CypherParser::BooleanLiteralContext>(0);
}

tree::TerminalNode* CypherParser::LiteralContext::K_NULL() {
  return getToken(CypherParser::K_NULL, 0);
}

CypherParser::NumberLiteralContext* CypherParser::LiteralContext::numberLiteral() {
  return getRuleContext<CypherParser::NumberLiteralContext>(0);
}

tree::TerminalNode* CypherParser::LiteralContext::StringLiteral() {
  return getToken(CypherParser::StringLiteral, 0);
}

CypherParser::MapLiteralContext* CypherParser::LiteralContext::mapLiteral() {
  return getRuleContext<CypherParser::MapLiteralContext>(0);
}


size_t CypherParser::LiteralContext::getRuleIndex() const {
  return CypherParser::RuleLiteral;
}


std::any CypherParser::LiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::LiteralContext* CypherParser::literal() {
  LiteralContext *_localctx = _tracker.createInstance<LiteralContext>(_ctx, getState());
  enterRule(_localctx, 140, CypherParser::RuleLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(799);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_FALSE:
      case CypherParser::K_TRUE: {
        enterOuterAlt(_localctx, 1);
        setState(794);
        booleanLiteral();
        break;
      }

      case CypherParser::K_NULL: {
        enterOuterAlt(_localctx, 2);
        setState(795);
        match(CypherParser::K_NULL);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger:
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 3);
        setState(796);
        numberLiteral();
        break;
      }

      case CypherParser::StringLiteral: {
        enterOuterAlt(_localctx, 4);
        setState(797);
        match(CypherParser::StringLiteral);
        break;
      }

      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 5);
        setState(798);
        mapLiteral();
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

//----------------- BooleanLiteralContext ------------------------------------------------------------------

CypherParser::BooleanLiteralContext::BooleanLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::BooleanLiteralContext::K_TRUE() {
  return getToken(CypherParser::K_TRUE, 0);
}

tree::TerminalNode* CypherParser::BooleanLiteralContext::K_FALSE() {
  return getToken(CypherParser::K_FALSE, 0);
}


size_t CypherParser::BooleanLiteralContext::getRuleIndex() const {
  return CypherParser::RuleBooleanLiteral;
}


std::any CypherParser::BooleanLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitBooleanLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::BooleanLiteralContext* CypherParser::booleanLiteral() {
  BooleanLiteralContext *_localctx = _tracker.createInstance<BooleanLiteralContext>(_ctx, getState());
  enterRule(_localctx, 142, CypherParser::RuleBooleanLiteral);
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
    setState(801);
    _la = _input->LA(1);
    if (!(_la == CypherParser::K_FALSE

    || _la == CypherParser::K_TRUE)) {
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

//----------------- NumberLiteralContext ------------------------------------------------------------------

CypherParser::NumberLiteralContext::NumberLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::NumberLiteralContext::DoubleLiteral() {
  return getToken(CypherParser::DoubleLiteral, 0);
}

CypherParser::IntegerLiteralContext* CypherParser::NumberLiteralContext::integerLiteral() {
  return getRuleContext<CypherParser::IntegerLiteralContext>(0);
}


size_t CypherParser::NumberLiteralContext::getRuleIndex() const {
  return CypherParser::RuleNumberLiteral;
}


std::any CypherParser::NumberLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitNumberLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::NumberLiteralContext* CypherParser::numberLiteral() {
  NumberLiteralContext *_localctx = _tracker.createInstance<NumberLiteralContext>(_ctx, getState());
  enterRule(_localctx, 144, CypherParser::RuleNumberLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(805);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 1);
        setState(803);
        match(CypherParser::DoubleLiteral);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger: {
        enterOuterAlt(_localctx, 2);
        setState(804);
        integerLiteral();
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

//----------------- IntegerLiteralContext ------------------------------------------------------------------

CypherParser::IntegerLiteralContext::IntegerLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::IntegerLiteralContext::DecimalInteger() {
  return getToken(CypherParser::DecimalInteger, 0);
}

tree::TerminalNode* CypherParser::IntegerLiteralContext::HexInteger() {
  return getToken(CypherParser::HexInteger, 0);
}

tree::TerminalNode* CypherParser::IntegerLiteralContext::OctalInteger() {
  return getToken(CypherParser::OctalInteger, 0);
}


size_t CypherParser::IntegerLiteralContext::getRuleIndex() const {
  return CypherParser::RuleIntegerLiteral;
}


std::any CypherParser::IntegerLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitIntegerLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::IntegerLiteralContext* CypherParser::integerLiteral() {
  IntegerLiteralContext *_localctx = _tracker.createInstance<IntegerLiteralContext>(_ctx, getState());
  enterRule(_localctx, 146, CypherParser::RuleIntegerLiteral);
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
    setState(807);
    _la = _input->LA(1);
    if (!(((((_la - 91) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 91)) & 7) != 0))) {
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

std::vector<CypherParser::PropertyKeyNameContext *> CypherParser::MapLiteralContext::propertyKeyName() {
  return getRuleContexts<CypherParser::PropertyKeyNameContext>();
}

CypherParser::PropertyKeyNameContext* CypherParser::MapLiteralContext::propertyKeyName(size_t i) {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::MapLiteralContext::COLON() {
  return getTokens(CypherParser::COLON);
}

tree::TerminalNode* CypherParser::MapLiteralContext::COLON(size_t i) {
  return getToken(CypherParser::COLON, i);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::MapLiteralContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::MapLiteralContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
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
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitMapLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::MapLiteralContext* CypherParser::mapLiteral() {
  MapLiteralContext *_localctx = _tracker.createInstance<MapLiteralContext>(_ctx, getState());
  enterRule(_localctx, 148, CypherParser::RuleMapLiteral);
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
    setState(809);
    match(CypherParser::LBRACE);
    setState(823);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(810);
      propertyKeyName();
      setState(811);
      match(CypherParser::COLON);
      setState(812);
      expression();
      setState(820);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(813);
        match(CypherParser::COMMA);
        setState(814);
        propertyKeyName();
        setState(815);
        match(CypherParser::COLON);
        setState(816);
        expression();
        setState(822);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(825);
    match(CypherParser::RBRACE);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ListLiteralContext ------------------------------------------------------------------

CypherParser::ListLiteralContext::ListLiteralContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ListLiteralContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

tree::TerminalNode* CypherParser::ListLiteralContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ListLiteralContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ListLiteralContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::ListLiteralContext::COMMA() {
  return getTokens(CypherParser::COMMA);
}

tree::TerminalNode* CypherParser::ListLiteralContext::COMMA(size_t i) {
  return getToken(CypherParser::COMMA, i);
}


size_t CypherParser::ListLiteralContext::getRuleIndex() const {
  return CypherParser::RuleListLiteral;
}


std::any CypherParser::ListLiteralContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitListLiteral(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ListLiteralContext* CypherParser::listLiteral() {
  ListLiteralContext *_localctx = _tracker.createInstance<ListLiteralContext>(_ctx, getState());
  enterRule(_localctx, 150, CypherParser::RuleListLiteral);
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
    setState(827);
    match(CypherParser::LBRACK);
    setState(836);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 8472838915) != 0)) {
      setState(828);
      expression();
      setState(833);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(829);
        match(CypherParser::COMMA);
        setState(830);
        expression();
        setState(835);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(838);
    match(CypherParser::RBRACK);
   
  }
  catch (RecognitionException &e) {
    _errHandler->reportError(this, e);
    _localctx->exception = std::current_exception();
    _errHandler->recover(this, _localctx->exception);
  }

  return _localctx;
}

//----------------- ParameterContext ------------------------------------------------------------------

CypherParser::ParameterContext::ParameterContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ParameterContext::DOLLAR() {
  return getToken(CypherParser::DOLLAR, 0);
}

CypherParser::SymbolicNameContext* CypherParser::ParameterContext::symbolicName() {
  return getRuleContext<CypherParser::SymbolicNameContext>(0);
}

tree::TerminalNode* CypherParser::ParameterContext::DecimalInteger() {
  return getToken(CypherParser::DecimalInteger, 0);
}


size_t CypherParser::ParameterContext::getRuleIndex() const {
  return CypherParser::RuleParameter;
}


std::any CypherParser::ParameterContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitParameter(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ParameterContext* CypherParser::parameter() {
  ParameterContext *_localctx = _tracker.createInstance<ParameterContext>(_ctx, getState());
  enterRule(_localctx, 152, CypherParser::RuleParameter);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(840);
    match(CypherParser::DOLLAR);
    setState(843);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_CALL:
      case CypherParser::K_YIELD:
      case CypherParser::K_CREATE:
      case CypherParser::K_DELETE:
      case CypherParser::K_DETACH:
      case CypherParser::K_EXISTS:
      case CypherParser::K_LIMIT:
      case CypherParser::K_MATCH:
      case CypherParser::K_MERGE:
      case CypherParser::K_OPTIONAL:
      case CypherParser::K_ORDER:
      case CypherParser::K_BY:
      case CypherParser::K_SKIP:
      case CypherParser::K_ASCENDING:
      case CypherParser::K_ASC:
      case CypherParser::K_DESCENDING:
      case CypherParser::K_DESC:
      case CypherParser::K_REMOVE:
      case CypherParser::K_RETURN:
      case CypherParser::K_SET:
      case CypherParser::K_WHERE:
      case CypherParser::K_WITH:
      case CypherParser::K_UNION:
      case CypherParser::K_UNWIND:
      case CypherParser::K_AND:
      case CypherParser::K_AS:
      case CypherParser::K_CONTAINS:
      case CypherParser::K_DISTINCT:
      case CypherParser::K_ENDS:
      case CypherParser::K_IN:
      case CypherParser::K_IS:
      case CypherParser::K_NOT:
      case CypherParser::K_OR:
      case CypherParser::K_STARTS:
      case CypherParser::K_XOR:
      case CypherParser::K_FALSE:
      case CypherParser::K_TRUE:
      case CypherParser::K_NULL:
      case CypherParser::K_CASE:
      case CypherParser::K_WHEN:
      case CypherParser::K_THEN:
      case CypherParser::K_ELSE:
      case CypherParser::K_END:
      case CypherParser::K_COUNT:
      case CypherParser::K_FILTER:
      case CypherParser::K_EXTRACT:
      case CypherParser::K_ANY:
      case CypherParser::K_NONE:
      case CypherParser::K_SINGLE:
      case CypherParser::K_ALL:
      case CypherParser::K_INDEX:
      case CypherParser::K_ON:
      case CypherParser::K_SHOW:
      case CypherParser::K_DROP:
      case CypherParser::K_FOR:
      case CypherParser::K_CONSTRAINT:
      case CypherParser::K_DO:
      case CypherParser::K_REQUIRE:
      case CypherParser::K_UNIQUE:
      case CypherParser::K_MANDATORY:
      case CypherParser::K_SCALAR:
      case CypherParser::K_OF:
      case CypherParser::K_ADD:
      case CypherParser::K_VECTOR:
      case CypherParser::K_OPTIONS:
      case CypherParser::ID: {
        setState(841);
        symbolicName();
        break;
      }

      case CypherParser::DecimalInteger: {
        setState(842);
        match(CypherParser::DecimalInteger);
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

void CypherParser::initialize() {
#if ANTLR4_USE_THREAD_LOCAL_CACHE
  cypherparserParserInitialize();
#else
  ::antlr4::internal::call_once(cypherparserParserOnceFlag, cypherparserParserInitialize);
#endif
}
