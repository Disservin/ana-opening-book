project_source_files = [
    'analyze.cpp',
    'utils.cpp',
    '../external/gzip/gzstream.cpp',
]

zdep = dependency('zlib')

executable(
    meson.project_name(),
    project_source_files,
    dependencies: zdep,
)
