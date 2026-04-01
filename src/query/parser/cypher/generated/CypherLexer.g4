/*
 * CypherLexer.g4
 * Based on openCypher, customized for ZYX database.
 */
lexer grammar CypherLexer;

channels {
    COMMENTS
}

options {
    caseInsensitive = true;
}

// --- Keywords (Prefix K_) ---

K_CALL       : 'CALL';
K_YIELD      : 'YIELD';
K_CREATE     : 'CREATE';
K_DELETE     : 'DELETE';
K_DETACH     : 'DETACH';
K_EXISTS     : 'EXISTS';
K_LIMIT      : 'LIMIT';
K_MATCH      : 'MATCH';
K_MERGE      : 'MERGE';
K_OPTIONAL   : 'OPTIONAL';
K_ORDER      : 'ORDER';
K_BY         : 'BY';
K_SKIP       : 'SKIP';
K_ASCENDING  : 'ASCENDING';
K_ASC        : 'ASC';
K_DESCENDING : 'DESCENDING';
K_DESC       : 'DESC';
K_REMOVE     : 'REMOVE';
K_RETURN     : 'RETURN';
K_SET        : 'SET';
K_WHERE      : 'WHERE';
K_WITH       : 'WITH';
K_UNION      : 'UNION';
K_UNWIND     : 'UNWIND';
K_AND        : 'AND';
K_AS         : 'AS';
K_CONTAINS   : 'CONTAINS';
K_DISTINCT   : 'DISTINCT';
K_ENDS       : 'ENDS';
K_IN         : 'IN';
K_IS         : 'IS';
K_NOT        : 'NOT';
K_OR         : 'OR';
K_STARTS     : 'STARTS';
K_XOR        : 'XOR';
K_BETWEEN    : 'BETWEEN';
K_FALSE      : 'FALSE';
K_TRUE       : 'TRUE';
K_NULL       : 'NULL';
K_CASE       : 'CASE';
K_WHEN       : 'WHEN';
K_THEN       : 'THEN';
K_ELSE       : 'ELSE';
K_END        : 'END';
K_COUNT      : 'COUNT';
K_FILTER     : 'FILTER';
K_EXTRACT    : 'EXTRACT';
K_ANY        : 'ANY';
K_NONE       : 'NONE';
K_SINGLE     : 'SINGLE';
K_ALL        : 'ALL';
K_REDUCE     : 'REDUCE';

// Admin Keywords
K_INDEX      : 'INDEX' | 'INDEXES';
K_ON         : 'ON';
K_SHOW       : 'SHOW';
K_DROP       : 'DROP';
K_IF         : 'IF';

K_FOR        : 'FOR';

K_CONSTRAINT : 'CONSTRAINT';
K_DO         : 'DO';
K_REQUIRE    : 'REQUIRE';
K_UNIQUE     : 'UNIQUE';
K_MANDATORY  : 'MANDATORY';
K_SCALAR     : 'SCALAR';
K_OF         : 'OF';
K_ADD        : 'ADD';

// Transaction
K_BEGIN       : 'BEGIN';
K_COMMIT      : 'COMMIT';
K_ROLLBACK    : 'ROLLBACK';
K_TRANSACTION : 'TRANSACTION';

// Constraint type keywords
K_KEY        : 'KEY';
K_NODE       : 'NODE';
K_BOOLEAN    : 'BOOLEAN';
K_INTEGER    : 'INTEGER';
K_FLOAT      : 'FLOAT';
K_STRING     : 'STRING';
K_LIST       : 'LIST';
K_MAP        : 'MAP';

// Vector
K_VECTOR     : 'VECTOR';
K_OPTIONS    : 'OPTIONS';

// --- Symbols ---

EQ           : '=';
NEQ          : '<>';
LT           : '<';
GT           : '>';
LTE          : '<=';
GTE          : '>=';
PLUS         : '+';
MINUS        : '-';
MULTIPLY     : '*';
DIVIDE       : '/';
MODULO       : '%';
POWER        : '^';

LPAREN       : '(';
RPAREN       : ')';
LBRACE       : '{';
RBRACE       : '}';
LBRACK       : '[';
RBRACK       : ']';
COMMA        : ',';
DOT          : '.';
COLON        : ':';
PIPE         : '|';
DOLLAR       : '$';
RANGE        : '..';
SEMI         : ';';

// --- Literals ---

HexInteger     : '0x' [0-9a-f]+ ;
OctalInteger   : '0o' [0-7]+ ;
DecimalInteger : [0-9]+ ;

DoubleLiteral  : ([0-9]+ '.' [0-9]+ | '.' [0-9]+ | [0-9]+) ('e' [+-]? [0-9]+)? ;

ID : [a-z_] [a-z0-9_]* | '`' .*? '`' ;

StringLiteral
    : '"' ( ~('"'|'\\'|'\r'|'\n') | EscapeSequence )* '"'
    | '\'' ( ~('\''|'\\'|'\r'|'\n') | EscapeSequence )* '\''
    ;

fragment EscapeSequence
    : '\\' [btnfr"'\\]
    | '\\u' [0-9a-f] [0-9a-f] [0-9a-f] [0-9a-f]
    ;

WS           : [ \t\r\n\u000C]+ -> skip;
COMMENT      : '/*' .*? '*/'    -> skip;
LINE_COMMENT : '//' ~[\r\n]*    -> skip;