# SPDX-FileCopyrightText: 2026 Core Devices LLC
# SPDX-License-Identifier: Apache-2.0

pbl_library()
pbl_include_directories(.)
pbl_library_sources(
  init.c
  sleep.c
  idle.c
)
pbl_linker_sources(ramfunc ramfunc.ld)

# The deep sleep code runs from RAM with the flash powered down, so whatever it calls must be
# inlined or in RAM too. LTO can leave such helpers out of line, in flash.
if(CONFIG_LTO)
  set_property(SOURCE idle.c APPEND PROPERTY COMPILE_OPTIONS -fno-lto)
endif()
