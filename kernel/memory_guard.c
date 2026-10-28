// SPDX-License-Identifier: GPL-2.0-only
/* Check destination RAM against the running kernel's firmware reservations. */
#define pr_fmt(fmt) "quest_el1: " fmt

#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <asm/memory.h>
#include <asm/unaligned.h>

#include "kexec.h"
#include "memory_guard.h"
#include "kexec_compat.h"

#if !IS_ENABLED(CONFIG_OF_RESERVED_MEM)
#error "The Quest loader needs the stock OF reserved-memory implementation"
#endif

static rwlock_t *stock_resource_lock;
static const void * const *stock_fdt;
static u64 old_kernel_start, old_kernel_end;

int quest_memory_guard_init(void)
{
	unsigned long text = quest_lookup_name("_text");
	unsigned long end = quest_lookup_name("_end");

	stock_resource_lock = (void *)quest_lookup_name("resource_lock");
	stock_fdt = (void *)quest_lookup_name("initial_boot_params");
	if (!stock_resource_lock || !stock_fdt || !text || end <= text)
		return -ENOENT;
	old_kernel_start = text - kimage_voffset;
	old_kernel_end = end - kimage_voffset;
	return old_kernel_end > old_kernel_start &&
		old_kernel_end < BIT_ULL(39) ? 0 : -EINVAL;
}

/* Resource endpoints are inclusive; do not wrap an endpoint of U64_MAX. */
static int quest_check_reserved_end(struct kimage *image, u64 base, u64 end)
{
	unsigned long i;
	u64 block, block_end;

	if (end < base)
		return -EINVAL;
	for (i = 0; i < image->nr_segments; i++) {
		struct kexec_segment *seg = &image->segment[i];

		if (seg->mem <= end && base < seg->mem + seg->memsz)
			return -EBUSY;
	}
	/* ARM64 maps the early DTB through complete 2 MiB blocks. */
	for (i = 0; i < image->nr_segments; i++) {
		struct kexec_segment *seg = &image->segment[i];

		if (image->dtb < seg->mem ||
		    image->dtb >= seg->mem + seg->bufsz)
			continue;
		block = round_down(image->dtb, SZ_2M);
		block_end = round_up(seg->mem + seg->bufsz, SZ_2M);
		if (block <= end && base < block_end)
			return -EBUSY;
	}
	return 0;
}

static int quest_check_reserved(struct kimage *image, u64 base, u64 size)
{
	if (!size || base > U64_MAX - (size - 1))
		return -EINVAL;
	return quest_check_reserved_end(image, base, base + size - 1);
}

/* The flat boot reserve map is not represented by /reserved-memory nodes. */
static int quest_check_boot_reservations(struct kimage *image)
{
	const u8 *fdt = READ_ONCE(*stock_fdt);
	u32 total, offset;

	if (!fdt || get_unaligned_be32(fdt) != 0xd00dfeed)
		return -EINVAL;
	total = get_unaligned_be32(fdt + 4);
	offset = get_unaligned_be32(fdt + 16);
	if (total < 40 || total > SZ_2M || offset < 40 || (offset & 7))
		return -EINVAL;
	while (offset <= total && total - offset >= 16) {
		u64 base = get_unaligned_be64(fdt + offset);
		u64 size = get_unaligned_be64(fdt + offset + 8);
		int ret;

		if (!size)
			return base ? -EINVAL : 0;
		ret = quest_check_reserved(image, base, size);
		if (ret) {
			pr_err("Destination conflicts with running FDT reserve map\n");
			return ret;
		}
		offset += 16;
	}
	return -EINVAL;
}

/* Called with resource_lock held. Ordinary allocated pages have no resource
 * reservation and remain legal destinations. The old kernel is legal too;
 * its init sections may still appear as generic 'reserved' resources.
 * Other non-RAM resources must survive the handoff, including reservations
 * nested underneath a System RAM parent (page_is_ram alone misses these).
 */
static int quest_check_resource_tree(struct kimage *image)
{
	struct resource *res;
	int ret = 0;

	read_lock(stock_resource_lock);
	res = iomem_resource.child;
	while (res) {
		if ((res->flags & IORESOURCE_SYSTEM_RAM) != IORESOURCE_SYSTEM_RAM ||
		    (res->flags & IORESOURCE_SYSRAM_DRIVER_MANAGED)) {
			u64 start = res->start, end = res->end;

			if (end < start) {
				ret = -EINVAL;
				break;
			}
			/* Only setup.c's generic memblock reservation shape may
			 * contain reclaimable old kernel init sections. Never give
			 * MMIO, driver-managed RAM or named firmware this exemption.
			 * DT reserves are independently checked without exemptions.
			 */
			if (res->flags == (IORESOURCE_MEM | IORESOURCE_BUSY) &&
			    res->desc == IORES_DESC_NONE && res->name &&
			    !strcmp(res->name, "reserved") &&
			    res->parent &&
			    (res->parent->flags & IORESOURCE_SYSTEM_RAM) ==
				IORESOURCE_SYSTEM_RAM) {
				if (start < old_kernel_start)
					ret = quest_check_reserved_end(image, start,
						min(end, old_kernel_start - 1));
				if (!ret && end >= old_kernel_end)
					ret = quest_check_reserved_end(image,
						max(start, old_kernel_end), end);
			} else {
				ret = quest_check_reserved_end(image, start, end);
			}
			if (ret) {
				pr_err("Destination conflicts with physical resource %s\n",
				       res->name ? res->name : "(unnamed)");
				break;
			}
		}
		if (res->child) {
			res = res->child;
			continue;
		}
		while (res != &iomem_resource && !res->sibling)
			res = res->parent;
		res = res == &iomem_resource ? NULL : res->sibling;
	}
	read_unlock(stock_resource_lock);
	return ret;
}

int quest_validate_destinations(struct kimage *image)
{
	struct device_node *root, *child;
	unsigned long i;
	int ret;

	for (i = 0; i < image->nr_segments; i++) {
		unsigned long pfn = image->segment[i].mem >> PAGE_SHIFT;
		unsigned long pages = image->segment[i].memsz >> PAGE_SHIFT;
		unsigned long page;

		for (page = 0; page < pages; page++)
			if (!page_is_ram(pfn + page))
				return -EADDRNOTAVAIL;
	}
	ret = quest_check_boot_reservations(image);
	if (!ret)
		ret = quest_check_resource_tree(image);
	if (ret)
		return ret;

	root = of_find_node_by_path("/reserved-memory");
	if (!root) {
		pr_err("The running DT has no reserved-memory inventory\n");
		return -ENODEV;
	}
	/*
	 * Use public OF interfaces, including the resolved allocation of dynamic
	 * 'size' reservations. Ordinary allocated pages and the old kernel are
	 * deliberately allowed: the relocation core overwrites them only after
	 * CPU/device shutdown. Firmware reservations must survive the handoff.
	 */
	for_each_available_child_of_node(root, child) {
		const __be32 *reg;
		struct reserved_mem *rmem;
		int len, cells, ac, sc, offset;

		reg = of_get_property(child, "reg", &len);
		if (reg) {
			ac = of_n_addr_cells(child);
			sc = of_n_size_cells(child);
			if (ac < 1 || ac > 2 || sc < 1 || sc > 2) {
				ret = -EINVAL;
				goto out_child;
			}
			cells = ac + sc;
			if (len <= 0 || len % (cells * sizeof(*reg))) {
				ret = -EINVAL;
				goto out_child;
			}
			for (offset = 0; offset < len / (int)sizeof(*reg); offset += cells) {
				u64 base = of_read_number(reg + offset, ac);
				u64 size = of_read_number(reg + offset + ac, sc);

				/* Zero-sized bootloader placeholders reserve no memory. */
				if (!size)
					continue;
				ret = quest_check_reserved(image, base, size);
				if (ret)
					goto out_child;
			}
		} else if (of_find_property(child, "size", NULL)) {
			rmem = of_reserved_mem_lookup(child);
			if (!rmem || !rmem->size) {
				ret = -EOPNOTSUPP;
				goto out_child;
			}
			ret = quest_check_reserved(image, rmem->base, rmem->size);
			if (ret)
				goto out_child;
		}
	}
	of_node_put(root);
	return 0;

out_child:
	pr_err("Destination or DTB mapping conflicts with live reservation %pOF (%d)\n",
	       child, ret);
	of_node_put(child);
	of_node_put(root);
	return ret;
}
