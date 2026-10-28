// SPDX-License-Identifier: GPL-2.0-only
/* One executable identity page for the physically contiguous transition stub. */
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <asm/barrier.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>
#include <asm/tlbflush.h>

#include "idmap.h"
#include "machine_kexec_compat.h"

static pgd_t *quest_idmap_root;
static pmd_t *quest_idmap_pmd;
static pte_t *quest_idmap_pte;
static void *quest_empty_root;

int quest_idmap_init(void)
{
	quest_idmap_root = (void *)get_zeroed_page(GFP_KERNEL);
	quest_idmap_pmd = (void *)get_zeroed_page(GFP_KERNEL);
	quest_idmap_pte = (void *)get_zeroed_page(GFP_KERNEL);
	quest_empty_root = (void *)get_zeroed_page(GFP_KERNEL);
	if (!quest_idmap_root || !quest_idmap_pmd || !quest_idmap_pte ||
	    !quest_empty_root) {
		quest_idmap_exit();
		return -ENOMEM;
	}
	return 0;
}

void quest_idmap_exit(void)
{
	if (quest_idmap_root)
		free_page((unsigned long)quest_idmap_root);
	if (quest_idmap_pmd)
		free_page((unsigned long)quest_idmap_pmd);
	if (quest_idmap_pte)
		free_page((unsigned long)quest_idmap_pte);
	if (quest_empty_root)
		free_page((unsigned long)quest_empty_root);
	quest_idmap_root = NULL;
	quest_idmap_pmd = NULL;
	quest_idmap_pte = NULL;
	quest_empty_root = NULL;
}

/* Call only after the final image is selected and other CPUs are offline. */
void quest_idmap_install(phys_addr_t code_page)
{
	u64 tcr;

	memset(quest_idmap_root, 0, PAGE_SIZE);
	memset(quest_idmap_pmd, 0, PAGE_SIZE);
	memset(quest_idmap_pte, 0, PAGE_SIZE);
	quest_idmap_root[pgd_index(code_page)] =
		__pgd(virt_to_phys(quest_idmap_pmd) | PMD_TYPE_TABLE);
	quest_idmap_pmd[pmd_index(code_page)] =
		__pmd(virt_to_phys(quest_idmap_pte) | PMD_TYPE_TABLE);
	/* Read-only and executable only at EL1. The direct-map alias stays PXN. */
	quest_idmap_pte[pte_index(code_page)] = __pte(code_page |
		PTE_TYPE_PAGE | PTE_AF | PTE_SHARED | PTE_RDONLY | PTE_UXN |
		PTE_ATTRINDX(MT_NORMAL));
	quest_flush_dcache(quest_idmap_root, PAGE_SIZE);
	quest_flush_dcache(quest_idmap_pmd, PAGE_SIZE);
	quest_flush_dcache(quest_idmap_pte, PAGE_SIZE);
	quest_flush_dcache(quest_empty_root, PAGE_SIZE);

	/* Break old userspace translations before changing the translation size. */
	write_sysreg(phys_to_ttbr(virt_to_phys(quest_empty_root)), ttbr0_el1);
	isb();
	local_flush_tlb_all();
	tcr = read_sysreg(tcr_el1);
	tcr &= ~(TCR_T0SZ_MASK | TCR_EPD0_MASK);
	tcr |= TCR_T0SZ(39);
	write_sysreg(tcr, tcr_el1);
	isb();
	/* Uses the real stock init_mm, whose reserved ASID is zero. */
	quest_switch_mm(virt_to_phys(quest_idmap_root));
}

void quest_idmap_save(struct quest_mmu_state *s)
{
	s->ttbr0 = read_sysreg(ttbr0_el1);
	s->ttbr1 = read_sysreg(ttbr1_el1);
	s->tcr = read_sysreg(tcr_el1);
}

/*
 * cpu_do_switch_mm() also rewrote the ASID field of TTBR1 (TCR.A1 selects
 * TTBR1's ASID). Restore TCR, TTBR1 and TTBR0 exactly, passing through the
 * empty root so no walk ever uses a mismatched T0SZ/root pair.
 */
void quest_idmap_restore(const struct quest_mmu_state *s)
{
	write_sysreg(phys_to_ttbr(virt_to_phys(quest_empty_root)), ttbr0_el1);
	isb();
	local_flush_tlb_all();
	write_sysreg(s->tcr, tcr_el1);
	isb();
	write_sysreg(s->ttbr1, ttbr1_el1);
	isb();
	write_sysreg(s->ttbr0, ttbr0_el1);
	isb();
	local_flush_tlb_all();
}
