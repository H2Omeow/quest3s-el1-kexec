#!/usr/bin/env python3
"""Exercise the real memory guard with synthetic stock RAM/OF/FDT inventories."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "kernel/memory_guard.c").read_text()
# Keep the actual production implementation; replace only its kernel environment.
SOURCE = SOURCE[SOURCE.index("static rwlock_t *stock_resource_lock;"):]
STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint32_t __be32;
typedef int rwlock_t;
#define U64_MAX UINT64_MAX
#define BIT_ULL(n) (UINT64_C(1) << (n))
#define SZ_2M BIT_ULL(21)
#define PAGE_SHIFT 12
#define READ_ONCE(x) (x)
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
#define round_down(x,y) ((x) & ~((u64)(y)-1))
#define round_up(x,y) (((x)+(y)-1) & ~((u64)(y)-1))
#define pr_err(...) ((void)0)
#define IORESOURCE_MEM 0x200
#define IORESOURCE_BUSY 0x80000000UL
#define IORESOURCE_SYSRAM 0x01000000
#define IORESOURCE_SYSTEM_RAM (IORESOURCE_MEM|IORESOURCE_SYSRAM)
#define IORESOURCE_SYSRAM_DRIVER_MANAGED 0x02000000
#define IORES_DESC_NONE 0
struct kexec_segment { unsigned long mem, memsz, bufsz; };
struct kimage { unsigned long nr_segments, dtb; struct kexec_segment segment[4]; };
struct resource {
    u64 start, end;
    unsigned long flags, desc;
    const char *name;
    struct resource *parent, *child, *sibling;
};
struct reserved_mem { u64 base, size; };
struct device_node {
    struct device_node *child, *sibling;
    const __be32 *reg;
    int len, ac, sc;
    bool size_property;
    struct reserved_mem *rmem;
};
static rwlock_t guard_lock;
static int held, lock_calls;
static void read_lock(rwlock_t *p) { assert(p == &guard_lock && !held); held=1; lock_calls++; }
static void read_unlock(rwlock_t *p) { assert(p == &guard_lock && held); held=0; }
static u8 fdt_blob[256];
static const void *boot_fdt;
static u64 kimage_voffset = UINT64_C(0xffffffc000000000);
static unsigned long lookup_missing;
static unsigned long quest_lookup_name(const char *name) {
    if (lookup_missing) return 0;
    if (!strcmp(name,"resource_lock")) return (unsigned long)&guard_lock;
    if (!strcmp(name,"initial_boot_params")) return (unsigned long)&boot_fdt;
    if (!strcmp(name,"_text")) return kimage_voffset + 0x200000;
    if (!strcmp(name,"_end")) return kimage_voffset + 0x600000;
    assert(!"Unexpected stock symbol"); return 0;
}
static u32 get_unaligned_be32(const void *p) {
    const u8 *b=p; return (u32)b[0]<<24 | (u32)b[1]<<16 | (u32)b[2]<<8 | b[3];
}
static u64 get_unaligned_be64(const void *p) {
    const u8 *b=p; return (u64)get_unaligned_be32(b)<<32 | get_unaligned_be32(b+4);
}
static void put32(void *p, u32 v) {
    u8 *b=p; for(int i=3;i>=0;i--) { b[i]=v; v>>=8; }
}
static void put64(void *p, u64 v) {
    u8 *b=p; for(int i=7;i>=0;i--) { b[i]=v; v>>=8; }
}
static struct resource iomem_resource, ram, extra, nested;
static unsigned long hole_pfn, ram_checks;
static int page_is_ram(unsigned long pfn) {
    ram_checks++;
    return pfn >= 1 && pfn < 0x2000 && pfn != hole_pfn;
}
static struct device_node dt_root, dt_child;
static bool root_missing;
static struct device_node *of_find_node_by_path(const char *name) {
    assert(!strcmp(name,"/reserved-memory")); return root_missing ? NULL : &dt_root;
}
static void of_node_put(struct device_node *n) { (void)n; }
#define for_each_available_child_of_node(root, child) \
    for ((child)=(root)->child; (child); (child)=(child)->sibling)
static const __be32 *of_get_property(struct device_node *n,const char *name,int *len) {
    assert(!strcmp(name,"reg")); *len=n->len; return n->reg;
}
static int of_n_addr_cells(struct device_node *n) { return n->ac; }
static int of_n_size_cells(struct device_node *n) { return n->sc; }
static const void *of_find_property(struct device_node *n,const char *name,void *unused) {
    (void)unused; assert(!strcmp(name,"size")); return n->size_property ? n : NULL;
}
static struct reserved_mem *of_reserved_mem_lookup(struct device_node *n) { return n->rmem; }
static u64 of_read_number(const __be32 *p,int cells) {
    return cells==1 ? get_unaligned_be32(p) : get_unaligned_be64(p);
}
'''
CASES = r'''
static struct kimage image;
static void reset(void) {
    memset(fdt_blob,0,sizeof(fdt_blob));
    put32(fdt_blob,0xd00dfeed); put32(fdt_blob+4,128); put32(fdt_blob+16,40);
    boot_fdt=fdt_blob; lookup_missing=0; hole_pfn=0; ram_checks=0; lock_calls=0;
    iomem_resource=(struct resource){.end=U64_MAX};
    ram=(struct resource){.start=0x1000,.end=0x1ffffff,
        .flags=IORESOURCE_SYSTEM_RAM|IORESOURCE_BUSY,.name="System RAM",.parent=&iomem_resource};
    iomem_resource.child=&ram;
    extra=(struct resource){.start=0x100000,.end=0x100fff,
        .flags=IORESOURCE_MEM|IORESOURCE_BUSY,.name="reserved",.parent=&ram};
    nested=(struct resource){0};
    dt_root=(struct device_node){0}; dt_child=(struct device_node){.ac=2,.sc=2}; root_missing=false;
    image=(struct kimage){.nr_segments=2,.dtb=0xa01000,
        .segment={{.mem=0x100000,.memsz=0x2000,.bufsz=0x2000},
                  {.mem=0xa01000,.memsz=0x1000,.bufsz=0x1000}}};
    assert(quest_memory_guard_init()==0);
}
static unsigned passed;
#define EXPECT(expr, expected) do { int got=(expr); \
    if (got!=(expected)) { fprintf(stderr,"line %d: got %d expected %d\n",__LINE__,got,(expected)); return 1; } \
    assert(!held); passed++; } while(0)
int main(void) {
    reset(); EXPECT(quest_validate_destinations(&image),0); assert(ram_checks==3 && lock_calls==1);
    /* No page allocator occupancy check: ordinary old userspace RAM is legal. */
    reset(); image.segment[0].mem=0x400000; ram.child=&extra;
    extra.start=0x400000; extra.end=0x401fff;
    EXPECT(quest_validate_destinations(&image),0); /* old kernel generic init reservation */
    extra.name="firmware";
    EXPECT(quest_validate_destinations(&image),-EBUSY); /* no generic kernel exemption for firmware */
    extra.name="reserved"; extra.flags=IORESOURCE_SYSTEM_RAM|IORESOURCE_SYSRAM_DRIVER_MANAGED;
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    reset(); ram.child=&extra; EXPECT(quest_validate_destinations(&image),-EBUSY);
    reset(); ram.child=&extra; extra.flags=IORESOURCE_SYSTEM_RAM; extra.child=&nested;
    nested=(struct resource){.start=0x100800,.end=0x1008ff,.flags=IORESOURCE_MEM,
        .name="nested secure firmware",.parent=&extra};
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    /* Unwind more than one tree level before visiting an ancestor's sibling. */
    reset(); ram.child=&extra;
    extra=(struct resource){.start=0x20000,.end=0x2ffff,.flags=IORESOURCE_SYSTEM_RAM,
        .name="nested RAM",.parent=&ram,.child=&nested};
    nested=(struct resource){.start=0x20000,.end=0x20fff,.flags=IORESOURCE_SYSTEM_RAM,
        .name="deep RAM",.parent=&extra};
    struct resource peer={.start=0xe00000,.end=0xe00fff,.flags=IORESOURCE_MEM,
        .name="ancestor sibling firmware",.parent=&ram};
    extra.sibling=&peer;
    EXPECT(quest_validate_destinations(&image),0);
    peer.start=0x100000; peer.end=0x100fff;
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    /* A generic reservation spanning the old kernel keeps both outer pieces. */
    reset(); ram.child=&extra; extra.start=0x100000; extra.end=0x6fffff;
    EXPECT(quest_validate_destinations(&image),-EBUSY); /* left of _text */
    image.segment[0].mem=0x600000;
    EXPECT(quest_validate_destinations(&image),-EBUSY); /* at/after _end */
    image.segment[0].mem=0x400000;
    EXPECT(quest_validate_destinations(&image),0); /* only the kernel overlap is reclaimed */
    reset(); hole_pfn=0x101; EXPECT(quest_validate_destinations(&image),-EADDRNOTAVAIL);
    reset(); ram.sibling=&extra; extra.parent=&iomem_resource;
    extra.start=BIT_ULL(60); extra.end=U64_MAX;
    EXPECT(quest_validate_destinations(&image),0); /* unrelated resource may end at U64_MAX */
    extra.start=0; EXPECT(quest_validate_destinations(&image),-EBUSY);
    reset(); ram.child=&extra; extra.start=0xa00000; extra.end=0xa00fff;
    EXPECT(quest_validate_destinations(&image),-EBUSY); /* DTB 2 MiB block, before DTB bytes */
    reset(); put64(fdt_blob+40,0x100000); put64(fdt_blob+48,0x1000);
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    reset(); image.segment[0].mem=0x400000;
    put64(fdt_blob+40,0x400000); put64(fdt_blob+48,0x1000);
    EXPECT(quest_validate_destinations(&image),-EBUSY); /* no old-kernel exemption for FDT reserves */
    reset(); put64(fdt_blob+40,0xa00000); put64(fdt_blob+48,0x1000);
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    reset(); put32(fdt_blob+16,41); EXPECT(quest_validate_destinations(&image),-EINVAL);
    reset(); put32(fdt_blob+4,55); EXPECT(quest_validate_destinations(&image),-EINVAL);
    reset(); put64(fdt_blob+40,1); EXPECT(quest_validate_destinations(&image),-EINVAL);
    reset(); put64(fdt_blob+40,U64_MAX-7); put64(fdt_blob+48,16);
    EXPECT(quest_validate_destinations(&image),-EINVAL);
    reset(); put64(fdt_blob+40,U64_MAX); put64(fdt_blob+48,1);
    EXPECT(quest_validate_destinations(&image),0); /* inclusive endpoint can be U64_MAX */
    put64(fdt_blob+40,U64_MAX-1); put64(fdt_blob+48,3);
    EXPECT(quest_validate_destinations(&image),-EINVAL);
    reset(); boot_fdt=NULL; EXPECT(quest_validate_destinations(&image),-EINVAL);
    reset(); root_missing=true; EXPECT(quest_validate_destinations(&image),-ENODEV);
    reset(); dt_root.child=&dt_child; dt_child.size_property=true;
    EXPECT(quest_validate_destinations(&image),-EOPNOTSUPP);
    struct reserved_mem dynamic={.base=0x100000,.size=0x1000}; dt_child.rmem=&dynamic;
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    dynamic.base=0xe00000; EXPECT(quest_validate_destinations(&image),0);
    __be32 reg[4]; put64(reg,0x100000); put64(reg+2,0x1000);
    dt_child.size_property=false; dt_child.reg=reg; dt_child.len=sizeof(reg);
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    put64(reg+2,0); EXPECT(quest_validate_destinations(&image),0); /* empty bootloader placeholder */
    dt_child.len=12; EXPECT(quest_validate_destinations(&image),-EINVAL);
    dt_child.ac=dt_child.sc=INT_MAX; EXPECT(quest_validate_destinations(&image),-EINVAL);
    /* A later reg tuple must be checked even when the first one is disjoint. */
    reset(); dt_root.child=&dt_child;
    __be32 multi_reg[8];
    put64(multi_reg,0xe00000); put64(multi_reg+2,0x1000);
    put64(multi_reg+4,0x100000); put64(multi_reg+6,0x1000);
    dt_child.reg=multi_reg; dt_child.len=sizeof(multi_reg);
    EXPECT(quest_validate_destinations(&image),-EBUSY);
    put64(multi_reg+4,0xf00000);
    EXPECT(quest_validate_destinations(&image),0);
    reset(); lookup_missing=1; EXPECT(quest_memory_guard_init(),-ENOENT);
    printf("memory guard: %u production-code cases passed\n",passed); return 0;
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix="quest-memory-guard-") as directory:
        c_file = Path(directory) / "guard.c"
        executable = Path(directory) / "guard"
        c_file.write_text(STUBS + SOURCE + CASES)
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined",
                        "-fno-sanitize-recover=all", str(c_file), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)

if __name__ == "__main__":
    main()
