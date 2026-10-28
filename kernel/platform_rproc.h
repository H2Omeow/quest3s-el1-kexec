/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_PLATFORM_RPROC_H
#define QUEST_EL1_PLATFORM_RPROC_H

struct quest_report;

/* spec NULL = module parameter stop_remoteprocs. 0 when nothing selected. */
int quest_rproc_stop(const char *spec);
/* Undo quest_rproc_stop() (cancel or rehearsal). */
void quest_rproc_restart(void);
/* The handoff is irreversible now; drop bookkeeping. */
void quest_rproc_commit(void);
void quest_rproc_report(struct quest_report *r);

#endif
