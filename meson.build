project('ssifbridge', 'cpp',
    default_options: [
        'buildtype=debugoptimized',
        'warning_level=3',
        'werror=true',
        'cpp_std=c++17'
    ],
    version: '1.0',
)

add_project_arguments('-Wno-psabi', language: 'cpp')

deps = [dependency('libsystemd', version : '>=221'),
        dependency('systemd'),
        dependency('phosphor-logging'),
        dependency('sdbusplus'),
        dependency('threads')

]

executable('ssifbridged','ssifbridged.cpp',
           dependencies: deps,
           install: true)
