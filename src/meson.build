subdir('parser')

# Find source files, but we need to exclude the parser directory since it is already handled as a separate library
python = import('python').find_installation()
find_result = run_command(python, '-c', '''
import os
import sys

# Paths to exclude (relative to src)
EXCLUDE_DIRS = ['parser']

base_dir = sys.argv[1]
for root, dirs, files in os.walk(base_dir):
	# Compute the current root relative to base_dir
	rel_root = os.path.relpath(root, base_dir)

	# Skip if the current directory starts with an excluded path
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
		# Clean up the path string
		rel_path = file.replace(meson.current_source_dir() + '/', '')
		# Handle Windows path separators
		rel_path = rel_path.replace(meson.current_source_dir() + '\\', '')
		db_src += files(rel_path)
	endif
endforeach

# Define the main library (Metrix Core)
db_lib = static_library('metrix',
						sources : db_src,
						include_directories : inc_dirs,
						dependencies : [
							zlib_dep,
							boost_dep,
							antlr4_dep, # Core may need to reference Parser interfaces
							cypher_dep  # Link the Parser library built earlier!
						],
						install : false
)

# Export dependencies
db_dep = declare_dependency(
	include_directories : inc_dirs,
	link_with : db_lib,
	dependencies : [cypher_dep] # Pass through dependencies
)