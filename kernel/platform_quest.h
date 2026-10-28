/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_PLATFORM_QUEST_H
#define QUEST_EL1_PLATFORM_QUEST_H

#include <linux/types.h>

/*
 * Call after serializing against reboot/suspend, freeze_processes() and
 * device_block_probing(), on the same task that owns the freezer. Blocking
 * probes makes the live driver inventory stable through device_shutdown().
 * No hardware state changes in prepare().
 * A failed prepare leaves all callbacks and module references unchanged.
 *
 * Successful prepare pins the loader and participating driver modules. Call
 * cancel() on EVERY subsequent recoverable failure, before unblocking probes
 * and thaw_processes().
 * Once kernel_restart_prepare() begins, do not return to userspace. Check
 * shutdown_ok() after device_shutdown(); failure then requires panic/reboot,
 * not an attempted resume. Successful handoff never calls cancel().
 *
 * By default this adapter rejects bound QTEE, QSEE and smcinvoke drivers.
 * Their secure-world state has no source-backed ownership transfer contract.
 * allow_tee=1 lets an operator proceed anyway; the target DTB must then
 * disable those nodes (the Quest Pro procedure). The adapter does not solve
 * the secure-world ownership transfer itself.
 *
 * Watchdogs: msm_watchdog (MMIO) and gh-watchdog (Gunyah SMC) are stopped
 * from their shutdown callbacks. Any other available watchdog DT node, or one
 * without a bound supported driver, makes prepare() fail.
 */
struct quest_report;

int quest_platform_prepare(void);
void quest_platform_cancel(void);
bool quest_platform_shutdown_ok(void);
void quest_platform_report(struct quest_report *r);
/* Reversible: stop and restart the watchdogs prepare() would handle. */
int quest_platform_rehearse_watchdog(unsigned int *stopped);

#endif
