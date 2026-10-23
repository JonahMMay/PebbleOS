/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/**
 * @defgroup services_boot_splash Boot splash
 * @ingroup services
 * @brief Splash screen shown while the firmware boots.
 *
 * With @c CONFIG_PBLBOOT or on QEMU, the firmware draws the logo with an animated progress bar
 * from its own task. Elsewhere the bootloader shows the splash and these functions do nothing.
 * @{
 */

/**
 * @brief Start the boot splash screen.
 *
 * Starts the animation task, which initializes the display first, so the caller can carry on
 * while the display comes up.
 */
void boot_splash_start(void);

/**
 * @brief Wait for the boot splash to finish initializing the display.
 *
 * Call before display_init(), which must not run while the animation task brings the display up.
 * The animation keeps running.
 */
void boot_splash_wait_for_display(void);

/**
 * @brief Stop the boot splash screen.
 *
 * Call when the first app is about to draw. Waits for the animation task to exit, so the display
 * is initialized on return, and leaves the logo on screen without the progress bar.
 */
void boot_splash_stop(void);

/** @} */
