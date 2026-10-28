// SPDX-License-Identifier: GPL-2.0-only
/*
 * Watchdog helpers which are independent of a driver binding.
 *
 * Quest 3S runs the HLOS kernel as a Gunyah VM. Its application watchdog can
 * be either the MMIO qcom_soc_wdt ("msm_watchdog") or the Gunyah virtual
 * watchdog ("gh-watchdog", drivers/virt/gunyah/gh_virt_wdt.c), which is
 * controlled through vendor-hypervisor SMCCC calls:
 *   0x86000005 CONTROL  arg1: 3 = enable, 2 = disable (bit 1 reserved, = 1)
 *   0x86000006 STATUS   a1 bit0 = enabled, a1 bit31 = expired, a2 = ms since pet
 *   0x86000007 PET
 * res.a0 is the Gunyah error code; 0 is success.
 *
 * The old adapter silently skipped a watchdog whose driver it did not know,
 * which let a handoff proceed with a live watchdog. This file adds the
 * missing fail-closed device-tree inventory and the gh-watchdog primitives.
 */
#define pr_fmt(fmt) "quest_el1_wdt: " fmt

#include <linux/arm-smccc.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <soc/qcom/watchdog.h>

#include "kexec_compat.h"
#include "platform_wdt.h"

#define QUEST_GH_WDT_CONTROL \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, \
			   ARM_SMCCC_OWNER_VENDOR_HYP, 0x0005)
#define QUEST_GH_WDT_STATUS \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, \
			   ARM_SMCCC_OWNER_VENDOR_HYP, 0x0006)
#define QUEST_GH_WDT_PET \
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32, \
			   ARM_SMCCC_OWNER_VENDOR_HYP, 0x0007)
#define QUEST_GH_WDT_ENABLE  3
#define QUEST_GH_WDT_DISABLE 2

static const char *const quest_wdt_compatibles[] = {
	"qcom,msm-watchdog",
	"qcom,gh-watchdog",
	"qcom,hh-watchdog",
};

/* Only meaningful when a gh-watchdog node exists: it proves SMC routing. */
int quest_gh_wdt_status(u32 *enabled, u32 *expired, u32 *since_pet_ms)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(QUEST_GH_WDT_STATUS, 0, &res);
	if (res.a0)
		return -EIO;
	if (enabled)
		*enabled = res.a1 & 1;
	if (expired)
		*expired = (res.a1 >> 31) & 1;
	if (since_pet_ms)
		*since_pet_ms = res.a2;
	return 0;
}

static int quest_gh_wdt_call(u32 id, u32 arg)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_smc(id, arg, &res);
	return res.a0 ? -EIO : 0;
}

int quest_gh_wdt_set(bool enable)
{
	u32 now;
	int ret;

	ret = quest_gh_wdt_status(&now, NULL, NULL);
	if (ret)
		return ret;
	if (now == enable)
		return 0;
	if (enable) {
		/* Pet first so a long-disabled watchdog does not bark at once. */
		quest_gh_wdt_call(QUEST_GH_WDT_PET, 0);
		ret = quest_gh_wdt_call(QUEST_GH_WDT_CONTROL, QUEST_GH_WDT_ENABLE);
	} else {
		ret = quest_gh_wdt_call(QUEST_GH_WDT_CONTROL, QUEST_GH_WDT_DISABLE);
	}
	if (ret)
		return ret;
	ret = quest_gh_wdt_status(&now, NULL, NULL);
	if (ret)
		return ret;
	return now == enable ? 0 : -EIO;
}

int quest_gh_wdt_pet(void)
{
	return quest_gh_wdt_call(QUEST_GH_WDT_PET, 0);
}

static bool quest_wdt_ops_is_gh(const struct qcom_wdt_ops *ops)
{
	unsigned long gh_ops = quest_lookup_name("gh_wdt_ops");

	return gh_ops && (unsigned long)ops == gh_ops;
}

/*
 * Verify a bound gh-watchdog device. The ops table identity is enough: the
 * table and its functions are static in the same module object, and the
 * hypervisor state is verified independently through STATUS.
 */
int quest_gh_wdt_check(struct device *dev, struct msm_watchdog_data **out)
{
	struct msm_watchdog_data *wd = dev_get_drvdata(dev);
	u32 enabled;

	if (!wd || wd->dev != dev || !quest_wdt_ops_is_gh(wd->ops) ||
	    IS_ERR(wd->watchdog_task)) {
		pr_err("gh-watchdog driver data has an unknown shape\n");
		return -EOPNOTSUPP;
	}
	if (quest_gh_wdt_status(&enabled, NULL, NULL)) {
		pr_err("Gunyah watchdog STATUS call failed\n");
		return -EOPNOTSUPP;
	}
	if (wd->watchdog_task && !quest_lookup_name("qcom_wdt_pet_suspend")) {
		pr_err("qcom_wdt_pet_suspend is not resolvable\n");
		return -EOPNOTSUPP;
	}
	*out = wd;
	return 0;
}

/*
 * Stop the Linux pet timer exactly like suspend_late does, then make sure the
 * hypervisor reports the watchdog disabled. Wakeup-capable configurations
 * leave the watchdog running in suspend_late, so always finish with an
 * explicit CONTROL(disable) when STATUS still reports it enabled.
 */
int __nocfi quest_gh_wdt_disable(struct device *dev, struct msm_watchdog_data *wd)
{
	int (*pet_suspend)(struct device *) =
		(void *)quest_lookup_name("qcom_wdt_pet_suspend");
	int ret = 0;

	if (wd->watchdog_task) {
		if (!pet_suspend)
			return -ENOENT;
		mutex_lock(&wd->disable_lock);
		ret = pet_suspend(dev);
	}
	if (!ret)
		ret = quest_gh_wdt_set(false);
	if (!ret)
		WRITE_ONCE(wd->enabled, false);
	if (wd->watchdog_task)
		mutex_unlock(&wd->disable_lock);
	return ret;
}

/* Rehearsal counterpart: re-arm the Linux pet timer and the watchdog. */
int __nocfi quest_gh_wdt_resume(struct device *dev, struct msm_watchdog_data *wd)
{
	int (*pet_resume)(struct device *) =
		(void *)quest_lookup_name("qcom_wdt_pet_resume");
	int ret;

	if (!wd->watchdog_task)
		return 0;
	if (!pet_resume)
		return -ENOENT;
	mutex_lock(&wd->disable_lock);
	ret = pet_resume(dev);
	mutex_unlock(&wd->disable_lock);
	if (!ret)
		ret = quest_gh_wdt_set(true);
	return ret;
}

static struct device *quest_node_device(struct device_node *np)
{
	struct platform_device *pdev = of_find_device_by_node(np);

	return pdev ? &pdev->dev : NULL;
}

/*
 * Every available watchdog node must be bound to one of the devices the
 * adapter knows how to stop. Anything else (unbound node, unknown driver,
 * second instance) refuses the handoff before any state changes.
 */
int quest_wdt_dt_check(const struct device *msm_dev, const struct device *gh_dev,
		       char *why, size_t why_len)
{
	struct device_node *np;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(quest_wdt_compatibles) && !ret; i++) {
		for_each_compatible_node(np, NULL, quest_wdt_compatibles[i]) {
			struct device *dev;

			if (!of_device_is_available(np))
				continue;
			dev = quest_node_device(np);
			if (!dev || (dev != msm_dev && dev != gh_dev)) {
				if (why)
					snprintf(why, why_len, "%pOF (%s) %s", np,
						 quest_wdt_compatibles[i],
						 dev ? (dev->driver ? dev->driver->name :
							"unbound") : "no device");
				ret = -EOPNOTSUPP;
			}
			if (dev)
				put_device(dev);
			if (ret) {
				of_node_put(np);
				break;
			}
		}
	}
	return ret;
}

/* Append one line per watchdog node for the preflight report. */
void quest_wdt_report(struct quest_report *r)
{
	struct device_node *np;
	unsigned int i;
	u32 en, exp, since;

	for (i = 0; i < ARRAY_SIZE(quest_wdt_compatibles); i++) {
		for_each_compatible_node(np, NULL, quest_wdt_compatibles[i]) {
			struct device *dev = quest_node_device(np);

			quest_report_add(r, "watchdog %pOF compatible=%s available=%d driver=%s\n",
					 np, quest_wdt_compatibles[i],
					 of_device_is_available(np),
					 dev && dev->driver ? dev->driver->name : "-");
			if (dev)
				put_device(dev);
			if (i && of_device_is_available(np) &&
			    !quest_gh_wdt_status(&en, &exp, &since))
				quest_report_add(r, "gh-watchdog status enabled=%u expired=%u since_pet_ms=%u\n",
						 en, exp, since);
		}
	}
}
