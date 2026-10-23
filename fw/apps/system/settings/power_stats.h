/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//! Push the Power Stats debugging window: idle residency, idle entries and battery drop, since
//! boot or since the last reset (hold Select). Select refreshes.
void power_stats_window_push(void);
