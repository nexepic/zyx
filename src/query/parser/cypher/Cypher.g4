grammar Cypher;

options {
    language = Cpp;
}

@header {
    #include "antlr4-runtime.h"
}

// --- Parser Rules ---

query
    : statement EOF
    ;

statement
    : matchStatement
    | createStatement
    | administrationStatement
    | callStatement
    ;

callStatement
    : K_CALL procedureName '(' argumentList? ')'
    ;

procedureName
    : symbolicName ('.' symbolicName)*
    ;

symbolicName
    : ID
    | K_INDEX | K_ON | K_MATCH | K_CREATE | K_WHERE | K_RETURN
    | K_SHOW | K_DROP | K_CALL | K_AS
    ;

argumentList
    : expression (',' expression)*
    ;

administrationStatement
    : showIndexesStatement
    | dropIndexStatement
    ;

// SHOW INDEXES
showIndexesStatement
    : K_SHOW (K_INDEX | K_INDEXES)
    ;

// DROP INDEX ON :Label(prop)
dropIndexStatement
    : K_DROP K_INDEX K_ON ':' label=ID '(' property=ID ')'
    ;

// MATCH uses 'pattern' now, allowing (a)-[r]->(b)
matchStatement
    : K_MATCH pattern (K_WHERE whereClause)? (returnClause)?
    ;

returnClause
    : K_RETURN returnBody
    ;

returnBody
    : returnItem (',' returnItem)*
    | '*'
    ;

returnItem
    : expression (K_AS variable=ID)?
    ;

// [Modified] General Expression rule (Simple version for now)
expression
    : propertyExpression
    | ID // Variable name like 'n'
    | literal
    ;

// CREATE uses 'pattern' now
createStatement
    : K_CREATE ( indexDefinition | pattern )
    ;

indexDefinition
    : K_INDEX K_ON ':' label=ID '(' property=ID ')'
    ;

// --- Core Pattern Structure ---

// A pattern is a comma-separated list of paths (e.g. MATCH (a), (b))
pattern
    : patternPart (',' patternPart)*
    ;

// A single path: A start node followed by a chain of relationships
// e.g., (a)-[r]->(b)-[k]->(c)
patternPart
    : nodePattern patternElementChain*
    ;

// The chain link: Relationship + Target Node
patternElementChain
    : relationshipPattern nodePattern
    ;

// Relationship syntax: -[r:Label]-> or <-[r:Label]- or -[r:Label]-
relationshipPattern
    : (LT? '-') relationshipDetail ('-' GT?)
    ;

// The inside of the relationship: [r:KNOWS {date:...}]
relationshipDetail
    : '[' (variable=ID)? (':' label=ID)? (mapLiteral)? ']'
    ;

// Node syntax: (n:Person {name:...})
nodePattern
    : '(' (variable=ID)? (':' label=ID)? (mapLiteral)? ')'
    ;

// --- Expressions ---

whereClause
    : propertyExpression op=(EQ | NEQ | GT | LT | GTE | LTE) literal
    ;

propertyExpression
    : ID '.' ID
    ;

mapLiteral
    : LBRACE (mapEntry (',' mapEntry)*)? RBRACE
    ;

mapEntry
    : key=ID ':' literal
    ;

literal
    : STRING
    | INTEGER
    | DECIMAL
    | BOOLEAN
    | NULL_LITERAL
    ;

// --- Lexer Rules ---

K_INDEX  : 'INDEX' | 'index';
K_SHOW   : 'SHOW' | 'show';
K_DROP   : 'DROP' | 'drop';
K_INDEXES: 'INDEXES' | 'indexes';
K_ON     : 'ON' | 'on';
K_MATCH  : 'MATCH' | 'match';
K_CREATE : 'CREATE' | 'create';
K_WHERE  : 'WHERE' | 'where';
K_RETURN : 'RETURN' | 'return';
K_AS     : 'AS' | 'as';
K_CALL   : 'CALL' | 'call';

NULL_LITERAL : 'NULL' | 'null';
BOOLEAN      : 'TRUE' | 'true' | 'FALSE' | 'false';

ID : [a-zA-Z_] [a-zA-Z0-9_]* ;

STRING
    : '"' (~'"' | '\\"')* '"'
    | '\'' (~'\'' | '\\\'')* '\''
    ;

INTEGER : [0-9]+ ;
DECIMAL : [0-9]+ '.' [0-9]+ ;

WS : [ \t\r\n]+ -> skip ;

LPAREN : '(' ;
RPAREN : ')' ;
LBRACE : '{' ;
RBRACE : '}' ;
COLON  : ':' ;
COMMA  : ',' ;
DOT    : '.' ;

// Symbols for arrows
LT : '<' ;
GT : '>' ;
EQ  : '=' ;
NEQ : '<>' ;
GTE : '>=' ;
LTE : '<=' ;