benchmark_exe = executable('zyx-bench',
						   sources : ['main.cpp'],

						   # Allow including "zyx/zyx.hpp" from ../include
						   include_directories : [inc_dirs, include_directories('../../include')],

						   # Link core logic
						   link_with : db_lib,

						   # Same dependencies as CLI
						   dependencies : [zlib_dep, boost_dep, cli11_dep, antlr4_dep, cypher_dep],

						   install : false
)

# Register concurrency benchmark as a test so it can run via meson test
test('benchmark_concurrency',
	 benchmark_exe,
	 args : ['conc'],
	 timeout : 600,
	 is_parallel : false,
	 suite : 'benchmark'
)