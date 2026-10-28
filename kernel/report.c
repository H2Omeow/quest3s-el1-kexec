// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only preflight report: everything a person (or the planner) needs to
 * decide whether a handoff can work, gathered without changing any state.
 * Lines are "<key> <values...>"; unknown keys must be ignored by parsers.
 */
#define pr_fmt(fmt) "quest_el1_report: " fmt

#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/utsname.h>
#include <linux/vmalloc.h>
#include <asm/sysreg.h>

#include "kexec_compat.h"
#include "arch/arm64/machine_kexec_compat.h"
#include "platform_quest.h"
#include "platform_rproc.h"
#include "platform_secure.h"
#include "platform_wdt.h"
#include "report.h"
#include "../include/quest_el1_uapi.h"

/* Up to 4096 "secure" plus 4096 "dmabuf_shared" lines must fit. */
#define QUEST_REPORT_CAP (1024 * 1024)

void quest_report_add(struct quest_report *r, const char *fmt, ...)
{
	va_list args;
	int n;

	if (!r->buf || r->truncated)
		return;
	va_start(args, fmt);
	n = vsnprintf(r->buf + r->len, r->cap - r->len, fmt, args);
	va_end(args);
	if (n < 0 || r->len + n >= r->cap) {
		r->truncated = true;
		r->buf[r->len] = '\0';
		return;
	}
	r->len += n;
}

static void quest_report_cpu(struct quest_report *r)
{
	u64 tcr = read_sysreg(tcr_el1);

	quest_report_add(r, "abi %u\n", QUEST_EL1_ABI_VERSION);
	quest_report_add(r, "kernel %s %s\n", init_utsname()->release,
			 init_utsname()->version);
	quest_report_add(r, "current_el %llu\n", (read_sysreg(CurrentEL) >> 2) & 3);
	quest_report_add(r, "tcr_el1 0x%llx t0sz %llu t1sz %llu\n", tcr,
			 tcr & 0x3f, (tcr >> 16) & 0x3f);
	quest_report_add(r, "sctlr_el1 0x%llx\n", read_sysreg(sctlr_el1));
	quest_report_add(r, "cpus online %u possible %u\n", num_online_cpus(),
			 num_possible_cpus());
	quest_report_add(r, "arch_check %d\n", quest_arch_check());
}

/*
 * of_reserved_mem.c keeps every /reserved-memory region, including the
 * dynamically placed (size-only) ones, in a static array. Their final
 * addresses exist nowhere else; the planner pins them in the target DTB so
 * the new kernel does not hand out RAM that CMA/firmware/ramoops still own.
 */
static void quest_report_rmem(struct quest_report *r)
{
	struct reserved_mem *rmem = (void *)quest_lookup_name("reserved_mem");
	int *count = (void *)quest_lookup_name("reserved_mem_count");
	int i, n;

	if (!rmem || !count) {
		quest_report_add(r, "rmem_unavailable\n");
		return;
	}
	n = READ_ONCE(*count);
	if (n < 0 || n > 256) {
		quest_report_add(r, "rmem_unavailable count=%d\n", n);
		return;
	}
	for (i = 0; i < n; i++)
		quest_report_add(r, "rmem %s 0x%llx 0x%llx\n",
				 rmem[i].name ? rmem[i].name : "?",
				 (unsigned long long)rmem[i].base,
				 (unsigned long long)rmem[i].size);
}

int quest_report_build(struct quest_report *r)
{
	memset(r, 0, sizeof(*r));
	r->buf = vzalloc(QUEST_REPORT_CAP);
	if (!r->buf)
		return -ENOMEM;
	r->cap = QUEST_REPORT_CAP;
	quest_report_cpu(r);
	quest_platform_report(r);
	quest_wdt_report(r);
	quest_rproc_report(r);
	quest_report_rmem(r);
	quest_secure_report(r);
	quest_report_rehearsal(r);
	/* A missing final line tells the reader the buffer overflowed. */
	quest_report_add(r, "end\n");
	return 0;
}

void quest_report_free(struct quest_report *r)
{
	vfree(r->buf);
	memset(r, 0, sizeof(*r));
}
