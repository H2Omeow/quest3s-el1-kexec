/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_MEMORY_GUARD_H
#define QUEST_EL1_MEMORY_GUARD_H

struct kimage;
int quest_memory_guard_init(void);
int quest_validate_destinations(struct kimage *image);

#endif
