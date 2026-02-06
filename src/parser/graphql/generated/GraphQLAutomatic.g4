/*
 * GraphQL ANTLR4 Grammar for QLever
 *
 * Based on the GraphQL specification (October 2021)
 * https://spec.graphql.org/October2021/
 *
 * This grammar covers the subset of GraphQL needed for QLever's use case:
 * - Queries (read operations)
 * - Fragments (named and inline)
 * - Variables
 * - Directives (@skip, @include)
 * - Introspection queries
 *
 * Author: QLever GraphQL Extension
 * License: BSD 3-Clause (consistent with QLever)
 */

grammar GraphQLAutomatic;

// =============================================================================
// PARSER RULES
// =============================================================================

// -----------------------------------------------------------------------------
// Document
// -----------------------------------------------------------------------------

document
    : definition+ EOF
    ;

definition
    : executableDefinition
    ;

executableDefinition
    : operationDefinition
    | fragmentDefinition
    ;

// -----------------------------------------------------------------------------
// Operations
// -----------------------------------------------------------------------------

operationDefinition
    : operationType name? variableDefinitions? directives? selectionSet
    | selectionSet
    ;

operationType
    : QUERY
    | MUTATION
    | SUBSCRIPTION
    ;

// -----------------------------------------------------------------------------
// Selection Sets
// -----------------------------------------------------------------------------

selectionSet
    : '{' selection+ '}'
    ;

selection
    : field
    | fragmentSpread
    | inlineFragment
    ;

// -----------------------------------------------------------------------------
// Fields
// -----------------------------------------------------------------------------

field
    : alias? name arguments? directives? selectionSet?
    ;

alias
    : name ':'
    ;

// -----------------------------------------------------------------------------
// Arguments
// -----------------------------------------------------------------------------

arguments
    : '(' argument+ ')'
    ;

argument
    : name ':' value
    ;

// -----------------------------------------------------------------------------
// Fragments
// -----------------------------------------------------------------------------

fragmentSpread
    : '...' fragmentName directives?
    ;

inlineFragment
    : '...' typeCondition? directives? selectionSet
    ;

fragmentDefinition
    : FRAGMENT fragmentName typeCondition directives? selectionSet
    ;

fragmentName
    : name  /* but not 'on' */
    ;

typeCondition
    : ON namedType
    ;

// -----------------------------------------------------------------------------
// Values
// -----------------------------------------------------------------------------

value
    : variable
    | intValue
    | floatValue
    | stringValue
    | booleanValue
    | nullValue
    | enumValue
    | listValue
    | objectValue
    ;

intValue
    : INT_VALUE
    ;

floatValue
    : FLOAT_VALUE
    ;

stringValue
    : STRING_VALUE
    | BLOCK_STRING_VALUE
    ;

booleanValue
    : TRUE
    | FALSE
    ;

nullValue
    : NULL
    ;

enumValue
    : name  /* but not true, false, or null */
    ;

listValue
    : '[' ']'
    | '[' value+ ']'
    ;

objectValue
    : '{' '}'
    | '{' objectField+ '}'
    ;

objectField
    : name ':' value
    ;

// -----------------------------------------------------------------------------
// Variables
// -----------------------------------------------------------------------------

variable
    : '$' name
    ;

variableDefinitions
    : '(' variableDefinition+ ')'
    ;

variableDefinition
    : variable ':' type defaultValue? directives?
    ;

defaultValue
    : '=' value
    ;

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------

type
    : namedType
    | listType
    | nonNullType
    ;

namedType
    : name
    ;

listType
    : '[' type ']'
    ;

nonNullType
    : namedType '!'
    | listType '!'
    ;

// -----------------------------------------------------------------------------
// Directives
// -----------------------------------------------------------------------------

directives
    : directive+
    ;

directive
    : '@' name arguments?
    ;

// -----------------------------------------------------------------------------
// Name
// -----------------------------------------------------------------------------

name
    : NAME
    | FRAGMENT
    | QUERY
    | MUTATION
    | SUBSCRIPTION
    | ON
    | TRUE
    | FALSE
    | NULL
    ;

// =============================================================================
// LEXER RULES
// =============================================================================

// -----------------------------------------------------------------------------
// Keywords
// -----------------------------------------------------------------------------

QUERY       : Q U E R Y ;
MUTATION    : M U T A T I O N ;
SUBSCRIPTION: S U B S C R I P T I O N ;
FRAGMENT    : F R A G M E N T ;
ON          : O N ;
TRUE        : T R U E ;
FALSE       : F A L S E ;
NULL        : N U L L ;

// -----------------------------------------------------------------------------
// Punctuators
// -----------------------------------------------------------------------------

// Note: Single-character punctuators are defined inline in parser rules:
// { } ( ) [ ] : , ! $ @ = ... |

SPREAD
    : '...'
    ;

// -----------------------------------------------------------------------------
// Names
// -----------------------------------------------------------------------------

NAME
    : NAME_START NAME_CONTINUE*
    ;

fragment NAME_START
    : [a-zA-Z_]
    ;

fragment NAME_CONTINUE
    : [a-zA-Z0-9_]
    ;

// -----------------------------------------------------------------------------
// Int Value
// -----------------------------------------------------------------------------

INT_VALUE
    : INTEGER_PART
    ;

fragment INTEGER_PART
    : NEGATIVE_SIGN? '0'
    | NEGATIVE_SIGN? NON_ZERO_DIGIT DIGIT*
    ;

fragment NEGATIVE_SIGN
    : '-'
    ;

fragment NON_ZERO_DIGIT
    : [1-9]
    ;

fragment DIGIT
    : [0-9]
    ;

// -----------------------------------------------------------------------------
// Float Value
// -----------------------------------------------------------------------------

FLOAT_VALUE
    : INTEGER_PART FRACTIONAL_PART
    | INTEGER_PART EXPONENT_PART
    | INTEGER_PART FRACTIONAL_PART EXPONENT_PART
    ;

fragment FRACTIONAL_PART
    : '.' DIGIT+
    ;

fragment EXPONENT_PART
    : EXPONENT_INDICATOR SIGN? DIGIT+
    ;

fragment EXPONENT_INDICATOR
    : [eE]
    ;

fragment SIGN
    : [+-]
    ;

// -----------------------------------------------------------------------------
// String Value
// -----------------------------------------------------------------------------

STRING_VALUE
    : '"' STRING_CHARACTER* '"'
    ;

fragment STRING_CHARACTER
    : ~["\\\u000A\u000D]
    | ESCAPED_CHARACTER
    | ESCAPED_UNICODE
    ;

fragment ESCAPED_CHARACTER
    : '\\' ["\\/bfnrt]
    ;

fragment ESCAPED_UNICODE
    : '\\u' HEX HEX HEX HEX
    ;

fragment HEX
    : [0-9A-Fa-f]
    ;

// -----------------------------------------------------------------------------
// Block String Value
// -----------------------------------------------------------------------------

BLOCK_STRING_VALUE
    : '"""' BLOCK_STRING_CHARACTER* '"""'
    ;

fragment BLOCK_STRING_CHARACTER
    : ~["\\]
    | '"' ~["]
    | '"' '"' ~["]
    | '\\"""'
    | ESCAPED_CHARACTER
    ;

// -----------------------------------------------------------------------------
// Comments
// -----------------------------------------------------------------------------

COMMENT
    : '#' ~[\r\n]* -> skip
    ;

// -----------------------------------------------------------------------------
// Whitespace and Line Terminators
// -----------------------------------------------------------------------------

// GraphQL ignores whitespace and line terminators (except inside strings)
// UnicodeBOM, WhiteSpace, LineTerminator, and Comma are all insignificant
WS
    : ( UNICODE_BOM
      | WHITE_SPACE
      | LINE_TERMINATOR
      | ','  /* Comma is treated as whitespace in GraphQL */
      )+ -> skip
    ;

fragment UNICODE_BOM
    : '\uFEFF'
    ;

fragment WHITE_SPACE
    : [\t ]
    ;

fragment LINE_TERMINATOR
    : [\n\r]
    ;

// -----------------------------------------------------------------------------
// Case-Insensitive Letter Fragments (following QLever SPARQL grammar style)
// -----------------------------------------------------------------------------

fragment A : [aA] ;
fragment B : [bB] ;
fragment C : [cC] ;
fragment D : [dD] ;
fragment E : [eE] ;
fragment F : [fF] ;
fragment G : [gG] ;
fragment H : [hH] ;
fragment I : [iI] ;
fragment J : [jJ] ;
fragment K : [kK] ;
fragment L : [lL] ;
fragment M : [mM] ;
fragment N : [nN] ;
fragment O : [oO] ;
fragment P : [pP] ;
fragment Q : [qQ] ;
fragment R : [rR] ;
fragment S : [sS] ;
fragment T : [tT] ;
fragment U : [uU] ;
fragment V : [vV] ;
fragment W : [wW] ;
fragment X : [xX] ;
fragment Y : [yY] ;
fragment Z : [zZ] ;
