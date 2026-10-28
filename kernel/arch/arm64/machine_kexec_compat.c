// SPDX-License-Identifier: GPL-2.0-only
/* Runtime bindings for a 5.10 ARM64 stock kernel with CONFIG_KEXEC=n. */
#define pr_fmt(fmt) "quest_el1_kexec: " fmt

#include <linux/errno.h>
#include <linux/compiler.h>
#include <linux/mm_types.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

#include "machine_kexec_compat.h"

extern unsigned long quest_lookup_name(const char *name);

static void (*switch_mm_fn)(phys_addr_t, struct mm_struct *);
static void (*flush_dcache_fn)(void *, size_t);
static bool (*stuck_cpus_fn)(void);
static struct mm_struct *stock_init_mm;
static const u32 *stock_boot_mode;

/* kallsyms resolves actual code, which need not be a canonical CFI entry. */
void __nocfi quest_switch_mm(phys_addr_t root)
{
	switch_mm_fn(root, stock_init_mm);
}

void __nocfi quest_flush_dcache(void *start, size_t size)
{
	flush_dcache_fn(start, size);
}

bool __nocfi quest_cpus_stuck(void)
{
	return stuck_cpus_fn();
}

int quest_arch_check(void)
{
	u64 tcr = read_sysreg(tcr_el1);
	u32 boot0 = READ_ONCE(stock_boot_mode[0]);
	u32 boot1 = READ_ONCE(stock_boot_mode[1]);

	if (read_sysreg(CurrentEL) != CurrentEL_EL1)
		return -EOPNOTSUPP;
	/*
	 * An nVHE kernel runs at EL1 after booting at EL2. Its secondaries may
	 * enter the replacement at EL2, so CurrentEL alone is insufficient.
	 * This source's head.S records 0xe11 for EL1 (not the stale zero value
	 * described by the comment above __boot_cpu_mode in asm/virt.h).
	 */
	if (boot0 != BOOT_CPU_MODE_EL1 || boot1 != BOOT_CPU_MODE_EL1) {
		pr_err("EL1-only handoff rejects boot CPU modes %#x/%#x\n",
		       boot0, boot1);
		return -EOPNOTSUPP;
	}
	if ((tcr & TCR_T1SZ_MASK) != TCR_T1SZ(39) ||
	    (tcr & TCR_TG0_MASK) != TCR_TG0_4K ||
	    (tcr & TCR_TG1_MASK) != TCR_TG1_4K)
		return -EOPNOTSUPP;
	if (quest_cpus_stuck())
		return -EBUSY;
	return 0;
}

int quest_arch_compat_init(void)
{
	/* The private idmap intentionally supports the stock 4K/VA39 layout. */
#if PAGE_SHIFT != 12 || VA_BITS != 39 || CONFIG_PGTABLE_LEVELS != 3
#error "Build this loader against the stock 4K/VA39, three-level kernel"
#endif

	switch_mm_fn = (void *)quest_lookup_name("cpu_do_switch_mm");
	flush_dcache_fn = (void *)quest_lookup_name("__flush_dcache_area");
	stuck_cpus_fn = (void *)quest_lookup_name("cpus_are_stuck_in_kernel");
	stock_init_mm = (void *)quest_lookup_name("init_mm");
	stock_boot_mode = (void *)quest_lookup_name("__boot_cpu_mode");
	if (!switch_mm_fn || !flush_dcache_fn || !stuck_cpus_fn || !stock_init_mm ||
	    !stock_boot_mode) {
		pr_err("Missing stock MM/cache/CPU-safety/boot-mode symbol\n");
		return -ENOENT;
	}

	return quest_arch_check();
}
