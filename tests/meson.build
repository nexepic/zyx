# Ensure we have the test dependencies
if get_option('tests').enabled() or get_option('tests').auto()
	gtest_dep = dependency('gtest', required : true)
	gtest_main_dep = dependency('gtest_main', required : true)
	# Add Google Mock dependencies
	gmock_dep = dependency('gmock', required : true)
	gmock_main_dep = dependency('gmock_main', required : true)

	python = import('python').find_installation()
	find_result = run_command(python, '-c', '''
import os
import sys
for root, dirs, files in os.walk(sys.argv[1]):
	for file in files:
		if file.startswith("test_") and file.endswith(".cpp"):
			rel_path = os.path.relpath(os.path.join(root, file), sys.argv[1])
			print(rel_path)
	''', meson.current_source_dir(), check : true)

	test_sources = []
	test_files = find_result.stdout().strip().split('\n')

	foreach file : test_files
		if file != '' and file != '.'
			test_sources += files(file)
		endif
	endforeach

	test_include_dirs = include_directories('include', '../tests/include')

	foreach file : test_files
		if file != '' and file != '.'
			# Extract just the filename without path and extension
			base_name = file.split('/')[-1].replace('.cpp', '')
			# Create a unique test name by replacing path separators with underscores
			test_name = file.replace('/', '_').replace('\\', '_').replace('.cpp', '')

			test_executable = executable(test_name,
										 sources : files(file),
										 dependencies : [
											 db_dep,
											 gtest_dep,
											 gtest_main_dep,
											 gmock_dep,
											 gmock_main_dep,
											 antlr4_dep,
											 boost_dep,
											 zlib_dep,
											 cli11_dep
										 ],
										 include_directories : test_include_dirs,
										 link_with : db_lib,
										 build_by_default : true
			)
			test(test_name, test_executable)
		endif
	endforeach
endif