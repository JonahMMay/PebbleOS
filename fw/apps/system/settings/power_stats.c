/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "power_stats.h"

#ifdef CONFIG_SOC_SF32LB52

#include <inttypes.h>
#include <stdio.h>

#include <pbl/drivers/rtc.h>
#include <pbl/services/battery/battery_state.h>
#include <pbl/services/i18n/i18n.h>
#include <pbl/soc/sf32lb/sleep.h>
#include <pbl/util/time.h>

#include <applib/fonts/fonts.h>
#include <applib/ui/option_menu_window.h>
#include <applib/ui/ui.h>
#include <kernel/pbl_malloc.h>

typedef struct PowerStatsBaseline {
  SocSf32lbSleepStats sleep;
  uint8_t charge_percent;
  bool charging;
} PowerStatsBaseline;

typedef struct PowerStatsWindowData {
  Window window; //!< Must be first: the base layer is cast back to this struct.
  StatusBarLayer status_layer;
  ScrollLayer scroll_layer;
  TextLayer text_layer;
  char text[512];
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

static void prv_update_text(PowerStatsWindowData *data) {
  SocSf32lbSleepStats now;
  soc_sf32lb_sleep_stats_get(&now);

  const SocSf32lbSleepStats *base = &s_baseline.sleep;
  if (!s_baseline_valid) {
    static const SocSf32lbSleepStats s_boot;
    base = &s_boot;
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

  int len = snprintf(data->text, sizeof(data->text),
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
