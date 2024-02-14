project('ssifbridge', 'cpp',
    meson_version: '>=1.1.1',
    default_options: [
        'buildtype=debugoptimized',
        'warning_level=3',
        'werror=true',
        'cpp_std=c++20',
        'b_lto=true',
    ],
    version: '1.0',
)

add_project_arguments(
    [
        '-Wno-psabi',
        '-DBOOST_ASIO_DISABLE_THREADS',
        '-DBOOST_ASIO_NO_DEPRECATED',
    ],
    language: 'cpp'
)

sdbusplus = dependency('sdbusplus')
cli11 = dependency('CLI11')
phosphor_logging = dependency('phosphor-logging')

boost = dependency(
  'boost',
  version : '>=1.83.0',
  required : false,
)

if not boost.found()
  cmake = import('cmake')
  opt = cmake.subproject_options()
  opt.add_cmake_defines({
    'BOOST_INCLUDE_LIBRARIES': 'asio;callable_traits'
  })
  boost_cmake = cmake.subproject('boost', required: true, options: opt)
  boost_asio = boost_cmake.dependency('boost_asio').as_system()
  boost_callable_traits = boost_cmake.dependency('boost_callable_traits').as_system()
  boost = [boost_asio, boost_callable_traits]
endif

# Configure and install systemd unit files
systemd = dependency('systemd')
systemd_system_unit_dir = systemd.get_variable(
    'systemdsystemunitdir',
    pkgconfig_define: ['prefix', get_option('prefix')])

fs = import('fs')
fs.copyfile(
    'ssifbridge.service',
    install: true,
    install_dir: systemd_system_unit_dir
)

executable('ssifbridged','ssifbridged.cpp',
           dependencies: [
		boost,
		sdbusplus,
		cli11,
		phosphor_logging,
	   ],
           install: true)
