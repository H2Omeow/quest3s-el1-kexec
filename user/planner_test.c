// SPDX-License-Identifier: GPL-2.0-only
#define main quest_el1_program_main
#include "quest-el1.c"
#undef main

static void expect(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static void set_reg(void *fdt, int node, const char *name,
		    uint64_t start, uint64_t size)
{
	fdt32_t reg[4] = {
		cpu_to_fdt32((uint32_t)(start >> 32)),
		cpu_to_fdt32((uint32_t)start),
		cpu_to_fdt32((uint32_t)(size >> 32)),
		cpu_to_fdt32((uint32_t)size),
	};
	expect(!fdt_setprop(fdt, node, name, reg, sizeof(reg)), "set FDT reg");
}

static void write_all(const char *path, const void *data, size_t size)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
	size_t done = 0;
	expect(fd >= 0, "create fixture");
	while (done < size) {
		ssize_t n = write(fd, (const char *)data + done, size - done);
		expect(n > 0, "write fixture");
		done += n;
	}
	close(fd);
}

static void make_fdt(const char *path, bool reserve_kernel)
{
	unsigned char buffer[2048];
	int root, memory, reserved, child;
	expect(!fdt_create_empty_tree(buffer, sizeof(buffer)), "create test FDT");
	root = 0;
	expect(!fdt_setprop_u32(buffer, root, "#address-cells", 2), "root address cells");
	expect(!fdt_setprop_u32(buffer, root, "#size-cells", 2), "root size cells");
	memory = fdt_add_subnode(buffer, root, "memory@80000000");
	expect(memory >= 0, "add memory node");
	expect(!fdt_setprop_string(buffer, memory, "device_type", "memory"), "memory type");
	set_reg(buffer, memory, "reg", 0x80000000ULL, 0x10000000ULL);
	reserved = fdt_add_subnode(buffer, root, "reserved-memory");
	expect(reserved >= 0, "add reserved-memory");
	expect(!fdt_setprop_u32(buffer, reserved, "#address-cells", 2), "reserved address cells");
	expect(!fdt_setprop_u32(buffer, reserved, "#size-cells", 2), "reserved size cells");
	expect(!fdt_setprop(buffer, reserved, "ranges", NULL, 0), "reserved ranges");
	if (reserve_kernel) {
		child = fdt_add_subnode(buffer, reserved, "kernel@80080000");
		expect(child >= 0, "add reserved child");
		set_reg(buffer, child, "reg", 0x80080000ULL, 0x2000ULL);
	}
	child = fdt_add_subnode(buffer, root, "chosen");
	expect(child >= 0, "add chosen");
	expect(!fdt_setprop_u64(buffer, child, "linux,initrd-start", 0xdead0000),
	       "add stale initrd address");
	expect(!fdt_setprop_u64(buffer, child, "linux,initrd-end", 0xdead1000),
	       "add stale initrd end");
	expect(!fdt_pack(buffer), "pack fixture FDT");
	write_all(path, buffer, fdt_totalsize(buffer));
}

static void set_fdt_region(const char *path, const char *node_path,
			   const char *property, uint64_t start, uint64_t size)
{
	unsigned char expanded[4096];
	void *source;
	size_t length;
	int node;
	expect(!read_file(path, MAX_DTB_SIZE, &source, &length), "read FDT fixture");
	expect(!fdt_open_into(source, expanded, sizeof(expanded)), "expand FDT fixture");
	free(source);
	node = fdt_path_offset(expanded, node_path);
	expect(node >= 0, "find FDT node");
	set_reg(expanded, node, property, start, size);
	expect(!fdt_pack(expanded), "pack limited FDT fixture");
	write_all(path, expanded, fdt_totalsize(expanded));
}

static void make_image(const char *path, uint64_t flags, uint64_t memory_size)
{
	unsigned char image[8192] = {0};
	uint64_t fields[] = {0x80000, memory_size, flags};
	unsigned i, j;
	image[56] = 0x41;
	image[57] = 0x52;
	image[58] = 0x4d;
	image[59] = 0x64;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 8; j++)
			image[8 + i * 8 + j] = fields[i] >> (j * 8);
	write_all(path, image, sizeof(image));
}


static uint64_t reg_cell64(const void *fdt, int node, int index)
{
	const fdt32_t *reg;
	int len;
	uint64_t value = 0;
	reg = fdt_getprop(fdt, node, "reg", &len);
	expect(reg && len >= (index + 1) * 8, "reg present with 2+2 cells");
	expect(!read_cells(reg + index * 2, 2, &value), "decode reg cells");
	return value;
}

/* Add a dynamically placed (size-only) reserved-memory node to a fixture. */
static void add_dynamic_node(const char *path, const char *name, uint64_t size)
{
	unsigned char expanded[8192];
	void *source;
	size_t length;
	int parent, node;
	fdt32_t cells[2] = {cpu_to_fdt32((uint32_t)(size >> 32)),
			    cpu_to_fdt32((uint32_t)size)};
	expect(!read_file(path, MAX_DTB_SIZE, &source, &length), "read FDT fixture");
	expect(!fdt_open_into(source, expanded, sizeof(expanded)), "expand FDT fixture");
	free(source);
	parent = fdt_path_offset(expanded, "/reserved-memory");
	expect(parent >= 0, "fixture has reserved-memory");
	node = fdt_add_subnode(expanded, parent, name);
	expect(node >= 0, "add dynamic node");
	expect(!fdt_setprop(expanded, node, "size", cells, sizeof(cells)), "size prop");
	expect(!fdt_setprop_string(expanded, node, "compatible", "ramoops"), "compat");
	expect(!fdt_setprop_u32(expanded, node, "alignment", 0x100000), "alignment");
	expect(!fdt_pack(expanded), "pack dynamic fixture");
	write_all(path, expanded, fdt_totalsize(expanded));
}

static void write_text(const char *path, const char *text)
{
	write_all(path, text, strlen(text));
}

/* Mirror an FDT into a sysfs-like directory tree, as Linux exposes it. */
static void fdt_to_fs(const void *fdt, int node, const char *path, bool root)
{
	int property, child;
	char file[1024];
	expect(!mkdir(path, 0700), "mkdir fs node");
	fdt_for_each_property_offset(property, fdt, node) {
		const char *name;
		int len;
		const void *value = fdt_getprop_by_offset(fdt, property, &name, &len);
		snprintf(file, sizeof(file), "%s/%s", path, name);
		write_all(file, value, (size_t)len);
	}
	if (!root) {
		/* Linux adds a synthesized "name" property to every node. */
		const char *name = fdt_get_name(fdt, node, NULL);
		size_t base = strcspn(name, "@");
		char value[256];
		memcpy(value, name, base);
		value[base] = '\0';
		snprintf(file, sizeof(file), "%s/name", path);
		write_all(file, value, base + 1);
	}
	fdt_for_each_subnode(child, fdt, node) {
		snprintf(file, sizeof(file), "%s/%s", path,
			 fdt_get_name(fdt, child, NULL));
		fdt_to_fs(fdt, child, file, false);
	}
}

static void expect_same_tree(const void *original, const void *dumped)
{
	int node = -1, depth = 0;
	while ((node = fdt_next_node(original, node, &depth)) >= 0) {
		char path[512];
		int other, property, count = 0, other_count = 0;
		expect(!fdt_get_path(original, node, path, sizeof(path)), "node path");
		other = fdt_path_offset(dumped, path);
		expect(other >= 0, "dumped tree has every node");
		fdt_for_each_property_offset(property, original, node) {
			const char *name;
			int len, other_len;
			const void *value = fdt_getprop_by_offset(original, property,
								  &name, &len);
			const void *copy = fdt_getprop(dumped, other, name, &other_len);
			expect(copy && other_len == len &&
			       (!len || !memcmp(copy, value, (size_t)len)),
			       "dumped property identical");
			count++;
		}
		fdt_for_each_property_offset(property, dumped, other)
			other_count++;
		expect(count == other_count, "no synthesized/extra properties dumped");
	}
}

static struct options fixture_options(const char *image, const char *dtb)
{
	struct options opt = {0};
	opt.image_path = image;
	opt.dtb_path = dtb;
	opt.kernel_base = 0x80000000ULL;
	opt.have_kernel_base = true;
	opt.dtb_addr = 0x81000000ULL;
	opt.have_dtb_addr = true;
	opt.ram[0] = (struct region){0x80000000ULL, 0x10000000ULL};
	opt.nr_ram = 1;
	opt.reserved[0] = (struct region){0x8ff00000ULL, 0x100000ULL};
	opt.nr_reserved = 1;
	return opt;
}

int main(void)
{
	char dir[] = "/tmp/quest-el1-test.XXXXXX";
	char image_path[256], dtb_path[256], initrd_path[256];
	struct options opt;
	struct plan plan;
	expect(mkdtemp(dir) != NULL, "create temp dir");
	snprintf(image_path, sizeof(image_path), "%s/Image", dir);
	snprintf(dtb_path, sizeof(dtb_path), "%s/board.dtb", dir);
	snprintf(initrd_path, sizeof(initrd_path), "%s/initrd", dir);
	make_image(image_path, (1ULL << 3) | (1ULL << 1), 8192);
	make_fdt(dtb_path, false);
	opt = fixture_options(image_path, dtb_path);
	expect(build_plan(&opt, &plan) == 0, "valid image and DTB plan accepted");
	expect(plan.entry == 0x80080000ULL && plan.nr_segments == 2,
	       "text_offset produces correct entry and two segments");
	{
		int chosen = fdt_path_offset(plan.segments[1].data, "/chosen");
		expect(chosen >= 0, "planned DTB has chosen node");
		expect(!fdt_getprop(plan.segments[1].data, chosen,
				    "linux,initrd-start", NULL),
		       "stale initrd addresses removed when no initrd supplied");
	}
	free_plan(&plan);
	set_fdt_region(dtb_path, "/memory@80000000", "linux,usable-memory",
			0x80000000ULL, 0x03000000ULL);
	expect(build_plan(&opt, &plan) == 0,
	       "valid destinations inside linux,usable-memory accepted");
	free_plan(&plan);
	set_fdt_region(dtb_path, "/memory@80000000", "linux,usable-memory",
			0x81000000ULL, 0x01000000ULL);
	expect(build_plan(&opt, &plan) == -ERANGE,
	       "memory linux,usable-memory restriction enforced");
	make_fdt(dtb_path, false);
	set_fdt_region(dtb_path, "/chosen", "linux,usable-memory-range",
			0x81000000ULL, 0x01000000ULL);
	expect(build_plan(&opt, &plan) == -EOPNOTSUPP,
	       "chosen linux,usable-memory-range is rejected");
	make_fdt(dtb_path, false);
	set_fdt_region(dtb_path, "/chosen", "linux,elfcorehdr",
			0x8f000000ULL, 0x00010000ULL);
	expect(build_plan(&opt, &plan) == -EOPNOTSUPP,
	       "crash-only linux,elfcorehdr is rejected");
	make_fdt(dtb_path, false);
	{
		void *fdt;
		size_t length;
		int memory;
		expect(!read_file(dtb_path, MAX_DTB_SIZE, &fdt, &length),
		       "read fixture to remove memory type");
		memory = fdt_path_offset(fdt, "/memory@80000000");
		expect(memory >= 0 && !fdt_delprop(fdt, memory, "device_type"),
		       "remove memory device_type");
		write_all(dtb_path, fdt, length);
		free(fdt);
		expect(build_plan(&opt, &plan) == -EINVAL,
		       "memory node name without device_type does not establish RAM");
	}
	make_fdt(dtb_path, false);
	make_image(image_path, (1ULL << 3) | (1ULL << 1), 0x18000);
	expect(build_plan(&opt, &plan) == 0,
	       "Image memory size may include zero-filled BSS");
	expect(plan.segments[0].file_size == 8192 &&
	       plan.segments[0].memory_size == 0x18000,
	       "Image source length differs from reserved memory size");
	free_plan(&plan);
	opt.dtb_addr = 0x80081000ULL;
	expect(build_plan(&opt, &plan) == -EINVAL,
	       "kernel and DTB overlap rejected");
	opt = fixture_options(image_path, dtb_path);
	opt.reserved[0] = (struct region){0x80080000ULL, 0x1000ULL};
	expect(build_plan(&opt, &plan) == -EBUSY,
	       "operator-reserved kernel destination rejected");
	opt = fixture_options(image_path, dtb_path);
	make_fdt(dtb_path, true);
	expect(build_plan(&opt, &plan) == -EBUSY,
	       "DT reserved-memory kernel destination rejected");
	make_fdt(dtb_path, false);
	make_image(image_path, (1ULL << 3) | (1ULL << 1) | (1ULL << 4), 8192);
	expect(build_plan(&opt, &plan) == -EINVAL,
	       "reserved Image flag bits rejected");
	make_image(image_path, (1ULL << 3) | (1ULL << 1), 8192);
	opt = fixture_options(image_path, dtb_path);
	opt.reserved[0] = (struct region){0x81001000ULL, 0x2000ULL};
	expect(build_plan(&opt, &plan) == -EBUSY,
	       "DTB 2 MiB mapping block overlapping reserve is rejected");
	opt = fixture_options(image_path, dtb_path);
	{
		unsigned char initrd[4097] = {1};
		const fdt32_t *property;
		uint64_t address;
		int chosen, len;
		write_all(initrd_path, initrd, sizeof(initrd));
		opt.initrd_path = initrd_path;
		opt.initrd_addr = 0x82000000ULL;
		opt.have_initrd_addr = true;
		opt.bootargs = "console=planner-test";
		expect(build_plan(&opt, &plan) == 0,
		       "valid external initrd accepted");
		expect(plan.nr_segments == 3 &&
		       plan.segments[2].file_size == sizeof(initrd) &&
		       plan.segments[2].memory_size == 8192,
		       "initrd source length and page rounding preserved");
		chosen = fdt_path_offset(plan.segments[1].data, "/chosen");
		property = fdt_getprop(plan.segments[1].data, chosen,
				       "linux,initrd-end", &len);
		expect(property && len == 8 &&
		       !read_cells(property, 2, &address) &&
		       address == opt.initrd_addr + sizeof(initrd),
		       "DTB initrd-end uses actual bytes rather than rounded memory");
		expect(!strcmp(fdt_getprop(plan.segments[1].data, chosen,
					 "bootargs", NULL), opt.bootargs),
		       "bootargs replaced in planned DTB");
		free_plan(&plan);
		{
			void *fdt;
			size_t size;
			int memory;
			expect(!read_file(dtb_path, MAX_DTB_SIZE, &fdt, &size),
			       "read fixture to widen memory");
			memory = fdt_path_offset(fdt, "/memory@80000000");
			set_reg(fdt, memory, "reg", 0x80000000ULL, 0x1000000000ULL);
			write_all(dtb_path, fdt, size);
			free(fdt);
		}
		opt.ram[0].size = 0x1000000000ULL;
		opt.initrd_addr = 0x900000000ULL;
		expect(build_plan(&opt, &plan) == -ERANGE,
		       "initrd outside common <=32 GiB window rejected");
	}
	{
		unsigned char truncated_dtb[8] = {0xd0, 0x0d, 0xfe, 0xed};
		write_all(dtb_path, truncated_dtb, sizeof(truncated_dtb));
		opt = fixture_options(image_path, dtb_path);
		expect(build_plan(&opt, &plan) == -EINVAL,
		       "truncated DTB rejected before header access");
	}
	{
		/* Region coalescing to 2 MiB blocks. */
		struct region in[3] = {{0x80201000ULL, 0x1000ULL},
				       {0x80000000ULL, 0x1000ULL},
				       {0x80600000ULL, 0x200000ULL}};
		struct region out[4];
		size_t n;
		expect(!coalesce_regions(in, 3, IMAGE_BASE_ALIGN, out, 4, &n) && n == 2,
		       "touching blocks merge, distant blocks stay separate");
		expect(out[0].start == 0x80000000ULL && out[0].size == 0x400000ULL &&
		       out[1].start == 0x80600000ULL && out[1].size == 0x200000ULL,
		       "coalesced ranges aligned and sorted");
		expect(coalesce_regions(in, 3, IMAGE_BASE_ALIGN, out, 1, &n) == -E2BIG,
		       "coalesce output limit enforced");
	}
	{
		/* /proc/iomem: only top-level System RAM, zeroed view refused. */
		struct region ram[MAX_REGIONS];
		size_t n = 0;
		expect(!parse_iomem_text("80000000-80dfffff : System RAM\n"
					 "  80080000-8133ffff : Kernel code\n"
					 "80e00000-8fffffff : reserved\n"
					 "90000000-97ffffff : System RAM\n", ram, &n) &&
		       n == 2 && ram[0].size == 0xe00000ULL &&
		       ram[1].start == 0x90000000ULL, "iomem System RAM parsed");
		n = 0;
		expect(parse_iomem_text("00000000-00000000 : System RAM\n", ram, &n) == -EPERM,
		       "unprivileged zeroed iomem refused");
	}
	{
		/* Live-state report: pin dynamic nodes, no-map secure pages. */
		char report_path[256];
		void *dtb;
		int node;
		snprintf(report_path, sizeof(report_path), "%s/report.txt", dir);
		make_fdt(dtb_path, false);
		add_dynamic_node(dtb_path, "ramoops_region", 0x200000ULL);
		opt = fixture_options(image_path, dtb_path);
		expect(build_plan(&opt, &plan) == -EOPNOTSUPP,
		       "size-only reserved node still refused without a report");
		write_text(report_path,
			   "abi 2\n"
			   "rmem ramoops_region 0x8e000000 0x200000\n"
			   "secure 0x8c001000 0x1000\n"
			   "secure 0x8c003000 0x2000\n"
			   "secure_summary pages=3 ranges=2\n"
			   "end\n");
		opt.report_path = report_path;
		expect(build_plan(&opt, &plan) == 0, "report pins dynamic reservation");
		dtb = plan.segments[1].data;
		node = fdt_path_offset(dtb, "/reserved-memory/ramoops_region");
		expect(node >= 0 && reg_cell64(dtb, node, 0) == 0x8e000000ULL &&
		       reg_cell64(dtb, node, 1) == 0x200000ULL,
		       "ramoops pinned at the running kernel's address");
		expect(!fdt_getprop(dtb, node, "size", NULL) &&
		       !fdt_getprop(dtb, node, "alignment", NULL),
		       "dynamic placement properties removed");
		node = fdt_path_offset(dtb, "/reserved-memory/quest-nomap@8c000000");
		expect(node >= 0 && fdt_getprop(dtb, node, "no-map", NULL) &&
		       reg_cell64(dtb, node, 1) == 0x200000ULL,
		       "secure pages become one 2 MiB no-map node");
		free_plan(&plan);

		write_text(report_path, "rmem ramoops_region 0x8e000000 0x200000\n");
		expect(build_plan(&opt, &plan) == -EINVAL, "truncated report refused");
		write_text(report_path, "rmem ramoops_region 0x8e000000 0x200000\n"
			   "secure_summary pages=9 ranges=4097 overflow\nend\n");
		expect(build_plan(&opt, &plan) == -E2BIG, "overflowing secure scan refused");
		write_text(report_path, "rmem_unavailable\nend\n");
		expect(build_plan(&opt, &plan) == -EOPNOTSUPP,
		       "report without reserved_mem table refused");
		write_text(report_path, "rmem ramoops_region 0x8e000000 0x200000\n"
			   "secure 0x80090000 0x1000\nend\n");
		expect(build_plan(&opt, &plan) == -EBUSY,
		       "secure page inside the kernel destination refused");
		write_text(report_path, "rmem ramoops_region 0x80000000 0x200000\nend\n");
		expect(build_plan(&opt, &plan) == -EBUSY,
		       "pinned reservation overlapping the kernel refused");

		/* Operator --nomap and --disable. */
		write_text(report_path, "rmem ramoops_region 0x8e000000 0x200000\nend\n");
		opt.nomap[0] = (struct region){0x8a100000ULL, 0x1000ULL};
		opt.nr_nomap = 1;
		opt.disable[0] = "/chosen";
		opt.nr_disable = 1;
		expect(build_plan(&opt, &plan) == 0, "nomap and disable accepted");
		dtb = plan.segments[1].data;
		node = fdt_path_offset(dtb, "/reserved-memory/quest-nomap@8a000000");
		expect(node >= 0 && fdt_getprop(dtb, node, "no-map", NULL),
		       "operator nomap node added");
		node = fdt_path_offset(dtb, "/chosen");
		expect(!strcmp(fdt_getprop(dtb, node, "status", NULL), "disabled"),
		       "disabled node marked");
		free_plan(&plan);
		opt.disable[0] = "/no-such-node";
		expect(build_plan(&opt, &plan) == -ENOENT, "unknown --disable path refused");
	}
	{
		/* dump-dt: sysfs-style tree -> identical DTB + memreserve copy. */
		char fs_root[256], boot_path[256];
		unsigned char boot[4096];
		void *source, *dumped;
		size_t length, dumped_size;
		int nr_memreserve;
		make_fdt(dtb_path, true);
		add_dynamic_node(dtb_path, "ramoops_region", 0x200000ULL);
		expect(!read_file(dtb_path, MAX_DTB_SIZE, &source, &length), "read dump fixture");
		snprintf(fs_root, sizeof(fs_root), "%s/base", dir);
		fdt_to_fs(source, 0, fs_root, true);
		expect(!fdt_open_into(source, boot, sizeof(boot)), "expand boot FDT");
		expect(!fdt_add_mem_rsv(boot, 0x85000000ULL, 0x100000ULL), "add memreserve");
		snprintf(boot_path, sizeof(boot_path), "%s/fdt", dir);
		write_all(boot_path, boot, fdt_totalsize(boot));
		expect(!dump_dt(fs_root, boot_path, &dumped, &dumped_size, &nr_memreserve),
		       "dump runtime tree");
		expect(nr_memreserve == 1 && fdt_num_mem_rsv(dumped) == 1,
		       "memreserve copied from the boot FDT");
		expect_same_tree(source, dumped);
		expect_same_tree(dumped, source);
		free(dumped);
		expect(!dump_dt(fs_root, "/nonexistent", &dumped, &dumped_size,
				&nr_memreserve) && nr_memreserve == -1,
		       "missing boot FDT only drops memreserve");
		free(dumped);
		free(source);
	}
	{
		uint32_t flags;
		expect(!parse_steps("watchdog,mmu", &flags) &&
		       flags == (QUEST_EL1_REHEARSE_WATCHDOG | QUEST_EL1_REHEARSE_MMU),
		       "rehearsal step list parsed");
		expect(!parse_steps("all", &flags) && flags == QUEST_EL1_REHEARSE_ALL,
		       "all rehearsal steps");
		expect(parse_steps("watchdog,jump", &flags) == -EINVAL,
		       "unknown rehearsal step refused");
	}
	{
		char cleanup[300];
		snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
		expect(system(cleanup) == 0, "remove temp dir");
	}
	puts("planner tests: all checks passed");
	return 0;
}
