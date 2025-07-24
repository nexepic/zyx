project('metrix', 'cpp',
        version : '0.1.0',
        default_options : [
            'cpp_std=c++20',
            'warning_level=3',
            'buildtype=debugoptimized',
            'b_coverage=false'
        ])

# Compiler setup
cc = meson.get_compiler('cpp')

# Global include directories
inc_dirs = include_directories('include')

# Core dependencies
zlib_dep = dependency('zlib')

# Boost dependency - Try both standard name and conda-forge name
boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: false)
if not boost_dep.found()
    boost_dep = dependency('boost-cpp', modules: ['filesystem', 'system'], required: true)
endif

# ===== Process subdirectories =====
# First build the core library
subdir('src')
# Then the applications
subdir('apps/cli')
# Finally the tests
subdir('tests')