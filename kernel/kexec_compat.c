// SPDX-License-Identifier: GPL-2.0
/* Runtime resolution of stock helpers that have no module export. */
#define pr_fmt(fmt) "quest_el1: " fmt
#include <linux/async.h>
#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kprobes.h>
#include <linux/freezer.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/pm_runtime.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "kexec_compat.h"

/* Implemented in kexec_drv.c; empty unless trace_path= was supplied. */
void quest_trace(const char *fmt, ...);
void quest_trace_storage_off(void);
bool quest_trace_is_storage_off(void);

/* Same enum umh_disable_depth values as include/linux/umh.h. */
#define QUEST_UMH_DISABLED 2

static unsigned long lookup_addr;
module_param(lookup_addr, ulong, 0400);
MODULE_PARM_DESC(lookup_addr,
	"Explicit stock kallsyms_lookup_name address if kprobe lookup is unavailable");

static unsigned long (*lookup_ptr)(const char *name);
static void (*restart_prepare_ptr)(char *cmd);
static void (*migrate_ptr)(void);
static void (*shutdown_ptr)(void);
static struct mutex *transition_mutex_ptr;
static int (*freeze_ptr)(void);
static void (*thaw_ptr)(void);
static void (*block_probing_ptr)(void);
static void (*unblock_probing_ptr)(void);
static void (*device_shutdown_ptr)(void);
static int (*umh_disable_ptr)(int depth);
static struct blocking_notifier_head *reboot_head;
/*
 * devices_kset is a pointer variable (struct kset *), not the kset itself:
 * kallsyms gives the address of the variable, so it has to be dereferenced
 * before the kset fields can be used.
 */
static struct kset **devices_kset_var;
static void (*cpufreq_suspend_ptr)(void);
static struct work_struct *deferred_probe_work_ptr;
static wait_queue_head_t *probe_waitqueue_ptr;
static atomic_t *probe_count_ptr;
static void (*async_full_ptr)(void);
static void (*kmsg_dump_ptr)(enum kmsg_dump_reason reason);

/*
 * The trace file lives on the UFS device, so its shutdown callback is the
 * point where logging dies. Skipping that single callback keeps the file
 * writable for the rest of the handoff, which is what makes the late
 * checkpoints (platform shutdown, machine_kexec) observable.
 */
static bool skip_storage_shutdown;
module_param(skip_storage_shutdown, bool, 0600);
MODULE_PARM_DESC(skip_storage_shutdown,
	"Skip the ufshcd shutdown callback so the trace file stays writable");

/*
 * The trace file cannot be used once the UFS controller is down, and this
 * kernel's ramoops never exposes pmsg/console zones through /sys/fs/pstore
 * (only PSTORE_TYPE_DMESG records get an entry). So the late checkpoints are
 * snapshotted into the dmesg zone instead: printk a marker, then run a pstore
 * dump. Boot the target with log_buf_len=128K so a single dump part carries
 * the whole ring, including the marker.
 */
void __nocfi quest_kmsg_snapshot(const char *tag)
{
	pr_info("quest snapshot: %s\n", tag);
	/*
	 * KMSG_DUMP_OOPS (rather than PANIC) waits for pstore's dump lock instead
	 * of skipping on contention, and it is what makes pstore rescan its
	 * records when update_ms is enabled (used to test the channel in place).
	 */
	if (kmsg_dump_ptr)
		kmsg_dump_ptr(KMSG_DUMP_OOPS);
}

/*
 * wait_for_device_probe() with a checkpoint after each of its three steps.
 * The stock helper is opaque: if one of these steps blocks forever, the app
 * watchdog turns the handoff into a silent reset instead of a failure.
 */
static void __nocfi quest_wait_for_device_probe_traced(void)
{
	if (!deferred_probe_work_ptr || !probe_waitqueue_ptr || !probe_count_ptr) {
		wait_for_device_probe();
		quest_trace("devshut: stock wait_for_device_probe done");
		return;
	}
	flush_work(deferred_probe_work_ptr);
	quest_trace("devshut: deferred probe work flushed, probe_count=%d",
		    atomic_read(probe_count_ptr));
	if (atomic_read(probe_count_ptr))
		wait_event_timeout(*probe_waitqueue_ptr,
				   atomic_read(probe_count_ptr) == 0, 10 * HZ);
	quest_trace("devshut: probe_count now %d",
		    atomic_read(probe_count_ptr));
	if (async_full_ptr) {
		async_full_ptr();
		quest_trace("devshut: async_synchronize_full done");
	}
}

/*
 * A faithful copy of device_shutdown() (drivers/base/core.c) with a trace line
 * before every class/bus/driver shutdown callback. The stock loop gives no
 * way to see which device kills the machine, and the machine dies silently
 * because the log reader is frozen and pstore has no backend here.
 */
void __nocfi quest_device_shutdown_traced(void)
{
	struct device *dev, *parent;
	struct kset *kset;
	struct list_head *list;
	spinlock_t *lock;
	unsigned int index = 0;
	bool is_storage;

	kset = devices_kset_var ? *devices_kset_var : NULL;
	if (!kset || !device_shutdown_ptr) {
		quest_trace("devshut: fallback stock=%d kset=%d",
			    device_shutdown_ptr != NULL,
			    kset != NULL);
		if (device_shutdown_ptr)
			device_shutdown_ptr();
		return;
	}
	quest_trace("devshut: enter");
	quest_wait_for_device_probe_traced();
	quest_block_probing();
	quest_trace("devshut: probing blocked");
	if (cpufreq_suspend_ptr) {
		quest_trace("devshut: cpufreq_suspend enter");
		cpufreq_suspend_ptr();
		quest_trace("devshut: cpufreq suspended");
	}

	list = &kset->list;
	lock = &kset->list_lock;
	quest_trace("devshut: list walk begin");
	spin_lock(lock);
	while (!list_empty(list)) {
		dev = list_entry(list->prev, struct device, kobj.entry);
		parent = get_device(dev->parent);
		get_device(dev);
		list_del_init(&dev->kobj.entry);
		spin_unlock(lock);

		++index;
		quest_trace("devshut: #%u detached", index);
		/*
		 * Name the device outside every device lock: the trace write
		 * performs file I/O plus a fsync, and one of these devices may
		 * well be the storage stack that backs the trace file.
		 */
		quest_trace("devshut: #%u dev=%s class=%s bus=%s drv=%s",
			    index, dev_name(dev),
			    dev->class ? dev->class->name : "-",
			    dev->bus ? dev->bus->name : "-",
			    dev->driver ? dev->driver->name : "-");

		/*
		 * Only one ramoops dmesg record exists and each dump overwrites
		 * it, so dumping right before every post-storage shutdown
		 * callback leaves the offending device in the surviving record.
		 */
		if (quest_trace_is_storage_off())
			quest_kmsg_snapshot("device-shutdown");

		if (parent)
			device_lock(parent);
		device_lock(dev);
		quest_trace("devshut: #%u locked", index);

		/* Don't allow any more runtime suspends */
		atomic_inc(&dev->power.usage_count);
		pm_runtime_barrier(dev);

		/*
		 * The trace file lives on the UFS device: its shutdown callback
		 * stops the controller, after which every write to that file
		 * blocks forever. Note the point, then optionally skip that one
		 * callback so the remaining checkpoints stay observable.
		 */
		is_storage = dev->driver && dev->driver->name &&
			     !strncmp(dev->driver->name, "ufshcd", 6);
		if (is_storage && !skip_storage_shutdown) {
			quest_trace_storage_off();
			quest_kmsg_snapshot("pre-ufs-shutdown");
		}
		if (is_storage && skip_storage_shutdown) {
			quest_trace("devshut: #%u storage shutdown skipped", index);
		} else if (dev->class && dev->class->shutdown_pre) {
			quest_trace("devshut: #%u shutdown_pre class=%s", index,
				    dev->class->name);
			dev->class->shutdown_pre(dev);
		} else if (dev->bus && dev->bus->shutdown) {
			quest_trace("devshut: #%u shutdown bus=%s", index,
				    dev->bus->name);
			dev->bus->shutdown(dev);
		} else if (dev->driver && dev->driver->shutdown) {
			quest_trace("devshut: #%u shutdown driver=%s", index,
				    dev->driver->name);
			dev->driver->shutdown(dev);
		}

		device_unlock(dev);
		if (parent)
			device_unlock(parent);
		put_device(dev);
		put_device(parent);

		spin_lock(lock);
	}
	spin_unlock(lock);
	quest_kmsg_snapshot("device-shutdown-done");
}

/* kallsyms supplies raw addresses, not cross-DSO CFI jump-table pointers. */
unsigned long __nocfi quest_lookup_name(const char *name)
{
	return lookup_ptr ? lookup_ptr(name) : 0;
}

void __nocfi quest_kernel_restart_prepare(char *cmd)
{
	/*
	 * The stock helper is split into its four steps so the trace file shows
	 * exactly where a silent reset happens: reboot notifiers, userspace
	 * helper shutdown or device_shutdown().
	 */
	if (!reboot_head || !device_shutdown_ptr) {
		restart_prepare_ptr(cmd);
		return;
	}
	blocking_notifier_call_chain(reboot_head, SYS_RESTART, cmd);
	quest_trace("exec: reboot notifiers done");
	system_state = SYSTEM_RESTART;
	if (umh_disable_ptr)
		umh_disable_ptr(QUEST_UMH_DISABLED);
	quest_trace("exec: usermodehelper disabled");
	quest_device_shutdown_traced();
	quest_trace("exec: device shutdown done");
	quest_kmsg_snapshot("restart-prepare-done");
}

void __nocfi quest_migrate_to_reboot_cpu(void)
{
	migrate_ptr();
}

void __nocfi quest_machine_shutdown(void)
{
	shutdown_ptr();
}

struct mutex *quest_transition_mutex(void)
{
	return transition_mutex_ptr;
}

/*
 * Android keeps wakeup sources busy (autosleep, sensor/USB wake IRQs), and a
 * wakeup event racing the freezer makes freeze_processes() abort with -EBUSY
 * after thawing everything again. Such events are transient; retry a few
 * times instead of failing the whole handoff (the Quest Pro port hit this on
 * almost every attempt).
 */
static unsigned int freeze_attempts = 5;
module_param(freeze_attempts, uint, 0600);
MODULE_PARM_DESC(freeze_attempts, "freeze_processes() attempts on -EBUSY (1-20)");

int __nocfi quest_freeze_processes(void)
{
	unsigned int attempt, attempts = clamp_val(READ_ONCE(freeze_attempts), 1U, 20U);
	int ret;

	for (attempt = 1;; attempt++) {
		ret = freeze_ptr();
		if (ret != -EBUSY || attempt >= attempts)
			return ret;
		pr_warn("freeze_processes busy (wakeup event), retry %u/%u\n",
			attempt, attempts - 1);
		msleep(200);
	}
}

void __nocfi quest_thaw_processes(void)
{
	thaw_ptr();
}

void __nocfi quest_block_probing(void)
{
	block_probing_ptr();
}

void __nocfi quest_unblock_probing(void)
{
	unblock_probing_ptr();
}

int kexec_compat_load(void)
{
	int ret;

#if !IS_ENABLED(CONFIG_FREEZER)
	pr_err("The warm handoff requires CONFIG_FREEZER\n");
	return -EOPNOTSUPP;
#endif

	if (lookup_addr) {
		if ((lookup_addr & 3) || (lookup_addr >> 63) != 1)
			return -EINVAL;
		lookup_ptr = (void *)lookup_addr;
		pr_warn("Using explicitly supplied stock symbol lookup address\n");
	} else {
#if IS_ENABLED(CONFIG_KPROBES)
		struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

		ret = register_kprobe(&kp);
		if (ret) {
			pr_err("kprobe symbol bootstrap failed: %d\n", ret);
			return ret;
		}
		lookup_ptr = (void *)kp.addr;
		unregister_kprobe(&kp);
#else
		pr_err("No kprobes; supply the verified stock lookup_addr parameter\n");
		return -EOPNOTSUPP;
#endif
	}

	restart_prepare_ptr = (void *)quest_lookup_name("kernel_restart_prepare");
	migrate_ptr = (void *)quest_lookup_name("migrate_to_reboot_cpu");
	shutdown_ptr = (void *)quest_lookup_name("machine_shutdown");
	transition_mutex_ptr = (void *)quest_lookup_name("system_transition_mutex");
	freeze_ptr = (void *)quest_lookup_name("freeze_processes");
	thaw_ptr = (void *)quest_lookup_name("thaw_processes");
	block_probing_ptr = (void *)quest_lookup_name("device_block_probing");
	unblock_probing_ptr = (void *)quest_lookup_name("device_unblock_probing");
	device_shutdown_ptr = (void *)quest_lookup_name("device_shutdown");
	umh_disable_ptr = (void *)quest_lookup_name("__usermodehelper_disable");
	reboot_head = (void *)quest_lookup_name("reboot_notifier_list");
	devices_kset_var = (void *)quest_lookup_name("devices_kset");
	cpufreq_suspend_ptr = (void *)quest_lookup_name("cpufreq_suspend");
	deferred_probe_work_ptr = (void *)quest_lookup_name("deferred_probe_work");
	probe_waitqueue_ptr = (void *)quest_lookup_name("probe_waitqueue");
	probe_count_ptr = (void *)quest_lookup_name("probe_count");
	async_full_ptr = (void *)quest_lookup_name("async_synchronize_full");
	kmsg_dump_ptr = (void *)quest_lookup_name("kmsg_dump");
	pr_info("helpers: devshut=%px umh=%px reboot=%px ksetvar=%px kset=%px cpufreq=%px ddwork=%px ddwq=%px ddcount=%px async=%px kdump=%px\n",
		(void *)device_shutdown_ptr, (void *)umh_disable_ptr,
		(void *)reboot_head, (void *)devices_kset_var,
		devices_kset_var ? *devices_kset_var : NULL,
		(void *)cpufreq_suspend_ptr, (void *)deferred_probe_work_ptr,
		(void *)probe_waitqueue_ptr, (void *)probe_count_ptr,
		(void *)async_full_ptr, (void *)kmsg_dump_ptr);
	if (!restart_prepare_ptr || !migrate_ptr || !shutdown_ptr ||
	    !transition_mutex_ptr || !freeze_ptr || !thaw_ptr ||
	    !block_probing_ptr || !unblock_probing_ptr) {
		pr_err("Required stock shutdown/freezer/probe helper is unavailable\n");
		kexec_compat_unload();
		return -ENOENT;
	}
	return 0;
}

void kexec_compat_unload(void)
{
	lookup_ptr = NULL;
	restart_prepare_ptr = NULL;
	migrate_ptr = NULL;
	shutdown_ptr = NULL;
	transition_mutex_ptr = NULL;
	freeze_ptr = NULL;
	thaw_ptr = NULL;
	block_probing_ptr = NULL;
	unblock_probing_ptr = NULL;
	device_shutdown_ptr = NULL;
	umh_disable_ptr = NULL;
	reboot_head = NULL;
	devices_kset_var = NULL;
	cpufreq_suspend_ptr = NULL;
	deferred_probe_work_ptr = NULL;
	probe_waitqueue_ptr = NULL;
	probe_count_ptr = NULL;
	async_full_ptr = NULL;
	kmsg_dump_ptr = NULL;
}
