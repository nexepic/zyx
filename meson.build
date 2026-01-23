project('metrix', 'cpp',
		version : '0.1.0',
		default_options : [
			'cpp_std=c++20',
			'warning_level=3',
			'werror=false',
			'buildtype=debug',
			'default_library=static',
		])

cxx = meson.get_compiler('cpp')

# MODIFY: Remove 'include/vendor'.
# Subprojects handle include paths automatically via dependencies.
inc_dirs = include_directories('include', 'src')

# Dependencies
zlib_dep = dependency('zlib', static : true)
boost_dep = dependency('boost', modules : ['filesystem', 'system'], static : true)
cli11_dep = dependency('CLI11')
antlr4_dep = dependency('antlr4-cppruntime', 'antlr4-runtime', required : true, static : true)

# ==============================================================================
# Vector Search Dependencies (SimSIMD via Subproject)
# ==============================================================================
# 1. Try to find system installed SimSIMD
simsimd_dep = dependency('simsimd', required : false)

# 2. If not found, use the subproject defined in subprojects/simsimd.wrap
if not simsimd_dep.found()
	# This downloads the code and uses the meson.build from packagefiles
	simsimd_proj = subproject('simsimd')

	# Extract the dependency object declared in packagefiles/simsimd/meson.build
	simsimd_dep = simsimd_proj.get_variable('simsimd_dep')
endif

# Subdirectories
subdir('src')
subdir('apps/cli')
subdir('apps/benchmark')

if not meson.is_subproject()
	subdir('tests')
endif