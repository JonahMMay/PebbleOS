/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "power_stats.h"

#ifdef CONFIG_SOC_SF32LB52

#include <inttypes.h>
#include <stdio.h>

#include <pbl/drivers/rtc.h>
#include <pbl/kernel/debug.h>
#include <pbl/services/battery/battery_state.h>
#include <pbl/services/i18n/i18n.h>
#include <pbl/soc/sf32lb/sleep.h>
#include <pbl/util/size.h>
#include <pbl/util/time.h>

#include <applib/fonts/fonts.h>
#include <applib/ui/option_menu_window.h>
#include <applib/ui/ui.h>
#include <kernel/pbl_malloc.h>
#include <kernel/task_timer_manager.h>
#include <system/boot_timing.h>
#include <system/reboot_reason.h>

typedef struct PowerStatsBaseline {
  SocSf32lbSleepStats sleep;
  SocSf32lbWakeStats wake;
  //! Timeout wakes of each thread, by thread number.
  uint32_t thread_number[CONFIG_KERNEL_MAX_THREADS];
  uint32_t thread_wakes[CONFIG_KERNEL_MAX_THREADS];
  size_t thread_count;
  TaskTimerCbStat timers[TASK_TIMER_CB_STATS];
  uint8_t charge_percent;
  bool charging;
} PowerStatsBaseline;

typedef struct PowerStatsWindowData {
  Window window; //!< Must be first: the base layer is cast back to this struct.
  StatusBarLayer status_layer;
  ScrollLayer scroll_layer;
  TextLayer text_layer;
  char text[2048];
} PowerStatsWindowData;

// Kept across visits to the window, so a reset can be compared against hours later. Until the
// first reset, the idle stats count from boot and there is no battery baseline.
static PowerStatsBaseline s_baseline;
static bool s_baseline_valid;

//! Formats a part of a whole as a percentage with two decimals.
static void prv_format_pct(char *buf, size_t size, uint64_t part, uint64_t whole) {
  const uint32_t pct_x100 = (whole == 0) ? 0 : (uint32_t)((part * 10000) / whole);
  snprintf(buf, size, "%" PRIu32 ".%02" PRIu32 "%%", pct_x100 / 100, pct_x100 % 100);
}

//! Wake causes shown, most frequent first.
#define TOP_WAKES 6
//! Deep sleep wake sources shown, most frequent first.
#define TOP_SOURCES 4

//! Wake causes are indexed as the tick, nothing pending, then each IRQ.
#define WAKE_SYSTICK 0
#define WAKE_NONE 1
#define WAKE_IRQ0 2
#define WAKE_CAUSES (WAKE_IRQ0 + SOC_SF32LB_WAKE_IRQS)

typedef uint32_t (*PowerStatsCountFn)(const SocSf32lbWakeStats *now,
                                      const SocSf32lbWakeStats *base, uint32_t i);

static uint32_t prv_wake_count(const SocSf32lbWakeStats *now, const SocSf32lbWakeStats *base,
                               uint32_t cause) {
  if (cause == WAKE_SYSTICK) {
    return now->systick - base->systick;
  }
  if (cause == WAKE_NONE) {
    return now->none - base->none;
  }
  return now->irq[cause - WAKE_IRQ0] - base->irq[cause - WAKE_IRQ0];
}

static uint32_t prv_source_count(const SocSf32lbWakeStats *now, const SocSf32lbWakeStats *base,
                                 uint32_t source) {
  return now->deepsleep_source[source] - base->deepsleep_source[source];
}

//! Picks the index with the largest nonzero count that is not yet in @p picked, or -1.
static int prv_pick_max(PowerStatsCountFn count, const SocSf32lbWakeStats *now,
                        const SocSf32lbWakeStats *base, uint32_t n, const int *picked,
                        int num_picked) {
  int best = -1;
  uint32_t best_count = 0;
  for (uint32_t i = 0; i < n; i++) {
    bool skip = false;
    for (int p = 0; p < num_picked; p++) {
      skip |= (picked[p] == (int)i);
    }
    const uint32_t c = count(now, base, i);
    if (!skip && c > best_count) {
      best = (int)i;
      best_count = c;
    }
  }
  return best;
}

//! Appends the most frequent wake causes, the deep sleep wake sources and the idle windows.
static int prv_append_wakes(char *buf, size_t size, const SocSf32lbWakeStats *now,
                            const SocSf32lbWakeStats *base, uint32_t seconds) {
  int len = snprintf(buf, size, "Wakes/min\n");

  int picked[TOP_WAKES];
  int num_picked = 0;
  while (num_picked < TOP_WAKES) {
    const int cause = prv_pick_max(prv_wake_count, now, base, WAKE_CAUSES, picked, num_picked);
    if (cause < 0) {
      break;
    }
    picked[num_picked++] = cause;

    const uint64_t c = prv_wake_count(now, base, cause);
    const uint32_t per_min = (seconds == 0) ? 0 : (uint32_t)(c * 60 / seconds);
    if (cause == WAKE_SYSTICK) {
      len += snprintf(buf + len, size - len, " Tick %" PRIu32 "\n", per_min);
    } else if (cause == WAKE_NONE) {
      len += snprintf(buf + len, size - len, " None %" PRIu32 "\n", per_min);
    } else {
      const uint32_t irq = cause - WAKE_IRQ0;
      const char *name = soc_sf32lb_irq_name(irq);
      if (name) {
        len += snprintf(buf + len, size - len, " %s %" PRIu32 "\n", name, per_min);
      } else {
        len += snprintf(buf + len, size - len, " IRQ%" PRIu32 " %" PRIu32 "\n", irq, per_min);
      }
    }
  }

  len += snprintf(buf + len, size - len, "Deep sleep woken by\n");
  int sources[TOP_SOURCES];
  int num_sources = 0;
  while (num_sources < TOP_SOURCES) {
    const int source = prv_pick_max(prv_source_count, now, base, SOC_SF32LB_WAKE_SOURCES,
                                    sources, num_sources);
    if (source < 0) {
      break;
    }
    sources[num_sources++] = source;

    const char *name = soc_sf32lb_wake_source_name(source);
    const uint32_t c = prv_source_count(now, base, source);
    if (name) {
      len += snprintf(buf + len, size - len, " %s %" PRIu32 "\n", name, c);
    } else {
      len += snprintf(buf + len, size - len, " WSR%d %" PRIu32 "\n", source, c);
    }
  }

  static const char *const s_gap_labels[SOC_SF32LB_GAP_BUCKETS] = {
    "1", "2-4", "5-9", "10-19", "20-49", "50-199", "200+",
  };
  len += snprintf(buf + len, size - len, "Idle windows (ms)\n");
  for (int i = 0; i < SOC_SF32LB_GAP_BUCKETS; i++) {
    len += snprintf(buf + len, size - len, " %s: %" PRIu32 "\n", s_gap_labels[i],
                    now->gap[i] - base->gap[i]);
  }
  return len;
}

//! Rows shown for the threads woken by timeouts and for the timer callbacks.
#define TIMER_ROWS 7

static uint32_t prv_thread_base(const struct pbl_thread_stats *t) {
  if (!s_baseline_valid) {
    return 0;
  }
  for (size_t i = 0; i < s_baseline.thread_count; i++) {
    if (s_baseline.thread_number[i] == t->number) {
      return s_baseline.thread_wakes[i];
    }
  }
  return 0;
}

static uint32_t prv_timer_base(const TaskTimerCbStat *t) {
  if (!s_baseline_valid) {
    return 0;
  }
  for (size_t i = 0; i < TASK_TIMER_CB_STATS; i++) {
    if (s_baseline.timers[i].count != 0 && s_baseline.timers[i].cb == t->cb) {
      return s_baseline.timers[i].count;
    }
  }
  return 0;
}

static void prv_baseline_threads_and_timers(void) {
  struct pbl_thread_stats *threads =
      app_malloc_check(sizeof(*threads) * CONFIG_KERNEL_MAX_THREADS);
  const size_t count = pbl_thread_stats_snapshot(threads, CONFIG_KERNEL_MAX_THREADS, nullptr);
  for (size_t i = 0; i < count; i++) {
    s_baseline.thread_number[i] = threads[i].number;
    s_baseline.thread_wakes[i] = threads[i].timeout_wakes;
  }
  s_baseline.thread_count = count;
  app_free(threads);
  task_timer_cb_stats_get(s_baseline.timers);
}

//! Appends the threads most often woken by a timeout, which is what the deep sleep timer wakes
//! up for, and the timer callbacks run most often.
static int prv_append_timers(char *buf, size_t size, uint32_t seconds) {
  if (seconds == 0) {
    return 0;
  }

  struct pbl_thread_stats *threads =
      app_malloc_check(sizeof(*threads) * CONFIG_KERNEL_MAX_THREADS);
  const size_t count = pbl_thread_stats_snapshot(threads, CONFIG_KERNEL_MAX_THREADS, nullptr);
  uint32_t deltas[CONFIG_KERNEL_MAX_THREADS];
  for (size_t i = 0; i < count; i++) {
    deltas[i] = threads[i].timeout_wakes - prv_thread_base(&threads[i]);
  }

  int len = snprintf(buf, size, "Timeout wakes/min\n");
  for (int row = 0; row < TIMER_ROWS; row++) {
    size_t best = count;
    for (size_t i = 0; i < count; i++) {
      if (deltas[i] != 0 && (best == count || deltas[i] > deltas[best])) {
        best = i;
      }
    }
    if (best == count) {
      break;
    }
    len += snprintf(buf + len, size - len, " %s %" PRIu32 "\n", threads[best].name,
                    (uint32_t)((uint64_t)deltas[best] * 60 / seconds));
    deltas[best] = 0;
  }
  app_free(threads);

  TaskTimerCbStat *timers = app_malloc_check(sizeof(*timers) * TASK_TIMER_CB_STATS);
  task_timer_cb_stats_get(timers);
  for (size_t i = 0; i < TASK_TIMER_CB_STATS; i++) {
    timers[i].count -= prv_timer_base(&timers[i]);
  }

  len += snprintf(buf + len, size - len, "Timer callbacks/min\n");
  for (int row = 0; row < TIMER_ROWS; row++) {
    size_t best = TASK_TIMER_CB_STATS;
    for (size_t i = 0; i < TASK_TIMER_CB_STATS; i++) {
      if (timers[i].count != 0 &&
          (best == TASK_TIMER_CB_STATS || timers[i].count > timers[best].count)) {
        best = i;
      }
    }
    if (best == TASK_TIMER_CB_STATS) {
      break;
    }
    const uint32_t per_min = (uint32_t)((uint64_t)timers[best].count * 60 / seconds);
    if (timers[best].cb == nullptr) {
      len += snprintf(buf + len, size - len, " Others %" PRIu32 "\n", per_min);
    } else {
      len += snprintf(buf + len, size - len, " %08" PRIx32 " %" PRIu32 "\n",
                      (uint32_t)(uintptr_t)timers[best].cb, per_min);
    }
    timers[best].count = 0;
  }
  app_free(timers);

  return len;
}

static const char *prv_reboot_reason_name(RebootReasonCode code) {
  switch (code) {
    case RebootReasonCode_LowBattery:
      return "Low battery";
    case RebootReasonCode_SoftwareUpdate:
      return "Update";
    case RebootReasonCode_ResetButtonsHeld:
      return "Buttons held";
    case RebootReasonCode_ShutdownMenuItem:
      return "Shutdown";
    case RebootReasonCode_RemoteReset:
      return "Remote reset";
    case RebootReasonCode_Watchdog:
      return "Watchdog";
    case RebootReasonCode_Assert:
      return "Assert";
    case RebootReasonCode_StackOverflow:
      return "Stack overflow";
    case RebootReasonCode_HardFault:
      return "Hard fault";
    case RebootReasonCode_OutOfMemory:
      return "Out of memory";
    case RebootReasonCode_CoreDump:
    case RebootReasonCode_ForcedCoreDump:
      return "Core dump";
    default:
      return nullptr;
  }
}

//! Appends why the watch last reset and how long the boot took to reach each point.
static int prv_append_boot(char *buf, size_t size) {
  const RebootReasonCode code = reboot_reason_get_last_reboot_reason();
  const char *name = prv_reboot_reason_name(code);
  int len = name ? snprintf(buf, size, "Last reset: %s\n", name)
                 : snprintf(buf, size, "Last reset: code %d\n", (int)code);

  static const struct {
    BootTimingPoint point;
    const char *name;
  } s_points[] = {
    { BootTimingSplashShown, "splash" },
    { BootTimingDriversReady, "drivers" },
    { BootTimingEarlyServicesReady, "early svc" },
    { BootTimingResourcesReady, "resources" },
    { BootTimingDisplayReady, "display" },
    { BootTimingBluetoothStarted, "bt" },
    { BootTimingServicesReady, "services" },
    { BootTimingDone, "done" },
  };
  len += snprintf(buf + len, size - len, "Boot ms\n");
  for (size_t i = 0; i < ARRAY_LENGTH(s_points); i++) {
    len += snprintf(buf + len, size - len, " %s %" PRId32 "\n", s_points[i].name,
                    boot_timing_get_ms(s_points[i].point));
  }
  return len;
}

static void prv_update_text(PowerStatsWindowData *data) {
  SocSf32lbSleepStats now;
  soc_sf32lb_sleep_stats_get(&now);

  const SocSf32lbSleepStats *base = &s_baseline.sleep;
  const SocSf32lbWakeStats *wake_base = &s_baseline.wake;
  if (!s_baseline_valid) {
    static const SocSf32lbSleepStats s_boot;
    static const SocSf32lbWakeStats s_wake_boot;
    base = &s_boot;
    wake_base = &s_wake_boot;
  }

  const uint64_t total = now.total_ticks - base->total_ticks;
  const uint64_t wfi = now.wfi_ticks - base->wfi_ticks;
  const uint64_t deepwfi = now.deepwfi_ticks - base->deepwfi_ticks;
  const uint64_t deepsleep = now.deepsleep_ticks - base->deepsleep_ticks;
  const uint64_t run = total - wfi - deepwfi - deepsleep;
  const uint32_t wfi_count = now.wfi_count - base->wfi_count;
  const uint32_t deepwfi_count = now.deepwfi_count - base->deepwfi_count;
  const uint32_t deepsleep_count = now.deepsleep_count - base->deepsleep_count;
  const uint32_t ipc_count = now.ipc_not_idle_count - base->ipc_not_idle_count;
  const uint32_t seconds = (uint32_t)(total / RTC_TICKS_HZ);
  const uint32_t entries = wfi_count + deepwfi_count + deepsleep_count;

  uint32_t days, hours, minutes, secs;
  pbl_time_split_seconds(seconds, &days, &hours, &minutes, &secs);

  char run_pct[16], wfi_pct[16], deepwfi_pct[16], deepsleep_pct[16];
  prv_format_pct(run_pct, sizeof(run_pct), run, total);
  prv_format_pct(wfi_pct, sizeof(wfi_pct), wfi, total);
  prv_format_pct(deepwfi_pct, sizeof(deepwfi_pct), deepwfi, total);
  prv_format_pct(deepsleep_pct, sizeof(deepsleep_pct), deepsleep, total);

  int len = prv_append_boot(data->text, sizeof(data->text));
  len += snprintf(data->text + len, sizeof(data->text) - len,
                     "%s %" PRIu32 "d %" PRIu32 "h %" PRIu32 "m %" PRIu32 "s\n"
                     "Run %s\n"
                     "WFI %s\n"
                     "Deep WFI %s\n"
                     "Deep sleep %s\n"
                     "Idle entries/min %" PRIu32 "\n"
                     " WFI %" PRIu32 "\n"
                     " Deep WFI %" PRIu32 "\n"
                     " Deep sleep %" PRIu32 "\n"
                     "IPC not idle %" PRIu32 "\n",
                     s_baseline_valid ? "Since reset" : "Since boot", days, hours, minutes, secs,
                     run_pct, wfi_pct, deepwfi_pct, deepsleep_pct,
                     (seconds == 0) ? 0 : (uint32_t)((uint64_t)entries * 60 / seconds), wfi_count,
                     deepwfi_count, deepsleep_count, ipc_count);

  const BatteryChargeState battery = battery_get_charge_state();
  len += snprintf(data->text + len, sizeof(data->text) - len, "Battery %u%% %u mV\n",
                  battery.charge_percent, battery_state_get_voltage());

  // Too big for the app stack.
  SocSf32lbWakeStats *wake = app_malloc_check(sizeof(*wake));
  soc_sf32lb_wake_stats_get(wake);
  len += prv_append_wakes(data->text + len, sizeof(data->text) - len, wake, wake_base, seconds);
  app_free(wake);
  len += prv_append_timers(data->text + len, sizeof(data->text) - len, seconds);

  if (!s_baseline_valid) {
    snprintf(data->text + len, sizeof(data->text) - len, "Hold Select to start a battery run");
  } else if (battery.is_plugged || s_baseline.charging) {
    snprintf(data->text + len, sizeof(data->text) - len, "Charger seen, reset to measure");
  } else {
    // Whole percents from the fuel gauge, so this needs a few hours to mean anything.
    const int drop = (int)s_baseline.charge_percent - (int)battery.charge_percent;
    const uint32_t per_day_x10 =
        (drop <= 0 || seconds == 0) ? 0 : (uint32_t)((uint64_t)drop * 864000 / seconds);
    snprintf(data->text + len, sizeof(data->text) - len,
             "Drop %d%%, %" PRIu32 ".%" PRIu32 "%%/day", drop, per_day_x10 / 10,
             per_day_x10 % 10);
  }

  text_layer_set_text(&data->text_layer, data->text);

  // Grow the text layer to fit, then let the scroll layer page through it.
  const GRect bounds = scroll_layer_get_layer(&data->scroll_layer)->bounds;
  layer_set_frame(&data->text_layer.layer, &GRect(0, 0, bounds.size.w, 2000));
  GSize size = app_text_layer_get_content_size(&data->text_layer);
  size.w = bounds.size.w;
  size.h += 8;
  layer_set_frame(&data->text_layer.layer, &(GRect){.size = size});
  scroll_layer_set_content_size(&data->scroll_layer, size);
}

static void prv_select_click_handler(ClickRecognizerRef recognizer, void *context) {
  prv_update_text(context);
}

static void prv_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  const BatteryChargeState battery = battery_get_charge_state();
  soc_sf32lb_sleep_stats_get(&s_baseline.sleep);
  soc_sf32lb_wake_stats_get(&s_baseline.wake);
  prv_baseline_threads_and_timers();
  s_baseline.charge_percent = battery.charge_percent;
  s_baseline.charging = battery.is_plugged;
  s_baseline_valid = true;
  prv_update_text(context);
}

static void prv_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, prv_select_long_click_handler, nullptr);
}

static void prv_window_load(Window *window) {
  PowerStatsWindowData *data = (PowerStatsWindowData *)window;

  status_bar_layer_init(&data->status_layer);
  status_bar_layer_set_title(&data->status_layer, i18n_get("Power Stats", data), false, false);
  status_bar_layer_set_separator_mode(&data->status_layer, OPTION_MENU_STATUS_SEPARATOR_MODE);
  status_bar_layer_set_colors(&data->status_layer, GColorWhite, GColorBlack);
  layer_add_child(&window->layer, status_bar_layer_get_layer(&data->status_layer));

  GRect bounds = grect_inset(window->layer.bounds, (GEdgeInsets){
                                                     .top = STATUS_BAR_LAYER_HEIGHT,
                                                     .left = 4,
                                                     .right = 4,
                                                   });
  scroll_layer_init(&data->scroll_layer, &bounds);
  scroll_layer_set_context(&data->scroll_layer, data);
  scroll_layer_set_callbacks(&data->scroll_layer, (ScrollLayerCallbacks){
                                                    .click_config_provider =
                                                        prv_click_config_provider,
                                                  });
  scroll_layer_set_click_config_onto_window(&data->scroll_layer, window);
  layer_add_child(&window->layer, scroll_layer_get_layer(&data->scroll_layer));

  text_layer_init_with_parameters(&data->text_layer, &GRect(0, 0, bounds.size.w, 0), "",
                                  fonts_get_system_font(FONT_KEY_GOTHIC_18), GColorBlack,
                                  GColorClear, GTextAlignmentLeft, GTextOverflowModeWordWrap);
  scroll_layer_add_child(&data->scroll_layer, &data->text_layer.layer);

  prv_update_text(data);
}

static void prv_window_unload(Window *window) {
  PowerStatsWindowData *data = (PowerStatsWindowData *)window;
  text_layer_deinit(&data->text_layer);
  scroll_layer_deinit(&data->scroll_layer);
  status_bar_layer_deinit(&data->status_layer);
  i18n_free_all(data);
  app_free(data);
}

void power_stats_window_push(void) {
  PowerStatsWindowData *data = app_zalloc_check(sizeof(*data));

  window_init(&data->window, WINDOW_NAME("Power Stats"));
  window_set_window_handlers(&data->window, &(WindowHandlers){
                                              .load = prv_window_load,
                                              .unload = prv_window_unload,
                                            });

  app_window_stack_push(&data->window, true /* animated */);
}

#endif // CONFIG_SOC_SF32LB52
