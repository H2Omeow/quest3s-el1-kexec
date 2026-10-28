/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stubs around the actual quest_execute(), injected by test_handoff.py. */
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mutex { bool locked; };
struct kimage { int unused; };
static struct mutex transition_mutex, kexec_mutex;
static struct kimage image, *kexec_image;
static bool kexec_in_progress, frozen, probes_blocked, irreversible;
static bool platform_held, shutdown_good, transfer_returns;
static int freeze_error, arch_error, memory_error, platform_error;
static char events[1024];
static jmp_buf escape;

static void note(const char *name)
{
	assert(strlen(events) + strlen(name) + 2 < sizeof(events));
	strcat(events, name);
	strcat(events, " ");
}

static struct mutex *quest_transition_mutex(void) { return &transition_mutex; }

static int mutex_trylock(struct mutex *lock)
{
	note(lock == &transition_mutex ? "transition_lock" : "image_lock");
	if (lock->locked)
		return 0;
	lock->locked = true;
	return 1;
}

static void mutex_unlock(struct mutex *lock)
{
	assert(!irreversible && lock->locked);
	note(lock == &transition_mutex ? "transition_unlock" : "image_unlock");
	lock->locked = false;
}

static int quest_freeze_processes(void)
{
	assert(transition_mutex.locked && !frozen);
	note("freeze");
	frozen = !freeze_error; /* Real freeze_processes thaws itself on failure. */
	return freeze_error;
}

static void quest_thaw_processes(void)
{
	assert(!irreversible && frozen && !probes_blocked && !platform_held);
	note("thaw");
	frozen = false;
}

static int quest_arch_check(void)
{
	assert(frozen && kexec_mutex.locked);
	note("arch");
	return arch_error;
}

static int quest_validate_destinations(struct kimage *loaded)
{
	assert(loaded == &image && frozen && probes_blocked);
	assert(kexec_mutex.locked && !platform_held);
	note("memory");
	return memory_error;
}

static void quest_block_probing(void)
{
	assert(frozen && !probes_blocked);
	note("block");
	probes_blocked = true;
}

static int quest_platform_prepare(void)
{
	assert(frozen && probes_blocked && kexec_mutex.locked);
	note("prepare");
	platform_held = true;
	return platform_error;
}

static void quest_platform_cancel(void)
{
	assert(!irreversible && frozen && probes_blocked && platform_held);
	note("cancel");
	platform_held = false;
}

static void quest_unblock_probing(void)
{
	assert(!irreversible && !platform_held && probes_blocked);
	note("unblock");
	probes_blocked = false;
}

static void quest_kernel_restart_prepare(char *command)
{
	assert(!command && frozen && probes_blocked && platform_held);
	assert(kexec_mutex.locked && transition_mutex.locked && kexec_in_progress);
	note("restart_prepare");
	irreversible = true;
}

static bool quest_platform_shutdown_ok(void)
{
	assert(irreversible);
	note("shutdown_check");
	return shutdown_good;
}

static void quest_pci_quiesce(void) { assert(irreversible); note("pci"); }
static void quest_migrate_to_reboot_cpu(void) { note("migrate"); }
static void cpu_hotplug_enable(void) { note("hotplug_enable"); }
static void quest_machine_shutdown(void) { note("machine_shutdown"); }

static void machine_kexec(struct kimage *loaded)
{
	assert(irreversible && loaded == &image);
	note("transfer");
	if (!transfer_returns)
		longjmp(escape, 2);
}

static _Noreturn void panic(const char *message)
{
	assert(irreversible);
	note(strstr(message, "platform") ? "panic_platform" : "panic_return");
	longjmp(escape, 1);
}

/* ACTUAL_FUNCTIONS */

static void reset(void)
{
	transition_mutex.locked = kexec_mutex.locked = false;
	kexec_image = &image;
	kexec_in_progress = frozen = probes_blocked = irreversible = false;
	platform_held = transfer_returns = false;
	shutdown_good = true;
	freeze_error = arch_error = memory_error = platform_error = 0;
	events[0] = '\0';
}

static void expect_return(int error, const char *trace)
{
	int result = quest_execute();
	assert(result == error);
	assert(!frozen && !probes_blocked && !platform_held);
	assert(!irreversible && !kexec_in_progress);
	assert(!strcmp(events, trace));
}

static void expect_terminal(int expected, const char *trace)
{
	int jumped = setjmp(escape);
	if (!jumped) {
		(void)quest_execute();
		abort(); /* Neither success nor an irreversible error may return. */
	}
	assert(jumped == expected);
	assert(irreversible && frozen && probes_blocked && platform_held);
	assert(transition_mutex.locked && kexec_mutex.locked && kexec_in_progress);
	assert(!strcmp(events, trace));
}

#define PREFIX "transition_lock freeze image_lock arch block memory prepare "
#define TRANSFER PREFIX "restart_prepare shutdown_check pci migrate hotplug_enable machine_shutdown transfer "

int main(void)
{
	reset(); transition_mutex.locked = true;
	expect_return(-EBUSY, "transition_lock ");
	assert(transition_mutex.locked && !kexec_mutex.locked);

	reset(); freeze_error = -ETIMEDOUT;
	expect_return(-ETIMEDOUT, "transition_lock freeze transition_unlock ");
	assert(!transition_mutex.locked && !kexec_mutex.locked);

	reset(); kexec_mutex.locked = true;
	expect_return(-EBUSY, "transition_lock freeze image_lock thaw transition_unlock ");
	assert(!transition_mutex.locked && kexec_mutex.locked);

	reset(); kexec_image = NULL;
	expect_return(-EINVAL, "transition_lock freeze image_lock image_unlock thaw transition_unlock ");

	reset(); arch_error = -EOPNOTSUPP;
	expect_return(-EOPNOTSUPP, "transition_lock freeze image_lock arch image_unlock thaw transition_unlock ");

	reset(); memory_error = -EADDRNOTAVAIL;
	expect_return(-EADDRNOTAVAIL, "transition_lock freeze image_lock arch block memory unblock image_unlock thaw transition_unlock ");
	assert(!transition_mutex.locked && !kexec_mutex.locked);

	reset(); platform_error = -EOPNOTSUPP;
	expect_return(-EOPNOTSUPP, PREFIX "cancel unblock image_unlock thaw transition_unlock ");
	assert(!transition_mutex.locked && !kexec_mutex.locked);

	reset(); shutdown_good = false;
	expect_terminal(1, PREFIX "restart_prepare shutdown_check panic_platform ");

	reset();
	expect_terminal(2, TRANSFER);

	reset(); transfer_returns = true;
	expect_terminal(1, TRANSFER "panic_return ");

	puts("handoff_exec: 10 actual EXEC control-flow scenarios passed");
	return 0;
}
