project('win-pkgconfig', version : '0.29.2.1')

py3 = import('python3').find_python()

if host_machine.system() != 'windows'
  error('Can only download pkgconfig for Windows, sorry')
endif

message('Downloading pkg-config.exe binary for Windows...')

zip_hash = '5db2afe01a0f97469f2875e9da5b39e0b55d9f632d71deaf8a49c6f4c157455a'

ret = run_command(py3, files('download-binary.py'), meson.project_version(), zip_hash, check: true)
if ret.returncode() != 0
  message(ret.stdout())
  error(ret.stderr())
endif

meson.override_find_program('pkg-config', find_program('pkg-config.exe'))
