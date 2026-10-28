/* SPDX-License-Identifier: GPL-2.0-only */
/* Check actual raw-symbol initialization and wrapper dispatch/failure cleanup. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define __nocfi
#define IS_ENABLED(option) (option)
#define CONFIG_FREEZER 1
#define CONFIG_KPROBES 1
#define pr_err(...) ((void)0)
#define pr_warn(...) ((void)0)
#define READ_ONCE(value) (value)
#define clamp_val(value, low, high) ((value) < (low) ? (low) : (value) > (high) ? (high) : (value))

struct mutex { bool locked; };
struct kprobe { const char *symbol_name; void *addr; };
static unsigned long lookup_addr;
static unsigned long (*lookup_ptr)(const char *name);
static void (*restart_prepare_ptr)(char *cmd);
static void (*migrate_ptr)(void);
static void (*shutdown_ptr)(void);
static struct mutex *transition_mutex_ptr;
static int (*freeze_ptr)(void);
static void (*thaw_ptr)(void);
static void (*block_probing_ptr)(void);
static void (*unblock_probing_ptr)(void);
static struct mutex transition_mutex;
static const char *missing;
static int probe_result, probe_registration, probe_removal, step;
static unsigned int freeze_attempts = 5, freeze_calls, sleeps, busy_calls = 1000;
static void msleep(unsigned int ms) { assert(ms == 200); sleeps++; }

static void stock_restart(char *cmd) { assert(!cmd); assert(step++ == 0); }
static void stock_migrate(void) { assert(step++ == 1); }
static void stock_shutdown(void) { assert(step++ == 2); }
static int stock_freeze(void)
{
	assert(step == 3);
	if (++freeze_calls <= busy_calls && freeze_calls < freeze_attempts)
		return -EBUSY; /* transient wakeup: wrapper retries */
	step++;
	return freeze_calls <= busy_calls ? -EBUSY : 0;
}
static void stock_thaw(void) { assert(step++ == 4); }
static void stock_block(void) { assert(step++ == 5); }
static void stock_unblock(void) { assert(step++ == 6); }

static unsigned long stock_lookup(const char *name)
{
	if (missing && !strcmp(name, missing)) return 0;
#define BIND(name_literal, address) if (!strcmp(name, name_literal)) return (unsigned long)(address)
	BIND("kernel_restart_prepare", stock_restart);
	BIND("migrate_to_reboot_cpu", stock_migrate);
	BIND("machine_shutdown", stock_shutdown);
	BIND("system_transition_mutex", &transition_mutex);
	BIND("freeze_processes", stock_freeze);
	BIND("thaw_processes", stock_thaw);
	BIND("device_block_probing", stock_block);
	BIND("device_unblock_probing", stock_unblock);
#undef BIND
	return 0;
}

static int register_kprobe(struct kprobe *probe)
{
	assert(!strcmp(probe->symbol_name, "kallsyms_lookup_name"));
	probe_registration++;
	probe->addr = (void *)stock_lookup;
	return probe_result;
}

static void unregister_kprobe(struct kprobe *probe)
{
	assert(probe->addr == (void *)stock_lookup);
	probe_removal++;
}

void kexec_compat_unload(void);

/* ACTUAL_FUNCTIONS */

static void clean(void)
{
	assert(!lookup_ptr && !restart_prepare_ptr && !migrate_ptr && !shutdown_ptr);
	assert(!transition_mutex_ptr && !freeze_ptr && !thaw_ptr);
	assert(!block_probing_ptr && !unblock_probing_ptr);
	assert(!quest_lookup_name("system_transition_mutex"));
}

int main(void)
{
	const char *symbols[] = {
		"kernel_restart_prepare", "migrate_to_reboot_cpu", "machine_shutdown",
		"system_transition_mutex", "freeze_processes", "thaw_processes",
		"device_block_probing", "device_unblock_probing",
	};
	size_t i;

	for (i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++) {
		missing = symbols[i];
		assert(kexec_compat_load() == -ENOENT);
		clean();
	}
	assert(probe_registration == 8 && probe_removal == 8);

	missing = NULL;
	probe_result = -EPERM;
	assert(kexec_compat_load() == -EPERM);
	assert(probe_registration == 9 && probe_removal == 8);
	clean();
	probe_result = 0;

	lookup_addr = 1; /* Reject an invalid explicitly supplied address. */
	assert(kexec_compat_load() == -EINVAL);
	clean();
	lookup_addr = 0;

	assert(kexec_compat_load() == 0);
	assert(quest_transition_mutex() == &transition_mutex);
	assert(quest_lookup_name("machine_shutdown") == (unsigned long)stock_shutdown);
	quest_kernel_restart_prepare(NULL);
	quest_migrate_to_reboot_cpu();
	quest_machine_shutdown();
	assert(quest_freeze_processes() == -EBUSY);
	assert(freeze_calls == 5 && sleeps == 4); /* bounded retries, then fail */
	quest_thaw_processes();
	quest_block_probing();
	quest_unblock_probing();
	assert(step == 7);
	step = 3; freeze_calls = sleeps = 0; busy_calls = 2;
	assert(quest_freeze_processes() == 0); /* succeeds on the third attempt */
	assert(freeze_calls == 3 && sleeps == 2 && step == 4);
	step = 3; freeze_calls = sleeps = 0; busy_calls = 1000; freeze_attempts = 0;
	assert(quest_freeze_processes() == -EBUSY); /* clamped to one attempt */
	assert(freeze_calls == 1 && sleeps == 0);
	step = 7;
	kexec_compat_unload();
	clean();
	kexec_compat_unload();
	clean();
	puts("compat_bindings: 13 production initialization/dispatch scenarios passed");
	return 0;
}
