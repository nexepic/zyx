python = import('python').find_installation()
find_result = run_command(python, '-c', '''
import os
import sys
for root, dirs, files in os.walk(sys.argv[1]):
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

# Static library - using dependencies declared in root meson.build
db_lib = static_library('metrix',
						sources : db_src,
						include_directories : inc_dirs,
						dependencies : [zlib_dep, boost_dep],
						install : false
)

# Export library dependency for other parts of the project
db_dep = declare_dependency(
	include_directories : inc_dirs,
	link_with : db_lib
)