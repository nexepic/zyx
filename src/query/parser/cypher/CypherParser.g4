/*
 * CypherParser.g4
 * Based on openCypher, customized for Metrix database.
 */
parser grammar CypherParser;

options {
    tokenVocab = CypherLexer;
}

@header {
    #include "antlr4-runtime.h"
}

// --- Entry Point ---

cypher
    : statement SEMI? EOF
    ;

statement
    : administrationStatement
    | query
    ;

// --- Administration ---

administrationStatement
    : showIndexesStatement
    | dropIndexStatement
    | createIndexStatement
    ;

showIndexesStatement
    : K_SHOW K_INDEX
    ;

dropIndexStatement
    : K_DROP K_INDEX K_ON nodeLabel LPAREN propertyKeyName RPAREN
    ;

createIndexStatement
    : K_CREATE K_INDEX K_ON nodeLabel LPAREN propertyKeyName RPAREN
    ;

// --- Query Structure ---

query
    : regularQuery
    | standaloneCallStatement
    ;

regularQuery
    : singleQuery ( K_UNION K_ALL? singleQuery )*
    ;

singleQuery
    : ( readingClause* returnStatement )
    | ( readingClause* updatingClause+ returnStatement? )
    ;

// --- Clauses ---

readingClause
    : matchStatement
    | unwindStatement
    | inQueryCallStatement
    ;

updatingClause
    : createStatement
    | mergeStatement
    | deleteStatement
    | setStatement
    | removeStatement
    ;

matchStatement
    : K_OPTIONAL? K_MATCH pattern ( K_WHERE where )?
    ;

unwindStatement
    : K_UNWIND expression K_AS variable
    ;

createStatement
    : K_CREATE pattern
    ;

mergeStatement
    : K_MERGE patternPart ( K_ON ( K_MATCH | K_CREATE ) setStatement )*
    ;

setStatement
    : K_SET setItem ( COMMA setItem )*
    ;

setItem
    : propertyExpression EQ expression
    | variable EQ expression
    | variable PLUS EQ expression
    | variable nodeLabels
    ;

deleteStatement
    : K_DETACH? K_DELETE expression ( COMMA expression )*
    ;

removeStatement
    : K_REMOVE removeItem ( COMMA removeItem )*
    ;

removeItem
    : variable nodeLabels
    | propertyExpression
    ;

inQueryCallStatement
    : K_CALL explicitProcedureInvocation ( K_YIELD yieldItems )?
    ;

standaloneCallStatement
    : K_CALL ( explicitProcedureInvocation | implicitProcedureInvocation ) ( K_YIELD ( MULTIPLY | yieldItems ) )?
    ;

yieldItems
    : yieldItem ( COMMA yieldItem )* ( K_WHERE where )?
    ;

yieldItem
    : ( procedureResultField K_AS )? variable
    ;

returnStatement
    : K_RETURN projectionBody
    ;

projectionBody
    : K_DISTINCT? projectionItems orderStatement? skipStatement? limitStatement?
    ;

projectionItems
    : MULTIPLY
    | projectionItem ( COMMA projectionItem )*
    ;

projectionItem
    : expression ( K_AS variable )?
    ;

orderStatement
    : K_ORDER K_BY sortItem ( COMMA sortItem )*
    ;

skipStatement
    : K_SKIP expression
    ;

limitStatement
    : K_LIMIT expression
    ;

sortItem
    : expression ( K_ASCENDING | K_ASC | K_DESCENDING | K_DESC )?
    ;

where
    : expression
    ;

// --- Patterns ---

pattern
    : patternPart ( COMMA patternPart )*
    ;

patternPart
    : ( variable EQ )? patternElement
    ;

patternElement
    : nodePattern patternElementChain*
    | LPAREN patternElement RPAREN
    ;

patternElementChain
    : relationshipPattern nodePattern
    ;

nodePattern
    : LPAREN ( variable )? ( nodeLabels )? ( properties )? RPAREN
    ;

relationshipPattern
    : ( LT? MINUS relationshipDetail? MINUS GT? )
    | ( MINUS relationshipDetail? MINUS )
    ;

relationshipDetail
    : LBRACK ( variable )? ( relationshipTypes )? rangeLiteral? ( properties )? RBRACK
    ;

properties
    : mapLiteral
    | parameter
    ;

nodeLabels
    : nodeLabel+
    ;

nodeLabel
    : COLON labelName
    ;

relationshipTypes
    : COLON relTypeName ( PIPE COLON? relTypeName )*
    ;

rangeLiteral
    : MULTIPLY ( integerLiteral )? ( RANGE ( integerLiteral )? )?
    ;

// --- Expressions ---

expression
    : orExpression
    ;

orExpression
    : xorExpression ( K_OR xorExpression )*
    ;

xorExpression
    : andExpression ( K_XOR andExpression )*
    ;

andExpression
    : notExpression ( K_AND notExpression )*
    ;

notExpression
    : K_NOT? comparisonExpression
    ;

comparisonExpression
    : arithmeticExpression ( ( EQ | NEQ | LT | GT | LTE | GTE ) arithmeticExpression )*
    ;

arithmeticExpression
    : unaryExpression ( ( PLUS | MINUS | MULTIPLY | DIVIDE | MODULO ) unaryExpression )*
    ;

unaryExpression
    : ( PLUS | MINUS )? atom
    ;

atom
    : literal
    | parameter
    | K_COUNT LPAREN MULTIPLY RPAREN
    | functionInvocation
    | variable
    | LPAREN expression RPAREN
    | listLiteral
    ;

propertyExpression
    : atom ( DOT propertyKeyName )+
    ;

functionInvocation
    : functionName LPAREN ( K_DISTINCT )? ( expression ( COMMA expression )* )? RPAREN
    ;

explicitProcedureInvocation
    : procedureName LPAREN ( expression ( COMMA expression )* )? RPAREN
    ;

implicitProcedureInvocation
    : procedureName
    ;

// --- Basic Elements ---

variable : symbolicName ;
labelName : schemaName ;
relTypeName : schemaName ;
propertyKeyName : schemaName ;
procedureName : namespace symbolicName ;
procedureResultField : symbolicName ;
functionName : namespace symbolicName ;
namespace : ( symbolicName DOT )* ;

schemaName
    : symbolicName
    | K_INDEX | K_ON | K_MATCH | K_CREATE | K_WHERE | K_RETURN
    ;

symbolicName
    : ID
    | K_COUNT | K_FILTER | K_EXTRACT | K_ANY | K_NONE | K_SINGLE
    ;

literal
    : booleanLiteral
    | K_NULL
    | numberLiteral
    | StringLiteral
    | mapLiteral
    ;

booleanLiteral : K_TRUE | K_FALSE ;

numberLiteral : DoubleLiteral | integerLiteral ;

integerLiteral : DecimalInteger | HexInteger | OctalInteger ;

mapLiteral
    : LBRACE ( propertyKeyName COLON expression ( COMMA propertyKeyName COLON expression )* )? RBRACE
    ;

listLiteral
    : LBRACK ( expression ( COMMA expression )* )? RBRACK
    ;

parameter
    : DOLLAR ( symbolicName | DecimalInteger )
    ;