/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_REHEARSE_ORCH_H
#define QUEST_EL1_REHEARSE_ORCH_H

struct quest_el1_rehearse;

/* Runs req->flags stages; per-stage outcome in req->result/done. */
int quest_rehearse(struct quest_el1_rehearse *req);

#endif
