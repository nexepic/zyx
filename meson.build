project('zyx', 'cpp',
        version : '0.1.0',
        default_options : [
            'cpp_std=c++20',
            'warning_level=3',
            'werror=false',
            'buildtype=debug',
            'default_library=static',
        ])

cxx = meson.get_compiler('cpp')

# ==============================================================================
# Project Configuration
# ==============================================================================
project_config = configuration_data()
project_config.set('PROJECT_NAME', 'zyx')
project_config.set('PROJECT_DISPLAY', 'ZYX')
project_config.set('PROJECT_VERSION', meson.project_version())

# Generate ProjectConfig.hpp from template
# The file will be generated at buildDir/ProjectConfig.hpp
configure_file(
    input: 'include/graph/core/ProjectConfig.hpp.in',
    output: 'ProjectConfig.hpp',
    configuration: project_config,
    install: false
)

# MODIFY: Remove 'include/vendor'.
# Subprojects handle include paths automatically via dependencies.
# Include both source and generated (build) directories
inc_dirs = include_directories('include', 'src', '.')

# Dependencies
zlib_dep = dependency('zlib', static : true)
boost_dep = dependency('boost', modules : ['filesystem', 'system'], method : 'pkg-config')
cli11_dep = dependency('CLI11')
antlr4_dep = dependency('antlr4-cppruntime', 'antlr4-runtime', required : true, static : true)

# Subdirectories
subdir('src')
subdir('apps/cli')
subdir('apps/benchmark')

if not meson.is_subproject()
    subdir('tests')
endif