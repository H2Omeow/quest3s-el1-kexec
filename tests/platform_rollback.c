/* SPDX-License-Identifier: GPL-2.0-only */
/* Minimal kernel/Qualcomm stubs around extracted production functions. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef uint32_t u32;

#define CONFIG_QCOM_WDT_CORE 1
#define IS_ENABLED(option) (option)
#define PF_SUSPEND_TASK 0x00000001
#define IORESOURCE_MEM 0x00000200
#define QUEST_WDT_ENABLE_OFFSET 0x08
#define QUEST_WDT_LAST_OFFSET 0x14
#define QUEST_WDT_ENABLE_BITS 0x03
#define READ_ONCE(value) (value)
#define WRITE_ONCE(value, new_value) ((value) = (new_value))
#define IS_ERR(ptr) ((intptr_t)(ptr) < 0 && (intptr_t)(ptr) > -4096)
#define THIS_MODULE (&loader_module)
#define __nocfi
#define qcom_scm_driver scm_driver
#define qcom_soc_wdt_driver wdt_driver
#define qcom_soc_wdt_ops wdt_ops
#define gh_wdt_driver gh_driver
#define system_transition_mutex transition_mutex

struct mutex { bool locked; };
struct module { int references; };
struct bus_type { int unused; };
struct device_driver;
struct platform_device;
struct qcom_wdt_ops;
struct msm_watchdog_data;

struct device {
	struct device_driver *driver;
	struct bus_type *bus;
	void *of_node;
	void *drvdata;
	struct resource *resource;
	uint32_t *registers;
	int refs;
	int lock_count;
	bool locked;
};

struct device_driver {
	const char *name;
	struct bus_type *bus;
	struct module *owner;
	void (*shutdown)(struct device *);
	void *of_match_table;
};

struct platform_driver {
	int (*probe)(struct platform_device *);
	int (*remove)(struct platform_device *);
	void (*shutdown)(struct platform_device *);
	struct device_driver driver;
};

struct platform_device { struct device dev; };
struct resource { uint64_t size; };
struct task_struct { int unused; };
struct qcom_wdt_ops {
	int (*disable_wdt)(struct msm_watchdog_data *);
	int (*reset_wdt)(struct msm_watchdog_data *);
	int (*enable_wdt)(uint32_t, struct msm_watchdog_data *);
};
struct msm_watchdog_data {
	struct device *dev;
	struct qcom_wdt_ops *ops;
	struct mutex disable_lock;
	struct task_struct *watchdog_task;
	void *base;
	bool enabled;
};
struct fake_task { unsigned long flags; };
struct quest_platform_binding {
	struct platform_driver *driver;
	struct device *dev;
	struct module *owner;
	void (*original_shutdown)(struct platform_device *);
	bool installed, shutdown_seen;
};

static struct mutex quest_platform_lock, module_mutex, transition_mutex;
static struct module loader_module, scm_module, wdt_module, tee_module, gh_module;
static struct bus_type platform_bus_type;
static struct fake_task task = { .flags = PF_SUSPEND_TASK };
#define current (&task)
static bool pm_freezing = true, defer_all_probes = true;
static bool prepared, self_pinned;
static int shutdown_error;
static struct quest_platform_binding scm_binding, wdt_binding, gh_binding;
static bool allow_tee;
static struct msm_watchdog_data *gh_watchdog_data;
static struct msm_watchdog_data *watchdog_data;
static struct qcom_wdt_ops *watchdog_ops;
static int (*watchdog_suspend)(struct device *);
static int (*secure_watchdog_disable)(void);
static struct resource wdt_resource = { .size = 0x18 };
static struct platform_device scm_pdev, wdt_pdev, tee_pdev, gh_pdev;
static struct platform_driver scm_driver, wdt_driver, tee_driver, gh_driver;
static struct msm_watchdog_data wdt_state, gh_state;
static char gh_wdt_match_table[1];
static bool gh_present;
static int gh_check_result, gh_disable_result, gh_resume_result, dt_check_result;
static int gh_disable_calls, gh_resume_calls, resume_calls;
static u32 gh_status_enabled;

static struct qcom_wdt_ops wdt_ops;
static char qcom_scm_dt_match[1], qcom_soc_match_table[1];
static struct task_struct wdt_task;
static uint32_t wdt_registers[8];
static int secure_disable_result, app_disable_result, suspend_result;
static int hardware_calls, callback_calls;
static bool tee_bound, stuck_enable;
static struct module *denied_module;
static struct device *collision_device;
static int collision_attempt;
static const char *missing_symbol;

static void competing_shutdown(struct platform_device *pdev) { (void)pdev; }

static void mutex_lock(struct mutex *mutex) { assert(!mutex->locked); mutex->locked = true; }
static void mutex_unlock(struct mutex *mutex) { assert(mutex->locked); mutex->locked = false; }
static bool mutex_is_locked(struct mutex *mutex) { return mutex->locked; }
static void lockdep_assert_held(struct mutex *mutex) { assert(mutex->locked); }
static bool try_module_get(struct module *module)
{
	if (module && module == denied_module) return false;
	if (module) module->references++;
	return true;
}
static void module_put(struct module *module) { if (module) { assert(module->references > 0); module->references--; } }
static struct device *get_device(struct device *device) { device->refs++; return device; }
static void put_device(struct device *device) { assert(device->refs > 0); device->refs--; }
static void device_lock(struct device *device)
{
	assert(!device->locked);
	device->locked = true;
	if (device == collision_device && ++device->lock_count == collision_attempt) {
		if (device == &scm_pdev.dev) scm_driver.shutdown = competing_shutdown;
		if (device == &wdt_pdev.dev) wdt_driver.shutdown = competing_shutdown;
	}
}
static void device_unlock(struct device *device) { assert(device->locked); device->locked = false; }
static void *of_match_device(void *table, struct device *device) { return table && device->of_node ? table : NULL; }
static void *dev_get_drvdata(struct device *device) { return device->drvdata; }
static uint32_t readl(void *address) { return *(uint32_t *)address; }
static struct resource *platform_get_resource_byname(struct platform_device *pdev,
							     int type, const char *name)
{
	assert(type == IORESOURCE_MEM && !strcmp(name, "wdt-base"));
	return pdev->dev.resource;
}
static struct platform_device *to_platform_device(struct device *device)
{
	return (struct platform_device *)device;
}
static struct platform_driver *to_platform_driver(struct device_driver *driver)
{
	return (struct platform_driver *)((char *)driver - offsetof(struct platform_driver, driver));
}
#define pr_err(...) ((void)0)
#define pr_info(...) ((void)0)
#define pr_emerg(...) ((void)0)
#define pr_warn(...) ((void)0)
#define resource_size(resource) ((resource)->size)

static void platform_drv_shutdown(struct device *device) { (void)device; }
static int qcom_scm_probe(struct platform_device *pdev) { (void)pdev; return 0; }
static void qcom_scm_shutdown(struct platform_device *pdev) { (void)pdev; }
static int qcom_soc_wdt_probe(struct platform_device *pdev) { (void)pdev; return 0; }
static int qcom_wdt_remove(struct platform_device *pdev) { (void)pdev; return 0; }
static int qcom_soc_disable_wdt(struct msm_watchdog_data *wd)
{
	hardware_calls++;
	if (!stuck_enable)
		*(uint32_t *)(wd->base + QUEST_WDT_ENABLE_OFFSET) = 0;
	return app_disable_result;
}
static int qcom_soc_reset_wdt(struct msm_watchdog_data *wd) { (void)wd; return 0; }
static int qcom_soc_enable_wdt(uint32_t value, struct msm_watchdog_data *wd)
{
	(void)value; (void)wd; return 0;
}
static int qcom_wdt_pet_suspend(struct device *device)
{
	(void)device;
	callback_calls++;
	return suspend_result;
}
static int qcom_scm_sec_wdog_deactivate(void)
{
	hardware_calls++;
	return secure_disable_result;
}
static int qcom_wdt_pet_resume(struct device *device)
{
	(void)device;
	resume_calls++;
	return 0;
}
static int gh_wdt_probe(struct platform_device *pdev) { (void)pdev; return 0; }
static int quest_gh_wdt_check(struct device *dev, struct msm_watchdog_data **out)
{
	assert(dev == &gh_pdev.dev && dev->locked);
	if (!gh_check_result)
		*out = dev->drvdata;
	return gh_check_result;
}
static int quest_gh_wdt_disable(struct device *dev, struct msm_watchdog_data *wd)
{
	assert(dev == &gh_pdev.dev && wd == &gh_state);
	gh_disable_calls++;
	if (!gh_disable_result)
		gh_status_enabled = 0;
	return gh_disable_result;
}
static int quest_gh_wdt_resume(struct device *dev, struct msm_watchdog_data *wd)
{
	assert(dev == &gh_pdev.dev && wd == &gh_state);
	gh_resume_calls++;
	gh_status_enabled = 1;
	return gh_resume_result;
}
static int quest_gh_wdt_status(u32 *enabled, u32 *expired, u32 *since)
{
	assert(!expired && !since);
	*enabled = gh_status_enabled;
	return 0;
}
static int quest_wdt_dt_check(const struct device *msm, const struct device *gh,
			      char *why, size_t len)
{
	assert(msm == wdt_binding.dev && gh == gh_binding.dev && why && len);
	return dt_check_result;
}

static struct device_driver *driver_find(const char *name, struct bus_type *bus)
{
	assert(bus == &platform_bus_type);
	if (!strcmp(name, "qcom_scm")) return &scm_driver.driver;
	if (!strcmp(name, "msm_watchdog")) return &wdt_driver.driver;
	if (!strcmp(name, "shared_memory_bridge")) return &tee_driver.driver;
	if (!strcmp(name, "gh-watchdog") && gh_present) return &gh_driver.driver;
	return NULL;
}

static int driver_for_each_device(struct device_driver *driver, void *data,
				  void *context,
				  int (*callback)(struct device *, void *))
{
	(void)data;
	if (driver == &scm_driver.driver && scm_pdev.dev.driver == driver)
		return callback(&scm_pdev.dev, context);
	if (driver == &wdt_driver.driver && wdt_pdev.dev.driver == driver)
		return callback(&wdt_pdev.dev, context);
	if (driver == &gh_driver.driver && gh_pdev.dev.driver == driver)
		return callback(&gh_pdev.dev, context);
	if (driver == &tee_driver.driver && tee_bound)
		return callback(&tee_pdev.dev, context);
	return 0;
}

static unsigned long quest_lookup_name(const char *name)
{
	if (missing_symbol && !strcmp(missing_symbol, name)) return 0;
#define SYMBOL(symbol) if (!strcmp(name, #symbol) || !strcmp(name, #symbol ".cfi") || \
		!strcmp(name, #symbol ".cfi_jt")) return (unsigned long)&(symbol)
	SYMBOL(module_mutex);
	SYMBOL(system_transition_mutex);
	SYMBOL(pm_freezing);
	SYMBOL(defer_all_probes);
	SYMBOL(qcom_scm_driver);
	SYMBOL(qcom_soc_wdt_driver);
	SYMBOL(qcom_scm_dt_match);
	SYMBOL(qcom_soc_match_table);
	SYMBOL(qcom_soc_wdt_ops);
	SYMBOL(platform_drv_shutdown);
	SYMBOL(qcom_scm_probe);
	SYMBOL(qcom_scm_shutdown);
	SYMBOL(qcom_soc_wdt_probe);
	SYMBOL(qcom_wdt_remove);
	SYMBOL(qcom_soc_disable_wdt);
	SYMBOL(qcom_soc_reset_wdt);
	SYMBOL(qcom_soc_enable_wdt);
	SYMBOL(qcom_wdt_pet_suspend);
	SYMBOL(qcom_scm_sec_wdog_deactivate);
	SYMBOL(qcom_wdt_pet_resume);
	SYMBOL(gh_wdt_driver);
	SYMBOL(gh_wdt_probe);
	SYMBOL(gh_wdt_match_table);
#undef SYMBOL
	return 0;
}

/* Production functions are extracted here, in dependency order. */
/* ACTUAL_FUNCTIONS */

static void reset(void)
{
	memset(&quest_platform_lock, 0, sizeof(quest_platform_lock));
	memset(&module_mutex, 0, sizeof(module_mutex));
	memset(&transition_mutex, 0, sizeof(transition_mutex));
	memset(&loader_module, 0, sizeof(loader_module));
	memset(&scm_module, 0, sizeof(scm_module));
	memset(&wdt_module, 0, sizeof(wdt_module));
	memset(&tee_module, 0, sizeof(tee_module));
	memset(&scm_pdev, 0, sizeof(scm_pdev));
	memset(&wdt_pdev, 0, sizeof(wdt_pdev));
	memset(&tee_pdev, 0, sizeof(tee_pdev));
	memset(&wdt_state, 0, sizeof(wdt_state));
	memset(&wdt_ops, 0, sizeof(wdt_ops));
	memset(&wdt_resource, 0, sizeof(wdt_resource));
	memset(wdt_registers, 0, sizeof(wdt_registers));
	memset(&gh_module, 0, sizeof(gh_module));
	memset(&gh_pdev, 0, sizeof(gh_pdev));
	memset(&gh_state, 0, sizeof(gh_state));
	gh_driver = (struct platform_driver) {
		.probe = gh_wdt_probe,
		.remove = qcom_wdt_remove,
		.driver = { .name = "gh-watchdog", .bus = &platform_bus_type,
			.owner = &gh_module, .shutdown = platform_drv_shutdown,
			.of_match_table = gh_wdt_match_table },
	};
	gh_pdev.dev.driver = &gh_driver.driver;
	gh_pdev.dev.bus = &platform_bus_type;
	gh_pdev.dev.of_node = &gh_driver;
	gh_pdev.dev.drvdata = &gh_state;
	gh_state.dev = &gh_pdev.dev;
	gh_state.watchdog_task = &wdt_task;
	gh_state.enabled = true;
	gh_present = allow_tee = false;
	gh_check_result = gh_disable_result = gh_resume_result = dt_check_result = 0;
	gh_disable_calls = gh_resume_calls = resume_calls = 0;
	gh_status_enabled = 1;
	memset(&gh_binding, 0, sizeof(gh_binding));
	gh_watchdog_data = NULL;
	scm_driver = (struct platform_driver) {
		.probe = qcom_scm_probe,
		.shutdown = qcom_scm_shutdown,
		.driver = { .name = "qcom_scm", .bus = &platform_bus_type,
			.owner = &scm_module, .shutdown = platform_drv_shutdown,
			.of_match_table = qcom_scm_dt_match },
	};
	wdt_driver = (struct platform_driver) {
		.probe = qcom_soc_wdt_probe,
		.remove = qcom_wdt_remove,
		.driver = { .name = "msm_watchdog", .bus = &platform_bus_type,
			.owner = &wdt_module, .shutdown = platform_drv_shutdown,
			.of_match_table = qcom_soc_match_table },
	};
	tee_driver.driver = (struct device_driver) {
		.name = "shared_memory_bridge", .bus = &platform_bus_type,
		.owner = &tee_module,
	};
	scm_pdev.dev.driver = &scm_driver.driver;
	scm_pdev.dev.bus = &platform_bus_type;
	scm_pdev.dev.of_node = &scm_driver;
	wdt_pdev.dev.driver = &wdt_driver.driver;
	wdt_pdev.dev.bus = &platform_bus_type;
	wdt_pdev.dev.of_node = &wdt_driver;
	wdt_pdev.dev.resource = &wdt_resource;
	wdt_pdev.dev.registers = wdt_registers;
	wdt_pdev.dev.drvdata = &wdt_state;
	wdt_resource.size = 0x18;
	wdt_state.dev = &wdt_pdev.dev;
	wdt_state.ops = &wdt_ops;
	wdt_state.base = wdt_registers;
	wdt_state.watchdog_task = &wdt_task;
	wdt_state.disable_lock.locked = false;
	wdt_state.enabled = true;
	wdt_ops.disable_wdt = qcom_soc_disable_wdt;
	wdt_ops.reset_wdt = qcom_soc_reset_wdt;
	wdt_ops.enable_wdt = qcom_soc_enable_wdt;
	transition_mutex.locked = true;
	pm_freezing = defer_all_probes = true;
	current->flags = PF_SUSPEND_TASK;
	prepared = self_pinned = false;
	shutdown_error = 0;
	secure_disable_result = app_disable_result = suspend_result = 0;
	hardware_calls = callback_calls = 0;
	tee_bound = stuck_enable = false;
	denied_module = NULL;
	collision_device = NULL;
	collision_attempt = 0;
	missing_symbol = NULL;
	memset(&scm_binding, 0, sizeof(scm_binding));
	memset(&wdt_binding, 0, sizeof(wdt_binding));
	watchdog_data = NULL;
	watchdog_ops = NULL;
	watchdog_suspend = NULL;
	secure_watchdog_disable = NULL;
}

static void clean(void (*scm)(struct platform_device *),
		  void (*wdt)(struct platform_device *))
{
	assert(!prepared && !self_pinned && !quest_platform_shutdown_ok());
	assert(!scm_binding.dev && !wdt_binding.dev);
	assert(scm_driver.shutdown == scm && wdt_driver.shutdown == wdt);
	assert(!loader_module.references && !scm_module.references && !wdt_module.references);
	assert(!tee_module.references && !scm_pdev.dev.refs && !wdt_pdev.dev.refs);
	assert(!watchdog_data && !watchdog_ops && !watchdog_suspend && !secure_watchdog_disable);
	assert(!quest_platform_lock.locked && !module_mutex.locked);
	assert(!scm_pdev.dev.locked && !wdt_pdev.dev.locked);
	assert(!hardware_calls && !callback_calls);
	assert(!gh_binding.dev && !gh_module.references && !gh_pdev.dev.refs);
	assert(!gh_watchdog_data && gh_driver.shutdown == NULL && !gh_pdev.dev.locked);
	assert(!gh_disable_calls && !gh_resume_calls && !resume_calls);
}

int main(void)
{
	/* Precondition failures must leave callback/ref/hardware state untouched. */
	reset(); missing_symbol = "module_mutex";
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	reset(); defer_all_probes = false;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	reset(); pm_freezing = false;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	reset(); current->flags = 0;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	reset(); transition_mutex.locked = false;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);

	reset();
	tee_bound = true;
	tee_pdev.dev.driver = &tee_driver.driver;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);

	reset(); scm_driver.probe = qcom_soc_wdt_probe;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	/* The remove callback is not part of the identity check: every module
	 * that assigns the exported qcom_wdt_remove holds its own CFI stub. */
	reset(); wdt_driver.remove = NULL;
	assert(quest_platform_prepare() == 0);
	quest_platform_cancel();
	clean(qcom_scm_shutdown, NULL);
	/* A present driver with no bound device is treated as absent. */
	reset(); wdt_pdev.dev.driver = NULL;
	assert(quest_platform_prepare() == 0);
	quest_platform_cancel();
	clean(qcom_scm_shutdown, NULL);
	reset(); wdt_ops.disable_wdt = qcom_soc_reset_wdt;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	reset(); missing_symbol = "qcom_scm_sec_wdog_deactivate";
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	reset(); denied_module = &loader_module;
	assert(quest_platform_prepare() == -EBUSY);
	clean(qcom_scm_shutdown, NULL);
	reset(); denied_module = &scm_module;
	assert(quest_platform_prepare() == -EBUSY);
	clean(qcom_scm_shutdown, NULL);
	reset(); denied_module = &wdt_module;
	assert(quest_platform_prepare() == -EBUSY);
	clean(qcom_scm_shutdown, NULL);

	/* Simulate a competing callback change just before either install. */
	reset(); collision_device = &scm_pdev.dev; collision_attempt = 2;
	assert(quest_platform_prepare() == -EBUSY);
	clean(competing_shutdown, NULL);
	reset(); collision_device = &wdt_pdev.dev; collision_attempt = 3;
	assert(quest_platform_prepare() == -EBUSY);
	clean(qcom_scm_shutdown, competing_shutdown);

	reset();
	wdt_resource.size = 0x17; /* Reject a truncated final register window. */
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);

	reset();
	assert(quest_platform_prepare() == 0);
	assert(prepared && self_pinned && scm_binding.installed && wdt_binding.installed);
	assert(scm_driver.shutdown == quest_scm_warm_shutdown);
	assert(wdt_driver.shutdown == quest_watchdog_shutdown);
	assert(loader_module.references == 1 && scm_module.references == 1 && wdt_module.references == 1);
	assert(!hardware_calls);
	assert(quest_platform_prepare() == -EBUSY); /* Keep the original prepare intact. */
	quest_platform_cancel();
	clean(qcom_scm_shutdown, NULL);
	quest_platform_cancel(); /* EXEC's redundant cancel must be harmless. */
	clean(qcom_scm_shutdown, NULL);

	reset();
	assert(quest_platform_prepare() == 0);
	quest_scm_warm_shutdown(&scm_pdev);
	quest_watchdog_shutdown(&wdt_pdev);
	assert(quest_platform_shutdown_ok());
	assert(hardware_calls == 2 && callback_calls == 1);
	assert(wdt_registers[2] == 0 && !wdt_state.enabled);

	reset();
	wdt_state.watchdog_task = NULL; /* panic-only path has no initialized lock */
	assert(quest_platform_prepare() == 0);
	quest_scm_warm_shutdown(&scm_pdev);
	quest_watchdog_shutdown(&wdt_pdev);
	assert(quest_platform_shutdown_ok());
	assert(hardware_calls == 2 && callback_calls == 0);

	reset();
	assert(quest_platform_prepare() == 0);
	secure_disable_result = -EIO;
	quest_watchdog_shutdown(&wdt_pdev);
	assert(!quest_platform_shutdown_ok());
	assert(hardware_calls == 1 && callback_calls == 0);
	assert(wdt_registers[2] == 0 && wdt_state.enabled);

	reset();
	assert(quest_platform_prepare() == 0);
	suspend_result = -EIO;
	quest_watchdog_shutdown(&wdt_pdev);
	assert(!quest_platform_shutdown_ok());
	assert(hardware_calls == 1 && callback_calls == 1);
	assert(wdt_state.disable_lock.locked == false);

	reset();
	assert(quest_platform_prepare() == 0);
	app_disable_result = -EIO;
	quest_watchdog_shutdown(&wdt_pdev);
	assert(!quest_platform_shutdown_ok());
	assert(hardware_calls == 2 && callback_calls == 1);
	assert(wdt_registers[2] == 0);

	reset();
	assert(quest_platform_prepare() == 0);
	stuck_enable = true;
	wdt_registers[2] = 1;
	quest_watchdog_shutdown(&wdt_pdev);
	assert(!quest_platform_shutdown_ok() && shutdown_error == -EIO);
	assert(hardware_calls == 2 && callback_calls == 1);
	assert(wdt_registers[2] == 1);

	/* allow_tee=1: a bound TEE driver no longer blocks prepare(). */
	reset(); allow_tee = true;
	tee_bound = true;
	tee_pdev.dev.driver = &tee_driver.driver;
	assert(quest_platform_prepare() == 0);
	quest_platform_cancel();
	clean(qcom_scm_shutdown, NULL);

	/* Gunyah watchdog: installed, stopped from its shutdown callback. */
	reset(); gh_present = true;
	assert(quest_platform_prepare() == 0);
	assert(gh_binding.installed && gh_driver.shutdown == quest_gh_watchdog_shutdown);
	assert(gh_module.references == 1 && gh_watchdog_data == &gh_state);
	quest_scm_warm_shutdown(&scm_pdev);
	quest_watchdog_shutdown(&wdt_pdev);
	assert(!quest_platform_shutdown_ok()); /* gh-watchdog not stopped yet */
	quest_gh_watchdog_shutdown(&gh_pdev);
	assert(quest_platform_shutdown_ok() && gh_disable_calls == 1);

	reset(); gh_present = true;
	assert(quest_platform_prepare() == 0);
	gh_disable_result = -EIO;
	quest_scm_warm_shutdown(&scm_pdev);
	quest_watchdog_shutdown(&wdt_pdev);
	quest_gh_watchdog_shutdown(&gh_pdev);
	assert(!quest_platform_shutdown_ok() && shutdown_error == -EIO);

	reset(); gh_present = true;
	assert(quest_platform_prepare() == 0);
	quest_gh_watchdog_shutdown(&wdt_pdev); /* wrong device */
	assert(shutdown_error == -ENODEV && !gh_disable_calls);

	reset(); gh_present = true; gh_check_result = -EOPNOTSUPP;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);
	reset(); gh_present = true; denied_module = &gh_module;
	assert(quest_platform_prepare() == -EBUSY);
	clean(qcom_scm_shutdown, NULL);
	reset(); gh_present = true; gh_pdev.dev.driver = NULL;
	assert(quest_platform_prepare() == -ENODEV);
	clean(qcom_scm_shutdown, NULL);

	/* An unhandled watchdog DT node fails closed before any install. */
	reset(); dt_check_result = -EOPNOTSUPP;
	assert(quest_platform_prepare() == -EOPNOTSUPP);
	clean(qcom_scm_shutdown, NULL);

	/* Watchdog rehearsal: stop+restart both, leave nothing installed. */
	reset(); gh_present = true; transition_mutex.locked = false;
	{
		unsigned int stopped = 0;

		assert(quest_platform_rehearse_watchdog(&stopped) == 0);
		assert(stopped == 2);
		assert(hardware_calls == 1 && callback_calls == 1 && resume_calls == 1);
		assert(gh_disable_calls == 1 && gh_resume_calls == 1 && gh_status_enabled);
		hardware_calls = callback_calls = resume_calls = 0;
		gh_disable_calls = gh_resume_calls = 0;
		clean(qcom_scm_shutdown, NULL);

		reset(); gh_present = true; gh_disable_result = -EIO;
		assert(quest_platform_rehearse_watchdog(&stopped) == -EIO);
		assert(gh_resume_calls == 1); /* always re-armed */
		hardware_calls = callback_calls = resume_calls = 0;
		gh_disable_calls = gh_resume_calls = 0;
		clean(qcom_scm_shutdown, NULL);

		reset(); dt_check_result = -EOPNOTSUPP;
		assert(quest_platform_rehearse_watchdog(&stopped) == -EOPNOTSUPP);
		assert(stopped == 0);
		clean(qcom_scm_shutdown, NULL);

		reset();
		assert(quest_platform_prepare() == 0);
		assert(quest_platform_rehearse_watchdog(&stopped) == -EBUSY);
		quest_platform_cancel();
		clean(qcom_scm_shutdown, NULL);
	}

	puts("platform_rollback: 38 production-path scenarios passed");
	return 0;
}
