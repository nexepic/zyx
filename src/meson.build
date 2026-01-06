subdir('query/parser')

# ==============================================================================
# 1. Source Discovery
# ==============================================================================
# Use Python to recursively find all .cpp files in src/ (excluding parser)
python = import('python').find_installation()
find_result = run_command(python, '-c', '''
import os
import sys

# Paths to exclude (relative to src)
EXCLUDE_DIRS = ['query/parser']

base_dir = sys.argv[1]
for root, dirs, files in os.walk(base_dir):
	rel_root = os.path.relpath(root, base_dir)

	skip = False
	for exclude in EXCLUDE_DIRS:
		if rel_root == exclude or rel_root.startswith(exclude + os.sep):
			skip = True
			break
	if skip:
		continue

	for file in files:
		if file.endswith(".cpp"):
			print(os.path.join(root, file))
''', meson.current_source_dir(), check : true)

cpp_files = find_result.stdout().strip().split('\n')

db_src = []
foreach file : cpp_files
	if file != ''
		rel_path = file.replace(meson.current_source_dir() + '/', '')
		rel_path = rel_path.replace(meson.current_source_dir() + '\\', '')
		db_src += files(rel_path)
	endif
endforeach

# ==============================================================================
# 2. Library Definitions
# ==============================================================================

# Internal Static Library (Used for Unit Tests & CLI)
# Renamed to 'metrix_core' to avoid name collision with shared lib
db_lib = static_library('metrix_core',
						sources : db_src,
						include_directories : inc_dirs,
						dependencies : [
							zlib_dep,
							boost_dep,
							cli11_dep,
							antlr4_dep,
							cypher_dep
						],
						install : false
)

# Export dependency for internal use
db_dep = declare_dependency(
	include_directories : inc_dirs,
	link_with : db_lib,
	dependencies : [cypher_dep]
)

# ==============================================================================
# 3. Public SDK Generation (Shared Library)
# ==============================================================================

# Define headers that will be installed to /usr/local/include/metrix/
# Ensure filenames match your actual files (metrixdb.hpp)
public_headers = [
	'../include/metrix/metrix.hpp', # Main C++ API
	'../include/metrix/metrix_c_api.h', # Main C API
	'../include/metrix/value.hpp'      # Data types
]

# Build Shared Library (The Product)
# Named 'metrixdb' -> produces libmetrix.dylib / libmetrix.so
libmetrix = shared_library('metrix',
						   sources : db_src, # db_src already includes api/CApi.cpp via recursive search
						   include_directories : inc_dirs,
						   dependencies : [zlib_dep, boost_dep, cli11_dep, antlr4_dep, cypher_dep],
						   install : true,
						   install_dir : get_option('libdir')
)

# Install Headers
install_headers(public_headers, subdir : 'metrix')

# Generate pkg-config file
pkg = import('pkgconfig')
pkg.generate(libmetrix,
			 name : 'Metrix',
			 description : 'High-performance Graph Database Engine',
			 version : '0.1.0',
			 subdirs : 'metrix'
)

# Declare dependency for external-like tests (linking against the .dylib)
metrix_dep = declare_dependency(
	link_with : libmetrix,
	include_directories : include_directories('../include')
)