/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_PLATFORM_WDT_H
#define QUEST_EL1_PLATFORM_WDT_H

#include <linux/types.h>

struct device;
struct msm_watchdog_data;
struct quest_report;

int quest_gh_wdt_status(u32 *enabled, u32 *expired, u32 *since_pet_ms);
int quest_gh_wdt_set(bool enable);
int quest_gh_wdt_pet(void);
int quest_gh_wdt_check(struct device *dev, struct msm_watchdog_data **out);
int quest_gh_wdt_disable(struct device *dev, struct msm_watchdog_data *wd);
int quest_gh_wdt_resume(struct device *dev, struct msm_watchdog_data *wd);
int quest_wdt_dt_check(const struct device *msm_dev, const struct device *gh_dev,
		       char *why, size_t why_len);
void quest_wdt_report(struct quest_report *r);

#endif
