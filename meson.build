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

# Core dependencies - Different approach for Windows vs Unix
if host_machine.system() == 'windows'
    # On Windows, find zlib directly from Conan's location
    zlib_lib = cc.find_library('zdll', dirs: [meson.current_build_dir() + '/lib'], required: false)
    if not zlib_lib.found()
        zlib_lib = cc.find_library('zlib', dirs: [meson.current_build_dir() + '/lib'], required: true)
    endif

    zlib_inc = include_directories(meson.current_build_dir() + '/include')
    zlib_dep = declare_dependency(
        include_directories: zlib_inc,
        dependencies: zlib_lib
    )
else
    # On Linux/macOS, use pkg-config as before
    zlib_dep = dependency('zlib')
endif

# Boost dependency - Try both standard name and conda-forge name
# For Windows, try direct library lookup first
if host_machine.system() == 'windows'
    boost_filesystem_lib = cc.find_library('boost_filesystem', dirs: [meson.current_build_dir() + '/lib'], required: false)
    boost_system_lib = cc.find_library('boost_system', dirs: [meson.current_build_dir() + '/lib'], required: false)

    if boost_filesystem_lib.found() and boost_system_lib.found()
        boost_inc = include_directories(meson.current_build_dir() + '/include')
        boost_dep = declare_dependency(
            include_directories: boost_inc,
            dependencies: [boost_filesystem_lib, boost_system_lib]
        )
    else
        # Fall back to standard methods
        boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: false)
        if not boost_dep.found()
            boost_dep = dependency('boost-cpp', modules: ['filesystem', 'system'], required: true)
        endif
    endif
else
    boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: false)
    if not boost_dep.found()
        boost_dep = dependency('boost-cpp', modules: ['filesystem', 'system'], required: true)
    endif
endif

# Testing dependencies - Handle Windows specially
if host_machine.system() == 'windows'
    gtest_lib = cc.find_library('gtest', dirs: [meson.current_build_dir() + '/lib'], required: false)
    gtest_main_lib = cc.find_library('gtest_main', dirs: [meson.current_build_dir() + '/lib'], required: false)

    if gtest_lib.found()
        gtest_inc = include_directories(meson.current_build_dir() + '/include')
        gtest_dep = declare_dependency(
            include_directories: gtest_inc,
            dependencies: gtest_lib
        )
        gtest_main_dep = declare_dependency(
            include_directories: gtest_inc,
            dependencies: gtest_main_lib
        )
    else
        gtest_dep = dependency('gtest', required: false)
        gtest_main_dep = dependency('gtest_main', required: false)
    endif
else
    gtest_dep = dependency('gtest', required: false)
    gtest_main_dep = dependency('gtest_main', required: false)
endif

# ===== Process subdirectories =====
# First build the core library
subdir('src')
# Then the applications
subdir('apps/cli')
# Finally the tests
subdir('tests')