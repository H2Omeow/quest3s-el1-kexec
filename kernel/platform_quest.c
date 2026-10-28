// SPDX-License-Identifier: GPL-2.0-only
/*
 * Narrow adapter for the Quest Qualcomm 5.10 driver shapes in this source.
 *
 * drivers/firmware/qcom_scm.c:qcom_scm_shutdown() frees a Linux IDR, disables
 * SDI/download mode and halts the PMIC arbiter. None is required for an EL1
 * warm handoff; the last operation prevents subsequent PMIC transactions and
 * has no Linux re-enable counterpart. Preserve this one driver's firmware
 * state, while still running every other device's ordinary shutdown.
 *
 * drivers/soc/qcom/qcom_soc_wdt.c has no shutdown callback. Its driver uses the
 * public msm_watchdog_data ABI. Supply a shutdown callback which follows the
 * secure-watchdog deactivation used by qcom_wdt_disable_set(), suspends the
 * pet timers and explicitly disables the app watchdog, including wakeup mode.
 *
 * qtee_shmbridge.c and qseecom.c have remove(), but no shutdown(). Calling
 * remove() by hand is unsafe with live clients. Do not guess how to reclaim
 * their secure-side handles or reset firmware: reject them before shutdown.
 */
#define pr_fmt(fmt) "quest_el1_platform: " fmt

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <soc/qcom/watchdog.h>

#include "kexec_compat.h"
#include "platform_quest.h"
#include "platform_wdt.h"
#include "report.h"

#define QUEST_WDT_ENABLE_OFFSET 0x08
#define QUEST_WDT_LAST_OFFSET   0x14
#define QUEST_WDT_ENABLE_BITS   0x03

struct quest_platform_binding {
	struct platform_driver *driver;
	struct device *dev;
	struct module *owner;
	void (*original_shutdown)(struct platform_device *);
	bool installed;
	bool shutdown_seen;
};

static DEFINE_MUTEX(quest_platform_lock);
static struct quest_platform_binding scm_binding, wdt_binding, gh_binding;

/*
 * 0 (default): refuse while QTEE/QSEE/smcinvoke drivers are bound, as before.
 * 1: proceed; the operator takes responsibility for stopping TEE clients and
 *    for disabling those nodes in the target DTB (quest-el1 --disable). The
 *    Quest Pro port showed that a target kernel which re-probes qseecom with
 *    stale TZ listener registrations hangs, while one with the node disabled
 *    boots; the secure world itself is never reset by either kernel.
 */
static bool allow_tee;
module_param(allow_tee, bool, 0600);
MODULE_PARM_DESC(allow_tee, "Allow handoff with bound TEE drivers (target DTB must disable them)");
static bool prepared, self_pinned;
static int shutdown_error;
#if IS_ENABLED(CONFIG_QCOM_WDT_CORE)
static struct msm_watchdog_data *watchdog_data;
static struct qcom_wdt_ops *watchdog_ops;
static int (*watchdog_suspend)(struct device *);
static int (*secure_watchdog_disable)(void);
static struct msm_watchdog_data *gh_watchdog_data;
#endif

/* Account for the canonical function aliases emitted by Clang CFI. */
static bool quest_function_is(const void *function, const char *name)
{
	char alias[128];

	if (!function)
		return false;
	if ((unsigned long)function == quest_lookup_name(name))
		return true;
	if (snprintf(alias, sizeof(alias), "%s.cfi_jt", name) >= sizeof(alias))
		return false;
	if ((unsigned long)function == quest_lookup_name(alias))
		return true;
	if (snprintf(alias, sizeof(alias), "%s.cfi", name) >= sizeof(alias))
		return false;
	return (unsigned long)function == quest_lookup_name(alias);
}

static int quest_bound_device(struct device *dev, void *data)
{
	struct quest_platform_binding *binding = data;
	int ret = 0;

	device_lock(dev);
	if (dev->driver != &binding->driver->driver)
		goto out;
	if (binding->dev) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (dev->bus != &platform_bus_type || !dev->of_node ||
	    !of_match_device(binding->driver->driver.of_match_table, dev)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	binding->dev = get_device(dev);
out:
	device_unlock(dev);
	return ret;
}

/* Caller holds the stock module_mutex until the owner is pinned. */
static int quest_get_binding(struct quest_platform_binding *binding,
		const char *driver_name, const char *object_name,
		const char *probe_name, const char *remove_name,
		const char *shutdown_name, const char *match_name)
{
	struct device_driver *driver;
	struct platform_driver *platform;
	int ret;

	driver = driver_find(driver_name, &platform_bus_type);
	if (!driver)
		return 0;
	if (!try_module_get(driver->owner))
		return -EBUSY;
	binding->owner = driver->owner;
	platform = to_platform_driver(driver);
	binding->driver = platform;
	if ((unsigned long)platform != quest_lookup_name(object_name) ||
	    driver->bus != &platform_bus_type ||
	    strcmp(driver->name, driver_name) ||
	    !quest_function_is(driver->shutdown, "platform_drv_shutdown") ||
	    !quest_function_is(platform->probe, probe_name) ||
	    (remove_name ? !quest_function_is(platform->remove, remove_name) :
			   platform->remove != NULL) ||
	    (shutdown_name ? !quest_function_is(platform->shutdown, shutdown_name) :
			     platform->shutdown != NULL) ||
	    (unsigned long)driver->of_match_table != quest_lookup_name(match_name)) {
		pr_err("Unknown %s driver/callback shape; refusing warm handoff\n",
		       driver_name);
		return -EOPNOTSUPP;
	}
	binding->original_shutdown = platform->shutdown;
	ret = driver_for_each_device(driver, NULL, binding, quest_bound_device);
	if (!ret && !binding->dev)
		ret = -ENODEV;
	if (ret)
		pr_err("%s must have exactly one supported platform device\n",
		       driver_name);
	return ret;
}

static int quest_note_bound(struct device *dev, void *data)
{
	bool *bound = data;

	*bound = true;
	return 1;
}

/* Caller has blocked probes and holds module_mutex: inventory cannot grow. */
static int quest_reject_tee_driver(const char *name)
{
	struct device_driver *driver = driver_find(name, &platform_bus_type);
	bool bound = false;
	int ret;

	if (!driver)
		return 0;
	ret = driver_for_each_device(driver, NULL, &bound, quest_note_bound);
	if (ret < 0)
		return ret;
	if (!bound)
		return 0;
	if (READ_ONCE(allow_tee)) {
		pr_warn("Bound %s allowed by allow_tee; target DTB must disable it\n",
			name);
		return 0;
	}
	pr_err("Bound %s has no supported secure-world ownership transfer protocol\n",
	       name);
	return -EOPNOTSUPP;
}

static void quest_scm_warm_shutdown(struct platform_device *pdev)
{
	/* device_shutdown() holds this device's lock while reading/calling us. */
	if (&pdev->dev != scm_binding.dev ||
	    pdev->dev.driver != &scm_binding.driver->driver) {
		WRITE_ONCE(shutdown_error, -ENODEV);
		return;
	}
	WRITE_ONCE(scm_binding.shutdown_seen, true);
	pr_info("Preserving SCM/PMIC state for EL1 handoff\n");
}

#if IS_ENABLED(CONFIG_QCOM_WDT_CORE)
/* Raw kallsyms helper pointers are not cross-DSO CFI jump-table pointers. */
static void __nocfi quest_watchdog_shutdown(struct platform_device *pdev)
{
	struct msm_watchdog_data *wd = watchdog_data;
	int ret;

	if (&pdev->dev != wdt_binding.dev ||
	    pdev->dev.driver != &wdt_binding.driver->driver ||
	    dev_get_drvdata(&pdev->dev) != wd || wd->ops != watchdog_ops) {
		WRITE_ONCE(shutdown_error, -ENODEV);
		return;
	}
	ret = secure_watchdog_disable();
	if (ret) {
		pr_emerg("Secure watchdog deactivation failed: %d; no handoff\n", ret);
		WRITE_ONCE(shutdown_error, ret);
		return;
	}
	/* The panic-only driver path never initialized locks or timers. */
	if (wd->watchdog_task) {
		mutex_lock(&wd->disable_lock);
		ret = watchdog_suspend(&pdev->dev);
		if (ret) {
			mutex_unlock(&wd->disable_lock);
			WRITE_ONCE(shutdown_error, ret);
			return;
		}
	}
	/* suspend_late deliberately leaves wakeup watchdogs enabled. */
	ret = wd->ops->disable_wdt(wd);
	WRITE_ONCE(wd->enabled, false);
	if (!ret && readl(wd->base + QUEST_WDT_ENABLE_OFFSET))
		ret = -EIO;
	if (wd->watchdog_task)
		mutex_unlock(&wd->disable_lock);
	if (ret) {
		pr_emerg("App watchdog disable did not complete: %d; no handoff\n", ret);
		WRITE_ONCE(shutdown_error, ret);
		return;
	}
	WRITE_ONCE(wdt_binding.shutdown_seen, true);
	pr_info("Secure and app watchdogs disabled for EL1 handoff\n");
}

static int quest_check_watchdog(void)
{
	struct device *dev = wdt_binding.dev;
	struct msm_watchdog_data *wd;
	struct resource *resource;
	int ret = -EOPNOTSUPP;

	if (!dev)
		return 0;
	device_lock(dev);
	if (dev->driver != &wdt_binding.driver->driver)
		goto out;
	wd = dev_get_drvdata(dev);
	resource = platform_get_resource_byname(to_platform_device(dev),
					       IORESOURCE_MEM, "wdt-base");
	if (!wd || !resource ||
	    resource_size(resource) < QUEST_WDT_LAST_OFFSET + sizeof(u32) ||
	    !wd->base || wd->dev != dev ||
	    (unsigned long)wd->ops != quest_lookup_name("qcom_soc_wdt_ops") ||
	    !quest_function_is(wd->ops->disable_wdt, "qcom_soc_disable_wdt") ||
	    !quest_function_is(wd->ops->reset_wdt, "qcom_soc_reset_wdt") ||
	    !quest_function_is(wd->ops->enable_wdt, "qcom_soc_enable_wdt") ||
	    IS_ERR(wd->watchdog_task) ||
	    (readl(wd->base + QUEST_WDT_ENABLE_OFFSET) & ~QUEST_WDT_ENABLE_BITS))
		goto out;
	watchdog_suspend = (void *)quest_lookup_name("qcom_wdt_pet_suspend");
	secure_watchdog_disable =
		(void *)quest_lookup_name("qcom_scm_sec_wdog_deactivate");
	if (!secure_watchdog_disable || !scm_binding.dev ||
	    (wd->watchdog_task && !watchdog_suspend))
		goto out;
	watchdog_data = wd;
	watchdog_ops = wd->ops;
	ret = 0;
out:
	device_unlock(dev);
	if (ret)
		pr_err("Unknown watchdog ABI/state or unavailable shutdown helper\n");
	return ret;
}

static void quest_gh_watchdog_shutdown(struct platform_device *pdev)
{
	int ret;

	if (&pdev->dev != gh_binding.dev ||
	    pdev->dev.driver != &gh_binding.driver->driver ||
	    dev_get_drvdata(&pdev->dev) != gh_watchdog_data) {
		WRITE_ONCE(shutdown_error, -ENODEV);
		return;
	}
	ret = quest_gh_wdt_disable(&pdev->dev, gh_watchdog_data);
	if (ret) {
		pr_emerg("Gunyah watchdog disable did not complete: %d; no handoff\n", ret);
		WRITE_ONCE(shutdown_error, ret);
		return;
	}
	WRITE_ONCE(gh_binding.shutdown_seen, true);
	pr_info("Gunyah virtual watchdog disabled for EL1 handoff\n");
}

static int quest_check_gh_watchdog(void)
{
	struct device *dev = gh_binding.dev;
	int ret;

	if (!dev)
		return 0;
	device_lock(dev);
	ret = dev->driver == &gh_binding.driver->driver ?
		quest_gh_wdt_check(dev, &gh_watchdog_data) : -EOPNOTSUPP;
	device_unlock(dev);
	return ret;
}
#else
static int quest_check_watchdog(void)
{
	return wdt_binding.dev ? -EOPNOTSUPP : 0;
}

static int quest_check_gh_watchdog(void)
{
	return gh_binding.dev ? -EOPNOTSUPP : 0;
}
#endif

/* Every available watchdog DT node must belong to a binding handled above. */
static int quest_check_watchdog_inventory(void)
{
	char why[160] = "";
	int ret = quest_wdt_dt_check(wdt_binding.dev, gh_binding.dev, why,
				     sizeof(why));

	if (ret)
		pr_err("Watchdog without supported shutdown: %s; refusing handoff\n",
		       why);
	return ret;
}

static void quest_release_binding(struct quest_platform_binding *binding,
		void (*replacement)(struct platform_device *))
{
	if (binding->dev) {
		device_lock(binding->dev);
		if (binding->installed) {
			if (READ_ONCE(binding->driver->shutdown) == replacement)
				WRITE_ONCE(binding->driver->shutdown,
					   binding->original_shutdown);
			else
				pr_err("Unexpected concurrent shutdown callback replacement\n");
		}
		device_unlock(binding->dev);
		put_device(binding->dev);
	}
	module_put(binding->owner);
	memset(binding, 0, sizeof(*binding));
}

static void quest_cancel_locked(void)
{
#if IS_ENABLED(CONFIG_QCOM_WDT_CORE)
	quest_release_binding(&gh_binding, quest_gh_watchdog_shutdown);
	gh_watchdog_data = NULL;
	quest_release_binding(&wdt_binding, quest_watchdog_shutdown);
	watchdog_data = NULL;
	watchdog_ops = NULL;
	watchdog_suspend = NULL;
	secure_watchdog_disable = NULL;
#else
	quest_release_binding(&gh_binding, NULL);
	quest_release_binding(&wdt_binding, NULL);
#endif
	quest_release_binding(&scm_binding, quest_scm_warm_shutdown);
	prepared = false;
	if (self_pinned) {
		self_pinned = false;
		module_put(THIS_MODULE);
	}
}

static int quest_install(struct quest_platform_binding *binding,
		void (*replacement)(struct platform_device *))
{
	int ret = 0;

	if (!binding->dev)
		return 0;
	device_lock(binding->dev);
	if (binding->dev->driver != &binding->driver->driver ||
	    READ_ONCE(binding->driver->shutdown) != binding->original_shutdown) {
		ret = -EBUSY;
	} else {
		WRITE_ONCE(binding->driver->shutdown, replacement);
		binding->installed = true;
	}
	device_unlock(binding->dev);
	return ret;
}

int quest_platform_prepare(void)
{
	struct mutex *stock_module_mutex;
	struct mutex *transition_mutex;
	bool *stock_pm_freezing;
	bool *stock_probes_blocked;
	int ret;

	mutex_lock(&quest_platform_lock);
	if (prepared) {
		ret = -EBUSY;
		goto out;
	}
	stock_module_mutex = (void *)quest_lookup_name("module_mutex");
	transition_mutex = (void *)quest_lookup_name("system_transition_mutex");
	stock_pm_freezing = (void *)quest_lookup_name("pm_freezing");
	stock_probes_blocked = (void *)quest_lookup_name("defer_all_probes");
	if (!stock_module_mutex || !transition_mutex || !stock_pm_freezing ||
	    !stock_probes_blocked || !READ_ONCE(*stock_probes_blocked) ||
	    !mutex_is_locked(transition_mutex) || !READ_ONCE(*stock_pm_freezing) ||
	    !(current->flags & PF_SUSPEND_TASK)) {
		pr_err("Platform preparation requires serialized reboot, frozen userspace and blocked probes\n");
		ret = -EOPNOTSUPP;
		goto out;
	}
	lockdep_assert_held(transition_mutex);
	mutex_lock(stock_module_mutex);
	ret = quest_reject_tee_driver("shared_memory_bridge");
	if (!ret)
		ret = quest_reject_tee_driver("qseecom");
	if (!ret)
		ret = quest_reject_tee_driver("smcinvoke");
	if (!ret)
		ret = quest_get_binding(&scm_binding, "qcom_scm", "qcom_scm_driver",
				"qcom_scm_probe", NULL, "qcom_scm_shutdown",
				"qcom_scm_dt_match");
	if (!ret)
		ret = quest_get_binding(&wdt_binding, "msm_watchdog", "qcom_soc_wdt_driver",
				"qcom_soc_wdt_probe", "qcom_wdt_remove", NULL,
				"qcom_soc_match_table");
	if (!ret)
		ret = quest_check_watchdog();
	if (!ret)
		ret = quest_get_binding(&gh_binding, "gh-watchdog", "gh_wdt_driver",
				"gh_wdt_probe", "qcom_wdt_remove", NULL,
				"gh_wdt_match_table");
	if (!ret)
		ret = quest_check_gh_watchdog();
	if (!ret)
		ret = quest_check_watchdog_inventory();
	mutex_unlock(stock_module_mutex);
	if (ret)
		goto cancel;
	if (!try_module_get(THIS_MODULE)) {
		ret = -EBUSY;
		goto cancel;
	}
	self_pinned = true;
	WRITE_ONCE(shutdown_error, 0);
	ret = quest_install(&scm_binding, quest_scm_warm_shutdown);
#if IS_ENABLED(CONFIG_QCOM_WDT_CORE)
	if (!ret)
		ret = quest_install(&wdt_binding, quest_watchdog_shutdown);
	if (!ret)
		ret = quest_install(&gh_binding, quest_gh_watchdog_shutdown);
#endif
	if (ret)
		goto cancel;
	prepared = true;
	goto out;
cancel:
	quest_cancel_locked();
out:
	mutex_unlock(&quest_platform_lock);
	return ret;
}

void quest_platform_cancel(void)
{
	mutex_lock(&quest_platform_lock);
	quest_cancel_locked();
	mutex_unlock(&quest_platform_lock);
}

bool quest_platform_shutdown_ok(void)
{
	return READ_ONCE(prepared) && !READ_ONCE(shutdown_error) &&
		(!scm_binding.dev || READ_ONCE(scm_binding.shutdown_seen)) &&
		(!wdt_binding.dev || READ_ONCE(wdt_binding.shutdown_seen)) &&
		(!gh_binding.dev || READ_ONCE(gh_binding.shutdown_seen));
}

static const char *const quest_report_drivers[] = {
	"qcom_scm", "msm_watchdog", "gh-watchdog",
	"shared_memory_bridge", "qseecom", "smcinvoke", "qcom_q6v5_pas",
};

static int quest_count_bound(struct device *dev, void *data)
{
	(*(unsigned int *)data)++;
	return 0;
}

/* Read-only: driver presence and bound-device counts for the report. */
void quest_platform_report(struct quest_report *r)
{
	struct mutex *stock_module_mutex = (void *)quest_lookup_name("module_mutex");
	unsigned int i;

	quest_report_add(r, "allow_tee %d\n", READ_ONCE(allow_tee));
	if (!stock_module_mutex)
		return;
	mutex_lock(stock_module_mutex);
	for (i = 0; i < ARRAY_SIZE(quest_report_drivers); i++) {
		struct device_driver *driver =
			driver_find(quest_report_drivers[i], &platform_bus_type);
		unsigned int bound = 0;

		if (driver)
			driver_for_each_device(driver, NULL, &bound,
					       quest_count_bound);
		quest_report_add(r, "driver %s present=%d bound=%u\n",
				 quest_report_drivers[i], !!driver, bound);
	}
	mutex_unlock(stock_module_mutex);
}

#if IS_ENABLED(CONFIG_QCOM_WDT_CORE)
/* MMIO watchdog: same steps as the shutdown callback minus the secure part. */
static int __nocfi quest_rehearse_msm_watchdog(void)
{
	struct msm_watchdog_data *wd = watchdog_data;
	int (*pet_resume)(struct device *) =
		(void *)quest_lookup_name("qcom_wdt_pet_resume");
	struct device *dev = wdt_binding.dev;
	int ret = 0, ret2;

	if (wd->watchdog_task && !pet_resume)
		return -ENOENT;
	if (wd->watchdog_task) {
		mutex_lock(&wd->disable_lock);
		ret = watchdog_suspend(dev);
	}
	if (!ret)
		ret = wd->ops->disable_wdt(wd);
	if (!ret && readl(wd->base + QUEST_WDT_ENABLE_OFFSET))
		ret = -EIO;
	pr_info("rehearsal: msm watchdog disable %d\n", ret);
	if (wd->watchdog_task) {
		ret2 = pet_resume(dev);
		mutex_unlock(&wd->disable_lock);
		if (!ret)
			ret = ret2;
	} else {
		ret2 = wd->ops->enable_wdt(1, wd);
		if (!ret)
			ret = ret2;
	}
	return ret;
}

static int quest_rehearse_gh_watchdog(void)
{
	struct device *dev = gh_binding.dev;
	u32 enabled = 1;
	int ret, ret2;

	ret = quest_gh_wdt_disable(dev, gh_watchdog_data);
	if (!ret)
		ret = quest_gh_wdt_status(&enabled, NULL, NULL);
	if (!ret && enabled)
		ret = -EIO;
	pr_info("rehearsal: gh watchdog disable %d (enabled=%u)\n", ret, enabled);
	ret2 = quest_gh_wdt_resume(dev, gh_watchdog_data);
	return ret ? ret : ret2;
}
#endif

/*
 * Reversible watchdog test: run the same inventory as prepare(), then stop
 * and restart every watchdog the handoff would stop. Nothing is installed.
 * Does not touch the secure watchdog (its deactivation has no inverse).
 */
int quest_platform_rehearse_watchdog(unsigned int *stopped)
{
	struct mutex *stock_module_mutex;
	int ret;

	*stopped = 0;
	mutex_lock(&quest_platform_lock);
	if (prepared) {
		ret = -EBUSY;
		goto out;
	}
	stock_module_mutex = (void *)quest_lookup_name("module_mutex");
	if (!stock_module_mutex) {
		ret = -ENOENT;
		goto out;
	}
	mutex_lock(stock_module_mutex);
	ret = quest_get_binding(&scm_binding, "qcom_scm", "qcom_scm_driver",
			"qcom_scm_probe", NULL, "qcom_scm_shutdown",
			"qcom_scm_dt_match");
	if (!ret)
		ret = quest_get_binding(&wdt_binding, "msm_watchdog", "qcom_soc_wdt_driver",
				"qcom_soc_wdt_probe", "qcom_wdt_remove", NULL,
				"qcom_soc_match_table");
	if (!ret)
		ret = quest_check_watchdog();
	if (!ret)
		ret = quest_get_binding(&gh_binding, "gh-watchdog", "gh_wdt_driver",
				"gh_wdt_probe", "qcom_wdt_remove", NULL,
				"gh_wdt_match_table");
	if (!ret)
		ret = quest_check_gh_watchdog();
	if (!ret)
		ret = quest_check_watchdog_inventory();
	mutex_unlock(stock_module_mutex);
#if IS_ENABLED(CONFIG_QCOM_WDT_CORE)
	if (!ret && wdt_binding.dev) {
		ret = quest_rehearse_msm_watchdog();
		(*stopped)++;
	}
	if (!ret && gh_binding.dev) {
		ret = quest_rehearse_gh_watchdog();
		(*stopped)++;
	}
#endif
	quest_cancel_locked();
out:
	mutex_unlock(&quest_platform_lock);
	return ret;
}
