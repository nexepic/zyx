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

	test_include_dirs = include_directories('include', '../tests/include', '../src/query/parser/cypher/generated')

	if test_sources.length() > 0
	        test_executable = executable('zyx_test_suite',
	                                     sources : test_sources,
	                                     dependencies : [
	                                             db_dep,
	                                             gtest_dep,
	                                             gmock_dep,
	                                             antlr4_dep,
	                                             boost_dep,
	                                             cli11_dep
	                                     ],	                                     include_directories : test_include_dirs,
	                                     build_by_default : true
	        )
	        test('zyx_all_tests', test_executable, protocol : 'gtest', timeout : 600)
	endif
endif