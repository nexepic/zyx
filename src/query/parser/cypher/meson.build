# Path to the generated sources
generated_dir = 'generated'

# [MODIFIED] Update file names to match ANTLR output for split grammars
cypher_generated = files(
	generated_dir / 'CypherLexer.cpp',
	generated_dir / 'CypherParser.cpp',
	generated_dir / 'CypherParserBaseVisitor.cpp', # Was CypherBaseVisitor.cpp
	generated_dir / 'CypherParserVisitor.cpp'      # Was CypherVisitor.cpp
)

# List your implementation files
cypher_impl = files(
	'CypherParserImpl.cpp',
	'CypherToPlanVisitor.cpp'
)

# Create a library
cypher_lib = static_library('cypher_parser',
							sources : [cypher_generated, cypher_impl],
							# Ensure internal headers can find generated files
							include_directories : [inc_dirs, include_directories('.', generated_dir)],
							dependencies : [antlr4_dep] # Runtime dependency only
)

# Declare dependency for the main engine to link
cypher_dep = declare_dependency(
	link_with : cypher_lib,
	# Export generated dir so main engine can include "CypherParser.h" if needed
	include_directories : include_directories('.', generated_dir)
)