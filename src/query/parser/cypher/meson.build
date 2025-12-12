# Path to the generated sources
generated_dir = 'generated'

# List the generated files explicitly
cypher_generated = files(
	generated_dir / 'CypherLexer.cpp',
	generated_dir / 'CypherParser.cpp',
	generated_dir / 'CypherBaseVisitor.cpp',
	generated_dir / 'CypherVisitor.cpp'
)

# List your implementation files
cypher_impl = files(
	'CypherParserImpl.cpp',
	'CypherToPlanVisitor.cpp'
)

# Create a library
cypher_lib = static_library('cypher_parser',
							sources : [cypher_generated, cypher_impl],
							include_directories : [inc_dirs, include_directories('.', generated_dir)],
							dependencies : [antlr4_dep] # Runtime dependency only
)

# Declare dependency for the main engine to link
cypher_dep = declare_dependency(
	link_with : cypher_lib,
	include_directories : include_directories('.', generated_dir)
)