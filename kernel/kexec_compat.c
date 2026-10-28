// SPDX-License-Identifier: GPL-2.0
/* Runtime resolution of stock helpers that have no module export. */
#define pr_fmt(fmt) "quest_el1: " fmt
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/kprobes.h>
#include <linux/freezer.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>

#include "kexec_compat.h"

static unsigned long lookup_addr;
module_param(lookup_addr, ulong, 0400);
MODULE_PARM_DESC(lookup_addr,
	"Explicit stock kallsyms_lookup_name address if kprobe lookup is unavailable");

static unsigned long (*lookup_ptr)(const char *name);
static void (*restart_prepare_ptr)(char *cmd);
static void (*migrate_ptr)(void);
static void (*shutdown_ptr)(void);
static struct mutex *transition_mutex_ptr;
static int (*freeze_ptr)(void);
static void (*thaw_ptr)(void);
static void (*block_probing_ptr)(void);
static void (*unblock_probing_ptr)(void);

/* kallsyms supplies raw addresses, not cross-DSO CFI jump-table pointers. */
unsigned long __nocfi quest_lookup_name(const char *name)
{
	return lookup_ptr ? lookup_ptr(name) : 0;
}

void __nocfi quest_kernel_restart_prepare(char *cmd)
{
	restart_prepare_ptr(cmd);
}

void __nocfi quest_migrate_to_reboot_cpu(void)
{
	migrate_ptr();
}

void __nocfi quest_machine_shutdown(void)
{
	shutdown_ptr();
}

struct mutex *quest_transition_mutex(void)
{
	return transition_mutex_ptr;
}

/*
 * Android keeps wakeup sources busy (autosleep, sensor/USB wake IRQs), and a
 * wakeup event racing the freezer makes freeze_processes() abort with -EBUSY
 * after thawing everything again. Such events are transient; retry a few
 * times instead of failing the whole handoff (the Quest Pro port hit this on
 * almost every attempt).
 */
static unsigned int freeze_attempts = 5;
module_param(freeze_attempts, uint, 0600);
MODULE_PARM_DESC(freeze_attempts, "freeze_processes() attempts on -EBUSY (1-20)");

int __nocfi quest_freeze_processes(void)
{
	unsigned int attempt, attempts = clamp_val(READ_ONCE(freeze_attempts), 1U, 20U);
	int ret;

	for (attempt = 1;; attempt++) {
		ret = freeze_ptr();
		if (ret != -EBUSY || attempt >= attempts)
			return ret;
		pr_warn("freeze_processes busy (wakeup event), retry %u/%u\n",
			attempt, attempts - 1);
		msleep(200);
	}
}

void __nocfi quest_thaw_processes(void)
{
	thaw_ptr();
}

void __nocfi quest_block_probing(void)
{
	block_probing_ptr();
}

void __nocfi quest_unblock_probing(void)
{
	unblock_probing_ptr();
}

int kexec_compat_load(void)
{
	int ret;

#if !IS_ENABLED(CONFIG_FREEZER)
	pr_err("The warm handoff requires CONFIG_FREEZER\n");
	return -EOPNOTSUPP;
#endif

	if (lookup_addr) {
		if ((lookup_addr & 3) || (lookup_addr >> 63) != 1)
			return -EINVAL;
		lookup_ptr = (void *)lookup_addr;
		pr_warn("Using explicitly supplied stock symbol lookup address\n");
	} else {
#if IS_ENABLED(CONFIG_KPROBES)
		struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

		ret = register_kprobe(&kp);
		if (ret) {
			pr_err("kprobe symbol bootstrap failed: %d\n", ret);
			return ret;
		}
		lookup_ptr = (void *)kp.addr;
		unregister_kprobe(&kp);
#else
		pr_err("No kprobes; supply the verified stock lookup_addr parameter\n");
		return -EOPNOTSUPP;
#endif
	}

	restart_prepare_ptr = (void *)quest_lookup_name("kernel_restart_prepare");
	migrate_ptr = (void *)quest_lookup_name("migrate_to_reboot_cpu");
	shutdown_ptr = (void *)quest_lookup_name("machine_shutdown");
	transition_mutex_ptr = (void *)quest_lookup_name("system_transition_mutex");
	freeze_ptr = (void *)quest_lookup_name("freeze_processes");
	thaw_ptr = (void *)quest_lookup_name("thaw_processes");
	block_probing_ptr = (void *)quest_lookup_name("device_block_probing");
	unblock_probing_ptr = (void *)quest_lookup_name("device_unblock_probing");
	if (!restart_prepare_ptr || !migrate_ptr || !shutdown_ptr ||
	    !transition_mutex_ptr || !freeze_ptr || !thaw_ptr ||
	    !block_probing_ptr || !unblock_probing_ptr) {
		pr_err("Required stock shutdown/freezer/probe helper is unavailable\n");
		kexec_compat_unload();
		return -ENOENT;
	}
	return 0;
}

void kexec_compat_unload(void)
{
	lookup_ptr = NULL;
	restart_prepare_ptr = NULL;
	migrate_ptr = NULL;
	shutdown_ptr = NULL;
	transition_mutex_ptr = NULL;
	freeze_ptr = NULL;
	thaw_ptr = NULL;
	block_probing_ptr = NULL;
	unblock_probing_ptr = NULL;
}
