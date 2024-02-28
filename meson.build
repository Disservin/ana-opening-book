project('analysis', 'cpp', default_options: ['warning_level=3, cpp_std=c++17'])

project_source_files = [
    'analyze.cpp',
    'external/gzip/gzstream.cpp',
]


zdep = dependency('zlib')

executable(
    meson.project_name(),
    project_source_files,
    dependencies: zdep,
    install: true,
)


