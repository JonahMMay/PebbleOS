/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <inttypes.h>

#include <pbl/services/analytics/analytics.h>
#include <pbl/soc/sf32lb/sleep.h>
#include <pbl/util/math.h>

#include <board/board.h>
#include <drivers/flash.h>
#include <drivers/rtc.h>
#include <drivers/sf32lb52/rc10k.h>
#include <kernel/util/idle.h>

#ifdef CONFIG_SHELL
#include <pbl/shell/shell.h>
#endif

#include <pbl/kernel/idle.h>
#include <pbl/kernel/irq.h>
#include <pbl/kernel/section.h>

#include <bf0_hal.h>
#include <ipc_queue.h>

// HAL tick counter (milliseconds) - used by HAL timeout functions
extern __IO uint32_t uwTick;

static LPTIM_HandleTypeDef s_lptim = {
  .Instance = LPTIM1,
};

// CPU analytics tracking
typedef enum {
  SleepTypeNone = 0,
  SleepTypeWfi,
  SleepTypeDeepWfi,
} SleepType;

static volatile SleepType s_last_sleep_type;
static RtcTicks s_analytics_wfi_ticks;
static RtcTicks s_analytics_deepwfi_ticks;
static RtcTicks s_analytics_deepsleep_ticks;
static RtcTicks s_last_ticks;
static uint32_t s_analytics_ipc_not_idle_count;
static bool s_force_wfi;

// Since-boot totals for the power stats screen. The analytics counters above are folded in at
// each heartbeat, before they are reset; the entry counts are never reset.
static RtcTicks s_total_wfi_ticks;
static RtcTicks s_total_deepwfi_ticks;
static RtcTicks s_total_deepsleep_ticks;
static uint32_t s_total_ipc_not_idle_count;
static uint32_t s_wfi_count;
static uint32_t s_deepwfi_count;
static uint32_t s_deepsleep_count;

// What ended each sleep and how long the idle windows were, since boot.
static SocSf32lbWakeStats s_wake_stats;

_Static_assert(SOC_SF32LB_WAKE_IRQS == CONFIG_NUM_IRQS, "wake stats must cover every IRQ");
#define NVIC_WORDS ((SOC_SF32LB_WAKE_IRQS + 31) / 32)

//! Early wake-up ticks (to avoid over-sleeping due to wake-up latency)
static const uint32_t EARLY_WAKEUP_TICKS = 4;
//! Minimum ticks to enter deep sleep
static const uint32_t MIN_DEEPSLEEP_TICKS = RTC_TICKS_HZ / 20;
//! Maximum LPTIM counter value (24-bit)
static const uint32_t MAX_LPTIM_CNT = 0xFFFFFFUL;

static uint32_t s_iser_bak[16];

static void prv_save_iser(void) {
  uint32_t i;
  for (i = 0; i < 16; i++) {
    s_iser_bak[i] = NVIC->ISER[i];
    NVIC->ICER[i] = 0xFFFFFFFF;
    __DSB();
    __ISB();
  }
}

static void prv_restore_iser(void) {
  uint32_t i;
  for (i = 0; i < 16; i++) {
    __COMPILER_BARRIER();
    NVIC->ISER[i] = s_iser_bak[i];
    __COMPILER_BARRIER();
  }
}

//! Counts what ended a sleep. Runs with interrupts masked, so whatever woke the CPU is still
//! pending; @p enabled is the NVIC enable mask that was in effect while asleep.
PBL_SECTION_RAM static void prv_count_wake(const volatile uint32_t *enabled, bool check_systick) {
  bool found = false;

  if (check_systick && (SCB->ICSR & SCB_ICSR_PENDSTSET_Msk)) {
    s_wake_stats.systick++;
    found = true;
  }

  for (uint32_t w = 0; w < NVIC_WORDS; w++) {
    uint32_t pending = NVIC->ISPR[w] & enabled[w];
    while (pending) {
      const uint32_t irq = w * 32 + __builtin_ctz(pending);
      pending &= pending - 1;
      if (irq < SOC_SF32LB_WAKE_IRQS) {
        s_wake_stats.irq[irq]++;
        found = true;
      }
    }
  }

  if (!found) {
    s_wake_stats.none++;
  }
}

//! Counts an idle window by length, to show how many a lower deep sleep threshold would catch.
PBL_SECTION_RAM static void prv_count_gap(pbl_tick_t max_ticks) {
  // Upper bounds in ticks, compared directly so this stays clear of library division.
  static const pbl_tick_t bounds[SOC_SF32LB_GAP_BUCKETS - 1] = {
    RTC_TICKS_HZ * 2 / 1000,  RTC_TICKS_HZ * 5 / 1000,  RTC_TICKS_HZ * 10 / 1000,
    RTC_TICKS_HZ * 20 / 1000, RTC_TICKS_HZ * 50 / 1000, RTC_TICKS_HZ * 200 / 1000,
  };
  uint32_t bucket = 0;
  while (bucket < SOC_SF32LB_GAP_BUCKETS - 1 && max_ticks >= bounds[bucket]) {
    bucket++;
  }
  s_wake_stats.gap[bucket]++;
}

static inline void prv_enter_wfi(void) {
  s_last_sleep_type = SleepTypeWfi;
  s_wfi_count++;
  __WFI();
  prv_count_wake(NVIC->ISER, true);
}

static void prv_enter_deepwfi(void) {
  s_last_sleep_type = SleepTypeDeepWfi;
  s_deepwfi_count++;

  flash_power_down_for_stop_mode();

  __DSB();
  __ISB();

  SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
  __WFI();
  SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
  prv_count_wake(NVIC->ISER, true);
}

PBL_SECTION_RAM static void prv_enter_deepslep(void) {
  QSPIPortState *flash_state;
  uint32_t dll1_freq = 0UL;
  int clk_src;

  flash_state = QSPI_FLASH->qspi->state;

  s_deepsleep_count++;

  prv_save_iser();

  HAL_FLASH_NOP_CMD(&flash_state->ctx.handle);
  HAL_FLASH_DEEP_PWRDOWN(&flash_state->ctx.handle);
  HAL_Delay_us(flash_state->t_enter_deep_us);

  NVIC_EnableIRQ(AON_IRQn);

  clk_src = HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_SYS);
  if (clk_src == RCC_SYSCLK_DLL1) {
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_HRC48);
    dll1_freq = HAL_RCC_HCPU_GetDLL1Freq();
    HAL_RCC_HCPU_DisableDLL1();
  }

  HAL_HPAON_DISABLE_PAD();
  HAL_HPAON_DISABLE_VHP();

  HAL_HPAON_CLEAR_HP_ACTIVE();
  HAL_HPAON_SET_POWER_MODE(AON_PMR_DEEP_SLEEP);

  __WFI();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();
  __NOP();

  // The AON interrupt is only enabled to wake us, so count against the mask from before sleep,
  // and record which wake sources fired before the AON handler clears them.
  uint32_t wsr = HAL_HPAON_GET_WSR();
  while (wsr) {
    s_wake_stats.deepsleep_source[__builtin_ctz(wsr)]++;
    wsr &= wsr - 1;
  }
  prv_count_wake(s_iser_bak, false);

  HAL_HPAON_ENABLE_PAD();
  HAL_HPAON_ENABLE_VHP();

  HAL_HPAON_SET_HP_ACTIVE();
  HAL_HPAON_CLEAR_POWER_MODE();

  // Wait for HXT48 to be ready
  if (dll1_freq != 0UL) {
    while (0 == (hwp_hpsys_aon->ACR & HPSYS_AON_ACR_HXT48_RDY)) {
      __NOP();
    }
  }

  if (clk_src == RCC_SYSCLK_DLL1) {
    HAL_RCC_HCPU_EnableDLL1(dll1_freq);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_DLL1);
    HAL_Delay_us(0);
  }

  HAL_FLASH_RELEASE_DPD(&flash_state->ctx.handle);
  HAL_Delay_us(flash_state->t_exit_deep_us);

  prv_restore_iser();
}

static uint32_t prv_calc_elapsed_ticks(uint32_t gtimer_cyc) {
  static uint16_t s_err_milli_ticks;
  uint32_t elapsed_milli_ticks;
  uint32_t elapsed_ticks;

  elapsed_milli_ticks = rc10k_cyc_to_milli_ticks(gtimer_cyc);
  elapsed_ticks = elapsed_milli_ticks / 1000U;
  s_err_milli_ticks += (elapsed_milli_ticks % 1000U);
  if (s_err_milli_ticks >= 1000U) {
    elapsed_ticks++;
    s_err_milli_ticks -= 1000U;
  }

  return elapsed_ticks;
}

PBL_SECTION_RAM void pbl_soc_idle(pbl_tick_t max_ticks) {
  if (!idle_is_allowed()) {
    return;
  }

  __disable_irq();

  if (!ipc_queue_check_idle()) {
    s_analytics_ipc_not_idle_count++;
    goto end;
  }

  if (pbl_idle_confirm()) {
    SocSf32lbSleepLevel max_level = soc_sf32lb_sleep_max_level();

    prv_count_gap(max_ticks);

    // Deep sleep needs a minimum idle window.
    if (max_ticks < MIN_DEEPSLEEP_TICKS) {
      max_level = MIN(max_level, SOC_SF32LB_DEEPWFI);
    }

    // The debug flag forces plain WFI (deep WFI and deep sleep disabled).
    if (s_force_wfi) {
      max_level = MIN(max_level, SOC_SF32LB_WFI);
    }

    switch (max_level) {
      case SOC_SF32LB_WFI:
        prv_enter_wfi();
        break;
      case SOC_SF32LB_DEEPWFI:
        prv_enter_deepwfi();
        break;
      case SOC_SF32LB_DEEPSLEEP: {
        uint32_t gtimer_start;
        uint32_t gtimer_stop;
        uint32_t gtimer_delta;
        uint32_t sleep_ticks;
        uint32_t lptim_ticks;
        uint32_t elapsed_ticks;

        // stop systick
        SysTick->CTRL &= ~(SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);
        SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;

        // configure LPTIM to wake us up after expected idle time
        sleep_ticks = max_ticks - EARLY_WAKEUP_TICKS;
        lptim_ticks = MIN(sleep_ticks * rc10k_get_freq_hz() / RTC_TICKS_HZ, MAX_LPTIM_CNT);
        HAL_LPTIM_Counter_Start_IT(&s_lptim, lptim_ticks);

        gtimer_start = HAL_GTIMER_READ();

        prv_enter_deepslep();

        // NOTE: GTIMER needs at least 1 LP clock cycle to be updated after sleep,
        // so spin until we see a change
        do {
          gtimer_stop = HAL_GTIMER_READ();
        } while (gtimer_stop == gtimer_start);

        if (gtimer_stop < gtimer_start) {
          gtimer_delta = (UINT32_MAX - gtimer_start) + gtimer_stop + 1UL;
        } else {
          gtimer_delta = gtimer_stop - gtimer_start;
        }

        elapsed_ticks = prv_calc_elapsed_ticks(gtimer_delta);

        pbl_idle_slept(elapsed_ticks);

        // increment HAL tick counter by elapsed ticks
        uwTick += elapsed_ticks;

        // Force RTC synchronization of shadow registers
        hwp_rtc->ISR &= RTC_RSF_MASK;

        // Track deep sleep time for analytics
        s_analytics_deepsleep_ticks += elapsed_ticks;

        // stop LPTIM
        HAL_LPTIM_Counter_Stop_IT(&s_lptim);

        // enable systick
        SysTick->CTRL |= (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);
        break;
      }
      default:
        break;
    }
  }

end:
  __enable_irq();
}

bool pbl_soc_tick_enable(void) {
  HAL_LPTIM_InitDefault(&s_lptim);
  HAL_LPTIM_Init(&s_lptim);

  // configure SYSTICK
  // - use HXT48 as TICK reference clock
  // - divide TICK reference clock to 1.92MHz (48MHz / 25) (TICK_CLK)
  // - enable TICK_CLK as SYSTICK clock source
  // - configure SYSTICK to generate interrupt at RTC_TICKS_HZ rate
  //
  // HXT48│\                        ┌────────────────┐
  //  ────┼ \                       │         SYSTICK│
  // HRC48│  │  ┌───────┐  TICK_CLK ││\              │
  //  ────┼  ├──│TICKDIV├───────────┼┤ │   ┌──────┐  │
  //   ...│  │  └───────┘.         ┌┼┤ ├───│RELOAD├──┤
  //      │ /                      │││/    └──────┘  │
  //      │/                       ││                │
  //           HCLK                ││                │
  //          ─────────────────────┘└────────────────┘

  HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_HP_TICK, RCC_CLK_TICK_HXT48);
  // delay to avoid systick config failure (undocumented silicon issue)
  HAL_Delay_us(200);
  HAL_RCC_HCPU_SetTickDiv(25);
  HAL_SYSTICK_Config(1920000 / RTC_TICKS_HZ);
  HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_TICK_CLK);

  SysTick->CTRL |= (SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);

  // configure clock dividers for deep WFI:
  // HCLK = 48MHz / 12 = 4MHz
  // PCLK1 = 4MHz / 2^0 = 4MHz
  // PCLK2 = 4MHz / 2^1 = 2MHz
  HAL_RCC_HCPU_SetDeepWFIDiv(12, 0, 1);

  // TODO(SF32LB52): to use deep WFI when audio is ON, HCLK needs to remain
  // at 48MHz (div=1). Also, clock needs to be forced ON during deep WFI
  // (FORCE_HP bit in HPSYS_RCC_DBGR register)

  return true;
}

PBL_IRQ_DIRECT(AON, 0, PBL_IRQ_ZERO_LATENCY) {
  uint32_t status;

  NVIC_DisableIRQ(AON_IRQn);
  HAL_HPAON_CLEAR_POWER_MODE();

  status = HAL_HPAON_GET_WSR();
  status &= ~HPSYS_AON_WSR_PIN_ALL;
  HAL_HPAON_CLEAR_WSR(status);
}

void SysTick_Handler(void) {
  pbl_kernel_tick_isr();

  HAL_IncTick();

  if (s_last_sleep_type == SleepTypeWfi) {
    s_analytics_wfi_ticks++;
  } else if (s_last_sleep_type == SleepTypeDeepWfi) {
    s_analytics_deepwfi_ticks++;
  }

  s_last_sleep_type = SleepTypeNone;

  // TODO(SF32LB52): we may need to handle tick loss compensation when using
  // SysTick due to flash erase times (runs with IRQs disabled to not interfere
  // with XIP, and can easily span multiple ticks)
}

void pbl_analytics_external_collect_cpu_stats(void) {
  uint32_t wfi_ticks = s_analytics_wfi_ticks;
  uint32_t deepwfi_ticks = s_analytics_deepwfi_ticks;
  uint32_t deepsleep_ticks = s_analytics_deepsleep_ticks;

  RtcTicks now_ticks = rtc_get_ticks();
  uint32_t total_ticks = (uint32_t)(now_ticks - s_last_ticks);
  uint32_t running_ticks = total_ticks - wfi_ticks - deepwfi_ticks - deepsleep_ticks;

  // Calculate percentages
  uint16_t running_pct = 0;
  uint16_t wfi_pct = 0;
  uint16_t deepwfi_pct = 0;
  uint16_t deepsleep_pct = 0;

  if (total_ticks > 0) {
    running_pct = (uint16_t)((running_ticks * 10000ULL) / total_ticks);
    wfi_pct = (uint16_t)((wfi_ticks * 10000ULL) / total_ticks);
    deepwfi_pct = (uint16_t)((deepwfi_ticks * 10000ULL) / total_ticks);
    deepsleep_pct = (uint16_t)((deepsleep_ticks * 10000ULL) / total_ticks);
  }

  // SF32LB52: sleep0 = WFI, sleep1 = Deep WFI, sleep2 = Deep sleep
  PBL_ANALYTICS_SET_UNSIGNED(cpu_running_pct, running_pct);
  PBL_ANALYTICS_SET_UNSIGNED(cpu_sleep0_pct, wfi_pct);
  PBL_ANALYTICS_SET_UNSIGNED(cpu_sleep1_pct, deepwfi_pct);
  PBL_ANALYTICS_SET_UNSIGNED(cpu_sleep2_pct, deepsleep_pct);
  PBL_ANALYTICS_SET_UNSIGNED(sifli_ipc_not_idle_count, s_analytics_ipc_not_idle_count);

  s_last_ticks = now_ticks;

  // Fold into the since-boot totals and reset in one go, so soc_sf32lb_sleep_stats_get() never
  // sees a tick counted twice or not at all.
  pbl_irq_lock();
  s_total_wfi_ticks += s_analytics_wfi_ticks;
  s_total_deepwfi_ticks += s_analytics_deepwfi_ticks;
  s_total_deepsleep_ticks += s_analytics_deepsleep_ticks;
  s_total_ipc_not_idle_count += s_analytics_ipc_not_idle_count;
  s_analytics_wfi_ticks = 0;
  s_analytics_deepwfi_ticks = 0;
  s_analytics_deepsleep_ticks = 0;
  s_analytics_ipc_not_idle_count = 0;
  pbl_irq_unlock();
}

void soc_sf32lb_sleep_stats_get(SocSf32lbSleepStats *stats) {
  const RtcTicks now_ticks = rtc_get_ticks();

  pbl_irq_lock();
  *stats = (SocSf32lbSleepStats){
    .total_ticks = now_ticks,
    .wfi_ticks = s_total_wfi_ticks + s_analytics_wfi_ticks,
    .deepwfi_ticks = s_total_deepwfi_ticks + s_analytics_deepwfi_ticks,
    .deepsleep_ticks = s_total_deepsleep_ticks + s_analytics_deepsleep_ticks,
    .wfi_count = s_wfi_count,
    .deepwfi_count = s_deepwfi_count,
    .deepsleep_count = s_deepsleep_count,
    .ipc_not_idle_count = s_total_ipc_not_idle_count + s_analytics_ipc_not_idle_count,
  };
  pbl_irq_unlock();
}

void soc_sf32lb_wake_stats_get(SocSf32lbWakeStats *stats) {
  pbl_irq_lock();
  *stats = s_wake_stats;
  pbl_irq_unlock();
}

const char *soc_sf32lb_irq_name(uint32_t irq) {
  switch (irq) {
    case AON_IRQn:
      return "AON";
    case LPTIM1_IRQn:
      return "LPTIM1";
    case LPTIM2_IRQn:
      return "LPTIM2";
    case LPTIM3_IRQn:
      return "LPTIM3";
    case PMUC_IRQn:
      return "PMUC";
    case RTC_IRQn:
      return "RTC";
    case LCPU2HCPU_IRQn:
      return "LCPU IPC";
    case GPIO1_IRQn:
      return "GPIO1";
    case GPIO2_IRQn:
      return "GPIO2";
    case USART1_IRQn:
      return "USART1";
    case USART2_IRQn:
      return "USART2";
    case USART3_IRQn:
      return "USART3";
    case SPI1_IRQn:
      return "SPI1";
    case SPI2_IRQn:
      return "SPI2";
    case I2C1_IRQn:
      return "I2C1";
    case I2C2_IRQn:
      return "I2C2";
    case I2C3_IRQn:
      return "I2C3";
    case I2C4_IRQn:
      return "I2C4";
    case LCDC1_IRQn:
      return "LCDC1";
    case EPIC_IRQn:
      return "EPIC";
    case EXTDMA_IRQn:
      return "EXTDMA";
    case GPADC_IRQn:
      return "GPADC";
    case MPI1_IRQn:
      return "MPI1";
    case MPI2_IRQn:
      return "MPI2";
    case GPTIM1_IRQn:
      return "GPTIM1";
    case GPTIM2_IRQn:
      return "GPTIM2";
    case BTIM1_IRQn:
      return "BTIM1";
    case BTIM2_IRQn:
      return "BTIM2";
    case ATIM1_IRQn:
      return "ATIM1";
    case PDM1_IRQn:
      return "PDM1";
    case I2S1_IRQn:
      return "I2S1";
    case AUDPRC_IRQn:
      return "AUDPRC";
    case AUD_HP_IRQn:
      return "AUD_HP";
    case USBC_IRQn:
      return "USBC";
    case TSEN_IRQn:
      return "TSEN";
    default:
      if (irq >= DMAC1_CH1_IRQn && irq <= DMAC1_CH8_IRQn) {
        return "DMAC1";
      }
      return NULL;
  }
}

const char *soc_sf32lb_wake_source_name(uint32_t source) {
  switch (source) {
    case HPSYS_AON_WSR_RTC_Pos:
      return "RTC";
    case HPSYS_AON_WSR_GPIO1_Pos:
      return "GPIO1";
    case HPSYS_AON_WSR_LPTIM1_Pos:
      return "LPTIM1";
    case HPSYS_AON_WSR_PMUC_Pos:
      return "PMUC";
    case HPSYS_AON_WSR_LP2HP_REQ_Pos:
      return "LCPU req";
    case HPSYS_AON_WSR_LP2HP_IRQ_Pos:
      return "LCPU irq";
    default: {
      static const char *const pins[] = {
        "PIN0",  "PIN1",  "PIN2",  "PIN3",  "PIN4",  "PIN5",  "PIN6",
        "PIN7",  "PIN8",  "PIN9",  "PIN10", "PIN11", "PIN12", "PIN13",
        "PIN14", "PIN15", "PIN16", "PIN17", "PIN18", "PIN19", "PIN20",
      };
      if (source >= HPSYS_AON_WSR_PIN0_Pos && source <= HPSYS_AON_WSR_PIN20_Pos) {
        return pins[source - HPSYS_AON_WSR_PIN0_Pos];
      }
      return NULL;
    }
  }
}

#ifdef CONFIG_SHELL
static int prv_cmd_cpustats(const struct pbl_shell *sh, size_t argc, char **argv) {
  uint32_t wfi_ticks = s_analytics_wfi_ticks;
  uint32_t deepwfi_ticks = s_analytics_deepwfi_ticks;
  uint32_t deepsleep_ticks = s_analytics_deepsleep_ticks;

  RtcTicks now_ticks = rtc_get_ticks();
  uint32_t total_ticks = (uint32_t)(now_ticks - s_last_ticks);
  uint32_t running_ticks = total_ticks - wfi_ticks - deepwfi_ticks - deepsleep_ticks;

  pbl_shell_print(sh, "Run:       %" PRIu32 " ticks (%" PRIu32 " %%)", running_ticks,
                  (running_ticks * 100) / total_ticks);
  pbl_shell_print(sh, "WFI:       %" PRIu32 " ticks (%" PRIu32 " %%)", wfi_ticks,
                  (wfi_ticks * 100) / total_ticks);
  pbl_shell_print(sh, "Deep WFI:  %" PRIu32 " ticks (%" PRIu32 " %%)", deepwfi_ticks,
                  (deepwfi_ticks * 100) / total_ticks);
  pbl_shell_print(sh, "Deepsleep: %" PRIu32 " ticks (%" PRIu32 " %%)", deepsleep_ticks,
                  (deepsleep_ticks * 100) / total_ticks);
  pbl_shell_print(sh, "Tot:       %" PRIu32 " ticks", total_ticks);
  return 0;
}

static int prv_cmd_wfi(const struct pbl_shell *sh, size_t argc, char **argv) {
  if (argv[1][0] == '1') {
    s_force_wfi = true;
    pbl_shell_print(sh, "WFI forced ON (deep WFI and deep sleep disabled)");
  } else {
    s_force_wfi = false;
    pbl_shell_print(sh, "WFI forced OFF (deep WFI and deep sleep allowed)");
  }
  return 0;
}

PBL_SHELL_SUBCMD_ADD(sub_sys, cpustats, nullptr, "Show CPU sleep statistics", prv_cmd_cpustats, 0,
                     0);
PBL_SHELL_SUBCMD_ADD(sub_sys, wfi, nullptr, "Force plain WFI when idle <0|1>", prv_cmd_wfi, 2, 0);
#endif
