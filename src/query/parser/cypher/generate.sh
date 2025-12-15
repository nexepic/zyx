#!/bin/bash
# Place antlr-4.13.0-complete.jar in a known tools directory
ANTLR_JAR="antlr-4.13.1-complete.jar"
OUTPUT_DIR="generated"

if [ ! -f "$ANTLR_JAR" ]; then
    echo "Downloading $ANTLR_JAR..."
    curl -O https://www.antlr.org/download/$ANTLR_JAR
else
    echo "$ANTLR_JAR already exists, skipping download."
fi

# Create the output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

echo "Generating ANTLR4 C++ sources into '$OUTPUT_DIR'..."

# Compile Lexer first (Parser depends on it)
java -jar "$ANTLR_JAR" \
    -Dlanguage=Cpp \
    -package graph::parser::cypher \
    -o "$OUTPUT_DIR" \
    CypherLexer.g4

# Compile Parser (Visitor option goes here)
java -jar "$ANTLR_JAR" \
    -Dlanguage=Cpp \
    -visitor \
    -no-listener \
    -package graph::parser::cypher \
    -o "$OUTPUT_DIR" \
    CypherParser.g4

echo "Done."