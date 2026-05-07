project('zyx', 'cpp',
        version : '0.1.2',
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
zlib_dep = dependency('zlib')
if host_machine.system() != 'emscripten'
    boost_dep = dependency('boost', modules : ['filesystem', 'system'])
    cli11_dep = dependency('CLI11')
else
    boost_dep = declare_dependency()  # empty stub — boost not used in WASM
    cli11_dep = declare_dependency()  # CLI not built for WASM
endif

# Windows SDK headers define macros (min/max from minwindef.h, ERROR from wingdi.h)
# that conflict with third-party headers like antlr4-runtime.
# NOMINMAX: prevents min/max function-like macros
# NOGDI:    prevents wingdi.h inclusion (which defines #define ERROR 0)
if host_machine.system() == 'windows'
    add_project_arguments('-DNOMINMAX', '-DNOGDI', language : 'cpp')
endif

# antlr4: Conan provides 'antlr4-cppruntime' via pkg-config,
# Homebrew provides 'antlr4-runtime' via cmake
antlr4_dep = dependency('antlr4-cppruntime', required : false)
if not antlr4_dep.found()
    antlr4_dep = dependency('antlr4-runtime', method : 'cmake', modules : ['antlr4_static'], required : true)
endif

# Subdirectories
subdir('lib/inputxx')
subdir('src')
if host_machine.system() != 'emscripten'
    subdir('apps/cli')
    subdir('apps/benchmark')
endif

if not meson.is_subproject() and get_option('tests').allowed()
    subdir('tests')
endif

# Python bindings are built separately via: pip install -e bindings/python
# See bindings/python/meson.build for standalone build configuration.