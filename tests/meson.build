# Ensure we have the test dependencies
if get_option('tests').enabled() or get_option('tests').auto()
	if not gtest_dep.found()
		gtest_dep = dependency('gtest', required : true)
	endif
	if not gtest_main_dep.found()
		gtest_main_dep = dependency('gtest_main', required : true)
	endif

	test_sources = [
		'apps/cli/test_Repl.cpp',
		'src/storage/test_FileStorage.cpp',
		'src/storage/test_StorageHeaders.cpp',
	]

	foreach test_source : test_sources
		test_name = test_source.split('/')[-1].split('.')[0]
		test_executable = executable(test_name,
									 sources : [test_source],
									 dependencies : [
										 db_dep,
										 gtest_dep,
										 gtest_main_dep,
										 boost_dep,
										 zlib_dep
									 ],
									 build_by_default : true
		)
		test(test_name, test_executable)
	endforeach
endif