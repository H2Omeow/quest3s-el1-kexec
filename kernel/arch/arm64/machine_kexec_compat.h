/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_COMPAT_H
#define QUEST_EL1_COMPAT_H

#include <linux/types.h>

int quest_arch_compat_init(void);
int quest_arch_check(void);
void quest_switch_mm(phys_addr_t root);
void quest_flush_dcache(void *start, size_t size);
bool quest_cpus_stuck(void);

int quest_arch_init(void);
void quest_arch_exit(void);

#endif
