// SPDX-License-Identifier: GPL-2.0-only
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libfdt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
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
#define MAX_REGIONS 512
#define MAX_RMEM 256
#define MAX_SECURE 4096
#define MAX_NOMAP_NODES 128
#define MAX_DISABLE 32
#define MAX_REPORT_SIZE (4U * 1024 * 1024)
#define MAX_DUMP_SIZE (8U * 1024 * 1024)
#define MAX_PROPERTY_SIZE (1U * 1024 * 1024)
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
	/* Live-state inputs produced on the device. */
	const char *report_path;	/* `cat /dev/quest_el1` output */
	const char *disable[MAX_DISABLE];
	size_t nr_disable;
	struct region nomap[MAX_REGIONS];
	size_t nr_nomap;
};

/* Parsed `cat /dev/quest_el1` report: what the running kernel really did. */
struct rmem_pin {
	char name[128];
	uint64_t base;
	uint64_t size;
};

struct report_info {
	struct rmem_pin rmem[MAX_RMEM];
	size_t nr_rmem;
	struct region secure[MAX_SECURE];
	size_t nr_secure;
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
	      "      [--report FILE] [--nomap START:SIZE ...] [--disable /dt/path ...]\n"
	      "      [--iomem FILE]   (adds every top-level 'System RAM' range as --ram)\n"
	      "  quest-el1 dump-dt OUTPUT.dtb [--root DIR] [--boot-fdt FILE]\n"
	      "      Rebuild the *runtime* tree (/proc/device-tree) as a DTB; copies the\n"
	      "      /memreserve/ map from /sys/firmware/fdt. Never use the boot FDT.\n"
	      "  quest-el1 report [--device PATH]       Read-only preflight report\n"
	      "  quest-el1 rehearse [--steps LIST] [--device PATH]\n"
	      "      LIST: watchdog,rproc,cpus,mmu,copy or all (default watchdog,cpus,mmu);\n"
	      "      every step is undone before the command returns\n"
	      "  quest-el1 status [--device PATH]\n"
	      "  quest-el1 unload [--device PATH]\n"
	      "  quest-el1 exec --yes [--device PATH]    Irreversible kernel handoff\n"
	      "\n"
	      "Addresses and sizes accept decimal or 0x-prefixed integers. RAM and\n"
	      "reserve ranges describe the target device, not the build host.\n"
	      "--report pins every size-only /reserved-memory node at the address the\n"
	      "running kernel chose (ramoops then survives the switch) and turns every\n"
	      "hypervisor-secured range into a no-map node; a truncated report is refused.\n",
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

/* For procfs/sysfs/char devices whose st_size is 0 or not meaningful. */
static int read_stream(const char *path, size_t limit, void **data, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t done = 0, capacity = 0;
	char *buffer = NULL;

	if (fd < 0)
		return -errno;
	for (;;) {
		ssize_t n;

		if (done == capacity) {
			size_t next = capacity ? capacity * 2 : 65536;
			char *grown;

			if (capacity >= limit) {
				free(buffer);
				close(fd);
				return -E2BIG;
			}
			if (next > limit)
				next = limit;
			grown = realloc(buffer, next);
			if (!grown) {
				free(buffer);
				close(fd);
				return -ENOMEM;
			}
			buffer = grown;
			capacity = next;
		}
		n = read(fd, buffer + done, capacity - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			int error = -errno;
			free(buffer);
			close(fd);
			return error;
		}
		if (!n)
			break;
		done += (size_t)n;
	}
	close(fd);
	if (!done) {
		free(buffer);
		return -ENODATA;
	}
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
			if (!fdt_getprop(fdt, child, "reg", NULL) && size_prop) {
				fprintf(stderr, "/reserved-memory/%s is dynamically placed "
					"(size only); pass --report so it is pinned\n",
					fdt_get_name(fdt, child, NULL));
				return -EOPNOTSUPP;
			}
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


static int compare_regions(const void *a, const void *b)
{
	const struct region *left = a, *right = b;
	if (left->start != right->start)
		return left->start < right->start ? -1 : 1;
	return 0;
}

/*
 * Grow every range to `alignment`, sort, and merge overlapping or touching
 * ranges. Early memblock cannot grow its region arrays, so thousands of
 * fragmented secure pages must become a few large no-map nodes.
 */
static int coalesce_regions(const struct region *in, size_t count,
			    uint64_t alignment, struct region *out,
			    size_t max_out, size_t *nr_out)
{
	struct region *sorted;
	size_t i, n = 0;

	*nr_out = 0;
	if (!count)
		return 0;
	sorted = calloc(count, sizeof(*sorted));
	if (!sorted)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		uint64_t start = in[i].start & ~(alignment - 1);
		uint64_t end;
		if (!in[i].size || in[i].start > UINT64_MAX - in[i].size ||
		    in[i].start + in[i].size > UINT64_MAX - alignment) {
			free(sorted);
			return -ERANGE;
		}
		end = align_up(in[i].start + in[i].size, alignment);
		sorted[i] = (struct region){start, end - start};
	}
	qsort(sorted, count, sizeof(*sorted), compare_regions);
	for (i = 0; i < count; i++) {
		if (n && sorted[i].start <= out[n - 1].start + out[n - 1].size) {
			uint64_t end = sorted[i].start + sorted[i].size;
			if (end > out[n - 1].start + out[n - 1].size)
				out[n - 1].size = end - out[n - 1].start;
			continue;
		}
		if (n == max_out) {
			free(sorted);
			return -E2BIG;
		}
		out[n++] = sorted[i];
	}
	free(sorted);
	*nr_out = n;
	return 0;
}

static int parse_report_text(char *text, struct report_info *info)
{
	char *line, *save = NULL;
	bool complete = false;

	memset(info, 0, sizeof(*info));
	for (line = strtok_r(text, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char name[128];
		unsigned long long base, size;

		if (complete)
			return -EINVAL; /* nothing may follow "end" */
		if (!strcmp(line, "end")) {
			complete = true;
		} else if (!strncmp(line, "rmem ", 5)) {
			if (sscanf(line, "rmem %127s %llx %llx", name, &base,
				   &size) != 3 || !size ||
			    base > UINT64_MAX - size)
				return -EINVAL;
			if (info->nr_rmem == MAX_RMEM)
				return -E2BIG;
			snprintf(info->rmem[info->nr_rmem].name,
				 sizeof(info->rmem[0].name), "%s", name);
			info->rmem[info->nr_rmem].base = base;
			info->rmem[info->nr_rmem].size = size;
			info->nr_rmem++;
		} else if (!strncmp(line, "rmem_unavailable", 16)) {
			return -EOPNOTSUPP;
		} else if (!strncmp(line, "secure ", 7)) {
			if (sscanf(line, "secure %llx %llx", &base, &size) != 2 ||
			    !size || base > UINT64_MAX - size)
				return -EINVAL;
			if (info->nr_secure == MAX_SECURE)
				return -E2BIG;
			info->secure[info->nr_secure++] =
				(struct region){base, size};
		} else if (!strncmp(line, "secure_summary", 14)) {
			/* The scan could not list every range: unsafe to use. */
			if (strstr(line, "overflow") || strstr(line, "error"))
				return -E2BIG;
		}
	}
	return complete ? 0 : -EINVAL;
}

static int load_report(const char *path, struct report_info *info)
{
	void *data;
	char *text;
	size_t size;
	int ret = read_stream(path, MAX_REPORT_SIZE, &data, &size);

	if (ret)
		return ret;
	text = malloc(size + 1);
	if (!text) {
		free(data);
		return -ENOMEM;
	}
	memcpy(text, data, size);
	text[size] = '\0';
	free(data);
	ret = parse_report_text(text, info);
	free(text);
	if (ret)
		fprintf(stderr, "%s: incomplete or unsupported quest_el1 report (%s)\n",
			path, strerror(-ret));
	return ret;
}

/* Top-level "START-END : System RAM" lines of /proc/iomem (root only). */
static int parse_iomem_text(const char *text, struct region *ram, size_t *count)
{
	const char *line = text;
	bool any_nonzero = false;

	while (*line) {
		const char *next = strchr(line, '\n');
		size_t len = next ? (size_t)(next - line) : strlen(line);
		unsigned long long start, end;
		char name[64];
		char buffer[160];

		if (len < sizeof(buffer) && line[0] != ' ') {
			memcpy(buffer, line, len);
			buffer[len] = '\0';
			if (sscanf(buffer, "%llx-%llx : %63[^\n]", &start, &end,
				   name) == 3 && !strcmp(name, "System RAM")) {
				if (end < start || *count == MAX_REGIONS)
					return -EINVAL;
				if (end)
					any_nonzero = true;
				ram[(*count)++] = (struct region){start,
								  end - start + 1};
			}
		}
		if (!next)
			break;
		line = next + 1;
	}
	/* Unprivileged readers see zeroed addresses. */
	return any_nonzero ? 0 : -EPERM;
}

static int load_iomem(const char *path, struct options *opt)
{
	void *data;
	char *text;
	size_t size;
	int ret = read_stream(path, MAX_REPORT_SIZE, &data, &size);

	if (ret)
		return ret;
	text = malloc(size + 1);
	if (!text) {
		free(data);
		return -ENOMEM;
	}
	memcpy(text, data, size);
	text[size] = '\0';
	free(data);
	ret = parse_iomem_text(text, opt->ram, &opt->nr_ram);
	free(text);
	return ret;
}

static int set_cells(void *fdt, int node, const char *name, uint64_t start,
		     uint64_t size, int addr_cells, int size_cells)
{
	fdt32_t cells[4];
	int n = 0;

	if (addr_cells < 1 || addr_cells > 2 || size_cells < 1 || size_cells > 2 ||
	    (addr_cells == 1 && start > UINT32_MAX) ||
	    (size_cells == 1 && size > UINT32_MAX))
		return -ERANGE;
	if (addr_cells == 2)
		cells[n++] = cpu_to_fdt32((uint32_t)(start >> 32));
	cells[n++] = cpu_to_fdt32((uint32_t)start);
	if (size_cells == 2)
		cells[n++] = cpu_to_fdt32((uint32_t)(size >> 32));
	cells[n++] = cpu_to_fdt32((uint32_t)size);
	return fdt_setprop(fdt, node, name, cells, n * (int)sizeof(fdt32_t));
}

static const struct rmem_pin *find_pin(const struct report_info *report,
				       const char *node_name)
{
	size_t i, base_len = strcspn(node_name, "@");

	for (i = 0; i < report->nr_rmem; i++)
		if (!strcmp(report->rmem[i].name, node_name))
			return &report->rmem[i];
	for (i = 0; i < report->nr_rmem; i++)
		if (strlen(report->rmem[i].name) == base_len &&
		    !strncmp(report->rmem[i].name, node_name, base_len))
			return &report->rmem[i];
	return NULL;
}

/*
 * Give every size-only (dynamically placed) reserved-memory node the static
 * address the running kernel picked for it. Offsets move after each edit,
 * so restart the scan after every change.
 */
static int pin_dynamic_reservations(void *fdt, const struct report_info *report,
				    unsigned int *pinned)
{
	int parent = fdt_path_offset(fdt, "/reserved-memory");
	int addr_cells, size_cells;
	bool changed = true;

	*pinned = 0;
	if (parent == -FDT_ERR_NOTFOUND)
		return 0;
	if (parent < 0)
		return parent;
	addr_cells = fdt_address_cells(fdt, parent);
	size_cells = fdt_size_cells(fdt, parent);
	while (changed) {
		int child;

		changed = false;
		parent = fdt_path_offset(fdt, "/reserved-memory");
		fdt_for_each_subnode(child, fdt, parent) {
			const struct rmem_pin *pin;
			const char *name = fdt_get_name(fdt, child, NULL);
			int ret;

			if (fdt_getprop(fdt, child, "reg", NULL) ||
			    !fdt_getprop(fdt, child, "size", NULL) || !name)
				continue;
			pin = find_pin(report, name);
			if (!pin)
				continue;
			ret = set_cells(fdt, child, "reg", pin->base, pin->size,
					addr_cells, size_cells);
			if (!ret)
				ret = delete_optional_prop(fdt, child, "size");
			if (!ret)
				ret = delete_optional_prop(fdt, child, "alignment");
			if (!ret)
				ret = delete_optional_prop(fdt, child, "alloc-ranges");
			if (ret)
				return ret;
			(*pinned)++;
			changed = true;
			break;
		}
	}
	return 0;
}

static int add_nomap_nodes(void *fdt, const struct region *regions, size_t count)
{
	int parent = fdt_path_offset(fdt, "/reserved-memory");
	int addr_cells, size_cells;
	size_t i;

	if (!count)
		return 0;
	if (parent == -FDT_ERR_NOTFOUND) {
		parent = fdt_add_subnode(fdt, 0, "reserved-memory");
		if (parent < 0)
			return parent;
		if (fdt_setprop_u32(fdt, parent, "#address-cells",
				    fdt_address_cells(fdt, 0)) ||
		    fdt_setprop_u32(fdt, parent, "#size-cells",
				    fdt_size_cells(fdt, 0)) ||
		    fdt_setprop(fdt, parent, "ranges", NULL, 0))
			return -FDT_ERR_NOSPACE;
	} else if (parent < 0) {
		return parent;
	}
	for (i = 0; i < count; i++) {
		char name[48];
		int node, ret;

		parent = fdt_path_offset(fdt, "/reserved-memory");
		addr_cells = fdt_address_cells(fdt, parent);
		size_cells = fdt_size_cells(fdt, parent);
		snprintf(name, sizeof(name), "quest-nomap@%" PRIx64,
			 regions[i].start);
		node = fdt_add_subnode(fdt, parent, name);
		if (node < 0)
			return node;
		ret = set_cells(fdt, node, "reg", regions[i].start,
				regions[i].size, addr_cells, size_cells);
		if (!ret)
			ret = fdt_setprop(fdt, node, "no-map", NULL, 0);
		if (ret)
			return ret;
	}
	return 0;
}

static int disable_nodes(void *fdt, const char *const *paths, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		int node = fdt_path_offset(fdt, paths[i]);
		if (node < 0) {
			fprintf(stderr, "--disable %s: node not found\n", paths[i]);
			return -ENOENT;
		}
		if (fdt_setprop_string(fdt, node, "status", "disabled"))
			return -FDT_ERR_NOSPACE;
	}
	return 0;
}

/*
 * Report-derived and operator no-map ranges, merged to 2 MiB blocks. Secure
 * pages are never readable by HLOS again; the next kernel must not map them
 * (speculative fetches into hyp-unmapped pages raise external aborts).
 */
static int collect_nomap(const struct options *opt,
			 const struct report_info *report,
			 struct region *out, size_t *nr_out)
{
	size_t total = opt->nr_nomap + (report ? report->nr_secure : 0);
	struct region *all;
	int ret;

	*nr_out = 0;
	if (!total)
		return 0;
	all = calloc(total, sizeof(*all));
	if (!all)
		return -ENOMEM;
	memcpy(all, opt->nomap, opt->nr_nomap * sizeof(*all));
	if (report && report->nr_secure)
		memcpy(all + opt->nr_nomap, report->secure,
		       report->nr_secure * sizeof(*all));
	ret = coalesce_regions(all, total, IMAGE_BASE_ALIGN, out,
			       MAX_NOMAP_NODES, nr_out);
	free(all);
	if (ret == -E2BIG)
		fprintf(stderr, "more than %d separate 2 MiB no-map blocks; refusing\n",
			MAX_NOMAP_NODES);
	return ret;
}

static int rewrite_dtb(const struct options *opt, const void *source,
		       size_t source_size, void **dtb_data, size_t *dtb_size,
		       uint64_t initrd_size, const struct report_info *report)
{
	void *working = NULL;
	size_t capacity;
	int chosen, root_addr_cells, ret;
	uint64_t initrd_end;
	struct region nomap[MAX_NOMAP_NODES];
	size_t nr_nomap = 0;

	if (source_size < sizeof(struct fdt_header) || fdt_check_header(source) ||
	    fdt_totalsize(source) > source_size ||
	    fdt_check_full(source, source_size))
		return -EINVAL;
	ret = collect_nomap(opt, report, nomap, &nr_nomap);
	if (ret)
		return ret;
	capacity = source_size + 65536 + opt->nr_disable * 32 +
		   nr_nomap * 128 + (report ? report->nr_rmem * 64 : 0);
	working = malloc(capacity);
	if (!working)
		return -ENOMEM;
	ret = fdt_open_into(source, working, (int)capacity);
	if (ret)
		goto fail;
	if (report) {
		unsigned int pinned;

		ret = pin_dynamic_reservations(working, report, &pinned);
		if (ret < 0)
			goto fail;
	}
	ret = add_nomap_nodes(working, nomap, nr_nomap);
	if (ret < 0)
		goto fail;
	ret = disable_nodes(working, opt->disable, opt->nr_disable);
	if (ret < 0)
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
	struct report_info *report = NULL;
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
	if (opt->report_path) {
		report = calloc(1, sizeof(*report));
		if (!report) {
			ret = -ENOMEM;
			goto out;
		}
		ret = load_report(opt->report_path, report);
		if (ret)
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
			  initrd_file_size, report);
	if (ret)
		goto out;
	/* Validate against the tree the next kernel will actually receive. */
	ret = collect_fdt_memory(dtb, fdt_memory, &nr_fdt_memory,
				 fdt_reserved, &nr_fdt_reserved);
	if (ret) {
		fprintf(stderr, "DTB RAM/reserved-memory is malformed or unsupported\n");
		goto out;
	}
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
	free(report);
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
		    !strcmp(arg, "--ram") || !strcmp(arg, "--reserve") ||
		    !strcmp(arg, "--report") || !strcmp(arg, "--nomap") ||
		    !strcmp(arg, "--disable") || !strcmp(arg, "--iomem")) {
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
		} else if (!strcmp(arg, "--nomap")) {
			if (add_region(opt->nomap, &opt->nr_nomap, value))
				return -EINVAL;
		} else if (!strcmp(arg, "--report")) {
			if (opt->report_path)
				return -EINVAL;
			opt->report_path = value;
		} else if (!strcmp(arg, "--disable")) {
			if (opt->nr_disable == MAX_DISABLE || value[0] != '/')
				return -EINVAL;
			opt->disable[opt->nr_disable++] = value;
		} else if (!strcmp(arg, "--iomem")) {
			int iomem_ret = load_iomem(value, opt);
			if (iomem_ret) {
				fprintf(stderr, "%s: no usable System RAM lines (%s)\n",
					value, strerror(-iomem_ret));
				return -EINVAL;
			}
		}
		if (number) {
			if (*present || parse_u64(value, number))
				return -EINVAL;
			*present = true;
		}
	}
	return 0;
}


/*
 * Runtime device tree -> DTB, on the device, without host dtc.
 * The Quest Pro port only booted with the *runtime* tree: /sys/firmware/fdt
 * is the pre-overlay boot FDT and handing it over caused cold resets.
 */
static int compare_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void free_names(char **names, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++)
		free(names[i]);
	free(names);
}

static int list_dir(const char *path, char ***names_out, size_t *count_out)
{
	DIR *dir = opendir(path);
	struct dirent *entry;
	char **names = NULL;
	size_t count = 0, capacity = 0;

	if (!dir)
		return -errno;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		if (count == capacity) {
			size_t next = capacity ? capacity * 2 : 32;
			char **grown = realloc(names, next * sizeof(*names));
			if (!grown) {
				closedir(dir);
				free_names(names, count);
				return -ENOMEM;
			}
			names = grown;
			capacity = next;
		}
		names[count] = strdup(entry->d_name);
		if (!names[count]) {
			closedir(dir);
			free_names(names, count);
			return -ENOMEM;
		}
		count++;
	}
	closedir(dir);
	if (count)
		qsort(names, count, sizeof(*names), compare_names);
	*names_out = names;
	*count_out = count;
	return 0;
}

static int fdt_errno(int fdt_ret)
{
	return fdt_ret == -FDT_ERR_NOSPACE ? -ENOSPC : -EINVAL;
}

static int dump_dt_node(void *fdt, const char *path, const char *node_name,
			unsigned int depth)
{
	char **names = NULL;
	size_t count = 0, i, base_len = strcspn(node_name, "@");
	int ret, pass;

	if (depth > 64)
		return -ELOOP;
	ret = list_dir(path, &names, &count);
	if (ret)
		return ret;
	/* FDT order: all properties of a node, then its subnodes. */
	for (pass = 0; pass < 2 && !ret; pass++) {
		for (i = 0; i < count && !ret; i++) {
			char child[4096];
			struct stat st;

			if ((size_t)snprintf(child, sizeof(child), "%s/%s", path,
					     names[i]) >= sizeof(child)) {
				ret = -ENAMETOOLONG;
				break;
			}
			if (lstat(child, &st)) {
				ret = -errno;
				break;
			}
			if (pass == 0 && S_ISREG(st.st_mode)) {
				void *value = NULL;
				size_t size = 0;

				if (st.st_size) {
					ret = read_file(child, MAX_PROPERTY_SIZE,
							&value, &size);
					if (ret)
						break;
				}
				/* Linux synthesizes "name" for every node. */
				if (depth && !strcmp(names[i], "name") &&
				    size == base_len + 1 &&
				    !memcmp(value, node_name, base_len) &&
				    !((char *)value)[base_len]) {
					free(value);
					continue;
				}
				ret = fdt_property(fdt, names[i], value, (int)size);
				free(value);
				if (ret)
					ret = fdt_errno(ret);
			} else if (pass == 1 && S_ISDIR(st.st_mode)) {
				ret = fdt_begin_node(fdt, names[i]);
				if (ret) {
					ret = fdt_errno(ret);
					break;
				}
				ret = dump_dt_node(fdt, child, names[i], depth + 1);
				if (!ret && fdt_end_node(fdt))
					ret = -ENOSPC;
			}
		}
	}
	free_names(names, count);
	return ret;
}

static int dump_dt(const char *root, const char *boot_fdt_path,
		   void **out, size_t *out_size, int *nr_memreserve)
{
	void *boot = NULL, *fdt;
	size_t boot_size = 0;
	int ret, i;

	*nr_memreserve = -1;
	if (boot_fdt_path &&
	    !read_stream(boot_fdt_path, MAX_DUMP_SIZE, &boot, &boot_size) &&
	    (boot_size < sizeof(struct fdt_header) || fdt_check_header(boot) ||
	     fdt_totalsize(boot) > boot_size)) {
		free(boot);
		boot = NULL;
	}
	fdt = malloc(MAX_DUMP_SIZE);
	if (!fdt) {
		free(boot);
		return -ENOMEM;
	}
	ret = fdt_create(fdt, MAX_DUMP_SIZE);
	if (!ret && boot) {
		*nr_memreserve = fdt_num_mem_rsv(boot);
		for (i = 0; !ret && i < *nr_memreserve; i++) {
			uint64_t address, size;
			ret = fdt_get_mem_rsv(boot, i, &address, &size);
			if (!ret)
				ret = fdt_add_reservemap_entry(fdt, address, size);
		}
	}
	if (!ret)
		ret = fdt_finish_reservemap(fdt);
	if (!ret)
		ret = fdt_begin_node(fdt, "");
	if (ret) {
		ret = fdt_errno(ret);
		goto fail;
	}
	ret = dump_dt_node(fdt, root, "", 0);
	if (ret)
		goto fail;
	if (fdt_end_node(fdt) || fdt_finish(fdt)) {
		ret = -ENOSPC;
		goto fail;
	}
	if (boot)
		fdt_set_boot_cpuid_phys(fdt, fdt_boot_cpuid_phys(boot));
	if (fdt_check_full(fdt, fdt_totalsize(fdt))) {
		ret = -EINVAL;
		goto fail;
	}
	free(boot);
	*out = fdt;
	*out_size = fdt_totalsize(fdt);
	return 0;
fail:
	free(boot);
	free(fdt);
	return ret;
}

static int write_file(const char *path, const void *data, size_t size)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
	size_t done = 0;

	if (fd < 0)
		return -errno;
	while (done < size) {
		ssize_t n = write(fd, (const char *)data + done, size - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			int err = n < 0 ? -errno : -EIO;
			close(fd);
			return err;
		}
		done += (size_t)n;
	}
	if (close(fd))
		return -errno;
	return 0;
}

static int parse_steps(const char *text, uint32_t *flags)
{
	char *copy = strdup(text), *token, *save = NULL;
	int ret = 0;

	if (!copy)
		return -ENOMEM;
	*flags = 0;
	for (token = strtok_r(copy, ",", &save); token;
	     token = strtok_r(NULL, ",", &save)) {
		if (!strcmp(token, "watchdog"))
			*flags |= QUEST_EL1_REHEARSE_WATCHDOG;
		else if (!strcmp(token, "rproc"))
			*flags |= QUEST_EL1_REHEARSE_RPROC;
		else if (!strcmp(token, "cpus"))
			*flags |= QUEST_EL1_REHEARSE_CPUS;
		else if (!strcmp(token, "mmu"))
			*flags |= QUEST_EL1_REHEARSE_MMU;
		else if (!strcmp(token, "copy"))
			*flags |= QUEST_EL1_REHEARSE_COPY;
		else if (!strcmp(token, "all"))
			*flags |= QUEST_EL1_REHEARSE_ALL;
		else
			ret = -EINVAL;
	}
	free(copy);
	return (ret || !*flags) ? -EINVAL : 0;
}

static void print_rehearsal(const struct quest_el1_rehearse *req)
{
	static const char *const names[] = {"watchdog", "rproc", "cpus", "mmu", "copy"};
	unsigned int i;

	for (i = 0; i < 5; i++) {
		uint32_t bit = 1U << i;
		if (!(req->flags & bit))
			continue;
		printf("%-8s %s\n", names[i], (req->done & bit) ? "ok" :
		       (req->result && !(req->done & bit) &&
			(req->done & (bit - 1)) == (req->flags & (bit - 1))) ?
		       "FAILED" : "not run");
	}
	printf("result=%d (%s) watchdogs_stopped=%u rprocs_stopped=%u cpus_offlined=%u\n",
	       req->result, req->result ? strerror(-req->result) : "ok",
	       req->watchdogs_stopped, req->rprocs_stopped, req->cpus_offlined);
	if (req->done & QUEST_EL1_REHEARSE_MMU)
		printf("mmu_off_sctlr=0x%" PRIx64 "\n", (uint64_t)req->mmu_sctlr);
	if (req->done & QUEST_EL1_REHEARSE_COPY)
		printf("copy_pages=%" PRIu64 " copy_ns=%" PRIu64 "\n",
		       (uint64_t)req->copy_pages, (uint64_t)req->copy_ns);
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
	if (!strcmp(command, "dump-dt")) {
		const char *root = "/proc/device-tree";
		const char *boot = "/sys/firmware/fdt";
		void *fdt;
		size_t size;
		int i, nr_memreserve;

		if (argc < 3 || argv[2][0] == '-')
			goto usage_error;
		for (i = 3; i < argc; i++) {
			if (!strcmp(argv[i], "--root") && i + 1 < argc)
				root = argv[++i];
			else if (!strcmp(argv[i], "--boot-fdt") && i + 1 < argc)
				boot = argv[++i];
			else
				goto usage_error;
		}
		ret = dump_dt(root, boot, &fdt, &size, &nr_memreserve);
		if (!ret) {
			ret = write_file(argv[2], fdt, size);
			free(fdt);
		}
		if (ret) {
			errno = -ret;
			perror("dump-dt");
			return 1;
		}
		printf("runtime DTB %s: %zu bytes", argv[2], size);
		if (nr_memreserve < 0)
			printf(" (WARNING: %s unreadable, no /memreserve/ entries)\n", boot);
		else
			printf(", %d /memreserve/ entries from %s\n", nr_memreserve, boot);
		if (size > MAX_DTB_SIZE)
			fprintf(stderr, "warning: larger than the 2 MiB loader limit\n");
		return 0;
	}
	if (!strcmp(command, "report")) {
		char buffer[65536];
		ssize_t n;
		int fd;

		if (parse_device_option(argc, argv, &opt.device))
			goto usage_error;
		fd = open(opt.device, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			perror(opt.device);
			return 1;
		}
		while ((n = read(fd, buffer, sizeof(buffer))) > 0)
			if (fwrite(buffer, 1, (size_t)n, stdout) != (size_t)n)
				break;
		if (n < 0) {
			perror("report");
			close(fd);
			return 1;
		}
		close(fd);
		return 0;
	}
	if (!strcmp(command, "rehearse")) {
		struct quest_el1_rehearse req = {0};
		const char *device = QUEST_EL1_DEVICE;
		uint32_t flags = QUEST_EL1_REHEARSE_WATCHDOG |
				 QUEST_EL1_REHEARSE_CPUS | QUEST_EL1_REHEARSE_MMU;
		int i;

		for (i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "--steps") && i + 1 < argc) {
				if (parse_steps(argv[++i], &flags))
					goto usage_error;
			} else if (!strcmp(argv[i], "--device") && i + 1 < argc) {
				device = argv[++i];
			} else {
				goto usage_error;
			}
		}
		req.hdr = make_header();
		req.flags = flags;
		ret = run_ioctl(device, QUEST_EL1_IOC_REHEARSE, &req);
		if (ret) {
			errno = -ret;
			perror("rehearse");
			return 1;
		}
		print_rehearsal(&req);
		return req.result ? 1 : 0;
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
