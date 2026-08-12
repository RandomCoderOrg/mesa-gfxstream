# Copyright © 2023 Collabora
# SPDX-License-Identifier: MIT

libpankmod_lib_files = files(
  'kbase_kmod.c',
  'pan_kmod.c',
  'panfrost_kmod.c',
  'panthor_kmod.c',
)

libpankmod_lib = static_library(
  'pankmod_lib',
  [libpankmod_lib_files],
  include_directories : [inc_include, inc_src, inc_panfrost],
  c_args : [no_override_init_args],
  gnu_symbol_visibility : 'hidden',
  dependencies: [dep_libdrm, idep_mesautil, idep_pan_packers, libpanfrost_model_dep, libpanfrost_base_dep],
  link_with: [libpanfrost_base_per_arch, libpanfrost_base],
  build_by_default : false,
)

libpankmod_dep = declare_dependency(
  include_directories: [inc_include, inc_src],
  dependencies: [dep_libdrm],
)

if with_tools.contains('panfrost')
  executable(
    'pan-kbase-probe',
    ['kbase_probe.c', 'pan_kmod.c', 'kbase_kmod.c'],
    include_directories : [inc_include, inc_src, inc_panfrost],
    dependencies : [dep_libdrm, idep_mesautil, libpankmod_dep,
                    libpanfrost_base_dep],
    link_with : [libpanfrost_base_per_arch, libpanfrost_base],
    c_args : ['-DPAN_KMOD_KBASE_ONLY'],
    build_by_default : false,
  )
endif
