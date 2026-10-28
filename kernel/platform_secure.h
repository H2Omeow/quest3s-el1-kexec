/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_PLATFORM_SECURE_H
#define QUEST_EL1_PLATFORM_SECURE_H

struct quest_report;

/* "secure <base> <size>" per hyp-assigned run, then dma-buf audit lines. */
void quest_secure_report(struct quest_report *r);

#endif
