project('ssifbridge', 'c',
        default_options: [
            'buildtype=debugoptimized',
            'warning_level=3',
            'werror=true',
           ],
        version: '1.0',
)

# Wno-psabi reduces the number of "Note:" messages when cross-compiling some STL
# stuff for ARM. See https://stackoverflow.com/questions/48149323/strange-gcc-warning-when-compiling-qt-project
# Basically, gcc 6 and gcc 7 are not ABI compatible, but since the whole OpenBMC
# project uses the same compiler, we can safely ignmore these info notes.
add_project_arguments('-Wno-psabi', language: 'c')

deps = [dependency('libsystemd', version : '>=221'),
        dependency('systemd'),
       ]

executable('ssifbridged','ssifbridged.c',
           dependencies: deps,
           install: true)
