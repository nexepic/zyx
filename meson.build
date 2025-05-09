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

# Boost dependency - Try multiple approaches
boost_dep = dependency('boost', modules: ['filesystem', 'system'], required: false)
if not boost_dep.found()
    boost_dep = dependency('boost-cpp', modules: ['filesystem', 'system'], required: false)
endif

# If still not found via pkg-config, try direct method with conda env path
if not boost_dep.found()
    conda_env_path = ''
    if build_machine.system() == 'windows'
        conda_env_cmd = run_command('cmd', '/c', 'echo %CONDA_PREFIX%', check: false)
    else
        conda_env_cmd = run_command('bash', '-c', 'echo $CONDA_PREFIX', check: false)
    endif

    if conda_env_cmd.returncode() == 0
        conda_env_path = conda_env_cmd.stdout().strip()
        message('Found Conda env path: ' + conda_env_path)

        boost_inc_dir = include_directories(conda_env_path + '/include')
        boost_lib_dir = conda_env_path + '/lib'

        boost_fs = cc.find_library('boost_filesystem', dirs: [boost_lib_dir], required: false)
        boost_sys = cc.find_library('boost_system', dirs: [boost_lib_dir], required: false)

        if boost_fs.found() and boost_sys.found()
            boost_dep = declare_dependency(
                include_directories: boost_inc_dir,
                dependencies: [boost_fs, boost_sys]
            )
            message('Found Boost manually in Conda environment')
        else
            error('Could not find Boost libraries in: ' + boost_lib_dir)
        endif
    else
        error('Could not determine Conda environment path')
    endif
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