#!/bin/bash
# Generate ANTLR4 C++ parser from Cypher grammar files
#
# Usage: cd src/query/parser/cypher && bash generate.sh
#
# This script:
# 1. Downloads ANTLR4 JAR if not present
# 2. Generates C++ lexer and parser from .g4 grammar files
# 3. Places output in the generated/ directory

set -e

# Find Java runtime - try multiple methods
JAVA_CMD=""

# Method 1: Use JAVA_HOME if set
if [ -n "$JAVA_HOME" ]; then
    if [ -f "$JAVA_HOME/bin/java" ]; then
        JAVA_CMD="$JAVA_HOME/bin/java"
    fi
fi

# Method 2: Try common Homebrew paths for macOS
if [ -z "$JAVA_CMD" ]; then
    for jvm in /opt/homebrew/Cellar/openjdk/*; do
        if [ -f "$jvm/libexec/openjdk.jdk/Contents/Home/bin/java" ]; then
            JAVA_CMD="$jvm/libexec/openjdk.jdk/Contents/Home/bin/java"
            break
        fi
    done
fi

# Method 3: Try system java
if [ -z "$JAVA_CMD" ]; then
    if command -v java >/dev/null 2>&1; then
        JAVA_CMD="java"
    fi
fi

# Check if we found Java
if [ -z "$JAVA_CMD" ]; then
    echo "Error: Java runtime not found."
    echo ""
    echo "Please install Java or set JAVA_HOME:"
    echo "  macOS: brew install openjdk"
    echo "  Linux: sudo apt-get install default-jdk"
    echo ""
    exit 1
fi

if [ ! -f "$JAVA_CMD" ]; then
    echo "Error: Java command not found at: $JAVA_CMD"
    exit 1
fi

echo "Using Java: $JAVA_CMD"

# ANTLR4 configuration
ANTLR_VERSION="4.13.1"
ANTLR_JAR="antlr-${ANTLR_VERSION}-complete.jar"
OUTPUT_DIR="generated"

# Download ANTLR4 if not present
if [ ! -f "$ANTLR_JAR" ]; then
    echo "Downloading $ANTLR_JAR..."
    curl -sSL -o "$ANTLR_JAR" "https://www.antlr.org/download/$ANTLR_JAR"
    echo "Downloaded $ANTLR_JAR"
else
    echo "$ANTLR_JAR already exists, skipping download."
fi

# Create the output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

echo ""
echo "=========================================="
echo "Generating ANTLR4 C++ sources"
echo "=========================================="
echo "Grammar files: CypherLexer.g4, CypherParser.g4"
echo "Output directory: $OUTPUT_DIR"
echo ""

# Generate Lexer first (Parser depends on it)
echo "Generating Lexer..."
"$JAVA_CMD" -jar "$ANTLR_JAR" \
    -Dlanguage=Cpp \
    CypherLexer.g4

# Generate Parser with visitor
echo "Generating Parser..."
"$JAVA_CMD" -jar "$ANTLR_JAR" \
    -Dlanguage=Cpp \
    -visitor \
    -no-listener \
    CypherParser.g4

# Move generated files to the output directory
echo ""
echo "Moving generated files to $OUTPUT_DIR/..."
mv -f CypherLexer.* CypherParser.* CypherParserBaseVisitor.* CypherParserVisitor.* "$OUTPUT_DIR/" 2>/dev/null || true

echo ""
echo "=========================================="
echo "Generation complete!"
echo "=========================================="
echo ""
echo "Generated files:"
ls -1 "$OUTPUT_DIR"/Cypher*.cpp "$OUTPUT_DIR"/Cypher*.h 2>/dev/null | sed 's|^|  - |'
echo ""
echo "Next steps:"
echo "  1. Review the generated files in $OUTPUT_DIR/"
echo "  2. Commit the changes to git"
echo "  3. Run tests to verify everything works"
echo ""