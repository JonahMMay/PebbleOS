/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "boot_timing.h"

#include <pbl/drivers/rtc.h>

static RtcTicks s_ticks[BootTiming_Count];

void boot_timing_mark(BootTimingPoint point) {
  if (s_ticks[point] == 0) {
    // Never zero, so that zero can mean not reached.
    const RtcTicks now = rtc_get_ticks();
    s_ticks[point] = (now == 0) ? 1 : now;
  }
}

int32_t boot_timing_get_ms(BootTimingPoint point) {
  if (s_ticks[point] == 0 || s_ticks[BootTimingKernelStart] == 0) {
    return -1;
  }
  return (int32_t)((s_ticks[point] - s_ticks[BootTimingKernelStart]) * 1000 / RTC_TICKS_HZ);
}
