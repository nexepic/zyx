benchmark_exe = executable('metrix-bench',
						   sources : ['main.cpp'],

						   # Allow including "metrix/metrix.hpp" from ../include
						   include_directories : [inc_dirs, include_directories('../../include')],

						   # Link core logic
						   link_with : db_lib,

						   # Same dependencies as CLI
						   dependencies : [zlib_dep, boost_dep, cli11_dep, antlr4_dep, cypher_dep],

						   install : false
)