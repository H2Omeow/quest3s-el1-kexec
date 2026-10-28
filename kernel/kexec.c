// SPDX-License-Identifier: GPL-2.0
/* Validate the direct ARM64 Image + DTB boot contract before staging pages. */
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <asm/unaligned.h>

#include "kexec.h"

static int quest_validate_image_data(struct kimage *image, bool staged)
{
	unsigned long i;
	bool have_image = false, have_dtb = false;

	if (image->dtb & 7)
		return -EINVAL;
	for (i = 0; i < image->nr_segments; i++) {
		struct kexec_segment *seg = &image->segment[i];
		unsigned long end = seg->mem + seg->bufsz;

		if (!staged && !access_ok(seg->buf, seg->bufsz))
			return -EFAULT;
		if (image->start == seg->mem) {
			u8 hdr[64];
			u64 text_offset, image_size, flags;

			if (seg->bufsz < sizeof(hdr))
				return -ENOEXEC;
			if (staged) {
				if (kimage_read_staged(image, image->start, hdr,
						      sizeof(hdr)))
					return -EFAULT;
			} else if (copy_from_user(hdr, seg->buf, sizeof(hdr))) {
				return -EFAULT;
			}
			if (get_unaligned_le32(hdr + 56) != 0x644d5241)
				return -ENOEXEC;
			text_offset = get_unaligned_le64(hdr + 8);
			image_size = get_unaligned_le64(hdr + 16);
			flags = get_unaligned_le64(hdr + 24);
			if (image->start < text_offset ||
			    ((image->start - text_offset) & (SZ_2M - 1)) ||
			    image_size < seg->bufsz || image_size > seg->memsz ||
			    (flags & 1) || ((flags >> 1) & 3) != 1)
				return -ENOEXEC;
			have_image = true;
		}
		if (image->dtb >= seg->mem && image->dtb < end) {
			u8 hdr[40];
			u32 total;
			unsigned long offset = image->dtb - seg->mem;

			if (end - image->dtb < sizeof(hdr))
				return -EINVAL;
			if (staged) {
				if (kimage_read_staged(image, image->dtb, hdr,
						      sizeof(hdr)))
					return -EFAULT;
			} else if (copy_from_user(hdr,
						  (u8 __user *)seg->buf + offset,
						  sizeof(hdr))) {
				return -EFAULT;
			}
			total = get_unaligned_be32(hdr + 4);
			if (get_unaligned_be32(hdr) != 0xd00dfeed ||
			    total < sizeof(hdr) || total > SZ_2M ||
			    total > end - image->dtb)
				return -EINVAL;
			have_dtb = true;
		}
	}
	return have_image && have_dtb ? 0 : -EINVAL;
}

int quest_validate_image(struct kimage *image)
{
	return quest_validate_image_data(image, false);
}

int quest_validate_staged_image(struct kimage *image)
{
	return quest_validate_image_data(image, true);
}
