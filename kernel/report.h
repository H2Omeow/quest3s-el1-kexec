/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_REPORT_H
#define QUEST_EL1_REPORT_H

#include <linux/compiler.h>
#include <linux/types.h>

/*
 * Plain-text "key value" report returned by read() on /dev/quest_el1.
 * Everything in it is gathered read-only; generating it changes no device,
 * watchdog, CPU or memory state. The userspace planner consumes the
 * "rmem" and "secure" lines to build a runtime DTB.
 */
struct quest_report {
	char *buf;
	size_t len;
	size_t cap;
	bool truncated;
};

__printf(2, 3) void quest_report_add(struct quest_report *r, const char *fmt, ...);
int quest_report_build(struct quest_report *r);
void quest_report_free(struct quest_report *r);

#endif
