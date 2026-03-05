
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
      "arithmeticExpression", "powerExpression", "unaryExpression", "accessor", 
      "atom", "caseExpression", "propertyExpression", "functionInvocation", 
      "explicitProcedureInvocation", "implicitProcedureInvocation", "variable", 
      "labelName", "relTypeName", "propertyKeyName", "procedureName", "procedureResultField", 
      "functionName", "namespace", "schemaName", "symbolicName", "literal", 
      "booleanLiteral", "numberLiteral", "integerLiteral", "mapLiteral", 
      "listLiteral", "parameter", "listComprehension"
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
  	4,1,99,909,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,6,2,
  	7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,7,
  	14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,2,20,7,20,2,21,7,
  	21,2,22,7,22,2,23,7,23,2,24,7,24,2,25,7,25,2,26,7,26,2,27,7,27,2,28,7,
  	28,2,29,7,29,2,30,7,30,2,31,7,31,2,32,7,32,2,33,7,33,2,34,7,34,2,35,7,
  	35,2,36,7,36,2,37,7,37,2,38,7,38,2,39,7,39,2,40,7,40,2,41,7,41,2,42,7,
  	42,2,43,7,43,2,44,7,44,2,45,7,45,2,46,7,46,2,47,7,47,2,48,7,48,2,49,7,
  	49,2,50,7,50,2,51,7,51,2,52,7,52,2,53,7,53,2,54,7,54,2,55,7,55,2,56,7,
  	56,2,57,7,57,2,58,7,58,2,59,7,59,2,60,7,60,2,61,7,61,2,62,7,62,2,63,7,
  	63,2,64,7,64,2,65,7,65,2,66,7,66,2,67,7,67,2,68,7,68,2,69,7,69,2,70,7,
  	70,2,71,7,71,2,72,7,72,2,73,7,73,2,74,7,74,2,75,7,75,2,76,7,76,2,77,7,
  	77,2,78,7,78,2,79,7,79,1,0,1,0,3,0,163,8,0,1,0,1,0,1,1,1,1,3,1,169,8,
  	1,1,2,1,2,1,2,3,2,174,8,2,1,3,1,3,1,3,1,4,1,4,1,4,1,4,1,4,1,4,1,4,1,4,
  	1,4,1,4,1,4,3,4,190,8,4,1,5,1,5,1,5,3,5,195,8,5,1,5,1,5,1,5,1,5,1,5,1,
  	5,1,5,1,5,1,5,1,5,3,5,207,8,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,
  	3,5,219,8,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,3,5,228,8,5,3,5,230,8,5,1,6,1,
  	6,3,6,234,8,6,1,7,1,7,1,7,3,7,239,8,7,1,7,5,7,242,8,7,10,7,12,7,245,9,
  	7,1,8,4,8,248,8,8,11,8,12,8,249,1,8,5,8,253,8,8,10,8,12,8,256,9,8,1,8,
  	1,8,1,8,4,8,261,8,8,11,8,12,8,262,1,8,5,8,266,8,8,10,8,12,8,269,9,8,1,
  	8,4,8,272,8,8,11,8,12,8,273,1,8,3,8,277,8,8,1,8,5,8,280,8,8,10,8,12,8,
  	283,9,8,1,8,4,8,286,8,8,11,8,12,8,287,1,8,5,8,291,8,8,10,8,12,8,294,9,
  	8,1,8,1,8,1,8,5,8,299,8,8,10,8,12,8,302,9,8,1,8,4,8,305,8,8,11,8,12,8,
  	306,1,8,5,8,310,8,8,10,8,12,8,313,9,8,1,8,4,8,316,8,8,11,8,12,8,317,1,
  	8,3,8,321,8,8,1,8,5,8,324,8,8,10,8,12,8,327,9,8,1,8,1,8,5,8,331,8,8,10,
  	8,12,8,334,9,8,1,8,4,8,337,8,8,11,8,12,8,338,1,8,3,8,342,8,8,3,8,344,
  	8,8,1,9,1,9,1,9,3,9,349,8,9,1,10,1,10,1,10,1,10,1,10,3,10,356,8,10,1,
  	11,1,11,1,11,1,11,3,11,362,8,11,1,12,3,12,365,8,12,1,12,1,12,1,12,1,12,
  	3,12,371,8,12,1,13,1,13,1,13,1,13,1,13,1,14,1,14,1,14,1,14,3,14,382,8,
  	14,1,15,1,15,1,15,1,16,1,16,1,16,1,16,1,16,5,16,392,8,16,10,16,12,16,
  	395,9,16,1,17,1,17,1,17,1,17,5,17,401,8,17,10,17,12,17,404,9,17,1,18,
  	1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,1,18,
  	1,18,3,18,422,8,18,1,19,3,19,425,8,19,1,19,1,19,1,19,1,19,5,19,431,8,
  	19,10,19,12,19,434,9,19,1,20,1,20,1,20,1,20,5,20,440,8,20,10,20,12,20,
  	443,9,20,1,21,1,21,1,21,1,21,3,21,449,8,21,1,22,1,22,1,22,1,23,1,23,1,
  	23,3,23,457,8,23,1,23,1,23,1,23,3,23,462,8,23,3,23,464,8,23,1,24,1,24,
  	1,24,5,24,469,8,24,10,24,12,24,472,9,24,1,24,1,24,3,24,476,8,24,1,25,
  	1,25,1,25,3,25,481,8,25,1,25,1,25,1,26,3,26,486,8,26,1,26,1,26,3,26,490,
  	8,26,1,26,3,26,493,8,26,1,26,3,26,496,8,26,1,27,1,27,1,27,1,27,5,27,502,
  	8,27,10,27,12,27,505,9,27,3,27,507,8,27,1,28,1,28,1,28,3,28,512,8,28,
  	1,29,1,29,1,29,1,29,1,29,5,29,519,8,29,10,29,12,29,522,9,29,1,30,1,30,
  	1,30,1,31,1,31,1,31,1,32,1,32,3,32,532,8,32,1,33,1,33,1,34,1,34,1,34,
  	5,34,539,8,34,10,34,12,34,542,9,34,1,35,1,35,1,35,3,35,547,8,35,1,35,
  	1,35,1,36,1,36,5,36,553,8,36,10,36,12,36,556,9,36,1,36,1,36,1,36,1,36,
  	3,36,562,8,36,1,37,1,37,1,37,1,38,1,38,3,38,569,8,38,1,38,3,38,572,8,
  	38,1,38,3,38,575,8,38,1,38,1,38,1,39,3,39,580,8,39,1,39,1,39,3,39,584,
  	8,39,1,39,1,39,3,39,588,8,39,1,39,1,39,3,39,592,8,39,1,39,3,39,595,8,
  	39,1,40,1,40,3,40,599,8,40,1,40,3,40,602,8,40,1,40,3,40,605,8,40,1,40,
  	3,40,608,8,40,1,40,1,40,1,41,1,41,3,41,614,8,41,1,42,4,42,617,8,42,11,
  	42,12,42,618,1,43,1,43,1,43,1,44,1,44,1,44,1,44,3,44,628,8,44,1,44,5,
  	44,631,8,44,10,44,12,44,634,9,44,1,45,1,45,3,45,638,8,45,1,45,1,45,3,
  	45,642,8,45,3,45,644,8,45,1,46,1,46,1,47,1,47,1,47,5,47,651,8,47,10,47,
  	12,47,654,9,47,1,48,1,48,1,48,5,48,659,8,48,10,48,12,48,662,9,48,1,49,
  	1,49,1,49,5,49,667,8,49,10,49,12,49,670,9,49,1,50,3,50,673,8,50,1,50,
  	1,50,1,51,1,51,1,51,5,51,680,8,51,10,51,12,51,683,9,51,1,51,1,51,3,51,
  	687,8,51,1,51,3,51,690,8,51,1,52,1,52,1,52,5,52,695,8,52,10,52,12,52,
  	698,9,52,1,53,1,53,1,53,5,53,703,8,53,10,53,12,53,706,9,53,1,54,3,54,
  	709,8,54,1,54,1,54,5,54,713,8,54,10,54,12,54,716,9,54,1,55,1,55,1,55,
  	1,55,3,55,722,8,55,1,55,1,55,3,55,726,8,55,3,55,728,8,55,1,55,3,55,731,
  	8,55,1,56,1,56,1,56,1,56,1,56,1,56,1,56,1,56,1,56,1,56,1,56,1,56,1,56,
  	1,56,1,56,3,56,748,8,56,1,57,1,57,3,57,752,8,57,1,57,1,57,1,57,1,57,1,
  	57,4,57,759,8,57,11,57,12,57,760,1,57,1,57,3,57,765,8,57,1,57,1,57,1,
  	58,1,58,1,58,4,58,772,8,58,11,58,12,58,773,1,59,1,59,1,59,3,59,779,8,
  	59,1,59,1,59,1,59,5,59,784,8,59,10,59,12,59,787,9,59,3,59,789,8,59,1,
  	59,1,59,1,60,1,60,1,60,1,60,1,60,5,60,798,8,60,10,60,12,60,801,9,60,3,
  	60,803,8,60,1,60,1,60,1,61,1,61,1,62,1,62,1,63,1,63,1,64,1,64,1,65,1,
  	65,1,66,1,66,1,66,1,67,1,67,1,68,1,68,1,68,1,69,1,69,1,69,5,69,828,8,
  	69,10,69,12,69,831,9,69,1,70,1,70,1,70,1,70,1,70,1,70,1,70,3,70,840,8,
  	70,1,71,1,71,1,72,1,72,1,72,1,72,1,72,3,72,849,8,72,1,73,1,73,1,74,1,
  	74,3,74,855,8,74,1,75,1,75,1,76,1,76,1,76,1,76,1,76,1,76,1,76,1,76,1,
  	76,5,76,868,8,76,10,76,12,76,871,9,76,3,76,873,8,76,1,76,1,76,1,77,1,
  	77,1,77,1,77,5,77,881,8,77,10,77,12,77,884,9,77,3,77,886,8,77,1,77,1,
  	77,1,78,1,78,1,78,3,78,893,8,78,1,79,1,79,1,79,1,79,1,79,1,79,3,79,901,
  	8,79,1,79,1,79,3,79,905,8,79,1,79,1,79,1,79,0,0,80,0,2,4,6,8,10,12,14,
  	16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,58,60,
  	62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,104,
  	106,108,110,112,114,116,118,120,122,124,126,128,130,132,134,136,138,140,
  	142,144,146,148,150,152,154,156,158,0,8,2,0,3,3,8,8,1,0,14,17,2,0,30,
  	30,66,71,1,0,72,76,1,0,72,73,2,0,1,65,95,95,1,0,36,37,1,0,91,93,973,0,
  	160,1,0,0,0,2,168,1,0,0,0,4,173,1,0,0,0,6,175,1,0,0,0,8,189,1,0,0,0,10,
  	229,1,0,0,0,12,233,1,0,0,0,14,235,1,0,0,0,16,343,1,0,0,0,18,348,1,0,0,
  	0,20,355,1,0,0,0,22,357,1,0,0,0,24,364,1,0,0,0,26,372,1,0,0,0,28,377,
  	1,0,0,0,30,383,1,0,0,0,32,386,1,0,0,0,34,396,1,0,0,0,36,421,1,0,0,0,38,
  	424,1,0,0,0,40,435,1,0,0,0,42,448,1,0,0,0,44,450,1,0,0,0,46,453,1,0,0,
  	0,48,465,1,0,0,0,50,480,1,0,0,0,52,485,1,0,0,0,54,506,1,0,0,0,56,508,
  	1,0,0,0,58,513,1,0,0,0,60,523,1,0,0,0,62,526,1,0,0,0,64,529,1,0,0,0,66,
  	533,1,0,0,0,68,535,1,0,0,0,70,546,1,0,0,0,72,561,1,0,0,0,74,563,1,0,0,
  	0,76,566,1,0,0,0,78,594,1,0,0,0,80,596,1,0,0,0,82,613,1,0,0,0,84,616,
  	1,0,0,0,86,620,1,0,0,0,88,623,1,0,0,0,90,635,1,0,0,0,92,645,1,0,0,0,94,
  	647,1,0,0,0,96,655,1,0,0,0,98,663,1,0,0,0,100,672,1,0,0,0,102,676,1,0,
  	0,0,104,691,1,0,0,0,106,699,1,0,0,0,108,708,1,0,0,0,110,730,1,0,0,0,112,
  	747,1,0,0,0,114,749,1,0,0,0,116,768,1,0,0,0,118,775,1,0,0,0,120,792,1,
  	0,0,0,122,806,1,0,0,0,124,808,1,0,0,0,126,810,1,0,0,0,128,812,1,0,0,0,
  	130,814,1,0,0,0,132,816,1,0,0,0,134,819,1,0,0,0,136,821,1,0,0,0,138,829,
  	1,0,0,0,140,839,1,0,0,0,142,841,1,0,0,0,144,848,1,0,0,0,146,850,1,0,0,
  	0,148,854,1,0,0,0,150,856,1,0,0,0,152,858,1,0,0,0,154,876,1,0,0,0,156,
  	889,1,0,0,0,158,894,1,0,0,0,160,162,3,2,1,0,161,163,5,90,0,0,162,161,
  	1,0,0,0,162,163,1,0,0,0,163,164,1,0,0,0,164,165,5,0,0,1,165,1,1,0,0,0,
  	166,169,3,12,6,0,167,169,3,4,2,0,168,166,1,0,0,0,168,167,1,0,0,0,169,
  	3,1,0,0,0,170,174,3,6,3,0,171,174,3,8,4,0,172,174,3,10,5,0,173,170,1,
  	0,0,0,173,171,1,0,0,0,173,172,1,0,0,0,174,5,1,0,0,0,175,176,5,53,0,0,
  	176,177,5,51,0,0,177,7,1,0,0,0,178,179,5,54,0,0,179,180,5,51,0,0,180,
  	190,3,142,71,0,181,182,5,54,0,0,182,183,5,51,0,0,183,184,5,52,0,0,184,
  	185,3,86,43,0,185,186,5,78,0,0,186,187,3,130,65,0,187,188,5,79,0,0,188,
  	190,1,0,0,0,189,178,1,0,0,0,189,181,1,0,0,0,190,9,1,0,0,0,191,192,5,3,
  	0,0,192,194,5,51,0,0,193,195,3,142,71,0,194,193,1,0,0,0,194,195,1,0,0,
  	0,195,196,1,0,0,0,196,197,5,55,0,0,197,198,3,76,38,0,198,199,5,52,0,0,
  	199,200,5,78,0,0,200,201,3,116,58,0,201,202,5,79,0,0,202,230,1,0,0,0,
  	203,204,5,3,0,0,204,206,5,51,0,0,205,207,3,142,71,0,206,205,1,0,0,0,206,
  	207,1,0,0,0,207,208,1,0,0,0,208,209,5,52,0,0,209,210,3,86,43,0,210,211,
  	5,78,0,0,211,212,3,130,65,0,212,213,5,79,0,0,213,230,1,0,0,0,214,215,
  	5,3,0,0,215,216,5,64,0,0,216,218,5,51,0,0,217,219,3,142,71,0,218,217,
  	1,0,0,0,218,219,1,0,0,0,219,220,1,0,0,0,220,221,5,52,0,0,221,222,3,86,
  	43,0,222,223,5,78,0,0,223,224,3,130,65,0,224,227,5,79,0,0,225,226,5,65,
  	0,0,226,228,3,152,76,0,227,225,1,0,0,0,227,228,1,0,0,0,228,230,1,0,0,
  	0,229,191,1,0,0,0,229,203,1,0,0,0,229,214,1,0,0,0,230,11,1,0,0,0,231,
  	234,3,14,7,0,232,234,3,46,23,0,233,231,1,0,0,0,233,232,1,0,0,0,234,13,
  	1,0,0,0,235,243,3,16,8,0,236,238,5,23,0,0,237,239,5,50,0,0,238,237,1,
  	0,0,0,238,239,1,0,0,0,239,240,1,0,0,0,240,242,3,16,8,0,241,236,1,0,0,
  	0,242,245,1,0,0,0,243,241,1,0,0,0,243,244,1,0,0,0,244,15,1,0,0,0,245,
  	243,1,0,0,0,246,248,3,22,11,0,247,246,1,0,0,0,248,249,1,0,0,0,249,247,
  	1,0,0,0,249,250,1,0,0,0,250,254,1,0,0,0,251,253,3,18,9,0,252,251,1,0,
  	0,0,253,256,1,0,0,0,254,252,1,0,0,0,254,255,1,0,0,0,255,257,1,0,0,0,256,
  	254,1,0,0,0,257,258,3,44,22,0,258,344,1,0,0,0,259,261,3,22,11,0,260,259,
  	1,0,0,0,261,262,1,0,0,0,262,260,1,0,0,0,262,263,1,0,0,0,263,267,1,0,0,
  	0,264,266,3,18,9,0,265,264,1,0,0,0,266,269,1,0,0,0,267,265,1,0,0,0,267,
  	268,1,0,0,0,268,271,1,0,0,0,269,267,1,0,0,0,270,272,3,20,10,0,271,270,
  	1,0,0,0,272,273,1,0,0,0,273,271,1,0,0,0,273,274,1,0,0,0,274,276,1,0,0,
  	0,275,277,3,44,22,0,276,275,1,0,0,0,276,277,1,0,0,0,277,344,1,0,0,0,278,
  	280,3,18,9,0,279,278,1,0,0,0,280,283,1,0,0,0,281,279,1,0,0,0,281,282,
  	1,0,0,0,282,285,1,0,0,0,283,281,1,0,0,0,284,286,3,22,11,0,285,284,1,0,
  	0,0,286,287,1,0,0,0,287,285,1,0,0,0,287,288,1,0,0,0,288,292,1,0,0,0,289,
  	291,3,18,9,0,290,289,1,0,0,0,291,294,1,0,0,0,292,290,1,0,0,0,292,293,
  	1,0,0,0,293,295,1,0,0,0,294,292,1,0,0,0,295,296,3,44,22,0,296,344,1,0,
  	0,0,297,299,3,18,9,0,298,297,1,0,0,0,299,302,1,0,0,0,300,298,1,0,0,0,
  	300,301,1,0,0,0,301,304,1,0,0,0,302,300,1,0,0,0,303,305,3,22,11,0,304,
  	303,1,0,0,0,305,306,1,0,0,0,306,304,1,0,0,0,306,307,1,0,0,0,307,311,1,
  	0,0,0,308,310,3,18,9,0,309,308,1,0,0,0,310,313,1,0,0,0,311,309,1,0,0,
  	0,311,312,1,0,0,0,312,315,1,0,0,0,313,311,1,0,0,0,314,316,3,20,10,0,315,
  	314,1,0,0,0,316,317,1,0,0,0,317,315,1,0,0,0,317,318,1,0,0,0,318,320,1,
  	0,0,0,319,321,3,44,22,0,320,319,1,0,0,0,320,321,1,0,0,0,321,344,1,0,0,
  	0,322,324,3,18,9,0,323,322,1,0,0,0,324,327,1,0,0,0,325,323,1,0,0,0,325,
  	326,1,0,0,0,326,328,1,0,0,0,327,325,1,0,0,0,328,344,3,44,22,0,329,331,
  	3,18,9,0,330,329,1,0,0,0,331,334,1,0,0,0,332,330,1,0,0,0,332,333,1,0,
  	0,0,333,336,1,0,0,0,334,332,1,0,0,0,335,337,3,20,10,0,336,335,1,0,0,0,
  	337,338,1,0,0,0,338,336,1,0,0,0,338,339,1,0,0,0,339,341,1,0,0,0,340,342,
  	3,44,22,0,341,340,1,0,0,0,341,342,1,0,0,0,342,344,1,0,0,0,343,247,1,0,
  	0,0,343,260,1,0,0,0,343,281,1,0,0,0,343,300,1,0,0,0,343,325,1,0,0,0,343,
  	332,1,0,0,0,344,17,1,0,0,0,345,349,3,24,12,0,346,349,3,26,13,0,347,349,
  	3,28,14,0,348,345,1,0,0,0,348,346,1,0,0,0,348,347,1,0,0,0,349,19,1,0,
  	0,0,350,356,3,30,15,0,351,356,3,32,16,0,352,356,3,38,19,0,353,356,3,34,
  	17,0,354,356,3,40,20,0,355,350,1,0,0,0,355,351,1,0,0,0,355,352,1,0,0,
  	0,355,353,1,0,0,0,355,354,1,0,0,0,356,21,1,0,0,0,357,358,5,22,0,0,358,
  	361,3,52,26,0,359,360,5,21,0,0,360,362,3,66,33,0,361,359,1,0,0,0,361,
  	362,1,0,0,0,362,23,1,0,0,0,363,365,5,10,0,0,364,363,1,0,0,0,364,365,1,
  	0,0,0,365,366,1,0,0,0,366,367,5,8,0,0,367,370,3,68,34,0,368,369,5,21,
  	0,0,369,371,3,66,33,0,370,368,1,0,0,0,370,371,1,0,0,0,371,25,1,0,0,0,
  	372,373,5,24,0,0,373,374,3,92,46,0,374,375,5,26,0,0,375,376,3,124,62,
  	0,376,27,1,0,0,0,377,378,5,1,0,0,378,381,3,120,60,0,379,380,5,2,0,0,380,
  	382,3,48,24,0,381,379,1,0,0,0,381,382,1,0,0,0,382,29,1,0,0,0,383,384,
  	5,3,0,0,384,385,3,68,34,0,385,31,1,0,0,0,386,387,5,9,0,0,387,393,3,70,
  	35,0,388,389,5,52,0,0,389,390,7,0,0,0,390,392,3,34,17,0,391,388,1,0,0,
  	0,392,395,1,0,0,0,393,391,1,0,0,0,393,394,1,0,0,0,394,33,1,0,0,0,395,
  	393,1,0,0,0,396,397,5,20,0,0,397,402,3,36,18,0,398,399,5,84,0,0,399,401,
  	3,36,18,0,400,398,1,0,0,0,401,404,1,0,0,0,402,400,1,0,0,0,402,403,1,0,
  	0,0,403,35,1,0,0,0,404,402,1,0,0,0,405,406,3,116,58,0,406,407,5,66,0,
  	0,407,408,3,92,46,0,408,422,1,0,0,0,409,410,3,124,62,0,410,411,5,66,0,
  	0,411,412,3,92,46,0,412,422,1,0,0,0,413,414,3,124,62,0,414,415,5,72,0,
  	0,415,416,5,66,0,0,416,417,3,92,46,0,417,422,1,0,0,0,418,419,3,124,62,
  	0,419,420,3,84,42,0,420,422,1,0,0,0,421,405,1,0,0,0,421,409,1,0,0,0,421,
  	413,1,0,0,0,421,418,1,0,0,0,422,37,1,0,0,0,423,425,5,5,0,0,424,423,1,
  	0,0,0,424,425,1,0,0,0,425,426,1,0,0,0,426,427,5,4,0,0,427,432,3,92,46,
  	0,428,429,5,84,0,0,429,431,3,92,46,0,430,428,1,0,0,0,431,434,1,0,0,0,
  	432,430,1,0,0,0,432,433,1,0,0,0,433,39,1,0,0,0,434,432,1,0,0,0,435,436,
  	5,18,0,0,436,441,3,42,21,0,437,438,5,84,0,0,438,440,3,42,21,0,439,437,
  	1,0,0,0,440,443,1,0,0,0,441,439,1,0,0,0,441,442,1,0,0,0,442,41,1,0,0,
  	0,443,441,1,0,0,0,444,445,3,124,62,0,445,446,3,84,42,0,446,449,1,0,0,
  	0,447,449,3,116,58,0,448,444,1,0,0,0,448,447,1,0,0,0,449,43,1,0,0,0,450,
  	451,5,19,0,0,451,452,3,52,26,0,452,45,1,0,0,0,453,456,5,1,0,0,454,457,
  	3,120,60,0,455,457,3,122,61,0,456,454,1,0,0,0,456,455,1,0,0,0,457,463,
  	1,0,0,0,458,461,5,2,0,0,459,462,5,74,0,0,460,462,3,48,24,0,461,459,1,
  	0,0,0,461,460,1,0,0,0,462,464,1,0,0,0,463,458,1,0,0,0,463,464,1,0,0,0,
  	464,47,1,0,0,0,465,470,3,50,25,0,466,467,5,84,0,0,467,469,3,50,25,0,468,
  	466,1,0,0,0,469,472,1,0,0,0,470,468,1,0,0,0,470,471,1,0,0,0,471,475,1,
  	0,0,0,472,470,1,0,0,0,473,474,5,21,0,0,474,476,3,66,33,0,475,473,1,0,
  	0,0,475,476,1,0,0,0,476,49,1,0,0,0,477,478,3,134,67,0,478,479,5,26,0,
  	0,479,481,1,0,0,0,480,477,1,0,0,0,480,481,1,0,0,0,481,482,1,0,0,0,482,
  	483,3,124,62,0,483,51,1,0,0,0,484,486,5,28,0,0,485,484,1,0,0,0,485,486,
  	1,0,0,0,486,487,1,0,0,0,487,489,3,54,27,0,488,490,3,58,29,0,489,488,1,
  	0,0,0,489,490,1,0,0,0,490,492,1,0,0,0,491,493,3,60,30,0,492,491,1,0,0,
  	0,492,493,1,0,0,0,493,495,1,0,0,0,494,496,3,62,31,0,495,494,1,0,0,0,495,
  	496,1,0,0,0,496,53,1,0,0,0,497,507,5,74,0,0,498,503,3,56,28,0,499,500,
  	5,84,0,0,500,502,3,56,28,0,501,499,1,0,0,0,502,505,1,0,0,0,503,501,1,
  	0,0,0,503,504,1,0,0,0,504,507,1,0,0,0,505,503,1,0,0,0,506,497,1,0,0,0,
  	506,498,1,0,0,0,507,55,1,0,0,0,508,511,3,92,46,0,509,510,5,26,0,0,510,
  	512,3,124,62,0,511,509,1,0,0,0,511,512,1,0,0,0,512,57,1,0,0,0,513,514,
  	5,11,0,0,514,515,5,12,0,0,515,520,3,64,32,0,516,517,5,84,0,0,517,519,
  	3,64,32,0,518,516,1,0,0,0,519,522,1,0,0,0,520,518,1,0,0,0,520,521,1,0,
  	0,0,521,59,1,0,0,0,522,520,1,0,0,0,523,524,5,13,0,0,524,525,3,92,46,0,
  	525,61,1,0,0,0,526,527,5,7,0,0,527,528,3,92,46,0,528,63,1,0,0,0,529,531,
  	3,92,46,0,530,532,7,1,0,0,531,530,1,0,0,0,531,532,1,0,0,0,532,65,1,0,
  	0,0,533,534,3,92,46,0,534,67,1,0,0,0,535,540,3,70,35,0,536,537,5,84,0,
  	0,537,539,3,70,35,0,538,536,1,0,0,0,539,542,1,0,0,0,540,538,1,0,0,0,540,
  	541,1,0,0,0,541,69,1,0,0,0,542,540,1,0,0,0,543,544,3,124,62,0,544,545,
  	5,66,0,0,545,547,1,0,0,0,546,543,1,0,0,0,546,547,1,0,0,0,547,548,1,0,
  	0,0,548,549,3,72,36,0,549,71,1,0,0,0,550,554,3,76,38,0,551,553,3,74,37,
  	0,552,551,1,0,0,0,553,556,1,0,0,0,554,552,1,0,0,0,554,555,1,0,0,0,555,
  	562,1,0,0,0,556,554,1,0,0,0,557,558,5,78,0,0,558,559,3,72,36,0,559,560,
  	5,79,0,0,560,562,1,0,0,0,561,550,1,0,0,0,561,557,1,0,0,0,562,73,1,0,0,
  	0,563,564,3,78,39,0,564,565,3,76,38,0,565,75,1,0,0,0,566,568,5,78,0,0,
  	567,569,3,124,62,0,568,567,1,0,0,0,568,569,1,0,0,0,569,571,1,0,0,0,570,
  	572,3,84,42,0,571,570,1,0,0,0,571,572,1,0,0,0,572,574,1,0,0,0,573,575,
  	3,82,41,0,574,573,1,0,0,0,574,575,1,0,0,0,575,576,1,0,0,0,576,577,5,79,
  	0,0,577,77,1,0,0,0,578,580,5,68,0,0,579,578,1,0,0,0,579,580,1,0,0,0,580,
  	581,1,0,0,0,581,583,5,73,0,0,582,584,3,80,40,0,583,582,1,0,0,0,583,584,
  	1,0,0,0,584,585,1,0,0,0,585,587,5,73,0,0,586,588,5,69,0,0,587,586,1,0,
  	0,0,587,588,1,0,0,0,588,595,1,0,0,0,589,591,5,73,0,0,590,592,3,80,40,
  	0,591,590,1,0,0,0,591,592,1,0,0,0,592,593,1,0,0,0,593,595,5,73,0,0,594,
  	579,1,0,0,0,594,589,1,0,0,0,595,79,1,0,0,0,596,598,5,82,0,0,597,599,3,
  	124,62,0,598,597,1,0,0,0,598,599,1,0,0,0,599,601,1,0,0,0,600,602,3,88,
  	44,0,601,600,1,0,0,0,601,602,1,0,0,0,602,604,1,0,0,0,603,605,3,90,45,
  	0,604,603,1,0,0,0,604,605,1,0,0,0,605,607,1,0,0,0,606,608,3,82,41,0,607,
  	606,1,0,0,0,607,608,1,0,0,0,608,609,1,0,0,0,609,610,5,83,0,0,610,81,1,
  	0,0,0,611,614,3,152,76,0,612,614,3,156,78,0,613,611,1,0,0,0,613,612,1,
  	0,0,0,614,83,1,0,0,0,615,617,3,86,43,0,616,615,1,0,0,0,617,618,1,0,0,
  	0,618,616,1,0,0,0,618,619,1,0,0,0,619,85,1,0,0,0,620,621,5,86,0,0,621,
  	622,3,126,63,0,622,87,1,0,0,0,623,624,5,86,0,0,624,632,3,128,64,0,625,
  	627,5,87,0,0,626,628,5,86,0,0,627,626,1,0,0,0,627,628,1,0,0,0,628,629,
  	1,0,0,0,629,631,3,128,64,0,630,625,1,0,0,0,631,634,1,0,0,0,632,630,1,
  	0,0,0,632,633,1,0,0,0,633,89,1,0,0,0,634,632,1,0,0,0,635,637,5,74,0,0,
  	636,638,3,150,75,0,637,636,1,0,0,0,637,638,1,0,0,0,638,643,1,0,0,0,639,
  	641,5,89,0,0,640,642,3,150,75,0,641,640,1,0,0,0,641,642,1,0,0,0,642,644,
  	1,0,0,0,643,639,1,0,0,0,643,644,1,0,0,0,644,91,1,0,0,0,645,646,3,94,47,
  	0,646,93,1,0,0,0,647,652,3,96,48,0,648,649,5,33,0,0,649,651,3,96,48,0,
  	650,648,1,0,0,0,651,654,1,0,0,0,652,650,1,0,0,0,652,653,1,0,0,0,653,95,
  	1,0,0,0,654,652,1,0,0,0,655,660,3,98,49,0,656,657,5,35,0,0,657,659,3,
  	98,49,0,658,656,1,0,0,0,659,662,1,0,0,0,660,658,1,0,0,0,660,661,1,0,0,
  	0,661,97,1,0,0,0,662,660,1,0,0,0,663,668,3,100,50,0,664,665,5,25,0,0,
  	665,667,3,100,50,0,666,664,1,0,0,0,667,670,1,0,0,0,668,666,1,0,0,0,668,
  	669,1,0,0,0,669,99,1,0,0,0,670,668,1,0,0,0,671,673,5,32,0,0,672,671,1,
  	0,0,0,672,673,1,0,0,0,673,674,1,0,0,0,674,675,3,102,51,0,675,101,1,0,
  	0,0,676,681,3,104,52,0,677,678,7,2,0,0,678,680,3,104,52,0,679,677,1,0,
  	0,0,680,683,1,0,0,0,681,679,1,0,0,0,681,682,1,0,0,0,682,689,1,0,0,0,683,
  	681,1,0,0,0,684,686,5,31,0,0,685,687,5,32,0,0,686,685,1,0,0,0,686,687,
  	1,0,0,0,687,688,1,0,0,0,688,690,5,38,0,0,689,684,1,0,0,0,689,690,1,0,
  	0,0,690,103,1,0,0,0,691,696,3,106,53,0,692,693,7,3,0,0,693,695,3,106,
  	53,0,694,692,1,0,0,0,695,698,1,0,0,0,696,694,1,0,0,0,696,697,1,0,0,0,
  	697,105,1,0,0,0,698,696,1,0,0,0,699,704,3,108,54,0,700,701,5,77,0,0,701,
  	703,3,108,54,0,702,700,1,0,0,0,703,706,1,0,0,0,704,702,1,0,0,0,704,705,
  	1,0,0,0,705,107,1,0,0,0,706,704,1,0,0,0,707,709,7,4,0,0,708,707,1,0,0,
  	0,708,709,1,0,0,0,709,710,1,0,0,0,710,714,3,112,56,0,711,713,3,110,55,
  	0,712,711,1,0,0,0,713,716,1,0,0,0,714,712,1,0,0,0,714,715,1,0,0,0,715,
  	109,1,0,0,0,716,714,1,0,0,0,717,718,5,85,0,0,718,731,3,130,65,0,719,721,
  	5,82,0,0,720,722,3,92,46,0,721,720,1,0,0,0,721,722,1,0,0,0,722,727,1,
  	0,0,0,723,725,5,89,0,0,724,726,3,92,46,0,725,724,1,0,0,0,725,726,1,0,
  	0,0,726,728,1,0,0,0,727,723,1,0,0,0,727,728,1,0,0,0,728,729,1,0,0,0,729,
  	731,5,83,0,0,730,717,1,0,0,0,730,719,1,0,0,0,731,111,1,0,0,0,732,748,
  	3,144,72,0,733,748,3,156,78,0,734,735,5,44,0,0,735,736,5,78,0,0,736,737,
  	5,74,0,0,737,748,5,79,0,0,738,748,3,118,59,0,739,748,3,114,57,0,740,748,
  	3,124,62,0,741,742,5,78,0,0,742,743,3,92,46,0,743,744,5,79,0,0,744,748,
  	1,0,0,0,745,748,3,154,77,0,746,748,3,158,79,0,747,732,1,0,0,0,747,733,
  	1,0,0,0,747,734,1,0,0,0,747,738,1,0,0,0,747,739,1,0,0,0,747,740,1,0,0,
  	0,747,741,1,0,0,0,747,745,1,0,0,0,747,746,1,0,0,0,748,113,1,0,0,0,749,
  	751,5,39,0,0,750,752,3,92,46,0,751,750,1,0,0,0,751,752,1,0,0,0,752,758,
  	1,0,0,0,753,754,5,40,0,0,754,755,3,92,46,0,755,756,5,41,0,0,756,757,3,
  	92,46,0,757,759,1,0,0,0,758,753,1,0,0,0,759,760,1,0,0,0,760,758,1,0,0,
  	0,760,761,1,0,0,0,761,764,1,0,0,0,762,763,5,42,0,0,763,765,3,92,46,0,
  	764,762,1,0,0,0,764,765,1,0,0,0,765,766,1,0,0,0,766,767,5,43,0,0,767,
  	115,1,0,0,0,768,771,3,112,56,0,769,770,5,85,0,0,770,772,3,130,65,0,771,
  	769,1,0,0,0,772,773,1,0,0,0,773,771,1,0,0,0,773,774,1,0,0,0,774,117,1,
  	0,0,0,775,776,3,136,68,0,776,778,5,78,0,0,777,779,5,28,0,0,778,777,1,
  	0,0,0,778,779,1,0,0,0,779,788,1,0,0,0,780,785,3,92,46,0,781,782,5,84,
  	0,0,782,784,3,92,46,0,783,781,1,0,0,0,784,787,1,0,0,0,785,783,1,0,0,0,
  	785,786,1,0,0,0,786,789,1,0,0,0,787,785,1,0,0,0,788,780,1,0,0,0,788,789,
  	1,0,0,0,789,790,1,0,0,0,790,791,5,79,0,0,791,119,1,0,0,0,792,793,3,132,
  	66,0,793,802,5,78,0,0,794,799,3,92,46,0,795,796,5,84,0,0,796,798,3,92,
  	46,0,797,795,1,0,0,0,798,801,1,0,0,0,799,797,1,0,0,0,799,800,1,0,0,0,
  	800,803,1,0,0,0,801,799,1,0,0,0,802,794,1,0,0,0,802,803,1,0,0,0,803,804,
  	1,0,0,0,804,805,5,79,0,0,805,121,1,0,0,0,806,807,3,132,66,0,807,123,1,
  	0,0,0,808,809,3,142,71,0,809,125,1,0,0,0,810,811,3,140,70,0,811,127,1,
  	0,0,0,812,813,3,140,70,0,813,129,1,0,0,0,814,815,3,140,70,0,815,131,1,
  	0,0,0,816,817,3,138,69,0,817,818,3,142,71,0,818,133,1,0,0,0,819,820,3,
  	142,71,0,820,135,1,0,0,0,821,822,3,138,69,0,822,823,3,142,71,0,823,137,
  	1,0,0,0,824,825,3,142,71,0,825,826,5,85,0,0,826,828,1,0,0,0,827,824,1,
  	0,0,0,828,831,1,0,0,0,829,827,1,0,0,0,829,830,1,0,0,0,830,139,1,0,0,0,
  	831,829,1,0,0,0,832,840,3,142,71,0,833,840,5,51,0,0,834,840,5,52,0,0,
  	835,840,5,8,0,0,836,840,5,3,0,0,837,840,5,21,0,0,838,840,5,19,0,0,839,
  	832,1,0,0,0,839,833,1,0,0,0,839,834,1,0,0,0,839,835,1,0,0,0,839,836,1,
  	0,0,0,839,837,1,0,0,0,839,838,1,0,0,0,840,141,1,0,0,0,841,842,7,5,0,0,
  	842,143,1,0,0,0,843,849,3,146,73,0,844,849,5,38,0,0,845,849,3,148,74,
  	0,846,849,5,96,0,0,847,849,3,152,76,0,848,843,1,0,0,0,848,844,1,0,0,0,
  	848,845,1,0,0,0,848,846,1,0,0,0,848,847,1,0,0,0,849,145,1,0,0,0,850,851,
  	7,6,0,0,851,147,1,0,0,0,852,855,5,94,0,0,853,855,3,150,75,0,854,852,1,
  	0,0,0,854,853,1,0,0,0,855,149,1,0,0,0,856,857,7,7,0,0,857,151,1,0,0,0,
  	858,872,5,80,0,0,859,860,3,130,65,0,860,861,5,86,0,0,861,869,3,92,46,
  	0,862,863,5,84,0,0,863,864,3,130,65,0,864,865,5,86,0,0,865,866,3,92,46,
  	0,866,868,1,0,0,0,867,862,1,0,0,0,868,871,1,0,0,0,869,867,1,0,0,0,869,
  	870,1,0,0,0,870,873,1,0,0,0,871,869,1,0,0,0,872,859,1,0,0,0,872,873,1,
  	0,0,0,873,874,1,0,0,0,874,875,5,81,0,0,875,153,1,0,0,0,876,885,5,82,0,
  	0,877,882,3,92,46,0,878,879,5,84,0,0,879,881,3,92,46,0,880,878,1,0,0,
  	0,881,884,1,0,0,0,882,880,1,0,0,0,882,883,1,0,0,0,883,886,1,0,0,0,884,
  	882,1,0,0,0,885,877,1,0,0,0,885,886,1,0,0,0,886,887,1,0,0,0,887,888,5,
  	83,0,0,888,155,1,0,0,0,889,892,5,88,0,0,890,893,3,142,71,0,891,893,5,
  	93,0,0,892,890,1,0,0,0,892,891,1,0,0,0,893,157,1,0,0,0,894,895,5,82,0,
  	0,895,896,3,124,62,0,896,897,5,30,0,0,897,900,3,92,46,0,898,899,5,21,
  	0,0,899,901,3,92,46,0,900,898,1,0,0,0,900,901,1,0,0,0,901,904,1,0,0,0,
  	902,903,5,87,0,0,903,905,3,92,46,0,904,902,1,0,0,0,904,905,1,0,0,0,905,
  	906,1,0,0,0,906,907,5,83,0,0,907,159,1,0,0,0,118,162,168,173,189,194,
  	206,218,227,229,233,238,243,249,254,262,267,273,276,281,287,292,300,306,
  	311,317,320,325,332,338,341,343,348,355,361,364,370,381,393,402,421,424,
  	432,441,448,456,461,463,470,475,480,485,489,492,495,503,506,511,520,531,
  	540,546,554,561,568,571,574,579,583,587,591,594,598,601,604,607,613,618,
  	627,632,637,641,643,652,660,668,672,681,686,689,696,704,708,714,721,725,
  	727,730,747,751,760,764,773,778,785,788,799,802,829,839,848,854,869,872,
  	882,885,892,900,904
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
    setState(160);
    statement();
    setState(162);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::SEMI) {
      setState(161);
      match(CypherParser::SEMI);
    }
    setState(164);
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
    setState(168);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 1, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(166);
      query();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(167);
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
    setState(173);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_SHOW: {
        enterOuterAlt(_localctx, 1);
        setState(170);
        showIndexesStatement();
        break;
      }

      case CypherParser::K_DROP: {
        enterOuterAlt(_localctx, 2);
        setState(171);
        dropIndexStatement();
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 3);
        setState(172);
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
    setState(175);
    match(CypherParser::K_SHOW);
    setState(176);
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
    setState(189);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 3, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::DropIndexByNameContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(178);
      match(CypherParser::K_DROP);
      setState(179);
      match(CypherParser::K_INDEX);
      setState(180);
      symbolicName();
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::DropIndexByLabelContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(181);
      match(CypherParser::K_DROP);
      setState(182);
      match(CypherParser::K_INDEX);
      setState(183);
      match(CypherParser::K_ON);
      setState(184);
      nodeLabel();
      setState(185);
      match(CypherParser::LPAREN);
      setState(186);
      propertyKeyName();
      setState(187);
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
    setState(229);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 8, _ctx)) {
    case 1: {
      _localctx = _tracker.createInstance<CypherParser::CreateIndexByPatternContext>(_localctx);
      enterOuterAlt(_localctx, 1);
      setState(191);
      match(CypherParser::K_CREATE);
      setState(192);
      match(CypherParser::K_INDEX);
      setState(194);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 4, _ctx)) {
      case 1: {
        setState(193);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(196);
      match(CypherParser::K_FOR);
      setState(197);
      nodePattern();
      setState(198);
      match(CypherParser::K_ON);
      setState(199);
      match(CypherParser::LPAREN);
      setState(200);
      propertyExpression();
      setState(201);
      match(CypherParser::RPAREN);
      break;
    }

    case 2: {
      _localctx = _tracker.createInstance<CypherParser::CreateIndexByLabelContext>(_localctx);
      enterOuterAlt(_localctx, 2);
      setState(203);
      match(CypherParser::K_CREATE);
      setState(204);
      match(CypherParser::K_INDEX);
      setState(206);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 5, _ctx)) {
      case 1: {
        setState(205);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(208);
      match(CypherParser::K_ON);
      setState(209);
      nodeLabel();
      setState(210);
      match(CypherParser::LPAREN);
      setState(211);
      propertyKeyName();
      setState(212);
      match(CypherParser::RPAREN);
      break;
    }

    case 3: {
      _localctx = _tracker.createInstance<CypherParser::CreateVectorIndexContext>(_localctx);
      enterOuterAlt(_localctx, 3);
      setState(214);
      match(CypherParser::K_CREATE);
      setState(215);
      match(CypherParser::K_VECTOR);
      setState(216);
      match(CypherParser::K_INDEX);
      setState(218);
      _errHandler->sync(this);

      switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 6, _ctx)) {
      case 1: {
        setState(217);
        symbolicName();
        break;
      }

      default:
        break;
      }
      setState(220);
      match(CypherParser::K_ON);
      setState(221);
      nodeLabel();
      setState(222);
      match(CypherParser::LPAREN);
      setState(223);
      propertyKeyName();
      setState(224);
      match(CypherParser::RPAREN);
      setState(227);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_OPTIONS) {
        setState(225);
        match(CypherParser::K_OPTIONS);
        setState(226);
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
    setState(233);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 9, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(231);
      regularQuery();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(232);
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
    setState(235);
    singleQuery();
    setState(243);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_UNION) {
      setState(236);
      match(CypherParser::K_UNION);
      setState(238);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_ALL) {
        setState(237);
        match(CypherParser::K_ALL);
      }
      setState(240);
      singleQuery();
      setState(245);
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
    setState(343);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 30, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(247); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(246);
        withClause();
        setState(249); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(254);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(251);
        readingClause();
        setState(256);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(257);
      returnStatement();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(260); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(259);
        withClause();
        setState(262); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(267);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(264);
        readingClause();
        setState(269);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(271); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(270);
        updatingClause();
        setState(273); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0));
      setState(276);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(275);
        returnStatement();
      }
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(281);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(278);
        readingClause();
        setState(283);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(285); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(284);
        withClause();
        setState(287); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(292);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(289);
        readingClause();
        setState(294);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(295);
      returnStatement();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(300);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(297);
        readingClause();
        setState(302);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(304); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(303);
        withClause();
        setState(306); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while (_la == CypherParser::K_WITH);
      setState(311);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(308);
        readingClause();
        setState(313);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(315); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(314);
        updatingClause();
        setState(317); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0));
      setState(320);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(319);
        returnStatement();
      }
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(325);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(322);
        readingClause();
        setState(327);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(328);
      returnStatement();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(332);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(329);
        readingClause();
        setState(334);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(336); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(335);
        updatingClause();
        setState(338); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0));
      setState(341);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(340);
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
    setState(348);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_MATCH:
      case CypherParser::K_OPTIONAL: {
        enterOuterAlt(_localctx, 1);
        setState(345);
        matchStatement();
        break;
      }

      case CypherParser::K_UNWIND: {
        enterOuterAlt(_localctx, 2);
        setState(346);
        unwindStatement();
        break;
      }

      case CypherParser::K_CALL: {
        enterOuterAlt(_localctx, 3);
        setState(347);
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
    setState(355);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 1);
        setState(350);
        createStatement();
        break;
      }

      case CypherParser::K_MERGE: {
        enterOuterAlt(_localctx, 2);
        setState(351);
        mergeStatement();
        break;
      }

      case CypherParser::K_DELETE:
      case CypherParser::K_DETACH: {
        enterOuterAlt(_localctx, 3);
        setState(352);
        deleteStatement();
        break;
      }

      case CypherParser::K_SET: {
        enterOuterAlt(_localctx, 4);
        setState(353);
        setStatement();
        break;
      }

      case CypherParser::K_REMOVE: {
        enterOuterAlt(_localctx, 5);
        setState(354);
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
    setState(357);
    match(CypherParser::K_WITH);
    setState(358);
    projectionBody();
    setState(361);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(359);
      match(CypherParser::K_WHERE);
      setState(360);
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
    setState(364);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_OPTIONAL) {
      setState(363);
      match(CypherParser::K_OPTIONAL);
    }
    setState(366);
    match(CypherParser::K_MATCH);
    setState(367);
    pattern();
    setState(370);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(368);
      match(CypherParser::K_WHERE);
      setState(369);
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
    setState(372);
    match(CypherParser::K_UNWIND);
    setState(373);
    expression();
    setState(374);
    match(CypherParser::K_AS);
    setState(375);
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
    setState(377);
    match(CypherParser::K_CALL);
    setState(378);
    explicitProcedureInvocation();
    setState(381);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(379);
      match(CypherParser::K_YIELD);
      setState(380);
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
    setState(383);
    match(CypherParser::K_CREATE);
    setState(384);
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
    setState(386);
    match(CypherParser::K_MERGE);
    setState(387);
    patternPart();
    setState(393);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_ON) {
      setState(388);
      match(CypherParser::K_ON);
      setState(389);
      _la = _input->LA(1);
      if (!(_la == CypherParser::K_CREATE

      || _la == CypherParser::K_MATCH)) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(390);
      setStatement();
      setState(395);
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
    setState(396);
    match(CypherParser::K_SET);
    setState(397);
    setItem();
    setState(402);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(398);
      match(CypherParser::COMMA);
      setState(399);
      setItem();
      setState(404);
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
    setState(421);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 39, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(405);
      propertyExpression();
      setState(406);
      match(CypherParser::EQ);
      setState(407);
      expression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(409);
      variable();
      setState(410);
      match(CypherParser::EQ);
      setState(411);
      expression();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(413);
      variable();
      setState(414);
      match(CypherParser::PLUS);
      setState(415);
      match(CypherParser::EQ);
      setState(416);
      expression();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(418);
      variable();
      setState(419);
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
    setState(424);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_DETACH) {
      setState(423);
      match(CypherParser::K_DETACH);
    }
    setState(426);
    match(CypherParser::K_DELETE);
    setState(427);
    expression();
    setState(432);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(428);
      match(CypherParser::COMMA);
      setState(429);
      expression();
      setState(434);
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
    setState(435);
    match(CypherParser::K_REMOVE);
    setState(436);
    removeItem();
    setState(441);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(437);
      match(CypherParser::COMMA);
      setState(438);
      removeItem();
      setState(443);
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
    setState(448);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 43, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(444);
      variable();
      setState(445);
      nodeLabels();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(447);
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
    setState(450);
    match(CypherParser::K_RETURN);
    setState(451);
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
    setState(453);
    match(CypherParser::K_CALL);
    setState(456);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 44, _ctx)) {
    case 1: {
      setState(454);
      explicitProcedureInvocation();
      break;
    }

    case 2: {
      setState(455);
      implicitProcedureInvocation();
      break;
    }

    default:
      break;
    }
    setState(463);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(458);
      match(CypherParser::K_YIELD);
      setState(461);
      _errHandler->sync(this);
      switch (_input->LA(1)) {
        case CypherParser::MULTIPLY: {
          setState(459);
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
          setState(460);
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
    setState(465);
    yieldItem();
    setState(470);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(466);
      match(CypherParser::COMMA);
      setState(467);
      yieldItem();
      setState(472);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
    setState(475);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(473);
      match(CypherParser::K_WHERE);
      setState(474);
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
    setState(480);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 49, _ctx)) {
    case 1: {
      setState(477);
      procedureResultField();
      setState(478);
      match(CypherParser::K_AS);
      break;
    }

    default:
      break;
    }
    setState(482);
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
    setState(485);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 50, _ctx)) {
    case 1: {
      setState(484);
      match(CypherParser::K_DISTINCT);
      break;
    }

    default:
      break;
    }
    setState(487);
    projectionItems();
    setState(489);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_ORDER) {
      setState(488);
      orderStatement();
    }
    setState(492);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_SKIP) {
      setState(491);
      skipStatement();
    }
    setState(495);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_LIMIT) {
      setState(494);
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
    setState(506);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::MULTIPLY: {
        enterOuterAlt(_localctx, 1);
        setState(497);
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
        setState(498);
        projectionItem();
        setState(503);
        _errHandler->sync(this);
        _la = _input->LA(1);
        while (_la == CypherParser::COMMA) {
          setState(499);
          match(CypherParser::COMMA);
          setState(500);
          projectionItem();
          setState(505);
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
    setState(508);
    expression();
    setState(511);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_AS) {
      setState(509);
      match(CypherParser::K_AS);
      setState(510);
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
    setState(513);
    match(CypherParser::K_ORDER);
    setState(514);
    match(CypherParser::K_BY);
    setState(515);
    sortItem();
    setState(520);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(516);
      match(CypherParser::COMMA);
      setState(517);
      sortItem();
      setState(522);
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
    setState(523);
    match(CypherParser::K_SKIP);
    setState(524);
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
    setState(526);
    match(CypherParser::K_LIMIT);
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
    setState(529);
    expression();
    setState(531);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 245760) != 0)) {
      setState(530);
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
    setState(533);
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
    setState(535);
    patternPart();
    setState(540);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(536);
      match(CypherParser::COMMA);
      setState(537);
      patternPart();
      setState(542);
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
    setState(546);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(543);
      variable();
      setState(544);
      match(CypherParser::EQ);
    }
    setState(548);
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
    setState(561);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 62, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(550);
      nodePattern();
      setState(554);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::LT

      || _la == CypherParser::MINUS) {
        setState(551);
        patternElementChain();
        setState(556);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(557);
      match(CypherParser::LPAREN);
      setState(558);
      patternElement();
      setState(559);
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
    setState(563);
    relationshipPattern();
    setState(564);
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
    setState(566);
    match(CypherParser::LPAREN);
    setState(568);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(567);
      variable();
    }
    setState(571);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(570);
      nodeLabels();
    }
    setState(574);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(573);
      properties();
    }
    setState(576);
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
    setState(594);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 70, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(579);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LT) {
        setState(578);
        match(CypherParser::LT);
      }
      setState(581);
      match(CypherParser::MINUS);
      setState(583);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(582);
        relationshipDetail();
      }
      setState(585);
      match(CypherParser::MINUS);
      setState(587);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::GT) {
        setState(586);
        match(CypherParser::GT);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(589);
      match(CypherParser::MINUS);
      setState(591);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(590);
        relationshipDetail();
      }
      setState(593);
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
    setState(596);
    match(CypherParser::LBRACK);
    setState(598);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(597);
      variable();
    }
    setState(601);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(600);
      relationshipTypes();
    }
    setState(604);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::MULTIPLY) {
      setState(603);
      rangeLiteral();
    }
    setState(607);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(606);
      properties();
    }
    setState(609);
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
    setState(613);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 1);
        setState(611);
        mapLiteral();
        break;
      }

      case CypherParser::DOLLAR: {
        enterOuterAlt(_localctx, 2);
        setState(612);
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
    setState(616); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(615);
      nodeLabel();
      setState(618); 
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
    setState(620);
    match(CypherParser::COLON);
    setState(621);
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
    setState(623);
    match(CypherParser::COLON);
    setState(624);
    relTypeName();
    setState(632);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::PIPE) {
      setState(625);
      match(CypherParser::PIPE);
      setState(627);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::COLON) {
        setState(626);
        match(CypherParser::COLON);
      }
      setState(629);
      relTypeName();
      setState(634);
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
    setState(635);
    match(CypherParser::MULTIPLY);
    setState(637);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 91) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 91)) & 7) != 0)) {
      setState(636);
      integerLiteral();
    }
    setState(643);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::RANGE) {
      setState(639);
      match(CypherParser::RANGE);
      setState(641);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (((((_la - 91) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 91)) & 7) != 0)) {
        setState(640);
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
    setState(645);
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
    setState(647);
    xorExpression();
    setState(652);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_OR) {
      setState(648);
      match(CypherParser::K_OR);
      setState(649);
      xorExpression();
      setState(654);
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
    setState(655);
    andExpression();
    setState(660);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_XOR) {
      setState(656);
      match(CypherParser::K_XOR);
      setState(657);
      andExpression();
      setState(662);
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
    setState(663);
    notExpression();
    setState(668);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_AND) {
      setState(664);
      match(CypherParser::K_AND);
      setState(665);
      notExpression();
      setState(670);
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
    setState(672);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 85, _ctx)) {
    case 1: {
      setState(671);
      match(CypherParser::K_NOT);
      break;
    }

    default:
      break;
    }
    setState(674);
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

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_IS() {
  return getToken(CypherParser::K_IS, 0);
}

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_NULL() {
  return getToken(CypherParser::K_NULL, 0);
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

tree::TerminalNode* CypherParser::ComparisonExpressionContext::K_NOT() {
  return getToken(CypherParser::K_NOT, 0);
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
    setState(676);
    arithmeticExpression();
    setState(681);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (((((_la - 30) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 30)) & 4329327034369) != 0)) {
      setState(677);
      _la = _input->LA(1);
      if (!(((((_la - 30) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 30)) & 4329327034369) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(678);
      arithmeticExpression();
      setState(683);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
    setState(689);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_IS) {
      setState(684);
      match(CypherParser::K_IS);
      setState(686);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_NOT) {
        setState(685);
        match(CypherParser::K_NOT);
      }
      setState(688);
      match(CypherParser::K_NULL);
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

std::vector<CypherParser::PowerExpressionContext *> CypherParser::ArithmeticExpressionContext::powerExpression() {
  return getRuleContexts<CypherParser::PowerExpressionContext>();
}

CypherParser::PowerExpressionContext* CypherParser::ArithmeticExpressionContext::powerExpression(size_t i) {
  return getRuleContext<CypherParser::PowerExpressionContext>(i);
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
    setState(691);
    powerExpression();
    setState(696);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (((((_la - 72) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 72)) & 31) != 0)) {
      setState(692);
      _la = _input->LA(1);
      if (!(((((_la - 72) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 72)) & 31) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(693);
      powerExpression();
      setState(698);
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

//----------------- PowerExpressionContext ------------------------------------------------------------------

CypherParser::PowerExpressionContext::PowerExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

std::vector<CypherParser::UnaryExpressionContext *> CypherParser::PowerExpressionContext::unaryExpression() {
  return getRuleContexts<CypherParser::UnaryExpressionContext>();
}

CypherParser::UnaryExpressionContext* CypherParser::PowerExpressionContext::unaryExpression(size_t i) {
  return getRuleContext<CypherParser::UnaryExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::PowerExpressionContext::POWER() {
  return getTokens(CypherParser::POWER);
}

tree::TerminalNode* CypherParser::PowerExpressionContext::POWER(size_t i) {
  return getToken(CypherParser::POWER, i);
}


size_t CypherParser::PowerExpressionContext::getRuleIndex() const {
  return CypherParser::RulePowerExpression;
}


std::any CypherParser::PowerExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitPowerExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::PowerExpressionContext* CypherParser::powerExpression() {
  PowerExpressionContext *_localctx = _tracker.createInstance<PowerExpressionContext>(_ctx, getState());
  enterRule(_localctx, 106, CypherParser::RulePowerExpression);
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
    setState(699);
    unaryExpression();
    setState(704);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::POWER) {
      setState(700);
      match(CypherParser::POWER);
      setState(701);
      unaryExpression();
      setState(706);
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
  enterRule(_localctx, 108, CypherParser::RuleUnaryExpression);
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
    setState(708);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::PLUS

    || _la == CypherParser::MINUS) {
      setState(707);
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
    setState(710);
    atom();
    setState(714);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::LBRACK

    || _la == CypherParser::DOT) {
      setState(711);
      accessor();
      setState(716);
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

tree::TerminalNode* CypherParser::AccessorContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::AccessorContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::AccessorContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::AccessorContext::RANGE() {
  return getToken(CypherParser::RANGE, 0);
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
  enterRule(_localctx, 110, CypherParser::RuleAccessor);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(730);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::DOT: {
        enterOuterAlt(_localctx, 1);
        setState(717);
        match(CypherParser::DOT);
        setState(718);
        propertyKeyName();
        break;
      }

      case CypherParser::LBRACK: {
        enterOuterAlt(_localctx, 2);
        setState(719);
        match(CypherParser::LBRACK);
        setState(721);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if ((((_la & ~ 0x3fULL) == 0) &&
          ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
          ((1ULL << (_la - 64)) & 8472838915) != 0)) {
          setState(720);
          expression();
        }
        setState(727);
        _errHandler->sync(this);

        _la = _input->LA(1);
        if (_la == CypherParser::RANGE) {
          setState(723);
          match(CypherParser::RANGE);
          setState(725);
          _errHandler->sync(this);

          _la = _input->LA(1);
          if ((((_la & ~ 0x3fULL) == 0) &&
            ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
            ((1ULL << (_la - 64)) & 8472838915) != 0)) {
            setState(724);
            expression();
          }
        }
        setState(729);
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

CypherParser::CaseExpressionContext* CypherParser::AtomContext::caseExpression() {
  return getRuleContext<CypherParser::CaseExpressionContext>(0);
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

CypherParser::ListComprehensionContext* CypherParser::AtomContext::listComprehension() {
  return getRuleContext<CypherParser::ListComprehensionContext>(0);
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
  enterRule(_localctx, 112, CypherParser::RuleAtom);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(747);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 97, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(732);
      literal();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(733);
      parameter();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(734);
      match(CypherParser::K_COUNT);
      setState(735);
      match(CypherParser::LPAREN);
      setState(736);
      match(CypherParser::MULTIPLY);
      setState(737);
      match(CypherParser::RPAREN);
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(738);
      functionInvocation();
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(739);
      caseExpression();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(740);
      variable();
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(741);
      match(CypherParser::LPAREN);
      setState(742);
      expression();
      setState(743);
      match(CypherParser::RPAREN);
      break;
    }

    case 8: {
      enterOuterAlt(_localctx, 8);
      setState(745);
      listLiteral();
      break;
    }

    case 9: {
      enterOuterAlt(_localctx, 9);
      setState(746);
      listComprehension();
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

//----------------- CaseExpressionContext ------------------------------------------------------------------

CypherParser::CaseExpressionContext::CaseExpressionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_CASE() {
  return getToken(CypherParser::K_CASE, 0);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_END() {
  return getToken(CypherParser::K_END, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::CaseExpressionContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::CaseExpressionContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

std::vector<tree::TerminalNode *> CypherParser::CaseExpressionContext::K_WHEN() {
  return getTokens(CypherParser::K_WHEN);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_WHEN(size_t i) {
  return getToken(CypherParser::K_WHEN, i);
}

std::vector<tree::TerminalNode *> CypherParser::CaseExpressionContext::K_THEN() {
  return getTokens(CypherParser::K_THEN);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_THEN(size_t i) {
  return getToken(CypherParser::K_THEN, i);
}

tree::TerminalNode* CypherParser::CaseExpressionContext::K_ELSE() {
  return getToken(CypherParser::K_ELSE, 0);
}


size_t CypherParser::CaseExpressionContext::getRuleIndex() const {
  return CypherParser::RuleCaseExpression;
}


std::any CypherParser::CaseExpressionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCaseExpression(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CaseExpressionContext* CypherParser::caseExpression() {
  CaseExpressionContext *_localctx = _tracker.createInstance<CaseExpressionContext>(_ctx, getState());
  enterRule(_localctx, 114, CypherParser::RuleCaseExpression);
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
    setState(749);
    match(CypherParser::K_CASE);
    setState(751);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 98, _ctx)) {
    case 1: {
      setState(750);
      expression();
      break;
    }

    default:
      break;
    }
    setState(758); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(753);
      match(CypherParser::K_WHEN);
      setState(754);
      expression();
      setState(755);
      match(CypherParser::K_THEN);
      setState(756);
      expression();
      setState(760); 
      _errHandler->sync(this);
      _la = _input->LA(1);
    } while (_la == CypherParser::K_WHEN);
    setState(764);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_ELSE) {
      setState(762);
      match(CypherParser::K_ELSE);
      setState(763);
      expression();
    }
    setState(766);
    match(CypherParser::K_END);
   
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
  enterRule(_localctx, 116, CypherParser::RulePropertyExpression);
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
    setState(768);
    atom();
    setState(771); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(769);
      match(CypherParser::DOT);
      setState(770);
      propertyKeyName();
      setState(773); 
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
  enterRule(_localctx, 118, CypherParser::RuleFunctionInvocation);
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
    setState(775);
    functionName();
    setState(776);
    match(CypherParser::LPAREN);
    setState(778);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 102, _ctx)) {
    case 1: {
      setState(777);
      match(CypherParser::K_DISTINCT);
      break;
    }

    default:
      break;
    }
    setState(788);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 8472838915) != 0)) {
      setState(780);
      expression();
      setState(785);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(781);
        match(CypherParser::COMMA);
        setState(782);
        expression();
        setState(787);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(790);
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
  enterRule(_localctx, 120, CypherParser::RuleExplicitProcedureInvocation);
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
    procedureName();
    setState(793);
    match(CypherParser::LPAREN);
    setState(802);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 8472838915) != 0)) {
      setState(794);
      expression();
      setState(799);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(795);
        match(CypherParser::COMMA);
        setState(796);
        expression();
        setState(801);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(804);
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
  enterRule(_localctx, 122, CypherParser::RuleImplicitProcedureInvocation);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(806);
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
  enterRule(_localctx, 124, CypherParser::RuleVariable);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(808);
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
  enterRule(_localctx, 126, CypherParser::RuleLabelName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(810);
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
  enterRule(_localctx, 128, CypherParser::RuleRelTypeName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(812);
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
  enterRule(_localctx, 130, CypherParser::RulePropertyKeyName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(814);
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
  enterRule(_localctx, 132, CypherParser::RuleProcedureName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(816);
    namespace_();
    setState(817);
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
  enterRule(_localctx, 134, CypherParser::RuleProcedureResultField);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(819);
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
  enterRule(_localctx, 136, CypherParser::RuleFunctionName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(821);
    namespace_();
    setState(822);
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
  enterRule(_localctx, 138, CypherParser::RuleNamespace);

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
    setState(829);
    _errHandler->sync(this);
    alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 107, _ctx);
    while (alt != 2 && alt != atn::ATN::INVALID_ALT_NUMBER) {
      if (alt == 1) {
        setState(824);
        symbolicName();
        setState(825);
        match(CypherParser::DOT); 
      }
      setState(831);
      _errHandler->sync(this);
      alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 107, _ctx);
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
  enterRule(_localctx, 140, CypherParser::RuleSchemaName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(839);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 108, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(832);
      symbolicName();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(833);
      match(CypherParser::K_INDEX);
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(834);
      match(CypherParser::K_ON);
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(835);
      match(CypherParser::K_MATCH);
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(836);
      match(CypherParser::K_CREATE);
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(837);
      match(CypherParser::K_WHERE);
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(838);
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
  enterRule(_localctx, 142, CypherParser::RuleSymbolicName);
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
    setState(841);
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
  enterRule(_localctx, 144, CypherParser::RuleLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(848);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_FALSE:
      case CypherParser::K_TRUE: {
        enterOuterAlt(_localctx, 1);
        setState(843);
        booleanLiteral();
        break;
      }

      case CypherParser::K_NULL: {
        enterOuterAlt(_localctx, 2);
        setState(844);
        match(CypherParser::K_NULL);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger:
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 3);
        setState(845);
        numberLiteral();
        break;
      }

      case CypherParser::StringLiteral: {
        enterOuterAlt(_localctx, 4);
        setState(846);
        match(CypherParser::StringLiteral);
        break;
      }

      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 5);
        setState(847);
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
  enterRule(_localctx, 146, CypherParser::RuleBooleanLiteral);
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
    setState(850);
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
  enterRule(_localctx, 148, CypherParser::RuleNumberLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(854);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 1);
        setState(852);
        match(CypherParser::DoubleLiteral);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger: {
        enterOuterAlt(_localctx, 2);
        setState(853);
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
  enterRule(_localctx, 150, CypherParser::RuleIntegerLiteral);
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
    setState(856);
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
  enterRule(_localctx, 152, CypherParser::RuleMapLiteral);
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
    setState(858);
    match(CypherParser::LBRACE);
    setState(872);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 2147483651) != 0)) {
      setState(859);
      propertyKeyName();
      setState(860);
      match(CypherParser::COLON);
      setState(861);
      expression();
      setState(869);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(862);
        match(CypherParser::COMMA);
        setState(863);
        propertyKeyName();
        setState(864);
        match(CypherParser::COLON);
        setState(865);
        expression();
        setState(871);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(874);
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
  enterRule(_localctx, 154, CypherParser::RuleListLiteral);
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
    setState(876);
    match(CypherParser::LBRACK);
    setState(885);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & -2) != 0) || ((((_la - 64) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 64)) & 8472838915) != 0)) {
      setState(877);
      expression();
      setState(882);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(878);
        match(CypherParser::COMMA);
        setState(879);
        expression();
        setState(884);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(887);
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
  enterRule(_localctx, 156, CypherParser::RuleParameter);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(889);
    match(CypherParser::DOLLAR);
    setState(892);
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
        setState(890);
        symbolicName();
        break;
      }

      case CypherParser::DecimalInteger: {
        setState(891);
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

//----------------- ListComprehensionContext ------------------------------------------------------------------

CypherParser::ListComprehensionContext::ListComprehensionContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::ListComprehensionContext::LBRACK() {
  return getToken(CypherParser::LBRACK, 0);
}

CypherParser::VariableContext* CypherParser::ListComprehensionContext::variable() {
  return getRuleContext<CypherParser::VariableContext>(0);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::K_IN() {
  return getToken(CypherParser::K_IN, 0);
}

std::vector<CypherParser::ExpressionContext *> CypherParser::ListComprehensionContext::expression() {
  return getRuleContexts<CypherParser::ExpressionContext>();
}

CypherParser::ExpressionContext* CypherParser::ListComprehensionContext::expression(size_t i) {
  return getRuleContext<CypherParser::ExpressionContext>(i);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::RBRACK() {
  return getToken(CypherParser::RBRACK, 0);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::K_WHERE() {
  return getToken(CypherParser::K_WHERE, 0);
}

tree::TerminalNode* CypherParser::ListComprehensionContext::PIPE() {
  return getToken(CypherParser::PIPE, 0);
}


size_t CypherParser::ListComprehensionContext::getRuleIndex() const {
  return CypherParser::RuleListComprehension;
}


std::any CypherParser::ListComprehensionContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitListComprehension(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::ListComprehensionContext* CypherParser::listComprehension() {
  ListComprehensionContext *_localctx = _tracker.createInstance<ListComprehensionContext>(_ctx, getState());
  enterRule(_localctx, 158, CypherParser::RuleListComprehension);
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
    setState(894);
    match(CypherParser::LBRACK);
    setState(895);
    variable();
    setState(896);
    match(CypherParser::K_IN);
    setState(897);
    expression();
    setState(900);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(898);
      match(CypherParser::K_WHERE);
      setState(899);
      antlrcpp::downCast<ListComprehensionContext *>(_localctx)->whereExpression = expression();
    }
    setState(904);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::PIPE) {
      setState(902);
      match(CypherParser::PIPE);
      setState(903);
      antlrcpp::downCast<ListComprehensionContext *>(_localctx)->mapExpression = expression();
    }
    setState(906);
    match(CypherParser::RBRACK);
   
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
