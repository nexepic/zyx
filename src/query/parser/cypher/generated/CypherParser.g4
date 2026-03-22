/*
 * CypherParser.g4
 * Based on openCypher, customized for ZYX database.
 */
parser grammar CypherParser;

options {
    tokenVocab = CypherLexer;
}

@header {
    #include "antlr4-runtime.h"
}

// ============================================================================
// ENTRY POINT
// ============================================================================

cypher
    : statement SEMI? EOF
    ;

statement
    : query
    | administrationStatement
    ;

// ============================================================================
// ADMINISTRATION (DDL)
// ============================================================================

administrationStatement
    : showIndexesStatement
    | dropIndexStatement
    | createIndexStatement
    ;

showIndexesStatement
    : K_SHOW K_INDEX
    ;

// Use LPAREN propertyKeyName RPAREN to explicitly match :Label(prop)
dropIndexStatement
    : K_DROP K_INDEX symbolicName                                     # DropIndexByName
    | K_DROP K_INDEX K_ON nodeLabel LPAREN propertyKeyName RPAREN     # DropIndexByLabel
    ;

createIndexStatement
    : K_CREATE K_INDEX symbolicName? K_FOR nodePattern K_ON LPAREN propertyExpression RPAREN # CreateIndexByPattern
    | K_CREATE K_INDEX symbolicName? K_ON nodeLabel LPAREN propertyKeyName RPAREN            # CreateIndexByLabel
    // Vector Index
	| K_CREATE K_VECTOR K_INDEX symbolicName? K_ON nodeLabel LPAREN propertyKeyName RPAREN (K_OPTIONS mapLiteral)? # CreateVectorIndex
	;

// ============================================================================
// QUERY STRUCTURE
// ============================================================================

query
    : regularQuery
    | standaloneCallStatement
    ;

regularQuery
    : singleQuery ( K_UNION K_ALL? singleQuery )*
    ;

// Support WITH clause as intermediate projection (openCypher compliant)
// WITH can appear: 1) Multiple times at start, 2) After readingClauses, 3) Before more readingClauses/updatingClauses
// This allows queries like: WITH n WITH n.value RETURN n.value
singleQuery
    : ( withClause+ readingClause* returnStatement )
    | ( withClause+ readingClause* updatingClause+ returnStatement? )
    | ( readingClause* withClause+ readingClause* returnStatement )
    | ( readingClause* withClause+ readingClause* updatingClause+ returnStatement? )
    | ( readingClause* returnStatement )
    | ( readingClause* updatingClause+ returnStatement? )
    ;

// ============================================================================
// CLAUSES
// ============================================================================

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

// --- Projection Clauses (WITH and RETURN) ---

withClause
    : K_WITH projectionBody ( K_WHERE where )?
    ;

// --- Reading ---

matchStatement
    : K_OPTIONAL? K_MATCH pattern ( K_WHERE where )?
    ;

unwindStatement
    : K_UNWIND expression K_AS variable
    ;

inQueryCallStatement
    : K_CALL explicitProcedureInvocation ( K_YIELD yieldItems )?
    ;

// --- Updating ---

createStatement
    : K_CREATE pattern
    ;

mergeStatement
    : K_MERGE patternPart ( K_ON ( K_MATCH | K_CREATE ) setStatement )*
    ;

setStatement
    : K_SET setItem ( COMMA setItem )*
    ;

// setItem uses propertyExpression to ensure we assign to a property, not just any expression
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

// --- Return & Call ---

returnStatement
    : K_RETURN projectionBody
    ;

standaloneCallStatement
    : K_CALL ( explicitProcedureInvocation | implicitProcedureInvocation ) ( K_YIELD ( MULTIPLY | yieldItems ) )?
    ;

// ============================================================================
// SUB-CLAUSE STRUCTURES
// ============================================================================

yieldItems
    : yieldItem ( COMMA yieldItem )* ( K_WHERE where )?
    ;

yieldItem
    : ( procedureResultField K_AS )? variable
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

// ============================================================================
// PATTERNS
// ============================================================================

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

// ============================================================================
// EXPRESSIONS (Official Structure: Atom + Suffixes)
// ============================================================================

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
    : arithmeticExpression ( ( EQ | NEQ | LT | GT | LTE | GTE | K_IN
        | K_STARTS K_WITH | K_ENDS K_WITH | K_CONTAINS ) arithmeticExpression )* ( K_IS K_NOT? K_NULL )?
    ;

arithmeticExpression
    : powerExpression ( ( PLUS | MINUS | MULTIPLY | DIVIDE | MODULO ) powerExpression )*
    ;

// Power expression has higher precedence than multiply/divide
powerExpression
    : unaryExpression ( POWER unaryExpression )*
    ;

// Official structure: An expression is an Atom followed by optional accessors.
// This allows 'n' (atom) to become 'n.age' (atom + accessor) validly.
unaryExpression
    : ( PLUS | MINUS )? atom ( accessor )*
    ;

// Accessor handles property lookup (.prop), list indexing ([0]), or list slicing ([0..2])
accessor
    : DOT propertyKeyName
    | LBRACK (expression)? (RANGE (expression)?)? RBRACK
    ;

atom
    : literal
    | parameter
    | K_COUNT LPAREN MULTIPLY RPAREN
    | quantifierExpression
    | reduceExpression
    | existsExpression
    | functionInvocation
    | caseExpression
    | variable
    | LPAREN expression RPAREN
    | listLiteral
    | listComprehension
    | patternComprehension
    ;

// Quantifier functions: all(x IN list WHERE cond), any(...), none(...), single(...)
quantifierExpression
    : (K_ALL | K_ANY | K_NONE | K_SINGLE) LPAREN variable K_IN expression K_WHERE expression RPAREN
    ;

// EXISTS pattern expression: exists((n)-[:KNOWS]->()) or EXISTS { (n)-[:KNOWS]->() }
existsExpression
    : K_EXISTS LPAREN patternElement (K_WHERE expression)? RPAREN
    ;

// CASE expression support
caseExpression
    : K_CASE (expression)? (K_WHEN expression K_THEN expression)+ (K_ELSE expression)? K_END
    ;

// Helper rule for SET/REMOVE clauses that requires a property (not just any atom)
// Matches: atom (accessor)+
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

// ============================================================================
// BASIC ELEMENTS
// ============================================================================

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
    // Keywords from Standard Cypher
    | K_CALL | K_YIELD | K_CREATE | K_DELETE | K_DETACH | K_EXISTS
    | K_LIMIT | K_MATCH | K_MERGE | K_OPTIONAL | K_ORDER | K_BY | K_SKIP
    | K_ASCENDING | K_ASC | K_DESCENDING | K_DESC | K_REMOVE | K_RETURN
    | K_SET | K_WHERE | K_WITH | K_UNION | K_UNWIND | K_AND | K_AS
    | K_CONTAINS | K_DISTINCT | K_ENDS | K_IN | K_IS | K_NOT | K_OR
    | K_STARTS | K_XOR | K_FALSE | K_TRUE | K_NULL
    | K_CASE | K_WHEN | K_THEN | K_ELSE | K_END
    | K_COUNT | K_FILTER | K_EXTRACT | K_ANY | K_NONE | K_SINGLE | K_ALL | K_REDUCE
    // Admin Keywords
    | K_INDEX | K_ON | K_SHOW | K_DROP
    | K_FOR | K_CONSTRAINT | K_DO | K_REQUIRE | K_UNIQUE | K_MANDATORY
    | K_SCALAR | K_OF | K_ADD
    // Vector Keywords
    | K_VECTOR | K_OPTIONS
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

// List comprehension: [x IN list WHERE condition | expression]
listComprehension
    : LBRACK variable K_IN expression (K_WHERE whereExpression=expression)? (PIPE mapExpression=expression)? RBRACK
    ;

// REDUCE function: reduce(accumulator = initial, variable IN list | expression)
reduceExpression
    : K_REDUCE LPAREN variable EQ expression COMMA variable K_IN expression PIPE expression RPAREN
    ;

// Pattern comprehension: [(n)-[:REL]->(m) | m.name]
patternComprehension
    : LBRACK patternElement (K_WHERE expression)? PIPE expression RBRACK
    ;