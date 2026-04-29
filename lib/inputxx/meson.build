python = import('python').find_installation()
find_result = run_command(python, '-c', '''
import os
import sys
base_dir = sys.argv[1]
for root, dirs, files in os.walk(base_dir):
	for file in files:
		if file.endswith(".cpp"):
			print(os.path.relpath(os.path.join(root, file), base_dir))
''', meson.current_source_dir() / 'src', check : true)

inputxx_src = []
foreach file : find_result.stdout().strip().split('\n')
	if file != ''
		inputxx_src += files('src' / file)
	endif
endforeach

inputxx_inc = include_directories('include')

inputxx_lib = static_library('inputxx',
	sources : inputxx_src,
	include_directories : inputxx_inc,
	install : false
)

inputxx_dep = declare_dependency(
	include_directories : inputxx_inc,
	link_with : inputxx_lib
)
