// SPDX-License-Identifier: GPL-2.0-only
/*
 * QUEST_EL1_IOC_REHEARSE: run the risky pieces of the handoff one by one and
 * undo each of them, so that a single on-device session can show which stage
 * would fail, before anything irreversible is attempted.
 *
 *   WATCHDOG  stop and restart every watchdog prepare() would stop
 *   RPROC     rproc_shutdown()/rproc_boot() the selected remote processors
 *   CPUS      remove_cpu()/add_cpu() every secondary CPU (PSCI CPU_OFF path
 *             used by machine_shutdown()), then check none is stuck
 *   MMU       one page through the real transition blob with the MMU off
 *   COPY      every page of the loaded image through the real blob into
 *             scratch pages, verified, timed
 *
 * Stages run in that order; the first failing stage stops the sequence.
 * Not covered (by construction): device_shutdown() of all drivers, the
 * final branch into the new Image, and anything the new kernel does.
 */
#define pr_fmt(fmt) "quest_el1_rehearse: " fmt

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../include/quest_el1_uapi.h"
#include "kexec.h"
#include "kexec_compat.h"
#include "kexec_internal.h"
#include "platform_quest.h"
#include "platform_rproc.h"
#include "rehearse.h"
#include "report.h"
#include "arch/arm64/machine_kexec_compat.h"
#include "arch/arm64/rehearse.h"

static DEFINE_MUTEX(quest_last_lock);
static struct quest_el1_rehearse quest_last;
static bool quest_have_last;

static int quest_rehearse_cpus(u32 *offlined)
{
	static unsigned long removed[BITS_TO_LONGS(NR_CPUS)];
	unsigned int cpu;
	int ret = 0, ret2;

	*offlined = 0;
	bitmap_zero(removed, NR_CPUS);
	/* machine_shutdown() keeps the reboot CPU, which is CPU 0 here. */
	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		ret = remove_cpu(cpu);
		if (ret) {
			pr_err("remove_cpu(%u): %d\n", cpu, ret);
			break;
		}
		set_bit(cpu, removed);
		(*offlined)++;
	}
	if (!ret && num_online_cpus() != 1)
		ret = -EBUSY;
	if (!ret && quest_cpus_stuck())
		ret = -EBUSY;
	pr_info("rehearsal: %u secondary CPU(s) offline, online=%u: %d\n",
		*offlined, num_online_cpus(), ret);
	for_each_set_bit(cpu, removed, NR_CPUS) {
		ret2 = add_cpu(cpu);
		if (ret2) {
			pr_err("add_cpu(%u): %d\n", cpu, ret2);
			if (!ret)
				ret = ret2;
		}
	}
	return ret;
}

static int quest_rehearse_rproc(u32 *stopped)
{
	int ret = quest_rproc_stop(NULL);

	/* An unset stop_remoteprocs parameter rehearses the PAS processors. */
	if (!ret && !quest_rproc_stopped_count())
		ret = quest_rproc_stop("pas");
	*stopped = quest_rproc_stopped_count();
	quest_rproc_restart();
	return ret;
}

int quest_rehearse(struct quest_el1_rehearse *req)
{
	struct mutex *transition = quest_transition_mutex();
	struct quest_el1_rehearse_result arch = {};
	u32 flags = req->flags;
	int ret = 0;

	req->done = 0;
	req->result = 0;
	if (!flags || (flags & ~QUEST_EL1_REHEARSE_ALL))
		return -EINVAL;
	/* No concurrent suspend/reboot, LOAD/UNLOAD or EXEC while rehearsing. */
	if (!mutex_trylock(transition))
		return -EBUSY;
	if (!mutex_trylock(&kexec_mutex)) {
		mutex_unlock(transition);
		return -EBUSY;
	}

	if (flags & QUEST_EL1_REHEARSE_WATCHDOG) {
		ret = quest_platform_rehearse_watchdog(&req->watchdogs_stopped);
		req->done |= QUEST_EL1_REHEARSE_WATCHDOG;
		if (ret)
			goto out;
	}
	if (flags & QUEST_EL1_REHEARSE_RPROC) {
		ret = quest_rehearse_rproc(&req->rprocs_stopped);
		req->done |= QUEST_EL1_REHEARSE_RPROC;
		if (ret)
			goto out;
	}
	if (flags & QUEST_EL1_REHEARSE_CPUS) {
		ret = quest_rehearse_cpus(&req->cpus_offlined);
		req->done |= QUEST_EL1_REHEARSE_CPUS;
		if (ret)
			goto out;
	}
	if (flags & QUEST_EL1_REHEARSE_MMU) {
		ret = quest_arch_rehearse_mmu(&arch);
		req->done |= QUEST_EL1_REHEARSE_MMU;
		req->mmu_sctlr = arch.sctlr;
		if (ret)
			goto out;
	}
	if (flags & QUEST_EL1_REHEARSE_COPY) {
		memset(&arch, 0, sizeof(arch));
		ret = kexec_image ? quest_arch_rehearse_copy(kexec_image, &arch) :
				    -ENOENT;
		req->done |= QUEST_EL1_REHEARSE_COPY;
		req->copy_pages = arch.pages;
		req->copy_ns = arch.ns;
		if (ret)
			goto out;
	}
out:
	req->result = ret;
	mutex_unlock(&kexec_mutex);
	mutex_unlock(transition);

	mutex_lock(&quest_last_lock);
	quest_last = *req;
	quest_have_last = true;
	mutex_unlock(&quest_last_lock);
	pr_info("rehearsal flags=%#x done=%#x result=%d\n", flags, req->done, ret);
	/* Stage failures are reported in req->result, not as an ioctl error. */
	return 0;
}

void quest_report_rehearsal(struct quest_report *r)
{
	struct quest_el1_rehearse last;
	bool have;

	mutex_lock(&quest_last_lock);
	last = quest_last;
	have = quest_have_last;
	mutex_unlock(&quest_last_lock);
	if (!have) {
		quest_report_add(r, "rehearse none\n");
		return;
	}
	quest_report_add(r, "rehearse flags=0x%x done=0x%x result=%d watchdogs=%u rprocs=%u cpus=%u sctlr=0x%llx copy_pages=%llu copy_ns=%llu\n",
			 last.flags, last.done, last.result,
			 last.watchdogs_stopped, last.rprocs_stopped,
			 last.cpus_offlined, (unsigned long long)last.mmu_sctlr,
			 (unsigned long long)last.copy_pages,
			 (unsigned long long)last.copy_ns);
}
