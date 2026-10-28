// SPDX-License-Identifier: GPL-2.0-only
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libfdt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "quest_el1_uapi.h"

#define PAGE_SIZE_U64 4096ULL
#define IMAGE_BASE_ALIGN (2ULL * 1024 * 1024)
#define ARM64_IMAGE_MAGIC 0x644d5241U
#define MAX_REGIONS 64
#define MAX_DTB_SIZE (2U * 1024 * 1024)
#define PHYS_LIMIT (1ULL << 39)

struct region {
	uint64_t start;
	uint64_t size;
};

struct options {
	const char *image_path;
	const char *dtb_path;
	const char *initrd_path;
	const char *device;
	const char *bootargs;
	uint64_t kernel_base;
	uint64_t dtb_addr;
	uint64_t initrd_addr;
	bool have_kernel_base;
	bool have_dtb_addr;
	bool have_initrd_addr;
	struct region ram[MAX_REGIONS];
	struct region reserved[MAX_REGIONS];
	size_t nr_ram;
	size_t nr_reserved;
};

struct segment_buf {
	void *data;
	size_t file_size;
	uint64_t destination;
	uint64_t memory_size;
};

struct plan {
	struct segment_buf segments[3];
	size_t nr_segments;
	uint64_t entry;
	uint64_t dtb_addr;
	uint64_t text_offset;
	uint64_t image_size;
	uint64_t flags;
	uint64_t dtb_size;
	uint64_t initrd_size;
};

static void usage(FILE *stream)
{
	fputs("Usage:\n"
	      "  quest-el1 plan --image Image --dtb board.dtb --kernel-base ADDR\n"
	      "      --dtb-addr ADDR --ram START:SIZE [--ram START:SIZE ...]\n"
	      "      --reserve START:SIZE [--reserve START:SIZE ...]\n"
	      "      [--initrd FILE --initrd-addr ADDR] [--bootargs TEXT]\n"
	      "  quest-el1 load [same planning options]   Load only; never executes\n"
	      "  quest-el1 status [--device PATH]\n"
	      "  quest-el1 unload [--device PATH]\n"
	      "  quest-el1 exec --yes [--device PATH]    Irreversible kernel handoff\n"
	      "\n"
	      "Addresses and sizes accept decimal or 0x-prefixed integers. RAM and\n"
	      "reserve ranges describe the target device, not the build host.\n",
	      stream);
}

static int parse_u64(const char *text, uint64_t *out)
{
	char *end;
	unsigned long long value;

	if (!text || !*text || *text == '-')
		return -EINVAL;
	errno = 0;
	value = strtoull(text, &end, 0);
	if (errno || *end)
		return -EINVAL;
	*out = (uint64_t)value;
	return 0;
}

static int parse_region(const char *text, struct region *out)
{
	char *copy = strdup(text), *colon;
	int ret;

	if (!copy)
		return -ENOMEM;
	colon = strchr(copy, ':');
	if (!colon || strchr(colon + 1, ':')) {
		free(copy);
		return -EINVAL;
	}
	*colon++ = '\0';
	ret = parse_u64(copy, &out->start);
	if (!ret)
		ret = parse_u64(colon, &out->size);
	free(copy);
	if (ret)
		return ret;
	if (!out->size || out->start > UINT64_MAX - out->size)
		return -EINVAL;
	return 0;
}

static int add_region(struct region *regions, size_t *count,
		      const char *argument)
{
	if (*count == MAX_REGIONS)
		return -E2BIG;
	if (parse_region(argument, &regions[*count]))
		return -EINVAL;
	(*count)++;
	return 0;
}

static int read_file(const char *path, size_t limit, void **data, size_t *size)
{
	struct stat st;
	size_t done = 0;
	int fd;
	void *buffer;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st)) {
		int error = -errno;
		close(fd);
		return error;
	}
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (uint64_t)st.st_size > SIZE_MAX ||
	    (limit && (uint64_t)st.st_size > limit)) {
		close(fd);
		return -EINVAL;
	}
	buffer = malloc((size_t)st.st_size);
	if (!buffer) {
		close(fd);
		return -ENOMEM;
	}
	while (done < (size_t)st.st_size) {
		ssize_t n = read(fd, (char *)buffer + done,
				(size_t)st.st_size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			int error = n < 0 ? -errno : -EIO;
			free(buffer);
			close(fd);
			return error;
		}
		done += (size_t)n;
	}
	close(fd);
	*data = buffer;
	*size = done;
	return 0;
}

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const unsigned char *p)
{
	return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
	if (value > UINT64_MAX - (alignment - 1))
		return 0;
	return (value + alignment - 1) & ~(alignment - 1);
}

static bool ranges_overlap(uint64_t a, uint64_t asize,
			   uint64_t b, uint64_t bsize)
{
	return a < b + bsize && b < a + asize;
}

static bool range_in_set(const struct region *regions, size_t count,
			 uint64_t start, uint64_t size)
{
	size_t i;
	if (!size || start > UINT64_MAX - size)
		return false;
	for (i = 0; i < count; i++)
		if (start >= regions[i].start &&
		    start + size <= regions[i].start + regions[i].size)
			return true;
	return false;
}

static bool range_overlaps_set(const struct region *regions, size_t count,
			       uint64_t start, uint64_t size)
{
	size_t i;
	for (i = 0; i < count; i++)
		if (ranges_overlap(start, size, regions[i].start,
				   regions[i].size))
			return true;
	return false;
}

static int read_cells(const fdt32_t *cells, int count, uint64_t *value)
{
	uint64_t result = 0;
	int i;
	if (count < 1 || count > 2)
		return -EOPNOTSUPP;
	for (i = 0; i < count; i++)
		result = (result << 32) | fdt32_to_cpu(cells[i]);
	*value = result;
	return 0;
}

static int append_fdt_reg(const void *fdt, int node, const char *property,
			  int addr_cells, int size_cells,
			  struct region *regions, size_t *count)
{
	const fdt32_t *reg;
	int len, stride, offset;

	reg = fdt_getprop(fdt, node, property, &len);
	if (!reg)
		return len == -FDT_ERR_NOTFOUND ? 0 : -EINVAL;
	stride = (addr_cells + size_cells) * (int)sizeof(fdt32_t);
	if (stride <= 0 || len <= 0 || len % stride)
		return -EINVAL;
	for (offset = 0; offset < len; offset += stride) {
		struct region region;
		if (*count == MAX_REGIONS ||
		    read_cells(reg + offset / 4, addr_cells, &region.start) ||
		    read_cells(reg + offset / 4 + addr_cells, size_cells,
			       &region.size) ||
		    !region.size || region.start > UINT64_MAX - region.size)
			return -EINVAL;
		regions[(*count)++] = region;
	}
	return 0;
}

static int collect_fdt_memory(const void *fdt, struct region *memory,
			      size_t *nr_memory, struct region *reserved,
			      size_t *nr_reserved)
{
	int node = -1, depth = 0, ret, addr_cells, size_cells;
	int reserve_node;
	int i;

	addr_cells = fdt_address_cells(fdt, 0);
	size_cells = fdt_size_cells(fdt, 0);
	if (addr_cells < 1 || size_cells < 1)
		return -EINVAL;
	while ((node = fdt_next_node(fdt, node, &depth)) >= 0) {
		const char *type;
		const char *memory_property;
		int len;
		type = fdt_getprop(fdt, node, "device_type", &len);
		if (!type || len != sizeof("memory") ||
		    memcmp(type, "memory", sizeof("memory")))
			continue;
		memory_property = fdt_getprop(fdt, node, "linux,usable-memory",
					      NULL) ? "linux,usable-memory" : "reg";
		ret = append_fdt_reg(fdt, node, memory_property,
					     addr_cells, size_cells,
					     memory, nr_memory);
		if (ret)
			return ret;
	}
	if (*nr_memory == 0)
		return -EINVAL;
	{
		int chosen = fdt_path_offset(fdt, "/chosen");
		if (chosen >= 0) {
			if (fdt_getprop(fdt, chosen, "linux,usable-memory-range", NULL) ||
			    fdt_getprop(fdt, chosen, "linux,elfcorehdr", NULL))
				return -EOPNOTSUPP;
		} else if (chosen != -FDT_ERR_NOTFOUND) {
			return -EINVAL;
		}
	}

	for (i = 0; i < fdt_num_mem_rsv(fdt); i++) {
		uint64_t address, size;
		if (fdt_get_mem_rsv(fdt, i, &address, &size) || !size ||
		    address > UINT64_MAX - size || *nr_reserved == MAX_REGIONS)
			return -EINVAL;
		reserved[(*nr_reserved)++] = (struct region){address, size};
	}
	reserve_node = fdt_path_offset(fdt, "/reserved-memory");
	if (reserve_node >= 0) {
		int child, reserve_addr_cells = fdt_address_cells(fdt, reserve_node);
		int reserve_size_cells = fdt_size_cells(fdt, reserve_node);
		int ranges_length;
		const void *ranges = fdt_getprop(fdt, reserve_node, "ranges",
						&ranges_length);
		if (reserve_addr_cells < 1 || reserve_size_cells < 1)
			return -EINVAL;
		if (!ranges || ranges_length != 0)
			return -EOPNOTSUPP;
		fdt_for_each_subnode(child, fdt, reserve_node) {
			const void *size_prop;
			int len;
			ret = append_fdt_reg(fdt, child, "reg", reserve_addr_cells,
					     reserve_size_cells, reserved,
					     nr_reserved);
			if (ret)
				return ret;
			size_prop = fdt_getprop(fdt, child, "size", &len);
			if (!fdt_getprop(fdt, child, "reg", NULL) && size_prop)
				return -EOPNOTSUPP;
		}
		if (child != -FDT_ERR_NOTFOUND)
			return -EINVAL;
	} else if (reserve_node != -FDT_ERR_NOTFOUND) {
		return -EINVAL;
	}
	return 0;
}

static int set_address_property(void *fdt, int chosen, const char *name,
				uint64_t value, int cells)
{
	fdt32_t encoded[2];
	int len;
	if (cells < 1 || cells > 2 || (cells == 1 && value > UINT32_MAX))
		return -ERANGE;
	if (cells == 1) {
		encoded[0] = cpu_to_fdt32((uint32_t)value);
		len = sizeof(encoded[0]);
	} else {
		encoded[0] = cpu_to_fdt32((uint32_t)(value >> 32));
		encoded[1] = cpu_to_fdt32((uint32_t)value);
		len = sizeof(encoded);
	}
	return fdt_setprop(fdt, chosen, name, encoded, len);
}

static int delete_optional_prop(void *fdt, int node, const char *name)
{
	int ret = fdt_delprop(fdt, node, name);
	return ret == -FDT_ERR_NOTFOUND ? 0 : ret;
}

static int rewrite_dtb(const struct options *opt, const void *source,
		       size_t source_size, void **dtb_data, size_t *dtb_size,
		       uint64_t initrd_size)
{
	void *working = NULL;
	size_t capacity;
	int chosen, root_addr_cells, ret;
	uint64_t initrd_end;

	if (source_size < sizeof(struct fdt_header) || fdt_check_header(source) ||
	    fdt_totalsize(source) > source_size ||
	    fdt_check_full(source, source_size))
		return -EINVAL;
	capacity = source_size + 16384;
	working = malloc(capacity);
	if (!working)
		return -ENOMEM;
	ret = fdt_open_into(source, working, (int)capacity);
	if (ret)
		goto fail;
	chosen = fdt_path_offset(working, "/chosen");
	if (chosen < 0) {
		chosen = fdt_add_subnode(working, 0, "chosen");
		if (chosen < 0) {
			ret = chosen;
			goto fail;
		}
	}
	if (opt->bootargs &&
	    fdt_setprop_string(working, chosen, "bootargs", opt->bootargs)) {
		ret = -FDT_ERR_NOSPACE;
		goto fail;
	}
	root_addr_cells = fdt_address_cells(working, 0);
	if (root_addr_cells < 1 || root_addr_cells > 2) {
		ret = -EINVAL;
		goto fail;
	}
	if (initrd_size) {
		if (opt->initrd_addr > UINT64_MAX - initrd_size) {
			ret = -ERANGE;
			goto fail;
		}
		initrd_end = opt->initrd_addr + initrd_size;
		ret = set_address_property(working, chosen, "linux,initrd-start",
					   opt->initrd_addr, root_addr_cells);
		if (ret < 0)
			goto fail;
		ret = set_address_property(working, chosen, "linux,initrd-end",
					   initrd_end, root_addr_cells);
		if (ret < 0)
			goto fail;
		ret = set_address_property(working, chosen, "initrd-start",
					   opt->initrd_addr, root_addr_cells);
		if (ret < 0)
			goto fail;
		ret = set_address_property(working, chosen, "initrd-end",
					   initrd_end, root_addr_cells);
		if (ret < 0)
			goto fail;
	} else {
		const char *props[] = {"linux,initrd-start", "linux,initrd-end",
				       "initrd-start", "initrd-end"};
		size_t i;
		for (i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
			ret = delete_optional_prop(working, chosen, props[i]);
			if (ret < 0)
				goto fail;
		}
	}
	ret = fdt_pack(working);
	if (ret)
		goto fail;
	*dtb_size = fdt_totalsize(working);
	if (*dtb_size > MAX_DTB_SIZE) {
		ret = -E2BIG;
		goto fail;
	}
	*dtb_data = working;
	return 0;
fail:
	free(working);
	return ret < 0 ? ret : -EINVAL;
}

static void free_plan(struct plan *plan)
{
	size_t i;
	for (i = 0; i < plan->nr_segments; i++)
		free(plan->segments[i].data);
	memset(plan, 0, sizeof(*plan));
}

static int check_destination(const struct options *opt,
			     const struct region *fdt_memory,
			     size_t nr_fdt_memory,
			     const struct region *fdt_reserved,
			     size_t nr_fdt_reserved,
			     uint64_t start, uint64_t size,
			     const char *what)
{
	if (start >= PHYS_LIMIT || size > PHYS_LIMIT - start) {
		fprintf(stderr, "%s exceeds the EL1 module's <512 GiB limit\n", what);
		return -ERANGE;
	}
	if (!range_in_set(opt->ram, opt->nr_ram, start, size) ||
	    !range_in_set(fdt_memory, nr_fdt_memory, start, size)) {
		fprintf(stderr, "%s is not fully inside both supplied RAM and DT RAM\n",
			what);
		return -ERANGE;
	}
	if (range_overlaps_set(opt->reserved, opt->nr_reserved, start, size) ||
	    range_overlaps_set(fdt_reserved, nr_fdt_reserved, start, size)) {
		fprintf(stderr, "%s overlaps a supplied or DT reserved range\n", what);
		return -EBUSY;
	}
	return 0;
}

static int build_plan(const struct options *opt, struct plan *plan)
{
	struct region fdt_memory[MAX_REGIONS], fdt_reserved[MAX_REGIONS];
	size_t nr_fdt_memory = 0, nr_fdt_reserved = 0;
	unsigned char *image = NULL;
	void *input_dtb = NULL, *dtb = NULL, *initrd = NULL;
	size_t image_file_size, input_dtb_size, dtb_size = 0, initrd_file_size = 0;
	uint64_t image_phys, image_memsz, initrd_memsz = 0, total_memsz;
	int ret;

	memset(plan, 0, sizeof(*plan));
	if (!opt->image_path || !opt->dtb_path || !opt->have_kernel_base ||
	    !opt->have_dtb_addr || opt->nr_ram == 0 || opt->nr_reserved == 0 ||
	    (opt->initrd_path && !opt->have_initrd_addr) ||
	    (!opt->initrd_path && opt->have_initrd_addr)) {
		fprintf(stderr, "image, dtb, target addresses, --ram, and --reserve are required\n");
		return -EINVAL;
	}
	if ((opt->kernel_base & (IMAGE_BASE_ALIGN - 1)) ||
	    (opt->dtb_addr & (PAGE_SIZE_U64 - 1)) ||
	    (opt->have_initrd_addr &&
	     (opt->initrd_addr & (PAGE_SIZE_U64 - 1)))) {
		fprintf(stderr, "kernel base must be 2 MiB aligned; DTB/initrd addresses page aligned\n");
		return -EINVAL;
	}
	ret = read_file(opt->image_path, 0, (void **)&image, &image_file_size);
	if (ret)
		goto out;
	if (image_file_size < 64 || get_le32(image + 56) != ARM64_IMAGE_MAGIC) {
		fprintf(stderr, "not a raw ARM64 Image (missing ARM\\x64 header magic)\n");
		ret = -EINVAL;
		goto out;
	}
	plan->text_offset = get_le64(image + 8);
	plan->image_size = get_le64(image + 16);
	plan->flags = get_le64(image + 24);
	if (!plan->image_size || plan->image_size < image_file_size ||
	    plan->text_offset >= IMAGE_BASE_ALIGN ||
	    (plan->flags & ~0xFULL) || (plan->flags & 1) ||
	    ((plan->flags >> 1) & 3) != 1) {
		fprintf(stderr, "unsupported Image size, offset, endianness, page-size, or reserved flags\n");
		ret = -EINVAL;
		goto out;
	}
	if (opt->kernel_base > UINT64_MAX - plan->text_offset) {
		ret = -ERANGE;
		goto out;
	}
	image_phys = opt->kernel_base + plan->text_offset;
	if (image_phys & (PAGE_SIZE_U64 - 1)) {
		fprintf(stderr, "Image text_offset does not produce a 4 KiB aligned load address\n");
		ret = -EINVAL;
		goto out;
	}
	image_memsz = align_up(plan->image_size, PAGE_SIZE_U64);
	if (!image_memsz || image_file_size > image_memsz) {
		ret = -ERANGE;
		goto out;
	}
	plan->entry = image_phys;
	ret = read_file(opt->dtb_path, MAX_DTB_SIZE, &input_dtb,
			&input_dtb_size);
	if (ret)
		goto out;
	if (input_dtb_size < sizeof(struct fdt_header) ||
	    fdt_check_header(input_dtb) || fdt_totalsize(input_dtb) > input_dtb_size ||
	    fdt_check_full(input_dtb, input_dtb_size)) {
		fprintf(stderr, "invalid or truncated DTB\n");
		ret = -EINVAL;
		goto out;
	}
	ret = collect_fdt_memory(input_dtb, fdt_memory, &nr_fdt_memory,
				 fdt_reserved, &nr_fdt_reserved);
	if (ret) {
		fprintf(stderr, "DTB RAM/reserved-memory is malformed or unsupported\n");
		goto out;
	}
	if (opt->initrd_path) {
		ret = read_file(opt->initrd_path, 0, &initrd, &initrd_file_size);
		if (ret)
			goto out;
		initrd_memsz = align_up(initrd_file_size, PAGE_SIZE_U64);
		if (!initrd_memsz) {
			ret = -ERANGE;
			goto out;
		}
	}
	ret = rewrite_dtb(opt, input_dtb, input_dtb_size, &dtb, &dtb_size,
			  initrd_file_size);
	if (ret)
		goto out;
	if (!range_in_set(opt->ram, opt->nr_ram, opt->kernel_base,
			  plan->text_offset + image_memsz) ||
	    !range_in_set(fdt_memory, nr_fdt_memory, opt->kernel_base,
			  plan->text_offset + image_memsz)) {
		fprintf(stderr, "ARM64 Image boot-protocol base/text_offset/image_size leaves RAM\n");
		ret = -ERANGE;
		goto out;
	}
	ret = check_destination(opt, fdt_memory, nr_fdt_memory,
				fdt_reserved, nr_fdt_reserved, image_phys,
				image_memsz, "kernel Image");
	if (ret)
		goto out;
	ret = check_destination(opt, fdt_memory, nr_fdt_memory,
				fdt_reserved, nr_fdt_reserved, opt->dtb_addr,
				align_up(dtb_size, PAGE_SIZE_U64), "DTB");
	if (ret)
		goto out;
	if (initrd_file_size) {
		ret = check_destination(opt, fdt_memory, nr_fdt_memory,
					fdt_reserved, nr_fdt_reserved,
					opt->initrd_addr, initrd_memsz, "initrd");
		if (ret)
			goto out;
		{
			const uint64_t one_gib = 1ULL << 30;
			const uint64_t max_window = 32ULL << 30;
			uint64_t window_start = (image_phys < opt->initrd_addr ?
					 image_phys : opt->initrd_addr) &
					~(one_gib - 1);
			uint64_t image_end = image_phys + image_memsz;
			uint64_t initrd_end = opt->initrd_addr + initrd_file_size;
			uint64_t range_end = image_end > initrd_end ?
					     image_end : initrd_end;
			uint64_t window_end = align_up(range_end, one_gib);
			if (!window_end || window_end < window_start ||
			    window_end - window_start > max_window) {
				fprintf(stderr, "Image and initrd do not fit one <=32 GiB window aligned to 1 GiB\n");
				ret = -ERANGE;
				goto out;
			}
		}
	}
	{
		uint64_t dtb_block = opt->dtb_addr & ~(IMAGE_BASE_ALIGN - 1);
		uint64_t dtb_block_end = align_up(opt->dtb_addr + dtb_size,
						IMAGE_BASE_ALIGN);
		uint64_t dtb_mapping_size = dtb_block_end - dtb_block;
		if (range_overlaps_set(opt->reserved, opt->nr_reserved,
				       dtb_block, dtb_mapping_size) ||
		    range_overlaps_set(fdt_reserved, nr_fdt_reserved,
				       dtb_block, dtb_mapping_size)) {
			fprintf(stderr, "DTB's 2 MiB mapping block overlaps reserved memory\n");
			ret = -EBUSY;
			goto out;
		}
	}
	if ((plan->flags & 8) == 0) {
		uint64_t first_dram = UINT64_MAX;
		size_t i;
		for (i = 0; i < nr_fdt_memory; i++)
			if (fdt_memory[i].start < first_dram)
				first_dram = fdt_memory[i].start;
		if (opt->kernel_base != align_up(first_dram, IMAGE_BASE_ALIGN)) {
			fprintf(stderr, "Image flags require placement near DRAM; kernel base is not the first 2 MiB-aligned DT RAM address\n");
			ret = -EINVAL;
			goto out;
		}
	}
	if (ranges_overlap(image_phys, image_memsz, opt->dtb_addr,
			   align_up(dtb_size, PAGE_SIZE_U64)) ||
	    (initrd_memsz && ranges_overlap(image_phys, image_memsz,
					    opt->initrd_addr, initrd_memsz)) ||
	    (initrd_memsz && ranges_overlap(opt->dtb_addr,
					    align_up(dtb_size, PAGE_SIZE_U64),
					    opt->initrd_addr, initrd_memsz))) {
		fprintf(stderr, "kernel, DTB, and initrd destination ranges overlap\n");
		ret = -EINVAL;
		goto out;
	}
	total_memsz = image_memsz + align_up(dtb_size, PAGE_SIZE_U64) + initrd_memsz;
	if (total_memsz < image_memsz || total_memsz > UINT64_MAX / 2) {
		ret = -ERANGE;
		goto out;
	}
	plan->segments[0] = (struct segment_buf){image, image_file_size,
						 image_phys, image_memsz};
	image = NULL;
	plan->segments[1] = (struct segment_buf){dtb, dtb_size, opt->dtb_addr,
						 align_up(dtb_size, PAGE_SIZE_U64)};
	dtb = NULL;
	plan->nr_segments = 2;
	plan->dtb_addr = opt->dtb_addr;
	plan->dtb_size = dtb_size;
	plan->initrd_size = initrd_file_size;
	if (initrd_file_size) {
		plan->segments[2] = (struct segment_buf){initrd,
							 initrd_file_size,
							 opt->initrd_addr,
							 initrd_memsz};
		initrd = NULL;
		plan->nr_segments++;
	}
	ret = 0;
out:
	free(image);
	free(input_dtb);
	free(dtb);
	free(initrd);
	if (ret)
		free_plan(plan);
	return ret;
}

static void print_plan(const struct options *opt, const struct plan *plan)
{
	size_t i;
	printf("ARM64 Image: text_offset=0x%" PRIx64 " image_size=0x%" PRIx64
	       " flags=0x%" PRIx64 "\n", plan->text_offset,
	       plan->image_size, plan->flags);
	printf("Entry/Image: 0x%" PRIx64 " (2 MiB base 0x%" PRIx64 ")\n",
	       plan->entry, opt->kernel_base);
	printf("DTB: 0x%" PRIx64 " (%" PRIu64 " bytes after update)\n",
	       plan->dtb_addr, plan->dtb_size);
	if (plan->initrd_size)
		printf("initrd: 0x%" PRIx64 " (%" PRIu64 " bytes)\n",
		       opt->initrd_addr, plan->initrd_size);
	for (i = 0; i < plan->nr_segments; i++)
		printf("segment[%zu]: destination=0x%" PRIx64
		       " memory=0x%" PRIx64 " source=%zu bytes\n", i,
		       plan->segments[i].destination,
		       plan->segments[i].memory_size,
		       plan->segments[i].file_size);
	fputs("Planner checks RAM and reserved ranges supplied by the operator and "
	      "the DTB. It cannot identify the correct device DTB, quiesce DMA, or establish hardware "
	      "compatibility. Do not load until those are verified.\n", stdout);
}

static int parse_options(int argc, char **argv, int start,
			 struct options *opt)
{
	int i;
	memset(opt, 0, sizeof(*opt));
	opt->device = QUEST_EL1_DEVICE;
	for (i = start; i < argc; i++) {
		const char *arg = argv[i], *value;
		uint64_t *number = NULL;
		bool *present = NULL;
		if (!strcmp(arg, "--image") || !strcmp(arg, "--dtb") ||
		    !strcmp(arg, "--initrd") || !strcmp(arg, "--device") ||
		    !strcmp(arg, "--bootargs") || !strcmp(arg, "--kernel-base") ||
		    !strcmp(arg, "--dtb-addr") || !strcmp(arg, "--initrd-addr") ||
		    !strcmp(arg, "--ram") || !strcmp(arg, "--reserve")) {
			if (++i == argc)
				return -EINVAL;
			value = argv[i];
		} else {
			fprintf(stderr, "unknown option: %s\n", arg);
			return -EINVAL;
		}
		if (!strcmp(arg, "--image"))
			opt->image_path = value;
		else if (!strcmp(arg, "--dtb"))
			opt->dtb_path = value;
		else if (!strcmp(arg, "--initrd"))
			opt->initrd_path = value;
		else if (!strcmp(arg, "--device"))
			opt->device = value;
		else if (!strcmp(arg, "--bootargs"))
			opt->bootargs = value;
		else if (!strcmp(arg, "--kernel-base")) {
			number = &opt->kernel_base;
			present = &opt->have_kernel_base;
		} else if (!strcmp(arg, "--dtb-addr")) {
			number = &opt->dtb_addr;
			present = &opt->have_dtb_addr;
		} else if (!strcmp(arg, "--initrd-addr")) {
			number = &opt->initrd_addr;
			present = &opt->have_initrd_addr;
		} else if (!strcmp(arg, "--ram")) {
			if (add_region(opt->ram, &opt->nr_ram, value))
				return -EINVAL;
		} else if (!strcmp(arg, "--reserve")) {
			if (add_region(opt->reserved, &opt->nr_reserved, value))
				return -EINVAL;
		}
		if (number) {
			if (*present || parse_u64(value, number))
				return -EINVAL;
			*present = true;
		}
	}
	return 0;
}

static int run_ioctl(const char *device, unsigned long request, void *arg)
{
	int fd = open(device, O_RDWR | O_CLOEXEC);
	int ret;
	if (fd < 0)
		return -errno;
	ret = ioctl(fd, request, arg);
	if (ret < 0)
		ret = -errno;
	close(fd);
	return ret;
}

static struct quest_el1_header make_header(void)
{
	return (struct quest_el1_header){QUEST_EL1_MAGIC,
					QUEST_EL1_ABI_VERSION};
}

static int parse_device_option(int argc, char **argv, const char **device)
{
	*device = QUEST_EL1_DEVICE;
	if (argc == 2)
		return 0;
	if (argc == 4 && !strcmp(argv[2], "--device")) {
		*device = argv[3];
		return 0;
	}
	return -EINVAL;
}

#ifndef QUEST_EL1_TEST
int main(int argc, char **argv)
{
	struct options opt;
	struct plan plan;
	const char *command;
	int ret;
	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	command = argv[1];
	if (!strcmp(command, "-h") || !strcmp(command, "--help")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(command, "status") || !strcmp(command, "unload")) {
		struct quest_el1_header hdr = make_header();
		if (parse_device_option(argc, argv, &opt.device))
			goto usage_error;
		if (!strcmp(command, "status")) {
			struct quest_el1_status status = {0};
			status.hdr = hdr;
			ret = run_ioctl(opt.device, QUEST_EL1_IOC_STATUS, &status);
			if (!ret)
				printf("loaded=%u EL=%u stock-va-bits=%u page-shift=%u "
				       "entry=0x%" PRIx64 " dtb=0x%" PRIx64
				       " segments=%" PRIu64 "\n", status.loaded,
				       status.current_el, status.stock_va_bits,
				       status.page_shift, (uint64_t)status.entry,
				       (uint64_t)status.dtb,
				       (uint64_t)status.nr_segments);
		} else {
			ret = run_ioctl(opt.device, QUEST_EL1_IOC_UNLOAD, &hdr);
		}
		if (ret) {
			errno = -ret;
			perror(command);
			return 1;
		}
		return 0;
	}
	if (!strcmp(command, "exec")) {
		struct quest_el1_header hdr = make_header();
		int i;
		bool confirmed = false;
		const char *device = QUEST_EL1_DEVICE;
		for (i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "--yes") && !confirmed) {
				confirmed = true;
			} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
				device = argv[++i];
			} else {
				goto usage_error;
			}
		}
		if (!confirmed)
			goto usage_error;
		ret = run_ioctl(device, QUEST_EL1_IOC_EXEC, &hdr);
		if (ret) {
			errno = -ret;
			perror("exec");
			return 1;
		}
		fprintf(stderr, "kernel handoff returned unexpectedly\n");
		return 1;
	}
	if (!strcmp(command, "plan") || !strcmp(command, "load")) {
		if (parse_options(argc, argv, 2, &opt))
			goto usage_error;
		ret = build_plan(&opt, &plan);
		if (ret) {
			errno = -ret;
			perror(command);
			return 1;
		}
		print_plan(&opt, &plan);
		if (!strcmp(command, "load")) {
			struct quest_el1_segment segments[3];
			struct quest_el1_load request = {0};
			size_t i;
			for (i = 0; i < plan.nr_segments; i++) {
				segments[i] = (struct quest_el1_segment){
					.buf = (uintptr_t)plan.segments[i].data,
					.bufsz = plan.segments[i].file_size,
					.mem = plan.segments[i].destination,
					.memsz = plan.segments[i].memory_size,
				};
			}
			request.hdr = make_header();
			request.entry = plan.entry;
			request.dtb = plan.dtb_addr;
			request.nr_segments = plan.nr_segments;
			request.flags = QUEST_EL1_ARCH_AARCH64;
			request.segments = (uintptr_t)segments;
			ret = run_ioctl(opt.device, QUEST_EL1_IOC_LOAD, &request);
			if (ret) {
				errno = -ret;
				perror("load");
				free_plan(&plan);
				return 1;
			}
			puts("Image loaded. The running kernel has not been switched.");
		}
		free_plan(&plan);
		return 0;
	}
usage_error:
	usage(stderr);
	return 2;
}
#endif
