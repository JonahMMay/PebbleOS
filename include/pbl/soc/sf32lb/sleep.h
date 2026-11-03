/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/**
 * @defgroup soc_sf32lb SF32LB
 * @ingroup soc
 * @brief SiFli SF32LB52 SoC interfaces.
 *
 * The idle thread enters the deepest sleep level that no driver has blocked. A driver whose
 * peripheral would not survive a level blocks it while active:
 *
 * @code{.c}
 * soc_sf32lb_sleep_block(SOC_SF32LB_DEEPSLEEP); // the PWM does not run in deep sleep
 * prv_pwm_start();
 * prv_pwm_wait_done();
 * prv_pwm_stop();
 * soc_sf32lb_sleep_release(SOC_SF32LB_DEEPSLEEP);
 * @endcode
 * @{
 */

/** @brief Sleep levels, ordered from shallowest to deepest. */
typedef enum {
  /** No sleep at all. */
  SOC_SF32LB_ACTIVE = 0,
  /** Light WFI. */
  SOC_SF32LB_WFI,
  /** Deep WFI. */
  SOC_SF32LB_DEEPWFI,
  /** Deep sleep. */
  SOC_SF32LB_DEEPSLEEP,
} SocSf32lbSleepLevel;

/**
 * @brief Block a sleep level and every deeper one.
 *
 * For example, blocking @ref SOC_SF32LB_DEEPWFI forbids deep WFI and deep sleep, leaving plain
 * WFI as the deepest permitted level. Refcounted; balance each call with
 * soc_sf32lb_sleep_release(). Safe to call concurrently, including from ISRs.
 *
 * @param level Level to block; @ref SOC_SF32LB_ACTIVE cannot be blocked.
 */
void soc_sf32lb_sleep_block(SocSf32lbSleepLevel level);

/**
 * @brief Release a block taken with soc_sf32lb_sleep_block().
 *
 * @param level Level passed to soc_sf32lb_sleep_block().
 */
void soc_sf32lb_sleep_release(SocSf32lbSleepLevel level);

/**
 * @brief Get the deepest sleep level currently permitted.
 *
 * @return One step shallower than the shallowest outstanding block, or
 *         @ref SOC_SF32LB_DEEPSLEEP with no blocks.
 */
SocSf32lbSleepLevel soc_sf32lb_sleep_max_level(void);

/** @brief Idle statistics since boot, in @ref RTC_TICKS_HZ ticks. */
typedef struct {
  /** Ticks since boot. */
  uint64_t total_ticks;
  /** Ticks spent in light WFI. */
  uint64_t wfi_ticks;
  /** Ticks spent in deep WFI. */
  uint64_t deepwfi_ticks;
  /** Ticks spent in deep sleep. */
  uint64_t deepsleep_ticks;
  /** Times the idle thread entered light WFI. */
  uint32_t wfi_count;
  /** Times the idle thread entered deep WFI. */
  uint32_t deepwfi_count;
  /** Times the idle thread entered deep sleep. */
  uint32_t deepsleep_count;
  /** Times the idle thread found IPC traffic the LCPU had not consumed yet. */
  uint32_t ipc_not_idle_count;
} SocSf32lbSleepStats;

/**
 * @brief Get idle statistics since boot.
 *
 * Unlike the hourly analytics these are never reset, so a caller can diff two snapshots.
 *
 * @param[out] stats Statistics.
 */
void soc_sf32lb_sleep_stats_get(SocSf32lbSleepStats *stats);

/** @brief Number of external interrupts tracked in @ref SocSf32lbWakeStats. */
#define SOC_SF32LB_WAKE_IRQS 99

/** @brief Number of deep sleep wake source bits tracked in @ref SocSf32lbWakeStats. */
#define SOC_SF32LB_WAKE_SOURCES 32

/**
 * @brief Number of idle gap buckets in @ref SocSf32lbWakeStats.
 *
 * Gaps are bucketed by the idle window offered to the SoC: 1 ms, 2-4 ms, 5-9 ms, 10-19 ms,
 * 20-49 ms, 50-199 ms and 200 ms or more (including no timeout at all).
 */
#define SOC_SF32LB_GAP_BUCKETS 7

/**
 * @brief What ended each sleep since boot, and how long the idle windows were.
 *
 * Each wake counts every enabled interrupt pending at that moment, so one wake can count more
 * than once.
 */
typedef struct {
  /** Light or deep WFI ended by the tick. */
  uint32_t systick;
  /** Wakes with nothing pending. */
  uint32_t none;
  /** Wakes per external interrupt. */
  uint32_t irq[SOC_SF32LB_WAKE_IRQS];
  /** Deep sleep wakes per wake source bit in the HPSYS AON WSR register. */
  uint32_t deepsleep_source[SOC_SF32LB_WAKE_SOURCES];
  /** Sleeps per idle window length; see @ref SOC_SF32LB_GAP_BUCKETS. */
  uint32_t gap[SOC_SF32LB_GAP_BUCKETS];
} SocSf32lbWakeStats;

/**
 * @brief Get wake statistics since boot.
 *
 * @param[out] stats Statistics.
 */
void soc_sf32lb_wake_stats_get(SocSf32lbWakeStats *stats);

/**
 * @brief Get the name of an external interrupt.
 *
 * @param irq Interrupt number.
 * @return Name, or NULL if unnamed.
 */
const char *soc_sf32lb_irq_name(uint32_t irq);

/**
 * @brief Get the name of a deep sleep wake source bit.
 *
 * @param source Bit in the HPSYS AON WSR register.
 * @return Name, or NULL if unnamed.
 */
const char *soc_sf32lb_wake_source_name(uint32_t source);

/** @} */
