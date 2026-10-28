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
#include <linux/kernel_read_file.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>

#include "../include/quest_el1_uapi.h"
#include "kexec.h"
#include "kexec_compat.h"
#include "kexec_internal.h"
#include "platform_pci.h"
#include "platform_quest.h"
#include "memory_guard.h"
#include "arch/arm64/machine_kexec_compat.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Quest EL1 kexec");
MODULE_DESCRIPTION("Self-contained EL1 kexec loader for ARM64");
MODULE_VERSION("0.1");

static int quest_check_credential(void)
{
	if (!capable(CAP_SYS_BOOT))
		return -EPERM;
	return 0;
}

static int quest_check_header(const struct quest_el1_header *hdr)
{
	if (hdr->magic != QUEST_EL1_MAGIC ||
	    hdr->abi_version != QUEST_EL1_ABI_VERSION)
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
	if (!mutex_trylock(transition))
		return -EBUSY;
	ret = quest_freeze_processes();
	if (ret)
		goto unlock_transition; /* freeze_processes() already thawed. */
	if (!mutex_trylock(&kexec_mutex)) {
		ret = -EBUSY;
		goto thaw;
	}
	if (!kexec_image) {
		ret = -EINVAL;
		goto unlock_image;
	}
	ret = quest_arch_check();
	if (ret)
		goto unlock_image;

	/* The userspace freezer alone does not stop deferred probe workers. */
	quest_block_probing();
	/* Recheck RAM after all in-flight probes have finished reserving it. */
	ret = quest_validate_destinations(kexec_image);
	if (ret)
		goto unblock;
	ret = quest_platform_prepare();
	if (ret) {
		quest_platform_cancel();
		goto unblock;
	}

	/* No return to userspace after shutdown notifiers/devices run. */
	kexec_in_progress = true;
	quest_kernel_restart_prepare(NULL);
	if (!quest_platform_shutdown_ok())
		panic("quest_el1: platform shutdown failed; refusing handoff");
	quest_pci_quiesce();
	quest_migrate_to_reboot_cpu();
	cpu_hotplug_enable();
	quest_machine_shutdown();
	machine_kexec(kexec_image);
	panic("quest_el1: machine_kexec unexpectedly returned");

unblock:
	quest_unblock_probing();
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

static const struct file_operations quest_fops = {
	.owner = THIS_MODULE,
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
}
module_exit(quest_exit);
