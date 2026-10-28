/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_REHEARSE_H
#define QUEST_EL1_REHEARSE_H

#include <linux/types.h>

struct kimage;

struct quest_el1_rehearse_result {
	u64 sctlr;	/* SCTLR_EL1 after the roundtrip (== before) */
	u64 pages;	/* pages relocated with the MMU off and verified */
	u64 ns;		/* time spent with the MMU off, including the walk */
};

/*
 * Call with kexec_mutex held (the private idmap tables are shared with the
 * final handoff). Both run the real transition blob under stop_machine().
 */
int quest_arch_rehearse_mmu(struct quest_el1_rehearse_result *out);
/* Relocates every source page of the loaded image to scratch pages. */
int quest_arch_rehearse_copy(struct kimage *image,
			     struct quest_el1_rehearse_result *out);

#endif
