/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_IDMAP_H
#define QUEST_EL1_IDMAP_H

#include <linux/types.h>

int quest_idmap_init(void);
void quest_idmap_exit(void);
void quest_idmap_install(phys_addr_t code_page);

/* Rehearsal only: undo quest_idmap_install() on the current CPU. */
struct quest_mmu_state {
	u64 ttbr0;
	u64 ttbr1;
	u64 tcr;
};
void quest_idmap_save(struct quest_mmu_state *s);
void quest_idmap_restore(const struct quest_mmu_state *s);

#endif
