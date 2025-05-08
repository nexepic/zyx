gtest_dep = dependency('gtest', required: true)
gtest_main_dep = dependency('gtest_main', required: true)

test_sources = [
    'apps/cli/test_Repl.cpp',
    'src/storage/test_FileStorage.cpp',
    'src/storage/test_StorageHeaders.cpp',
]

foreach test_source : test_sources
    test_name = test_source.split('/')[-1].split('.')[0]
    test_executable = executable(test_name,
                                 sources: [test_source],
                                 dependencies: [db_dep, readline_dep, gtest_dep, gtest_main_dep],
                                 build_by_default: true
    )
    test(test_name, test_executable)
endforeach