// Undo Python Token.h macro pollution that conflicts with ANTLR4 parser identifiers.
// Python's Token.h defines SEMI, COMMA, COLON, MINUS etc. as integer constants,
// which break ANTLR4 generated parser method names like SEMI(), COMMA(), etc.
#ifdef SEMI
#undef SEMI
#endif
#ifdef COMMA
#undef COMMA
#endif
#ifdef COLON
#undef COLON
#endif
#ifdef MINUS
#undef MINUS
#endif
#ifdef PLUS
#undef PLUS
#endif
#ifdef STAR
#undef STAR
#endif
#ifdef SLASH
#undef SLASH
#endif
#ifdef DOT
#undef DOT
#endif
#ifdef AT
#undef AT
#endif
#ifdef LPAR
#undef LPAR
#endif
#ifdef RPAR
#undef RPAR
#endif
#ifdef LSQB
#undef LSQB
#endif
#ifdef RSQB
#undef RSQB
#endif
