// SPDX-License-Identifier: GPL-2.0-only
/*
 * EL1-only normal kexec, derived from Linux ARM64 kexec.
 * Copyright (C) Linaro.
 * Copyright (C) Huawei Futurewei Technologies.
 */
#define pr_fmt(fmt) "quest_el1_kexec: " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <asm/barrier.h>
#include <asm/daifflags.h>
#include <asm/memory.h>

#include "../../kexec.h"
#include "cpu-reset.h"
#include "idmap.h"
#include "machine_kexec_compat.h"

extern const unsigned char quest_el1_transition[];
extern const unsigned char quest_el1_transition_end[];

void machine_kexec_cleanup(struct kimage *image)
{
	/* Control pages are owned and released by the core. */
}

int machine_kexec_prepare(struct kimage *image)
{
	phys_addr_t code = page_to_phys(image->control_code_page);
	int ret = quest_arch_check();

	if (ret)
		return ret;
	if (!image->dtb || (image->dtb & 7) || !image->start ||
	    code >= BIT_ULL(39) || image->start >= BIT_ULL(39) ||
	    image->dtb >= BIT_ULL(39))
		return -EINVAL;
	if (quest_el1_transition_end - quest_el1_transition > PAGE_SIZE)
		return -E2BIG;
	/* No global idmap change here: failed loads must leave an old image usable. */
	return 0;
}

static void quest_flush_image_list(struct kimage *image)
{
	kimage_entry_t *entry;

	for (entry = &image->head; ; entry++) {
		unsigned long flag = *entry & IND_FLAGS;
		void *addr;

		quest_flush_dcache(entry, sizeof(*entry));
		if (flag == IND_DONE)
			break;
		addr = phys_to_virt(*entry & PAGE_MASK);
		switch (flag) {
		case IND_INDIRECTION:
			entry = (kimage_entry_t *)addr - 1;
			break;
		case IND_SOURCE:
			quest_flush_dcache(addr, PAGE_SIZE);
			break;
		case IND_DESTINATION:
			break;
		default:
			BUG();
		}
	}
}

void machine_kexec(struct kimage *image)
{
	phys_addr_t code = page_to_phys(image->control_code_page);
	void *buffer = page_address(image->control_code_page);
	size_t size = quest_el1_transition_end - quest_el1_transition;

	/* All recoverable checks must also have passed before device shutdown. */
	BUG_ON(quest_arch_check() || num_online_cpus() != 1);
	BUG_ON(code >= BIT_ULL(39) || size > PAGE_SIZE);
	memcpy(buffer, quest_el1_transition, size);
	quest_flush_dcache(buffer, size);
	quest_flush_image_list(image);
	pr_emerg("EL1 handoff: Image=%pa DTB=%#lx transition=%pa\n",
		 &image->start, image->dtb, &code);

	local_daif_mask();
	/* No IPI-dependent cache routine after shutting the other CPUs down. */
	asm volatile("dsb sy\n\tic iallu\n\tdsb sy\n\tisb" ::: "memory");
	quest_idmap_install(code);
	quest_enter_stub(code, image->head, image->start, image->dtb);
}
