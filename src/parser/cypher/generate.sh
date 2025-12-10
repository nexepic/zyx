#!/bin/bash
# Place antlr-4.13.0-complete.jar in a known tools directory
ANTLR_JAR="antlr-4.13.1-complete.jar"

if [ ! -f "$ANTLR_JAR" ]; then
    echo "Downloading $ANTLR_JAR..."
    curl -O https://www.antlr.org/download/$ANTLR_JAR
else
    echo "$ANTLR_JAR already exists, skipping download."
fi

echo "Generating ANTLR4 C++ sources..."
java -jar "$ANTLR_JAR" \
    -Dlanguage=Cpp \
    -visitor \
    -no-listener \
    -package graph::parser::cypher \
    -o . \
    Cypher.g4
echo "Done."