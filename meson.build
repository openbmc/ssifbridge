project('ssifbridge', 'cpp',
    meson_version: '>=0.58.0',
    default_options: [
        'buildtype=debugoptimized',
        'warning_level=3',
        'werror=true',
        'cpp_std=c++20',
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

# Configure and install systemd unit files
systemd = dependency('systemd')
systemd_system_unit_dir = systemd.get_variable(
    'systemdsystemunitdir',
    pkgconfig_define: ['prefix', get_option('prefix')])

configure_file(
    copy: true,
    input: 'ssifbridge.service',
    install: true,
    install_dir: systemd_system_unit_dir,
    output: 'ssifbridge.service'
)

executable('ssifbridged','ssifbridged.cpp',
           dependencies: deps,
           install: true)
