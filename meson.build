project('metrix', 'cpp',
		version : '0.1.0',
		default_options : [
			'cpp_std=c++20',
			'warning_level=3',
			'werror=false',
			'buildtype=debug',
			'default_library=static'
		])

cxx = meson.get_compiler('cpp')

# Include directories: include/ for headers, src/ for internal headers
inc_dirs = include_directories('include', 'src')

# Dependencies
zlib_dep = dependency('zlib')
boost_dep = dependency('boost', modules : ['filesystem', 'system'])

# ANTLR4 Runtime (Required for linking, but we don't run the tool here anymore)
antlr4_dep = dependency('antlr4-cppruntime', 'antlr4-runtime', required : true)

# Subdirectories
subdir('src')
subdir('apps/cli')

if not meson.is_subproject()
	subdir('tests')
endif