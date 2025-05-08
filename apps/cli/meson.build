cc = meson.get_compiler('cpp')

readline_dep = dependency('readline', required : false)
if not readline_dep.found()
    readline_dep = cc.find_library('readline', required : true)
endif


executable('metrix-cli',
           sources: ['main.cpp'],
           dependencies: [db_dep, readline_dep],
           install: false # install to system path
)