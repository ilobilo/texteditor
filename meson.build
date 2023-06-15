project('TextEditor', 'cpp',
    version : 'v0.1',
    default_options : ['cpp_std=c++20']
)

sources = files(
    'src/main.cpp',
)

includes = include_directories('src')

cmake = import('cmake')

fmt_opts = cmake.subproject_options()
fmt_opts.add_cmake_defines({
    'FMT_INSTALL' : false,
    'FMT_TEST' : false,
    'FMT_DOC' : false
})

fmt_dep = cmake.subproject('fmt', options : fmt_opts).dependency('fmt')

executable('editor',
    dependencies : [
        fmt_dep
    ],
    sources : sources,
    include_directories : includes
)