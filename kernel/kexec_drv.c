// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "quest_el1: " fmt

#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/security.h>
#include <linux/ioport.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel_read_file.h>
#include <linux/timekeeping.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>

#include "../include/quest_el1_uapi.h"
#include "kexec.h"
#include "kexec_compat.h"
#include "kexec_internal.h"
#include "platform_pci.h"
#include "platform_quest.h"
#include "platform_rproc.h"
#include "rehearse.h"
#include "report.h"
#include "memory_guard.h"
#include "arch/arm64/machine_kexec_compat.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Quest EL1 kexec");
MODULE_DESCRIPTION("Self-contained EL1 kexec loader for ARM64");
MODULE_VERSION("0.2");
/* filp_open()/kernel_write() live in the VFS-internal export namespace. */
#ifndef MODULE_IMPORT_NS
#define MODULE_IMPORT_NS(ns)
#endif
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

/*
 * Post-freeze checkpoints cannot be seen from userspace: the process that
 * reads /proc/kmsg is frozen along with everything else, and the pstore
 * console backend is unavailable on this device. Write the checkpoints to a
 * file instead and fsync after every line so they survive the reset.
 *
 * That only works until device_shutdown() reaches the UFS controller: once
 * ufshcd-qcom is shut down, every later write (and its fsync) blocks forever.
 * From that point on the checkpoints go to the printk ring buffer instead,
 * which quest_kmsg_snapshot() copies into the ramoops dmesg zone -- the only
 * pstore zone this kernel exposes through /sys/fs/pstore.
 */
static char *trace_path = "";
module_param(trace_path, charp, 0600);
MODULE_PARM_DESC(trace_path, "File that receives EXEC checkpoints (empty: disabled)");

static struct file *trace_file;
static loff_t trace_pos;
static bool trace_storage_off;

/* One pstore dump at load: validates the checkpoint channel without a reset. */
static bool snapshot_test;
module_param(snapshot_test, bool, 0600);
MODULE_PARM_DESC(snapshot_test, "Take one pstore snapshot at load (channel test)");

/* Implemented in kexec_compat.c. */
void quest_kmsg_snapshot(const char *tag);

/* Also used by the platform shutdown hooks and the compat helpers. */
void quest_trace(const char *fmt, ...)
{
	va_list args;
	char msg[160];
	char buf[176];
	int mlen, len;

	if (!trace_file && !trace_storage_off)
		return;
	va_start(args, fmt);
	mlen = vsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	if (mlen <= 0)
		return;
	if (mlen > (int)sizeof(msg) - 1)
		mlen = sizeof(msg) - 1;
	if (trace_storage_off) {
		/* The trace file lives on the storage that is going down. */
		pr_info("quest: %s\n", msg);
		return;
	}
	/* Milliseconds since boot: a watchdog reset shows up as a long gap. */
	len = snprintf(buf, sizeof(buf), "[%llu] ", ktime_get_ns() / 1000000ULL);
	if (len + mlen > (int)sizeof(buf) - 2)
		len = (int)sizeof(buf) - 2 - mlen;
	memcpy(buf + len, msg, mlen);
	len += mlen;
	buf[len++] = '\n';

	kernel_write(trace_file, buf, len, &trace_pos);
	vfs_fsync(trace_file, 0);
}

/* Called before the storage controller's shutdown callback runs. */
void quest_trace_storage_off(void)
{
	if (trace_storage_off)
		return;
	trace_storage_off = true;
	pr_info("quest: trace storage off, checkpoints continue in dmesg\n");
}

/* True once the trace file is unusable; checkpoints go to the ring buffer. */
bool quest_trace_is_storage_off(void)
{
	return trace_storage_off;
}

static void quest_trace_open(void)
{
	if (trace_file || !trace_path || !*trace_path)
		return;
	trace_file = filp_open(trace_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(trace_file)) {
		pr_err("cannot open trace file %s: %ld\n", trace_path,
		       PTR_ERR(trace_file));
		trace_file = NULL;
	} else {
		trace_pos = 0;
	}
}

static void quest_trace_close(void)
{
	if (trace_file) {
		filp_close(trace_file, NULL);
		trace_file = NULL;
	}
	trace_storage_off = false;
}

static int quest_check_credential(void)
{
	if (!capable(CAP_SYS_BOOT))
		return -EPERM;
	return 0;
}

static int quest_check_header(const struct quest_el1_header *hdr)
{
	/* ABI 2 only added REHEARSE and read(); ABI-1 requests are unchanged. */
	if (hdr->magic != QUEST_EL1_MAGIC ||
	    (hdr->abi_version != QUEST_EL1_ABI_VERSION && hdr->abi_version != 1))
		return -EPROTO;
	return 0;
}

static int quest_check_load_policy(void)
{
	return security_kernel_load_data(LOADING_KEXEC_IMAGE, false);
}

static int quest_copy_segments(struct kimage *image,
			       const struct quest_el1_load *req)
{
	struct quest_el1_segment __user *user_segments;
	unsigned long i;

	if (!req->nr_segments || req->nr_segments > KEXEC_SEGMENT_MAX)
		return -EINVAL;
	user_segments = (void __user *)(unsigned long)req->segments;
	image->nr_segments = req->nr_segments;

	for (i = 0; i < image->nr_segments; i++) {
		struct quest_el1_segment in;
		struct kexec_segment *out = &image->segment[i];

		if (copy_from_user(&in, &user_segments[i], sizeof(in)))
			return -EFAULT;
		if (!in.buf || !in.bufsz)
			return -EINVAL;
		out->buf = (void __user *)(unsigned long)in.buf;
		out->bufsz = (size_t)in.bufsz;
		out->mem = (unsigned long)in.mem;
		out->memsz = (size_t)in.memsz;
	}
	return 0;
}

static struct kimage *quest_alloc_image(const struct quest_el1_load *req,
					int *error)
{
	struct kimage *image;
	unsigned long i;

	image = do_kimage_alloc_init();
	if (!image) {
		*error = -ENOMEM;
		return NULL;
	}
	image->start = req->entry;
	image->dtb = req->dtb;
	if (req->entry > ULONG_MAX || req->dtb > ULONG_MAX) {
		*error = -EINVAL;
		goto fail;
	}
	*error = quest_copy_segments(image, req);
	if (*error)
		goto fail;
	*error = sanity_check_segment_list(image);
	if (*error)
		goto fail;
	*error = quest_validate_image(image);
	if (*error)
		goto fail;
	*error = quest_validate_destinations(image);
	if (*error)
		goto fail;
	image->control_code_page = kimage_alloc_control_pages(
		image, get_order(KEXEC_CONTROL_PAGE_SIZE));
	if (!image->control_code_page) {
		*error = -ENOMEM;
		goto fail;
	}
	image->swap_page = kimage_alloc_control_pages(image, 0);
	if (!image->swap_page) {
		*error = -ENOMEM;
		goto fail;
	}
	*error = machine_kexec_prepare(image);
	if (*error)
		goto fail;
	for (i = 0; i < image->nr_segments; i++) {
		*error = kimage_load_segment(image, &image->segment[i]);
		if (*error)
			goto fail;
	}
	kimage_terminate(image);
	*error = quest_validate_staged_image(image);
	if (*error)
		goto fail;
	return image;

fail:
	kimage_free(image);
	return NULL;
}

static int quest_load(const struct quest_el1_load __user *arg)
{
	struct quest_el1_load req;
	struct kimage *new_image, *old_image;
	int ret;

	ret = quest_check_credential();
	if (ret)
		return ret;
	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;
	ret = quest_check_header(&req.hdr);
	if (ret)
		return ret;
	if (req.flags != 0 && req.flags != QUEST_EL1_ARCH_AARCH64)
		return -EINVAL;
	if (!req.entry || !req.dtb)
		return -EINVAL;
	ret = quest_check_load_policy();
	if (ret)
		return ret;

	if (!mutex_trylock(&kexec_mutex))
		return -EBUSY;
	new_image = quest_alloc_image(&req, &ret);
	if (new_image) {
		old_image = xchg(&kexec_image, new_image);
		kimage_free(old_image);
	}
	mutex_unlock(&kexec_mutex);
	return ret;
}

static int quest_execute(void)
{
	struct mutex *transition = quest_transition_mutex();
	int ret;

	/*
	 * Freeze before taking kexec_mutex: a task waiting in STATUS must be
	 * able to finish rather than preventing the freezer from completing.
	 * Serialize with ordinary reboot and suspend before changing freezer
	 * or driver-probe state. Trylock avoids sleeping in another freezer.
	 */
	pr_info("quest exec: enter\n");
	quest_trace_open();
	quest_trace("exec: enter");
	if (!mutex_trylock(transition)) {
		quest_trace("exec: transition mutex busy");
		return -EBUSY;
	}
	ret = quest_freeze_processes();
	if (ret) {
		quest_trace("exec: freeze failed ret=%d", ret);
		goto unlock_transition; /* freeze_processes() already thawed. */
	}
	pr_info("quest exec: processes frozen\n");
	quest_trace("exec: processes frozen");
	if (!mutex_trylock(&kexec_mutex)) {
		ret = -EBUSY;
		quest_trace("exec: kexec_mutex busy");
		goto thaw;
	}
	if (!kexec_image) {
		ret = -EINVAL;
		quest_trace("exec: no image");
		goto unlock_image;
	}
	ret = quest_arch_check();
	if (ret) {
		quest_trace("exec: arch check failed ret=%d", ret);
		goto unlock_image;
	}
	pr_info("quest exec: arch ok, image entry=%#lx\n", kexec_image->start);
	quest_trace("exec: arch ok entry=%#lx", kexec_image->start);
	/*
	 * Stop DMA-capable remote processors (stop_remoteprocs=) while their
	 * clients are frozen; q6v5_pas has no shutdown callback, so
	 * device_shutdown() alone would leave them running on the new kernel's
	 * memory. A failed stop restarts whatever it had stopped.
	 */
	pr_info("quest exec: stopping remote processors\n");
	quest_trace("exec: stopping remote processors");
	ret = quest_rproc_stop(NULL);
	if (ret) {
		quest_trace("exec: rproc stop failed ret=%d", ret);
		goto unlock_image;
	}
	pr_info("quest exec: remote processors stopped=%u\n",
		quest_rproc_stopped_count());
	quest_trace("exec: rprocs stopped=%u", quest_rproc_stopped_count());

	/* The userspace freezer alone does not stop deferred probe workers. */
	quest_block_probing();
	quest_trace("exec: probing blocked");
	/* Recheck RAM after all in-flight probes have finished reserving it. */
	ret = quest_validate_destinations(kexec_image);
	if (ret) {
		quest_trace("exec: destination validation failed ret=%d", ret);
		goto unblock;
	}
	pr_info("quest exec: destinations validated\n");
	quest_trace("exec: destinations validated");
	ret = quest_platform_prepare();
	if (ret) {
		quest_trace("exec: platform prepare failed ret=%d", ret);
		quest_platform_cancel();
		goto unblock;
	}
	pr_info("quest exec: platform prepared\n");
	quest_trace("exec: platform prepared");

	/* No return to userspace after shutdown notifiers/devices run. */
	kexec_in_progress = true;
	quest_rproc_commit();
	quest_kernel_restart_prepare(NULL);
	quest_trace("exec: restart prepare done");
	if (!quest_platform_shutdown_ok()) {
		quest_trace("exec: platform shutdown NOT ok, panicking");
		panic("quest_el1: platform shutdown failed; refusing handoff");
	}
	pr_info("quest exec: platform shutdown ok\n");
	quest_trace("exec: platform shutdown ok");
	quest_pci_quiesce();
	pr_info("quest exec: pci quiesced\n");
	quest_trace("exec: pci quiesced");
	quest_migrate_to_reboot_cpu();
	quest_trace("exec: migrated to reboot cpu");
	cpu_hotplug_enable();
	quest_trace("exec: hotplug re-enabled");
	pr_info("quest exec: machine shutdown begin\n");
	quest_trace("exec: machine shutdown begin");
	quest_kmsg_snapshot("pre-machine-shutdown");
	quest_machine_shutdown();
	pr_info("quest exec: machine kexec begin\n");
	quest_trace("exec: machine kexec begin");
	quest_kmsg_snapshot("pre-kexec-jump");
	machine_kexec(kexec_image);
	quest_trace("exec: machine kexec returned!");
	panic("quest_el1: machine_kexec unexpectedly returned");

unblock:
	quest_unblock_probing();
	quest_rproc_restart();
	quest_trace("exec: rolled back ret=%d", ret);
unlock_image:
	mutex_unlock(&kexec_mutex);
thaw:
	quest_thaw_processes();
unlock_transition:
	mutex_unlock(transition);
	return ret;
}

static long quest_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct quest_el1_header hdr;
	long ret;

	if (cmd == QUEST_EL1_IOC_STATUS) {
		struct quest_el1_status status = {
			.hdr = { QUEST_EL1_MAGIC, QUEST_EL1_ABI_VERSION },
			.current_el = (read_sysreg(CurrentEL) >> 2) & 3,
			.stock_va_bits = VA_BITS,
			.page_shift = PAGE_SHIFT,
		};

		ret = quest_check_credential();
		if (ret)
			return ret;
		if (copy_from_user(&hdr, (void __user *)arg, sizeof(hdr)))
			return -EFAULT;
		ret = quest_check_header(&hdr);
		if (ret)
			return ret;
		mutex_lock(&kexec_mutex);
		if (kexec_image) {
			status.loaded = 1;
			status.entry = kexec_image->start;
			status.dtb = kexec_image->dtb;
			status.nr_segments = kexec_image->nr_segments;
		}
		mutex_unlock(&kexec_mutex);
		return copy_to_user((void __user *)arg, &status, sizeof(status)) ?
			-EFAULT : 0;
	}

	if (copy_from_user(&hdr, (void __user *)arg, sizeof(hdr)))
		return -EFAULT;
	ret = quest_check_header(&hdr);
	if (ret)
		return ret;
	if (cmd == QUEST_EL1_IOC_LOAD)
		return quest_load((const void __user *)arg);
	if (cmd == QUEST_EL1_IOC_UNLOAD) {
		ret = quest_check_credential();
		if (ret)
			return ret;
		if (!mutex_trylock(&kexec_mutex))
			return -EBUSY;
		kimage_free(xchg(&kexec_image, NULL));
		mutex_unlock(&kexec_mutex);
		return 0;
	}
	if (cmd == QUEST_EL1_IOC_REHEARSE) {
		struct quest_el1_rehearse req;

		ret = quest_check_credential();
		if (ret)
			return ret;
		if (hdr.abi_version < 2)
			return -EPROTO;
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		ret = quest_rehearse(&req);
		if (ret)
			return ret;
		return copy_to_user((void __user *)arg, &req, sizeof(req)) ?
			-EFAULT : 0;
	}
	if (cmd == QUEST_EL1_IOC_EXEC) {
		ret = quest_check_credential();
		if (ret)
			return ret;
		ret = quest_check_load_policy();
		if (ret)
			return ret;
		return quest_execute();
	}
	return -ENOTTY;
}

/*
 * misc_open() leaves file->private_data pointing at the static struct
 * miscdevice, which read()/release() would mistake for a report. Clear it so
 * private_data only ever holds a report allocated by read().
 */
static int quest_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

/*
 * read(): the read-only preflight report, generated once per open file at
 * the first read and released on close, so `cat /dev/quest_el1` works.
 */
static ssize_t quest_read(struct file *file, char __user *buf, size_t count,
			  loff_t *ppos)
{
	struct quest_report *r = file->private_data;
	int ret;

	if (!r) {
		ret = quest_check_credential();
		if (ret)
			return ret;
		r = kzalloc(sizeof(*r), GFP_KERNEL);
		if (!r)
			return -ENOMEM;
		ret = quest_report_build(r);
		if (ret) {
			kfree(r);
			return ret;
		}
		if (cmpxchg(&file->private_data, NULL, r)) {
			quest_report_free(r);
			kfree(r);
			r = file->private_data;
		}
	}
	return simple_read_from_buffer(buf, count, ppos, r->buf, r->len);
}

static int quest_release(struct inode *inode, struct file *file)
{
	struct quest_report *r = file->private_data;

	if (r) {
		quest_report_free(r);
		kfree(r);
	}
	return 0;
}

static const struct file_operations quest_fops = {
	.owner = THIS_MODULE,
	.open = quest_open,
	.read = quest_read,
	.release = quest_release,
	.llseek = default_llseek,
	.unlocked_ioctl = quest_ioctl,
};

static struct miscdevice quest_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "quest_el1",
	.nodename = "quest_el1",
	.mode = 0600,
	.fops = &quest_fops,
};

static int __init quest_init(void)
{
	int ret;

	ret = kexec_compat_load();
	if (ret)
		return ret;
	ret = quest_pci_compat_init();
	if (ret)
		goto err_compat;
	ret = quest_memory_guard_init();
	if (ret)
		goto err_compat;
	ret = quest_arch_init();
	if (ret)
		goto err_compat;
	ret = misc_register(&quest_misc);
	if (ret)
		goto err_arch;
	if (snapshot_test)
		quest_kmsg_snapshot("load-test");
	pr_info("ready on /dev/quest_el1; CONFIG_KEXEC is not required\n");
	return 0;

err_arch:
	quest_arch_exit();
err_compat:
	kexec_compat_unload();
	return ret;
}
module_init(quest_init);

static void __exit quest_exit(void)
{
	misc_deregister(&quest_misc);
	mutex_lock(&kexec_mutex);
	kimage_free(xchg(&kexec_image, NULL));
	mutex_unlock(&kexec_mutex);
	quest_arch_exit();
	kexec_compat_unload();
	quest_trace_close();
}
module_exit(quest_exit);
