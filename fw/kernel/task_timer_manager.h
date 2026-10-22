/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "task_timer.h"

#include <pbl/kernel/sem.h>
#include <pbl/util/list.h>

//! Internal state object. Each task that wants to execute timers should allocate their own
//! instance of this object.
typedef struct TaskTimerManager {
  struct pbl_mutex mutex;

  //! List of timers that are currently running
  ListNode *running_timers;
  //! List of timers that are allocated but unscheduled
  ListNode *idle_timers;

  //! The next ID to assign to a new timer.
  TaskTimerID next_id;

  //! Externally provided semaphore that is given whenever the next timer to expire has changed.
  struct pbl_sem *semaphore;

  //! The callback we're currently executing, useful for debugging.
  void *current_cb;
} TaskTimerManager;

//! Number of distinct callbacks task_timer_cb_stats_get() reports.
#define TASK_TIMER_CB_STATS 32

//! How many times a timer callback ran.
typedef struct TaskTimerCbStat {
  //! The callback, or NULL for the ones that did not fit.
  void *cb;
  uint32_t count;
} TaskTimerCbStat;

//! Count one run of @p cb. Also called by services that dispatch timers of their own, with the
//! callback they dispatch to.
void task_timer_cb_stats_record(void *cb);

//! Copy the callback counts since boot. Callbacks seen after the table filled up are counted
//! together in the last entry.
void task_timer_cb_stats_get(TaskTimerCbStat stats[TASK_TIMER_CB_STATS]);

//! Initialize a passed in manager object.
//! @param[in] manager The manager object to initialize
//! @param[in] semaphore a semaphore the TaskTimerManager should give if the next expiring timer
//!                      has changed. The task event loop should block on this same semaphore to
//!                      handle timer updates in a timely fashion.
void task_timer_manager_init(TaskTimerManager *manager, struct pbl_sem *semaphore);

//! Execute any timers that are currently expired.
//! @return the number of ticks until the next timer expires. If there are no timers running,
//!         returns PBL_TICK_FOREVER.
pbl_tick_t task_timer_manager_execute_expired_timers(TaskTimerManager *manager);

//! Debugging interface to help understand why the task_timer execution is stuck and what
//! its stuck on.
//! @return A pointer to the current callback that's running, NULL if no callback
//!         is currently running.
void *task_timer_manager_get_current_cb(const TaskTimerManager *manager);
