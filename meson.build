project('metrix', 'cpp',
        version : '0.1.0',
        default_options : [
            'cpp_std=c++17',
            'warning_level=3',
            'buildtype=debugoptimized',
            'b_coverage=true'
        ])

db_inc = include_directories('include')

subdir('src')
subdir('apps/cli')
subdir('tests')