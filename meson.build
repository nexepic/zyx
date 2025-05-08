project('metrix', 'cpp',
        version : '0.1.0',
        default_options : [
            'cpp_std=c++20',
            'warning_level=3',
            'buildtype=debugoptimized',
            'b_coverage=true'
        ])

# Compiler setup
cc = meson.get_compiler('cpp')

# Global include directories
inc_dirs = include_directories('include')

# Core dependencies
zlib_dep = dependency('zlib')

# Boost dependency
boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: false)
if not boost_dep.found()
    boost_dep = cc.find_library('boost', required: true)
endif

# Readline dependency
readline_dep = dependency('readline', required : false)
if not readline_dep.found()
    readline_dep = cc.find_library('readline', required : true)
endif

# Testing dependencies
gtest_dep = dependency('gtest', required : false)
gtest_main_dep = dependency('gtest_main', required : false)
# If not found, they'll be required in tests/meson.build only if tests are enabled

# ===== Process subdirectories =====
# First build the core library
subdir('src')
# Then the applications
subdir('apps/cli')
# Finally the tests
subdir('tests')