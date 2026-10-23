/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! Points in the boot whose time is kept, for debugging slow boots.
typedef enum {
  BootTimingKernelStart, //!< main() starts.
  BootTimingSplashShown, //!< The boot splash puts its first frame on the display.
  BootTimingResourcesReady, //!< System resources are installed and checked.
  BootTimingDisplayReady, //!< The splash is gone and the compositor can take the display.
  BootTimingDone, //!< KernelMain is done initializing the system.
  BootTiming_Count,
} BootTimingPoint;

//! Note that the boot has reached @p point now. Only the first time counts.
void boot_timing_mark(BootTimingPoint point);

//! @return The time from the start of main() to when @p point was reached, in ms, or -1 if it
//! wasn't reached.
int32_t boot_timing_get_ms(BootTimingPoint point);
