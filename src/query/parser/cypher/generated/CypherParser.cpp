
    #include "antlr4-runtime.h"


// Generated from CypherParser.g4 by ANTLR 4.13.1


#include "CypherParserVisitor.h"

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
      "singleQuery", "readingClause", "updatingClause", "matchStatement", 
      "unwindStatement", "createStatement", "mergeStatement", "setStatement", 
      "setItem", "deleteStatement", "removeStatement", "removeItem", "inQueryCallStatement", 
      "standaloneCallStatement", "yieldItems", "yieldItem", "returnStatement", 
      "projectionBody", "projectionItems", "projectionItem", "orderStatement", 
      "skipStatement", "limitStatement", "sortItem", "where", "pattern", 
      "patternPart", "patternElement", "patternElementChain", "nodePattern", 
      "relationshipPattern", "relationshipDetail", "properties", "nodeLabels", 
      "nodeLabel", "relationshipTypes", "rangeLiteral", "expression", "orExpression", 
      "xorExpression", "andExpression", "notExpression", "comparisonExpression", 
      "arithmeticExpression", "unaryExpression", "atom", "propertyExpression", 
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
      "'='", "'<>'", "'<'", "'>'", "'<='", "'>='", "'+'", "'-'", "'*'", 
      "'/'", "'%'", "'^'", "'('", "')'", "'{'", "'}'", "'['", "']'", "','", 
      "'.'", "':'", "'|'", "'$'", "'..'", "';'"
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
      "K_DROP", "EQ", "NEQ", "LT", "GT", "LTE", "GTE", "PLUS", "MINUS", 
      "MULTIPLY", "DIVIDE", "MODULO", "POWER", "LPAREN", "RPAREN", "LBRACE", 
      "RBRACE", "LBRACK", "RBRACK", "COMMA", "DOT", "COLON", "PIPE", "DOLLAR", 
      "RANGE", "SEMI", "HexInteger", "OctalInteger", "DecimalInteger", "DoubleLiteral", 
      "ID", "StringLiteral", "WS", "COMMENT", "LINE_COMMENT"
    }
  );
  static const int32_t serializedATNSegment[] = {
  	4,1,88,710,2,0,7,0,2,1,7,1,2,2,7,2,2,3,7,3,2,4,7,4,2,5,7,5,2,6,7,6,2,
  	7,7,7,2,8,7,8,2,9,7,9,2,10,7,10,2,11,7,11,2,12,7,12,2,13,7,13,2,14,7,
  	14,2,15,7,15,2,16,7,16,2,17,7,17,2,18,7,18,2,19,7,19,2,20,7,20,2,21,7,
  	21,2,22,7,22,2,23,7,23,2,24,7,24,2,25,7,25,2,26,7,26,2,27,7,27,2,28,7,
  	28,2,29,7,29,2,30,7,30,2,31,7,31,2,32,7,32,2,33,7,33,2,34,7,34,2,35,7,
  	35,2,36,7,36,2,37,7,37,2,38,7,38,2,39,7,39,2,40,7,40,2,41,7,41,2,42,7,
  	42,2,43,7,43,2,44,7,44,2,45,7,45,2,46,7,46,2,47,7,47,2,48,7,48,2,49,7,
  	49,2,50,7,50,2,51,7,51,2,52,7,52,2,53,7,53,2,54,7,54,2,55,7,55,2,56,7,
  	56,2,57,7,57,2,58,7,58,2,59,7,59,2,60,7,60,2,61,7,61,2,62,7,62,2,63,7,
  	63,2,64,7,64,2,65,7,65,2,66,7,66,2,67,7,67,2,68,7,68,2,69,7,69,2,70,7,
  	70,2,71,7,71,2,72,7,72,2,73,7,73,2,74,7,74,1,0,1,0,3,0,153,8,0,1,0,1,
  	0,1,1,1,1,3,1,159,8,1,1,2,1,2,1,2,3,2,164,8,2,1,3,1,3,1,3,1,4,1,4,1,4,
  	1,4,1,4,1,4,1,4,1,4,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,5,1,6,1,6,3,6,187,8,
  	6,1,7,1,7,1,7,3,7,192,8,7,1,7,5,7,195,8,7,10,7,12,7,198,9,7,1,8,5,8,201,
  	8,8,10,8,12,8,204,9,8,1,8,1,8,5,8,208,8,8,10,8,12,8,211,9,8,1,8,4,8,214,
  	8,8,11,8,12,8,215,1,8,3,8,219,8,8,3,8,221,8,8,1,9,1,9,1,9,3,9,226,8,9,
  	1,10,1,10,1,10,1,10,1,10,3,10,233,8,10,1,11,3,11,236,8,11,1,11,1,11,1,
  	11,1,11,3,11,242,8,11,1,12,1,12,1,12,1,12,1,12,1,13,1,13,1,13,1,14,1,
  	14,1,14,1,14,1,14,5,14,257,8,14,10,14,12,14,260,9,14,1,15,1,15,1,15,1,
  	15,5,15,266,8,15,10,15,12,15,269,9,15,1,16,1,16,1,16,1,16,1,16,1,16,1,
  	16,1,16,1,16,1,16,1,16,1,16,1,16,1,16,1,16,1,16,3,16,287,8,16,1,17,3,
  	17,290,8,17,1,17,1,17,1,17,1,17,5,17,296,8,17,10,17,12,17,299,9,17,1,
  	18,1,18,1,18,1,18,5,18,305,8,18,10,18,12,18,308,9,18,1,19,1,19,1,19,1,
  	19,3,19,314,8,19,1,20,1,20,1,20,1,20,3,20,320,8,20,1,21,1,21,1,21,3,21,
  	325,8,21,1,21,1,21,1,21,3,21,330,8,21,3,21,332,8,21,1,22,1,22,1,22,5,
  	22,337,8,22,10,22,12,22,340,9,22,1,22,1,22,3,22,344,8,22,1,23,1,23,1,
  	23,3,23,349,8,23,1,23,1,23,1,24,1,24,1,24,1,25,3,25,357,8,25,1,25,1,25,
  	3,25,361,8,25,1,25,3,25,364,8,25,1,25,3,25,367,8,25,1,26,1,26,1,26,1,
  	26,5,26,373,8,26,10,26,12,26,376,9,26,3,26,378,8,26,1,27,1,27,1,27,3,
  	27,383,8,27,1,28,1,28,1,28,1,28,1,28,5,28,390,8,28,10,28,12,28,393,9,
  	28,1,29,1,29,1,29,1,30,1,30,1,30,1,31,1,31,3,31,403,8,31,1,32,1,32,1,
  	33,1,33,1,33,5,33,410,8,33,10,33,12,33,413,9,33,1,34,1,34,1,34,3,34,418,
  	8,34,1,34,1,34,1,35,1,35,5,35,424,8,35,10,35,12,35,427,9,35,1,35,1,35,
  	1,35,1,35,3,35,433,8,35,1,36,1,36,1,36,1,37,1,37,3,37,440,8,37,1,37,3,
  	37,443,8,37,1,37,3,37,446,8,37,1,37,1,37,1,38,3,38,451,8,38,1,38,1,38,
  	3,38,455,8,38,1,38,1,38,3,38,459,8,38,1,38,1,38,3,38,463,8,38,1,38,3,
  	38,466,8,38,1,39,1,39,3,39,470,8,39,1,39,3,39,473,8,39,1,39,3,39,476,
  	8,39,1,39,3,39,479,8,39,1,39,1,39,1,40,1,40,3,40,485,8,40,1,41,4,41,488,
  	8,41,11,41,12,41,489,1,42,1,42,1,42,1,43,1,43,1,43,1,43,3,43,499,8,43,
  	1,43,5,43,502,8,43,10,43,12,43,505,9,43,1,44,1,44,3,44,509,8,44,1,44,
  	1,44,3,44,513,8,44,3,44,515,8,44,1,45,1,45,1,46,1,46,1,46,5,46,522,8,
  	46,10,46,12,46,525,9,46,1,47,1,47,1,47,5,47,530,8,47,10,47,12,47,533,
  	9,47,1,48,1,48,1,48,5,48,538,8,48,10,48,12,48,541,9,48,1,49,3,49,544,
  	8,49,1,49,1,49,1,50,1,50,1,50,5,50,551,8,50,10,50,12,50,554,9,50,1,51,
  	1,51,1,51,5,51,559,8,51,10,51,12,51,562,9,51,1,52,3,52,565,8,52,1,52,
  	1,52,1,53,1,53,1,53,1,53,1,53,1,53,1,53,1,53,1,53,1,53,1,53,1,53,1,53,
  	3,53,582,8,53,1,54,1,54,1,54,4,54,587,8,54,11,54,12,54,588,1,55,1,55,
  	1,55,3,55,594,8,55,1,55,1,55,1,55,5,55,599,8,55,10,55,12,55,602,9,55,
  	3,55,604,8,55,1,55,1,55,1,56,1,56,1,56,1,56,1,56,5,56,613,8,56,10,56,
  	12,56,616,9,56,3,56,618,8,56,1,56,1,56,1,57,1,57,1,58,1,58,1,59,1,59,
  	1,60,1,60,1,61,1,61,1,62,1,62,1,62,1,63,1,63,1,64,1,64,1,64,1,65,1,65,
  	1,65,5,65,643,8,65,10,65,12,65,646,9,65,1,66,1,66,1,66,1,66,1,66,1,66,
  	1,66,3,66,655,8,66,1,67,1,67,1,68,1,68,1,68,1,68,1,68,3,68,664,8,68,1,
  	69,1,69,1,70,1,70,3,70,670,8,70,1,71,1,71,1,72,1,72,1,72,1,72,1,72,1,
  	72,1,72,1,72,1,72,5,72,683,8,72,10,72,12,72,686,9,72,3,72,688,8,72,1,
  	72,1,72,1,73,1,73,1,73,1,73,5,73,696,8,73,10,73,12,73,699,9,73,3,73,701,
  	8,73,1,73,1,73,1,74,1,74,1,74,3,74,708,8,74,1,74,0,0,75,0,2,4,6,8,10,
  	12,14,16,18,20,22,24,26,28,30,32,34,36,38,40,42,44,46,48,50,52,54,56,
  	58,60,62,64,66,68,70,72,74,76,78,80,82,84,86,88,90,92,94,96,98,100,102,
  	104,106,108,110,112,114,116,118,120,122,124,126,128,130,132,134,136,138,
  	140,142,144,146,148,0,8,2,0,3,3,8,8,1,0,14,17,1,0,55,60,1,0,61,65,1,0,
  	61,62,2,0,44,49,84,84,1,0,36,37,1,0,80,82,738,0,150,1,0,0,0,2,158,1,0,
  	0,0,4,163,1,0,0,0,6,165,1,0,0,0,8,168,1,0,0,0,10,176,1,0,0,0,12,186,1,
  	0,0,0,14,188,1,0,0,0,16,220,1,0,0,0,18,225,1,0,0,0,20,232,1,0,0,0,22,
  	235,1,0,0,0,24,243,1,0,0,0,26,248,1,0,0,0,28,251,1,0,0,0,30,261,1,0,0,
  	0,32,286,1,0,0,0,34,289,1,0,0,0,36,300,1,0,0,0,38,313,1,0,0,0,40,315,
  	1,0,0,0,42,321,1,0,0,0,44,333,1,0,0,0,46,348,1,0,0,0,48,352,1,0,0,0,50,
  	356,1,0,0,0,52,377,1,0,0,0,54,379,1,0,0,0,56,384,1,0,0,0,58,394,1,0,0,
  	0,60,397,1,0,0,0,62,400,1,0,0,0,64,404,1,0,0,0,66,406,1,0,0,0,68,417,
  	1,0,0,0,70,432,1,0,0,0,72,434,1,0,0,0,74,437,1,0,0,0,76,465,1,0,0,0,78,
  	467,1,0,0,0,80,484,1,0,0,0,82,487,1,0,0,0,84,491,1,0,0,0,86,494,1,0,0,
  	0,88,506,1,0,0,0,90,516,1,0,0,0,92,518,1,0,0,0,94,526,1,0,0,0,96,534,
  	1,0,0,0,98,543,1,0,0,0,100,547,1,0,0,0,102,555,1,0,0,0,104,564,1,0,0,
  	0,106,581,1,0,0,0,108,583,1,0,0,0,110,590,1,0,0,0,112,607,1,0,0,0,114,
  	621,1,0,0,0,116,623,1,0,0,0,118,625,1,0,0,0,120,627,1,0,0,0,122,629,1,
  	0,0,0,124,631,1,0,0,0,126,634,1,0,0,0,128,636,1,0,0,0,130,644,1,0,0,0,
  	132,654,1,0,0,0,134,656,1,0,0,0,136,663,1,0,0,0,138,665,1,0,0,0,140,669,
  	1,0,0,0,142,671,1,0,0,0,144,673,1,0,0,0,146,691,1,0,0,0,148,704,1,0,0,
  	0,150,152,3,2,1,0,151,153,5,79,0,0,152,151,1,0,0,0,152,153,1,0,0,0,153,
  	154,1,0,0,0,154,155,5,0,0,1,155,1,1,0,0,0,156,159,3,4,2,0,157,159,3,12,
  	6,0,158,156,1,0,0,0,158,157,1,0,0,0,159,3,1,0,0,0,160,164,3,6,3,0,161,
  	164,3,8,4,0,162,164,3,10,5,0,163,160,1,0,0,0,163,161,1,0,0,0,163,162,
  	1,0,0,0,164,5,1,0,0,0,165,166,5,53,0,0,166,167,5,51,0,0,167,7,1,0,0,0,
  	168,169,5,54,0,0,169,170,5,51,0,0,170,171,5,52,0,0,171,172,3,84,42,0,
  	172,173,5,67,0,0,173,174,3,122,61,0,174,175,5,68,0,0,175,9,1,0,0,0,176,
  	177,5,3,0,0,177,178,5,51,0,0,178,179,5,52,0,0,179,180,3,84,42,0,180,181,
  	5,67,0,0,181,182,3,122,61,0,182,183,5,68,0,0,183,11,1,0,0,0,184,187,3,
  	14,7,0,185,187,3,42,21,0,186,184,1,0,0,0,186,185,1,0,0,0,187,13,1,0,0,
  	0,188,196,3,16,8,0,189,191,5,23,0,0,190,192,5,50,0,0,191,190,1,0,0,0,
  	191,192,1,0,0,0,192,193,1,0,0,0,193,195,3,16,8,0,194,189,1,0,0,0,195,
  	198,1,0,0,0,196,194,1,0,0,0,196,197,1,0,0,0,197,15,1,0,0,0,198,196,1,
  	0,0,0,199,201,3,18,9,0,200,199,1,0,0,0,201,204,1,0,0,0,202,200,1,0,0,
  	0,202,203,1,0,0,0,203,205,1,0,0,0,204,202,1,0,0,0,205,221,3,48,24,0,206,
  	208,3,18,9,0,207,206,1,0,0,0,208,211,1,0,0,0,209,207,1,0,0,0,209,210,
  	1,0,0,0,210,213,1,0,0,0,211,209,1,0,0,0,212,214,3,20,10,0,213,212,1,0,
  	0,0,214,215,1,0,0,0,215,213,1,0,0,0,215,216,1,0,0,0,216,218,1,0,0,0,217,
  	219,3,48,24,0,218,217,1,0,0,0,218,219,1,0,0,0,219,221,1,0,0,0,220,202,
  	1,0,0,0,220,209,1,0,0,0,221,17,1,0,0,0,222,226,3,22,11,0,223,226,3,24,
  	12,0,224,226,3,40,20,0,225,222,1,0,0,0,225,223,1,0,0,0,225,224,1,0,0,
  	0,226,19,1,0,0,0,227,233,3,26,13,0,228,233,3,28,14,0,229,233,3,34,17,
  	0,230,233,3,30,15,0,231,233,3,36,18,0,232,227,1,0,0,0,232,228,1,0,0,0,
  	232,229,1,0,0,0,232,230,1,0,0,0,232,231,1,0,0,0,233,21,1,0,0,0,234,236,
  	5,10,0,0,235,234,1,0,0,0,235,236,1,0,0,0,236,237,1,0,0,0,237,238,5,8,
  	0,0,238,241,3,66,33,0,239,240,5,21,0,0,240,242,3,64,32,0,241,239,1,0,
  	0,0,241,242,1,0,0,0,242,23,1,0,0,0,243,244,5,24,0,0,244,245,3,90,45,0,
  	245,246,5,26,0,0,246,247,3,116,58,0,247,25,1,0,0,0,248,249,5,3,0,0,249,
  	250,3,66,33,0,250,27,1,0,0,0,251,252,5,9,0,0,252,258,3,68,34,0,253,254,
  	5,52,0,0,254,255,7,0,0,0,255,257,3,30,15,0,256,253,1,0,0,0,257,260,1,
  	0,0,0,258,256,1,0,0,0,258,259,1,0,0,0,259,29,1,0,0,0,260,258,1,0,0,0,
  	261,262,5,20,0,0,262,267,3,32,16,0,263,264,5,73,0,0,264,266,3,32,16,0,
  	265,263,1,0,0,0,266,269,1,0,0,0,267,265,1,0,0,0,267,268,1,0,0,0,268,31,
  	1,0,0,0,269,267,1,0,0,0,270,271,3,108,54,0,271,272,5,55,0,0,272,273,3,
  	90,45,0,273,287,1,0,0,0,274,275,3,116,58,0,275,276,5,55,0,0,276,277,3,
  	90,45,0,277,287,1,0,0,0,278,279,3,116,58,0,279,280,5,61,0,0,280,281,5,
  	55,0,0,281,282,3,90,45,0,282,287,1,0,0,0,283,284,3,116,58,0,284,285,3,
  	82,41,0,285,287,1,0,0,0,286,270,1,0,0,0,286,274,1,0,0,0,286,278,1,0,0,
  	0,286,283,1,0,0,0,287,33,1,0,0,0,288,290,5,5,0,0,289,288,1,0,0,0,289,
  	290,1,0,0,0,290,291,1,0,0,0,291,292,5,4,0,0,292,297,3,90,45,0,293,294,
  	5,73,0,0,294,296,3,90,45,0,295,293,1,0,0,0,296,299,1,0,0,0,297,295,1,
  	0,0,0,297,298,1,0,0,0,298,35,1,0,0,0,299,297,1,0,0,0,300,301,5,18,0,0,
  	301,306,3,38,19,0,302,303,5,73,0,0,303,305,3,38,19,0,304,302,1,0,0,0,
  	305,308,1,0,0,0,306,304,1,0,0,0,306,307,1,0,0,0,307,37,1,0,0,0,308,306,
  	1,0,0,0,309,310,3,116,58,0,310,311,3,82,41,0,311,314,1,0,0,0,312,314,
  	3,108,54,0,313,309,1,0,0,0,313,312,1,0,0,0,314,39,1,0,0,0,315,316,5,1,
  	0,0,316,319,3,112,56,0,317,318,5,2,0,0,318,320,3,44,22,0,319,317,1,0,
  	0,0,319,320,1,0,0,0,320,41,1,0,0,0,321,324,5,1,0,0,322,325,3,112,56,0,
  	323,325,3,114,57,0,324,322,1,0,0,0,324,323,1,0,0,0,325,331,1,0,0,0,326,
  	329,5,2,0,0,327,330,5,63,0,0,328,330,3,44,22,0,329,327,1,0,0,0,329,328,
  	1,0,0,0,330,332,1,0,0,0,331,326,1,0,0,0,331,332,1,0,0,0,332,43,1,0,0,
  	0,333,338,3,46,23,0,334,335,5,73,0,0,335,337,3,46,23,0,336,334,1,0,0,
  	0,337,340,1,0,0,0,338,336,1,0,0,0,338,339,1,0,0,0,339,343,1,0,0,0,340,
  	338,1,0,0,0,341,342,5,21,0,0,342,344,3,64,32,0,343,341,1,0,0,0,343,344,
  	1,0,0,0,344,45,1,0,0,0,345,346,3,126,63,0,346,347,5,26,0,0,347,349,1,
  	0,0,0,348,345,1,0,0,0,348,349,1,0,0,0,349,350,1,0,0,0,350,351,3,116,58,
  	0,351,47,1,0,0,0,352,353,5,19,0,0,353,354,3,50,25,0,354,49,1,0,0,0,355,
  	357,5,28,0,0,356,355,1,0,0,0,356,357,1,0,0,0,357,358,1,0,0,0,358,360,
  	3,52,26,0,359,361,3,56,28,0,360,359,1,0,0,0,360,361,1,0,0,0,361,363,1,
  	0,0,0,362,364,3,58,29,0,363,362,1,0,0,0,363,364,1,0,0,0,364,366,1,0,0,
  	0,365,367,3,60,30,0,366,365,1,0,0,0,366,367,1,0,0,0,367,51,1,0,0,0,368,
  	378,5,63,0,0,369,374,3,54,27,0,370,371,5,73,0,0,371,373,3,54,27,0,372,
  	370,1,0,0,0,373,376,1,0,0,0,374,372,1,0,0,0,374,375,1,0,0,0,375,378,1,
  	0,0,0,376,374,1,0,0,0,377,368,1,0,0,0,377,369,1,0,0,0,378,53,1,0,0,0,
  	379,382,3,90,45,0,380,381,5,26,0,0,381,383,3,116,58,0,382,380,1,0,0,0,
  	382,383,1,0,0,0,383,55,1,0,0,0,384,385,5,11,0,0,385,386,5,12,0,0,386,
  	391,3,62,31,0,387,388,5,73,0,0,388,390,3,62,31,0,389,387,1,0,0,0,390,
  	393,1,0,0,0,391,389,1,0,0,0,391,392,1,0,0,0,392,57,1,0,0,0,393,391,1,
  	0,0,0,394,395,5,13,0,0,395,396,3,90,45,0,396,59,1,0,0,0,397,398,5,7,0,
  	0,398,399,3,90,45,0,399,61,1,0,0,0,400,402,3,90,45,0,401,403,7,1,0,0,
  	402,401,1,0,0,0,402,403,1,0,0,0,403,63,1,0,0,0,404,405,3,90,45,0,405,
  	65,1,0,0,0,406,411,3,68,34,0,407,408,5,73,0,0,408,410,3,68,34,0,409,407,
  	1,0,0,0,410,413,1,0,0,0,411,409,1,0,0,0,411,412,1,0,0,0,412,67,1,0,0,
  	0,413,411,1,0,0,0,414,415,3,116,58,0,415,416,5,55,0,0,416,418,1,0,0,0,
  	417,414,1,0,0,0,417,418,1,0,0,0,418,419,1,0,0,0,419,420,3,70,35,0,420,
  	69,1,0,0,0,421,425,3,74,37,0,422,424,3,72,36,0,423,422,1,0,0,0,424,427,
  	1,0,0,0,425,423,1,0,0,0,425,426,1,0,0,0,426,433,1,0,0,0,427,425,1,0,0,
  	0,428,429,5,67,0,0,429,430,3,70,35,0,430,431,5,68,0,0,431,433,1,0,0,0,
  	432,421,1,0,0,0,432,428,1,0,0,0,433,71,1,0,0,0,434,435,3,76,38,0,435,
  	436,3,74,37,0,436,73,1,0,0,0,437,439,5,67,0,0,438,440,3,116,58,0,439,
  	438,1,0,0,0,439,440,1,0,0,0,440,442,1,0,0,0,441,443,3,82,41,0,442,441,
  	1,0,0,0,442,443,1,0,0,0,443,445,1,0,0,0,444,446,3,80,40,0,445,444,1,0,
  	0,0,445,446,1,0,0,0,446,447,1,0,0,0,447,448,5,68,0,0,448,75,1,0,0,0,449,
  	451,5,57,0,0,450,449,1,0,0,0,450,451,1,0,0,0,451,452,1,0,0,0,452,454,
  	5,62,0,0,453,455,3,78,39,0,454,453,1,0,0,0,454,455,1,0,0,0,455,456,1,
  	0,0,0,456,458,5,62,0,0,457,459,5,58,0,0,458,457,1,0,0,0,458,459,1,0,0,
  	0,459,466,1,0,0,0,460,462,5,62,0,0,461,463,3,78,39,0,462,461,1,0,0,0,
  	462,463,1,0,0,0,463,464,1,0,0,0,464,466,5,62,0,0,465,450,1,0,0,0,465,
  	460,1,0,0,0,466,77,1,0,0,0,467,469,5,71,0,0,468,470,3,116,58,0,469,468,
  	1,0,0,0,469,470,1,0,0,0,470,472,1,0,0,0,471,473,3,86,43,0,472,471,1,0,
  	0,0,472,473,1,0,0,0,473,475,1,0,0,0,474,476,3,88,44,0,475,474,1,0,0,0,
  	475,476,1,0,0,0,476,478,1,0,0,0,477,479,3,80,40,0,478,477,1,0,0,0,478,
  	479,1,0,0,0,479,480,1,0,0,0,480,481,5,72,0,0,481,79,1,0,0,0,482,485,3,
  	144,72,0,483,485,3,148,74,0,484,482,1,0,0,0,484,483,1,0,0,0,485,81,1,
  	0,0,0,486,488,3,84,42,0,487,486,1,0,0,0,488,489,1,0,0,0,489,487,1,0,0,
  	0,489,490,1,0,0,0,490,83,1,0,0,0,491,492,5,75,0,0,492,493,3,118,59,0,
  	493,85,1,0,0,0,494,495,5,75,0,0,495,503,3,120,60,0,496,498,5,76,0,0,497,
  	499,5,75,0,0,498,497,1,0,0,0,498,499,1,0,0,0,499,500,1,0,0,0,500,502,
  	3,120,60,0,501,496,1,0,0,0,502,505,1,0,0,0,503,501,1,0,0,0,503,504,1,
  	0,0,0,504,87,1,0,0,0,505,503,1,0,0,0,506,508,5,63,0,0,507,509,3,142,71,
  	0,508,507,1,0,0,0,508,509,1,0,0,0,509,514,1,0,0,0,510,512,5,78,0,0,511,
  	513,3,142,71,0,512,511,1,0,0,0,512,513,1,0,0,0,513,515,1,0,0,0,514,510,
  	1,0,0,0,514,515,1,0,0,0,515,89,1,0,0,0,516,517,3,92,46,0,517,91,1,0,0,
  	0,518,523,3,94,47,0,519,520,5,33,0,0,520,522,3,94,47,0,521,519,1,0,0,
  	0,522,525,1,0,0,0,523,521,1,0,0,0,523,524,1,0,0,0,524,93,1,0,0,0,525,
  	523,1,0,0,0,526,531,3,96,48,0,527,528,5,35,0,0,528,530,3,96,48,0,529,
  	527,1,0,0,0,530,533,1,0,0,0,531,529,1,0,0,0,531,532,1,0,0,0,532,95,1,
  	0,0,0,533,531,1,0,0,0,534,539,3,98,49,0,535,536,5,25,0,0,536,538,3,98,
  	49,0,537,535,1,0,0,0,538,541,1,0,0,0,539,537,1,0,0,0,539,540,1,0,0,0,
  	540,97,1,0,0,0,541,539,1,0,0,0,542,544,5,32,0,0,543,542,1,0,0,0,543,544,
  	1,0,0,0,544,545,1,0,0,0,545,546,3,100,50,0,546,99,1,0,0,0,547,552,3,102,
  	51,0,548,549,7,2,0,0,549,551,3,102,51,0,550,548,1,0,0,0,551,554,1,0,0,
  	0,552,550,1,0,0,0,552,553,1,0,0,0,553,101,1,0,0,0,554,552,1,0,0,0,555,
  	560,3,104,52,0,556,557,7,3,0,0,557,559,3,104,52,0,558,556,1,0,0,0,559,
  	562,1,0,0,0,560,558,1,0,0,0,560,561,1,0,0,0,561,103,1,0,0,0,562,560,1,
  	0,0,0,563,565,7,4,0,0,564,563,1,0,0,0,564,565,1,0,0,0,565,566,1,0,0,0,
  	566,567,3,106,53,0,567,105,1,0,0,0,568,582,3,136,68,0,569,582,3,148,74,
  	0,570,571,5,44,0,0,571,572,5,67,0,0,572,573,5,63,0,0,573,582,5,68,0,0,
  	574,582,3,110,55,0,575,582,3,116,58,0,576,577,5,67,0,0,577,578,3,90,45,
  	0,578,579,5,68,0,0,579,582,1,0,0,0,580,582,3,146,73,0,581,568,1,0,0,0,
  	581,569,1,0,0,0,581,570,1,0,0,0,581,574,1,0,0,0,581,575,1,0,0,0,581,576,
  	1,0,0,0,581,580,1,0,0,0,582,107,1,0,0,0,583,586,3,106,53,0,584,585,5,
  	74,0,0,585,587,3,122,61,0,586,584,1,0,0,0,587,588,1,0,0,0,588,586,1,0,
  	0,0,588,589,1,0,0,0,589,109,1,0,0,0,590,591,3,128,64,0,591,593,5,67,0,
  	0,592,594,5,28,0,0,593,592,1,0,0,0,593,594,1,0,0,0,594,603,1,0,0,0,595,
  	600,3,90,45,0,596,597,5,73,0,0,597,599,3,90,45,0,598,596,1,0,0,0,599,
  	602,1,0,0,0,600,598,1,0,0,0,600,601,1,0,0,0,601,604,1,0,0,0,602,600,1,
  	0,0,0,603,595,1,0,0,0,603,604,1,0,0,0,604,605,1,0,0,0,605,606,5,68,0,
  	0,606,111,1,0,0,0,607,608,3,124,62,0,608,617,5,67,0,0,609,614,3,90,45,
  	0,610,611,5,73,0,0,611,613,3,90,45,0,612,610,1,0,0,0,613,616,1,0,0,0,
  	614,612,1,0,0,0,614,615,1,0,0,0,615,618,1,0,0,0,616,614,1,0,0,0,617,609,
  	1,0,0,0,617,618,1,0,0,0,618,619,1,0,0,0,619,620,5,68,0,0,620,113,1,0,
  	0,0,621,622,3,124,62,0,622,115,1,0,0,0,623,624,3,134,67,0,624,117,1,0,
  	0,0,625,626,3,132,66,0,626,119,1,0,0,0,627,628,3,132,66,0,628,121,1,0,
  	0,0,629,630,3,132,66,0,630,123,1,0,0,0,631,632,3,130,65,0,632,633,3,134,
  	67,0,633,125,1,0,0,0,634,635,3,134,67,0,635,127,1,0,0,0,636,637,3,130,
  	65,0,637,638,3,134,67,0,638,129,1,0,0,0,639,640,3,134,67,0,640,641,5,
  	74,0,0,641,643,1,0,0,0,642,639,1,0,0,0,643,646,1,0,0,0,644,642,1,0,0,
  	0,644,645,1,0,0,0,645,131,1,0,0,0,646,644,1,0,0,0,647,655,3,134,67,0,
  	648,655,5,51,0,0,649,655,5,52,0,0,650,655,5,8,0,0,651,655,5,3,0,0,652,
  	655,5,21,0,0,653,655,5,19,0,0,654,647,1,0,0,0,654,648,1,0,0,0,654,649,
  	1,0,0,0,654,650,1,0,0,0,654,651,1,0,0,0,654,652,1,0,0,0,654,653,1,0,0,
  	0,655,133,1,0,0,0,656,657,7,5,0,0,657,135,1,0,0,0,658,664,3,138,69,0,
  	659,664,5,38,0,0,660,664,3,140,70,0,661,664,5,85,0,0,662,664,3,144,72,
  	0,663,658,1,0,0,0,663,659,1,0,0,0,663,660,1,0,0,0,663,661,1,0,0,0,663,
  	662,1,0,0,0,664,137,1,0,0,0,665,666,7,6,0,0,666,139,1,0,0,0,667,670,5,
  	83,0,0,668,670,3,142,71,0,669,667,1,0,0,0,669,668,1,0,0,0,670,141,1,0,
  	0,0,671,672,7,7,0,0,672,143,1,0,0,0,673,687,5,69,0,0,674,675,3,122,61,
  	0,675,676,5,75,0,0,676,684,3,90,45,0,677,678,5,73,0,0,678,679,3,122,61,
  	0,679,680,5,75,0,0,680,681,3,90,45,0,681,683,1,0,0,0,682,677,1,0,0,0,
  	683,686,1,0,0,0,684,682,1,0,0,0,684,685,1,0,0,0,685,688,1,0,0,0,686,684,
  	1,0,0,0,687,674,1,0,0,0,687,688,1,0,0,0,688,689,1,0,0,0,689,690,5,70,
  	0,0,690,145,1,0,0,0,691,700,5,71,0,0,692,697,3,90,45,0,693,694,5,73,0,
  	0,694,696,3,90,45,0,695,693,1,0,0,0,696,699,1,0,0,0,697,695,1,0,0,0,697,
  	698,1,0,0,0,698,701,1,0,0,0,699,697,1,0,0,0,700,692,1,0,0,0,700,701,1,
  	0,0,0,701,702,1,0,0,0,702,703,5,72,0,0,703,147,1,0,0,0,704,707,5,77,0,
  	0,705,708,3,134,67,0,706,708,5,82,0,0,707,705,1,0,0,0,707,706,1,0,0,0,
  	708,149,1,0,0,0,84,152,158,163,186,191,196,202,209,215,218,220,225,232,
  	235,241,258,267,286,289,297,306,313,319,324,329,331,338,343,348,356,360,
  	363,366,374,377,382,391,402,411,417,425,432,439,442,445,450,454,458,462,
  	465,469,472,475,478,484,489,498,503,508,512,514,523,531,539,543,552,560,
  	564,581,588,593,600,603,614,617,644,654,663,669,684,687,697,700,707
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
    setState(150);
    statement();
    setState(152);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::SEMI) {
      setState(151);
      match(CypherParser::SEMI);
    }
    setState(154);
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

CypherParser::AdministrationStatementContext* CypherParser::StatementContext::administrationStatement() {
  return getRuleContext<CypherParser::AdministrationStatementContext>(0);
}

CypherParser::QueryContext* CypherParser::StatementContext::query() {
  return getRuleContext<CypherParser::QueryContext>(0);
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
    setState(158);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 1, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(156);
      administrationStatement();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(157);
      query();
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
    setState(163);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_SHOW: {
        enterOuterAlt(_localctx, 1);
        setState(160);
        showIndexesStatement();
        break;
      }

      case CypherParser::K_DROP: {
        enterOuterAlt(_localctx, 2);
        setState(161);
        dropIndexStatement();
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 3);
        setState(162);
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
    setState(165);
    match(CypherParser::K_SHOW);
    setState(166);
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

tree::TerminalNode* CypherParser::DropIndexStatementContext::K_DROP() {
  return getToken(CypherParser::K_DROP, 0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::DropIndexStatementContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::DropIndexStatementContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::DropIndexStatementContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}


size_t CypherParser::DropIndexStatementContext::getRuleIndex() const {
  return CypherParser::RuleDropIndexStatement;
}


std::any CypherParser::DropIndexStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
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
    setState(168);
    match(CypherParser::K_DROP);
    setState(169);
    match(CypherParser::K_INDEX);
    setState(170);
    match(CypherParser::K_ON);
    setState(171);
    nodeLabel();
    setState(172);
    match(CypherParser::LPAREN);
    setState(173);
    propertyKeyName();
    setState(174);
    match(CypherParser::RPAREN);
   
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

tree::TerminalNode* CypherParser::CreateIndexStatementContext::K_CREATE() {
  return getToken(CypherParser::K_CREATE, 0);
}

tree::TerminalNode* CypherParser::CreateIndexStatementContext::K_INDEX() {
  return getToken(CypherParser::K_INDEX, 0);
}

tree::TerminalNode* CypherParser::CreateIndexStatementContext::K_ON() {
  return getToken(CypherParser::K_ON, 0);
}

CypherParser::NodeLabelContext* CypherParser::CreateIndexStatementContext::nodeLabel() {
  return getRuleContext<CypherParser::NodeLabelContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexStatementContext::LPAREN() {
  return getToken(CypherParser::LPAREN, 0);
}

CypherParser::PropertyKeyNameContext* CypherParser::CreateIndexStatementContext::propertyKeyName() {
  return getRuleContext<CypherParser::PropertyKeyNameContext>(0);
}

tree::TerminalNode* CypherParser::CreateIndexStatementContext::RPAREN() {
  return getToken(CypherParser::RPAREN, 0);
}


size_t CypherParser::CreateIndexStatementContext::getRuleIndex() const {
  return CypherParser::RuleCreateIndexStatement;
}


std::any CypherParser::CreateIndexStatementContext::accept(tree::ParseTreeVisitor *visitor) {
  if (auto parserVisitor = dynamic_cast<CypherParserVisitor*>(visitor))
    return parserVisitor->visitCreateIndexStatement(this);
  else
    return visitor->visitChildren(this);
}

CypherParser::CreateIndexStatementContext* CypherParser::createIndexStatement() {
  CreateIndexStatementContext *_localctx = _tracker.createInstance<CreateIndexStatementContext>(_ctx, getState());
  enterRule(_localctx, 10, CypherParser::RuleCreateIndexStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(176);
    match(CypherParser::K_CREATE);
    setState(177);
    match(CypherParser::K_INDEX);
    setState(178);
    match(CypherParser::K_ON);
    setState(179);
    nodeLabel();
    setState(180);
    match(CypherParser::LPAREN);
    setState(181);
    propertyKeyName();
    setState(182);
    match(CypherParser::RPAREN);
   
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
    setState(186);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 3, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(184);
      regularQuery();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(185);
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
    setState(188);
    singleQuery();
    setState(196);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_UNION) {
      setState(189);
      match(CypherParser::K_UNION);
      setState(191);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_ALL) {
        setState(190);
        match(CypherParser::K_ALL);
      }
      setState(193);
      singleQuery();
      setState(198);
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
    setState(220);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 10, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(202);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(199);
        readingClause();
        setState(204);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(205);
      returnStatement();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(209);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 16778498) != 0)) {
        setState(206);
        readingClause();
        setState(211);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      setState(213); 
      _errHandler->sync(this);
      _la = _input->LA(1);
      do {
        setState(212);
        updatingClause();
        setState(215); 
        _errHandler->sync(this);
        _la = _input->LA(1);
      } while ((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 1311288) != 0));
      setState(218);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::K_RETURN) {
        setState(217);
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
    setState(225);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_MATCH:
      case CypherParser::K_OPTIONAL: {
        enterOuterAlt(_localctx, 1);
        setState(222);
        matchStatement();
        break;
      }

      case CypherParser::K_UNWIND: {
        enterOuterAlt(_localctx, 2);
        setState(223);
        unwindStatement();
        break;
      }

      case CypherParser::K_CALL: {
        enterOuterAlt(_localctx, 3);
        setState(224);
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
    setState(232);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 1);
        setState(227);
        createStatement();
        break;
      }

      case CypherParser::K_MERGE: {
        enterOuterAlt(_localctx, 2);
        setState(228);
        mergeStatement();
        break;
      }

      case CypherParser::K_DELETE:
      case CypherParser::K_DETACH: {
        enterOuterAlt(_localctx, 3);
        setState(229);
        deleteStatement();
        break;
      }

      case CypherParser::K_SET: {
        enterOuterAlt(_localctx, 4);
        setState(230);
        setStatement();
        break;
      }

      case CypherParser::K_REMOVE: {
        enterOuterAlt(_localctx, 5);
        setState(231);
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
  enterRule(_localctx, 22, CypherParser::RuleMatchStatement);
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
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_OPTIONAL) {
      setState(234);
      match(CypherParser::K_OPTIONAL);
    }
    setState(237);
    match(CypherParser::K_MATCH);
    setState(238);
    pattern();
    setState(241);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(239);
      match(CypherParser::K_WHERE);
      setState(240);
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
  enterRule(_localctx, 24, CypherParser::RuleUnwindStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(243);
    match(CypherParser::K_UNWIND);
    setState(244);
    expression();
    setState(245);
    match(CypherParser::K_AS);
    setState(246);
    variable();
   
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
  enterRule(_localctx, 26, CypherParser::RuleCreateStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(248);
    match(CypherParser::K_CREATE);
    setState(249);
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
  enterRule(_localctx, 28, CypherParser::RuleMergeStatement);
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
    setState(251);
    match(CypherParser::K_MERGE);
    setState(252);
    patternPart();
    setState(258);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_ON) {
      setState(253);
      match(CypherParser::K_ON);
      setState(254);
      _la = _input->LA(1);
      if (!(_la == CypherParser::K_CREATE

      || _la == CypherParser::K_MATCH)) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(255);
      setStatement();
      setState(260);
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
  enterRule(_localctx, 30, CypherParser::RuleSetStatement);
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
    setState(261);
    match(CypherParser::K_SET);
    setState(262);
    setItem();
    setState(267);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(263);
      match(CypherParser::COMMA);
      setState(264);
      setItem();
      setState(269);
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
  enterRule(_localctx, 32, CypherParser::RuleSetItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(286);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 17, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(270);
      propertyExpression();
      setState(271);
      match(CypherParser::EQ);
      setState(272);
      expression();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(274);
      variable();
      setState(275);
      match(CypherParser::EQ);
      setState(276);
      expression();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(278);
      variable();
      setState(279);
      match(CypherParser::PLUS);
      setState(280);
      match(CypherParser::EQ);
      setState(281);
      expression();
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(283);
      variable();
      setState(284);
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
  enterRule(_localctx, 34, CypherParser::RuleDeleteStatement);
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
    setState(289);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_DETACH) {
      setState(288);
      match(CypherParser::K_DETACH);
    }
    setState(291);
    match(CypherParser::K_DELETE);
    setState(292);
    expression();
    setState(297);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(293);
      match(CypherParser::COMMA);
      setState(294);
      expression();
      setState(299);
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
  enterRule(_localctx, 36, CypherParser::RuleRemoveStatement);
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
    setState(300);
    match(CypherParser::K_REMOVE);
    setState(301);
    removeItem();
    setState(306);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(302);
      match(CypherParser::COMMA);
      setState(303);
      removeItem();
      setState(308);
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
  enterRule(_localctx, 38, CypherParser::RuleRemoveItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(313);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 21, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(309);
      variable();
      setState(310);
      nodeLabels();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(312);
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
  enterRule(_localctx, 40, CypherParser::RuleInQueryCallStatement);
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
    setState(315);
    match(CypherParser::K_CALL);
    setState(316);
    explicitProcedureInvocation();
    setState(319);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(317);
      match(CypherParser::K_YIELD);
      setState(318);
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
  enterRule(_localctx, 42, CypherParser::RuleStandaloneCallStatement);
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
    setState(321);
    match(CypherParser::K_CALL);
    setState(324);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 23, _ctx)) {
    case 1: {
      setState(322);
      explicitProcedureInvocation();
      break;
    }

    case 2: {
      setState(323);
      implicitProcedureInvocation();
      break;
    }

    default:
      break;
    }
    setState(331);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_YIELD) {
      setState(326);
      match(CypherParser::K_YIELD);
      setState(329);
      _errHandler->sync(this);
      switch (_input->LA(1)) {
        case CypherParser::MULTIPLY: {
          setState(327);
          match(CypherParser::MULTIPLY);
          break;
        }

        case CypherParser::K_COUNT:
        case CypherParser::K_FILTER:
        case CypherParser::K_EXTRACT:
        case CypherParser::K_ANY:
        case CypherParser::K_NONE:
        case CypherParser::K_SINGLE:
        case CypherParser::ID: {
          setState(328);
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
  enterRule(_localctx, 44, CypherParser::RuleYieldItems);
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
    setState(333);
    yieldItem();
    setState(338);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(334);
      match(CypherParser::COMMA);
      setState(335);
      yieldItem();
      setState(340);
      _errHandler->sync(this);
      _la = _input->LA(1);
    }
    setState(343);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_WHERE) {
      setState(341);
      match(CypherParser::K_WHERE);
      setState(342);
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
  enterRule(_localctx, 46, CypherParser::RuleYieldItem);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(348);
    _errHandler->sync(this);

    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 28, _ctx)) {
    case 1: {
      setState(345);
      procedureResultField();
      setState(346);
      match(CypherParser::K_AS);
      break;
    }

    default:
      break;
    }
    setState(350);
    variable();
   
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
  enterRule(_localctx, 48, CypherParser::RuleReturnStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(352);
    match(CypherParser::K_RETURN);
    setState(353);
    projectionBody();
   
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
  enterRule(_localctx, 50, CypherParser::RuleProjectionBody);
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
    setState(356);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_DISTINCT) {
      setState(355);
      match(CypherParser::K_DISTINCT);
    }
    setState(358);
    projectionItems();
    setState(360);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_ORDER) {
      setState(359);
      orderStatement();
    }
    setState(363);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_SKIP) {
      setState(362);
      skipStatement();
    }
    setState(366);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_LIMIT) {
      setState(365);
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
  enterRule(_localctx, 52, CypherParser::RuleProjectionItems);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(377);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::MULTIPLY: {
        enterOuterAlt(_localctx, 1);
        setState(368);
        match(CypherParser::MULTIPLY);
        break;
      }

      case CypherParser::K_NOT:
      case CypherParser::K_FALSE:
      case CypherParser::K_TRUE:
      case CypherParser::K_NULL:
      case CypherParser::K_COUNT:
      case CypherParser::K_FILTER:
      case CypherParser::K_EXTRACT:
      case CypherParser::K_ANY:
      case CypherParser::K_NONE:
      case CypherParser::K_SINGLE:
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
        setState(369);
        projectionItem();
        setState(374);
        _errHandler->sync(this);
        _la = _input->LA(1);
        while (_la == CypherParser::COMMA) {
          setState(370);
          match(CypherParser::COMMA);
          setState(371);
          projectionItem();
          setState(376);
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
  enterRule(_localctx, 54, CypherParser::RuleProjectionItem);
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
    setState(379);
    expression();
    setState(382);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_AS) {
      setState(380);
      match(CypherParser::K_AS);
      setState(381);
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
  enterRule(_localctx, 56, CypherParser::RuleOrderStatement);
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
    setState(384);
    match(CypherParser::K_ORDER);
    setState(385);
    match(CypherParser::K_BY);
    setState(386);
    sortItem();
    setState(391);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(387);
      match(CypherParser::COMMA);
      setState(388);
      sortItem();
      setState(393);
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
  enterRule(_localctx, 58, CypherParser::RuleSkipStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(394);
    match(CypherParser::K_SKIP);
    setState(395);
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
  enterRule(_localctx, 60, CypherParser::RuleLimitStatement);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(397);
    match(CypherParser::K_LIMIT);
    setState(398);
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
  enterRule(_localctx, 62, CypherParser::RuleSortItem);
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
    setState(400);
    expression();
    setState(402);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 245760) != 0)) {
      setState(401);
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
  enterRule(_localctx, 64, CypherParser::RuleWhere);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(404);
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
  enterRule(_localctx, 66, CypherParser::RulePattern);
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
    setState(406);
    patternPart();
    setState(411);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::COMMA) {
      setState(407);
      match(CypherParser::COMMA);
      setState(408);
      patternPart();
      setState(413);
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
  enterRule(_localctx, 68, CypherParser::RulePatternPart);
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
    setState(417);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 44) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 44)) & 1099511627839) != 0)) {
      setState(414);
      variable();
      setState(415);
      match(CypherParser::EQ);
    }
    setState(419);
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
  enterRule(_localctx, 70, CypherParser::RulePatternElement);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(432);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 41, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(421);
      nodePattern();
      setState(425);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::LT

      || _la == CypherParser::MINUS) {
        setState(422);
        patternElementChain();
        setState(427);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(428);
      match(CypherParser::LPAREN);
      setState(429);
      patternElement();
      setState(430);
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
  enterRule(_localctx, 72, CypherParser::RulePatternElementChain);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(434);
    relationshipPattern();
    setState(435);
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
  enterRule(_localctx, 74, CypherParser::RuleNodePattern);
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
    setState(437);
    match(CypherParser::LPAREN);
    setState(439);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 44) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 44)) & 1099511627839) != 0)) {
      setState(438);
      variable();
    }
    setState(442);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(441);
      nodeLabels();
    }
    setState(445);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(444);
      properties();
    }
    setState(447);
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
  enterRule(_localctx, 76, CypherParser::RuleRelationshipPattern);
  size_t _la = 0;

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(465);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 49, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(450);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LT) {
        setState(449);
        match(CypherParser::LT);
      }
      setState(452);
      match(CypherParser::MINUS);
      setState(454);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(453);
        relationshipDetail();
      }
      setState(456);
      match(CypherParser::MINUS);
      setState(458);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::GT) {
        setState(457);
        match(CypherParser::GT);
      }
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(460);
      match(CypherParser::MINUS);
      setState(462);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::LBRACK) {
        setState(461);
        relationshipDetail();
      }
      setState(464);
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
  enterRule(_localctx, 78, CypherParser::RuleRelationshipDetail);
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
    setState(467);
    match(CypherParser::LBRACK);
    setState(469);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 44) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 44)) & 1099511627839) != 0)) {
      setState(468);
      variable();
    }
    setState(472);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::COLON) {
      setState(471);
      relationshipTypes();
    }
    setState(475);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::MULTIPLY) {
      setState(474);
      rangeLiteral();
    }
    setState(478);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::LBRACE

    || _la == CypherParser::DOLLAR) {
      setState(477);
      properties();
    }
    setState(480);
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
  enterRule(_localctx, 80, CypherParser::RuleProperties);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(484);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 1);
        setState(482);
        mapLiteral();
        break;
      }

      case CypherParser::DOLLAR: {
        enterOuterAlt(_localctx, 2);
        setState(483);
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
  enterRule(_localctx, 82, CypherParser::RuleNodeLabels);
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
    setState(487); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(486);
      nodeLabel();
      setState(489); 
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
  enterRule(_localctx, 84, CypherParser::RuleNodeLabel);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(491);
    match(CypherParser::COLON);
    setState(492);
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
  enterRule(_localctx, 86, CypherParser::RuleRelationshipTypes);
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
    setState(494);
    match(CypherParser::COLON);
    setState(495);
    relTypeName();
    setState(503);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::PIPE) {
      setState(496);
      match(CypherParser::PIPE);
      setState(498);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (_la == CypherParser::COLON) {
        setState(497);
        match(CypherParser::COLON);
      }
      setState(500);
      relTypeName();
      setState(505);
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
  enterRule(_localctx, 88, CypherParser::RuleRangeLiteral);
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
    setState(506);
    match(CypherParser::MULTIPLY);
    setState(508);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 80) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 80)) & 7) != 0)) {
      setState(507);
      integerLiteral();
    }
    setState(514);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::RANGE) {
      setState(510);
      match(CypherParser::RANGE);
      setState(512);
      _errHandler->sync(this);

      _la = _input->LA(1);
      if (((((_la - 80) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 80)) & 7) != 0)) {
        setState(511);
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
  enterRule(_localctx, 90, CypherParser::RuleExpression);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(516);
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
  enterRule(_localctx, 92, CypherParser::RuleOrExpression);
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
    setState(518);
    xorExpression();
    setState(523);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_OR) {
      setState(519);
      match(CypherParser::K_OR);
      setState(520);
      xorExpression();
      setState(525);
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
  enterRule(_localctx, 94, CypherParser::RuleXorExpression);
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
    setState(526);
    andExpression();
    setState(531);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_XOR) {
      setState(527);
      match(CypherParser::K_XOR);
      setState(528);
      andExpression();
      setState(533);
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
  enterRule(_localctx, 96, CypherParser::RuleAndExpression);
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
    setState(534);
    notExpression();
    setState(539);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (_la == CypherParser::K_AND) {
      setState(535);
      match(CypherParser::K_AND);
      setState(536);
      notExpression();
      setState(541);
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
  enterRule(_localctx, 98, CypherParser::RuleNotExpression);
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
    setState(543);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_NOT) {
      setState(542);
      match(CypherParser::K_NOT);
    }
    setState(545);
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
  enterRule(_localctx, 100, CypherParser::RuleComparisonExpression);
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
    setState(547);
    arithmeticExpression();
    setState(552);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 2269814212194729984) != 0)) {
      setState(548);
      _la = _input->LA(1);
      if (!((((_la & ~ 0x3fULL) == 0) &&
        ((1ULL << _la) & 2269814212194729984) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(549);
      arithmeticExpression();
      setState(554);
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
  enterRule(_localctx, 102, CypherParser::RuleArithmeticExpression);
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
    setState(555);
    unaryExpression();
    setState(560);
    _errHandler->sync(this);
    _la = _input->LA(1);
    while (((((_la - 61) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 61)) & 31) != 0)) {
      setState(556);
      _la = _input->LA(1);
      if (!(((((_la - 61) & ~ 0x3fULL) == 0) &&
        ((1ULL << (_la - 61)) & 31) != 0))) {
      _errHandler->recoverInline(this);
      }
      else {
        _errHandler->reportMatch(this);
        consume();
      }
      setState(557);
      unaryExpression();
      setState(562);
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
  enterRule(_localctx, 104, CypherParser::RuleUnaryExpression);
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
    setState(564);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::PLUS

    || _la == CypherParser::MINUS) {
      setState(563);
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
    setState(566);
    atom();
   
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
  enterRule(_localctx, 106, CypherParser::RuleAtom);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(581);
    _errHandler->sync(this);
    switch (getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 68, _ctx)) {
    case 1: {
      enterOuterAlt(_localctx, 1);
      setState(568);
      literal();
      break;
    }

    case 2: {
      enterOuterAlt(_localctx, 2);
      setState(569);
      parameter();
      break;
    }

    case 3: {
      enterOuterAlt(_localctx, 3);
      setState(570);
      match(CypherParser::K_COUNT);
      setState(571);
      match(CypherParser::LPAREN);
      setState(572);
      match(CypherParser::MULTIPLY);
      setState(573);
      match(CypherParser::RPAREN);
      break;
    }

    case 4: {
      enterOuterAlt(_localctx, 4);
      setState(574);
      functionInvocation();
      break;
    }

    case 5: {
      enterOuterAlt(_localctx, 5);
      setState(575);
      variable();
      break;
    }

    case 6: {
      enterOuterAlt(_localctx, 6);
      setState(576);
      match(CypherParser::LPAREN);
      setState(577);
      expression();
      setState(578);
      match(CypherParser::RPAREN);
      break;
    }

    case 7: {
      enterOuterAlt(_localctx, 7);
      setState(580);
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
  enterRule(_localctx, 108, CypherParser::RulePropertyExpression);
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
    setState(583);
    atom();
    setState(586); 
    _errHandler->sync(this);
    _la = _input->LA(1);
    do {
      setState(584);
      match(CypherParser::DOT);
      setState(585);
      propertyKeyName();
      setState(588); 
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
  enterRule(_localctx, 110, CypherParser::RuleFunctionInvocation);
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
    functionName();
    setState(591);
    match(CypherParser::LPAREN);
    setState(593);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (_la == CypherParser::K_DISTINCT) {
      setState(592);
      match(CypherParser::K_DISTINCT);
    }
    setState(603);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 32) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 32)) & 17768831070236785) != 0)) {
      setState(595);
      expression();
      setState(600);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(596);
        match(CypherParser::COMMA);
        setState(597);
        expression();
        setState(602);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(605);
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
  enterRule(_localctx, 112, CypherParser::RuleExplicitProcedureInvocation);
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
    setState(607);
    procedureName();
    setState(608);
    match(CypherParser::LPAREN);
    setState(617);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 32) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 32)) & 17768831070236785) != 0)) {
      setState(609);
      expression();
      setState(614);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(610);
        match(CypherParser::COMMA);
        setState(611);
        expression();
        setState(616);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(619);
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
  enterRule(_localctx, 114, CypherParser::RuleImplicitProcedureInvocation);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(621);
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
  enterRule(_localctx, 116, CypherParser::RuleVariable);

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
  enterRule(_localctx, 118, CypherParser::RuleLabelName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(625);
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
  enterRule(_localctx, 120, CypherParser::RuleRelTypeName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(627);
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
  enterRule(_localctx, 122, CypherParser::RulePropertyKeyName);

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
  enterRule(_localctx, 124, CypherParser::RuleProcedureName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(631);
    namespace_();
    setState(632);
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
  enterRule(_localctx, 126, CypherParser::RuleProcedureResultField);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(634);
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
  enterRule(_localctx, 128, CypherParser::RuleFunctionName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(636);
    namespace_();
    setState(637);
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
  enterRule(_localctx, 130, CypherParser::RuleNamespace);

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
    setState(644);
    _errHandler->sync(this);
    alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 75, _ctx);
    while (alt != 2 && alt != atn::ATN::INVALID_ALT_NUMBER) {
      if (alt == 1) {
        setState(639);
        symbolicName();
        setState(640);
        match(CypherParser::DOT); 
      }
      setState(646);
      _errHandler->sync(this);
      alt = getInterpreter<atn::ParserATNSimulator>()->adaptivePredict(_input, 75, _ctx);
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
  enterRule(_localctx, 132, CypherParser::RuleSchemaName);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(654);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_COUNT:
      case CypherParser::K_FILTER:
      case CypherParser::K_EXTRACT:
      case CypherParser::K_ANY:
      case CypherParser::K_NONE:
      case CypherParser::K_SINGLE:
      case CypherParser::ID: {
        enterOuterAlt(_localctx, 1);
        setState(647);
        symbolicName();
        break;
      }

      case CypherParser::K_INDEX: {
        enterOuterAlt(_localctx, 2);
        setState(648);
        match(CypherParser::K_INDEX);
        break;
      }

      case CypherParser::K_ON: {
        enterOuterAlt(_localctx, 3);
        setState(649);
        match(CypherParser::K_ON);
        break;
      }

      case CypherParser::K_MATCH: {
        enterOuterAlt(_localctx, 4);
        setState(650);
        match(CypherParser::K_MATCH);
        break;
      }

      case CypherParser::K_CREATE: {
        enterOuterAlt(_localctx, 5);
        setState(651);
        match(CypherParser::K_CREATE);
        break;
      }

      case CypherParser::K_WHERE: {
        enterOuterAlt(_localctx, 6);
        setState(652);
        match(CypherParser::K_WHERE);
        break;
      }

      case CypherParser::K_RETURN: {
        enterOuterAlt(_localctx, 7);
        setState(653);
        match(CypherParser::K_RETURN);
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

//----------------- SymbolicNameContext ------------------------------------------------------------------

CypherParser::SymbolicNameContext::SymbolicNameContext(ParserRuleContext *parent, size_t invokingState)
  : ParserRuleContext(parent, invokingState) {
}

tree::TerminalNode* CypherParser::SymbolicNameContext::ID() {
  return getToken(CypherParser::ID, 0);
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
  enterRule(_localctx, 134, CypherParser::RuleSymbolicName);
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
    setState(656);
    _la = _input->LA(1);
    if (!(((((_la - 44) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 44)) & 1099511627839) != 0))) {
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
  enterRule(_localctx, 136, CypherParser::RuleLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(663);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_FALSE:
      case CypherParser::K_TRUE: {
        enterOuterAlt(_localctx, 1);
        setState(658);
        booleanLiteral();
        break;
      }

      case CypherParser::K_NULL: {
        enterOuterAlt(_localctx, 2);
        setState(659);
        match(CypherParser::K_NULL);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger:
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 3);
        setState(660);
        numberLiteral();
        break;
      }

      case CypherParser::StringLiteral: {
        enterOuterAlt(_localctx, 4);
        setState(661);
        match(CypherParser::StringLiteral);
        break;
      }

      case CypherParser::LBRACE: {
        enterOuterAlt(_localctx, 5);
        setState(662);
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
  enterRule(_localctx, 138, CypherParser::RuleBooleanLiteral);
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
    setState(665);
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
  enterRule(_localctx, 140, CypherParser::RuleNumberLiteral);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    setState(669);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::DoubleLiteral: {
        enterOuterAlt(_localctx, 1);
        setState(667);
        match(CypherParser::DoubleLiteral);
        break;
      }

      case CypherParser::HexInteger:
      case CypherParser::OctalInteger:
      case CypherParser::DecimalInteger: {
        enterOuterAlt(_localctx, 2);
        setState(668);
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
  enterRule(_localctx, 142, CypherParser::RuleIntegerLiteral);
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
    setState(671);
    _la = _input->LA(1);
    if (!(((((_la - 80) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 80)) & 7) != 0))) {
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
  enterRule(_localctx, 144, CypherParser::RuleMapLiteral);
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
    setState(673);
    match(CypherParser::LBRACE);
    setState(687);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if ((((_la & ~ 0x3fULL) == 0) &&
      ((1ULL << _la) & 7863707164475656) != 0) || _la == CypherParser::ID) {
      setState(674);
      propertyKeyName();
      setState(675);
      match(CypherParser::COLON);
      setState(676);
      expression();
      setState(684);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(677);
        match(CypherParser::COMMA);
        setState(678);
        propertyKeyName();
        setState(679);
        match(CypherParser::COLON);
        setState(680);
        expression();
        setState(686);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(689);
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
  enterRule(_localctx, 146, CypherParser::RuleListLiteral);
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
    match(CypherParser::LBRACK);
    setState(700);
    _errHandler->sync(this);

    _la = _input->LA(1);
    if (((((_la - 32) & ~ 0x3fULL) == 0) &&
      ((1ULL << (_la - 32)) & 17768831070236785) != 0)) {
      setState(692);
      expression();
      setState(697);
      _errHandler->sync(this);
      _la = _input->LA(1);
      while (_la == CypherParser::COMMA) {
        setState(693);
        match(CypherParser::COMMA);
        setState(694);
        expression();
        setState(699);
        _errHandler->sync(this);
        _la = _input->LA(1);
      }
    }
    setState(702);
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
  enterRule(_localctx, 148, CypherParser::RuleParameter);

#if __cplusplus > 201703L
  auto onExit = finally([=, this] {
#else
  auto onExit = finally([=] {
#endif
    exitRule();
  });
  try {
    enterOuterAlt(_localctx, 1);
    setState(704);
    match(CypherParser::DOLLAR);
    setState(707);
    _errHandler->sync(this);
    switch (_input->LA(1)) {
      case CypherParser::K_COUNT:
      case CypherParser::K_FILTER:
      case CypherParser::K_EXTRACT:
      case CypherParser::K_ANY:
      case CypherParser::K_NONE:
      case CypherParser::K_SINGLE:
      case CypherParser::ID: {
        setState(705);
        symbolicName();
        break;
      }

      case CypherParser::DecimalInteger: {
        setState(706);
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
