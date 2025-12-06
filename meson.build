project('metrix', 'cpp',
		version : '0.1.0',
		default_options : [
			'cpp_std=c++20',
			'warning_level=3',
			'werror=false',
			'buildtype=debug',
			'default_library=static'
		])

# Compiler setup
# In Meson, getting the compiler object explicitly is often useful for checks
cxx = meson.get_compiler('cpp')

# Global include directories
inc_dirs = include_directories('include')

# Dependencies
zlib_dep = dependency('zlib')
boost_dep = dependency('boost', modules : ['filesystem', 'system'])

# Subdirectories
subdir('src')
subdir('apps/cli')

# Build tests only if not a subproject
if not meson.is_subproject()
	subdir('tests')
endif