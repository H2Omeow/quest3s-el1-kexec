// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reversible rehearsal of the MMU-off relocation path.
 *
 * Everything the final handoff does after device shutdown, except the last
 * branch into the new Image, is exercised here with the real transition blob:
 * idmap install (TTBR0/TCR rewrite), entry through the physical alias, MMU
 * off, list walk, uncached page copies with destination invalidation, I-cache
 * maintenance and the x0 handover. Destinations are private scratch pages,
 * so the running kernel is never overwritten; afterwards the scratch pages
 * are compared with the sources and the original translation state is put
 * back. The whole roundtrip runs inside stop_machine(), so no other CPU can
 * send an IPI that would wait for a CPU running with the MMU off.
 */
#define pr_fmt(fmt) "quest_el1_rehearse: " fmt

#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <asm/barrier.h>
#include <asm/daifflags.h>
#include <asm/sysreg.h>

#include "../../kexec.h"
#include "idmap.h"
#include "machine_kexec_compat.h"
#include "rehearse.h"

extern const unsigned char quest_el1_transition[];
extern const unsigned char quest_el1_transition_end[];
extern const unsigned char quest_el1_return[];
extern const unsigned char quest_el1_return_end[];
u64 quest_rehearse_enter(phys_addr_t code, unsigned long head,
			 phys_addr_t ret_stub, u64 cookie);

#define QUEST_REHEARSE_COOKIE    0x5145524548454c31ULL
/* 1 GiB of payload pages is far beyond any Image+initrd+DTB we accept. */
#define QUEST_REHEARSE_MAX_PAGES (1UL << 18)
/* Pairs (DESTINATION, SOURCE) per list page; the last slot links or ends. */
#define QUEST_LIST_PAIRS ((PAGE_SIZE / sizeof(kimage_entry_t) - 1) / 2)

struct quest_rehearse_run {
	phys_addr_t code;
	phys_addr_t ret_stub;
	unsigned long head;
	u64 cookie;
	u64 sctlr_before;
	u64 sctlr_after;
	u64 ns;
};

struct quest_rehearse_buf {
	struct page *ctrl;
	struct page **list;
	struct page **scratch;
	unsigned long nr_list;
	unsigned long nr;
};

static size_t quest_return_offset(void)
{
	return ALIGN(quest_el1_transition_end - quest_el1_transition, 16);
}

static int quest_rehearse_cpu(void *data)
{
	struct quest_rehearse_run *run = data;
	struct quest_mmu_state saved;
	unsigned long flags;
	u64 t0;

	pr_info("cpu%d: installing private idmap for %pa\n",
		smp_processor_id(), &run->code);
	flags = local_daif_save();
	quest_idmap_save(&saved);
	run->sctlr_before = read_sysreg(sctlr_el1);
	asm volatile("dsb sy\n\tic iallu\n\tdsb sy\n\tisb" ::: "memory");
	quest_idmap_install(run->code);
	pr_info("cpu%d: idmap installed, entering blob\n", smp_processor_id());
	t0 = ktime_get_mono_fast_ns();
	run->cookie = quest_rehearse_enter(run->code, run->head, run->ret_stub,
					   QUEST_REHEARSE_COOKIE);
	run->ns = ktime_get_mono_fast_ns() - t0;
	pr_info("cpu%d: blob returned x0=%#llx\n", smp_processor_id(),
		run->cookie);
	quest_idmap_restore(&saved);
	run->sctlr_after = read_sysreg(sctlr_el1);
	pr_info("cpu%d: idmap restored sctlr=%#llx\n", smp_processor_id(),
		run->sctlr_after);
	local_daif_restore(flags);
	return 0;
}

static void quest_rehearse_free(struct quest_rehearse_buf *b)
{
	unsigned long i;

	if (b->scratch)
		for (i = 0; i < b->nr; i++)
			if (b->scratch[i])
				__free_page(b->scratch[i]);
	if (b->list)
		for (i = 0; i < b->nr_list; i++)
			if (b->list[i])
				__free_page(b->list[i]);
	kvfree(b->scratch);
	kvfree(b->list);
	if (b->ctrl)
		__free_page(b->ctrl);
	memset(b, 0, sizeof(*b));
}

static int quest_rehearse_alloc(struct quest_rehearse_buf *b, unsigned long nr)
{
	const gfp_t gfp = GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY;
	size_t ret_off = quest_return_offset();
	size_t ret_len = quest_el1_return_end - quest_el1_return;
	unsigned long i;
	void *code;

	if (!nr || nr > QUEST_REHEARSE_MAX_PAGES)
		return -E2BIG;
	if (ret_off + ret_len > PAGE_SIZE)
		return -E2BIG;
	b->nr = nr;
	b->nr_list = DIV_ROUND_UP(nr, QUEST_LIST_PAIRS);
	b->scratch = kvcalloc(nr, sizeof(*b->scratch), GFP_KERNEL);
	b->list = kvcalloc(b->nr_list, sizeof(*b->list), GFP_KERNEL);
	b->ctrl = alloc_page(GFP_KERNEL);
	if (!b->scratch || !b->list || !b->ctrl)
		return -ENOMEM;
	if (page_to_phys(b->ctrl) >= BIT_ULL(39))
		return -ERANGE;
	for (i = 0; i < nr; i++) {
		b->scratch[i] = alloc_page(gfp);
		if (!b->scratch[i])
			return -ENOMEM;
	}
	for (i = 0; i < b->nr_list; i++) {
		b->list[i] = alloc_page(gfp | __GFP_ZERO);
		if (!b->list[i])
			return -ENOMEM;
	}
	code = page_address(b->ctrl);
	memset(code, 0, PAGE_SIZE);
	memcpy(code, quest_el1_transition,
	       quest_el1_transition_end - quest_el1_transition);
	memcpy(code + ret_off, quest_el1_return, ret_len);
	quest_flush_dcache(code, PAGE_SIZE);
	return 0;
}

/*
 * Destinations are scattered scratch pages, so emit DESTINATION before every
 * SOURCE. The blob advances its destination cursor per source page exactly
 * as for a contiguous segment; only the list shape differs from a kimage.
 */
static unsigned long quest_rehearse_build(struct quest_rehearse_buf *b,
					  const phys_addr_t *src)
{
	unsigned long p, idx = 0;

	for (p = 0; p < b->nr_list; p++) {
		kimage_entry_t *e = page_address(b->list[p]);
		unsigned long k = 0, j;

		for (j = 0; j < QUEST_LIST_PAIRS && idx < b->nr; j++, idx++) {
			e[k++] = page_to_phys(b->scratch[idx]) | IND_DESTINATION;
			e[k++] = (src[idx] & PAGE_MASK) | IND_SOURCE;
		}
		e[k] = p + 1 < b->nr_list ?
			(page_to_phys(b->list[p + 1]) | IND_INDIRECTION) : IND_DONE;
		quest_flush_dcache(e, PAGE_SIZE);
	}
	return page_to_phys(b->list[0]) | IND_INDIRECTION;
}

/* Scratch = bitwise inverse of its source, so a skipped copy cannot pass. */
static void quest_rehearse_prefill(struct quest_rehearse_buf *b,
				   const phys_addr_t *src)
{
	unsigned long i, w;

	for (i = 0; i < b->nr; i++) {
		const u64 *s = phys_to_virt(src[i]);
		u64 *d = page_address(b->scratch[i]);

		for (w = 0; w < PAGE_SIZE / sizeof(u64); w++)
			d[w] = ~s[w];
		quest_flush_dcache(d, PAGE_SIZE);
		/* The blob reads sources with the MMU (and caches) off. */
		quest_flush_dcache((void *)s, PAGE_SIZE);
	}
}

static int quest_rehearse_pages(const phys_addr_t *src, unsigned long nr,
				struct quest_el1_rehearse_result *out)
{
	struct quest_rehearse_buf b = {};
	struct quest_rehearse_run run = {};
	unsigned long i, bad = 0, first_bad = 0;
	int ret;

	ret = quest_arch_check();
	if (ret)
		return ret;
	ret = quest_rehearse_alloc(&b, nr);
	if (ret)
		goto out;
	quest_rehearse_prefill(&b, src);
	run.code = page_to_phys(b.ctrl);
	run.ret_stub = run.code + quest_return_offset();
	run.head = quest_rehearse_build(&b, src);

	pr_info("mmu rehearsal: %lu page(s) code=%pa head=%#lx ret_stub=%pa\n",
		nr, &run.code, run.head, &run.ret_stub);
	ret = stop_machine(quest_rehearse_cpu, &run, NULL);
	if (ret)
		goto out;
	if (run.cookie != QUEST_REHEARSE_COOKIE) {
		pr_err("blob returned x0=%#llx, expected %#llx\n", run.cookie,
		       QUEST_REHEARSE_COOKIE);
		ret = -EIO;
		goto out;
	}
	if (run.sctlr_after != run.sctlr_before) {
		pr_err("SCTLR_EL1 %#llx -> %#llx after roundtrip\n",
		       run.sctlr_before, run.sctlr_after);
		ret = -EIO;
		goto out;
	}
	for (i = 0; i < nr; i++) {
		void *d = page_address(b.scratch[i]);

		/* Drop any line speculatively fetched before the uncached writes. */
		quest_flush_dcache(d, PAGE_SIZE);
		if (memcmp(d, phys_to_virt(src[i]), PAGE_SIZE)) {
			if (!bad++)
				first_bad = i;
		}
	}
	if (bad) {
		pr_err("%lu of %lu relocated pages differ (first #%lu src %pa)\n",
		       bad, nr, first_bad, &src[first_bad]);
		ret = -EIO;
		goto out;
	}
	out->sctlr = run.sctlr_after;
	out->pages = nr;
	out->ns = run.ns;
	pr_info("MMU-off relocation of %lu page(s) verified in %llu ns\n", nr,
		run.ns);
out:
	quest_rehearse_free(&b);
	return ret;
}

int quest_arch_rehearse_mmu(struct quest_el1_rehearse_result *out)
{
	struct page *page = alloc_page(GFP_KERNEL);
	phys_addr_t src;
	u64 *p;
	unsigned long w;
	int ret;

	if (!page)
		return -ENOMEM;
	p = page_address(page);
	for (w = 0; w < PAGE_SIZE / sizeof(u64); w++)
		p[w] = QUEST_REHEARSE_COOKIE ^ (w * 0x9e3779b97f4a7c15ULL);
	src = page_to_phys(page);
	ret = quest_rehearse_pages(&src, 1, out);
	__free_page(page);
	return ret;
}

/* Count, then collect, every SOURCE page of a loaded kimage. */
static unsigned long quest_image_sources(struct kimage *image, phys_addr_t *src)
{
	kimage_entry_t *entry;
	unsigned long n = 0;

	for (entry = &image->head; ; entry++) {
		unsigned long flag = *entry & IND_FLAGS;
		phys_addr_t addr = *entry & PAGE_MASK;

		if (flag == IND_DONE)
			break;
		if (flag == IND_INDIRECTION) {
			entry = (kimage_entry_t *)phys_to_virt(addr) - 1;
		} else if (flag == IND_SOURCE) {
			if (src)
				src[n] = addr;
			n++;
		}
	}
	return n;
}

int quest_arch_rehearse_copy(struct kimage *image,
			     struct quest_el1_rehearse_result *out)
{
	unsigned long n;
	phys_addr_t *src;
	int ret;

	if (!image)
		return -ENOENT;
	n = quest_image_sources(image, NULL);
	if (!n || n > QUEST_REHEARSE_MAX_PAGES)
		return -E2BIG;
	src = kvmalloc_array(n, sizeof(*src), GFP_KERNEL);
	if (!src)
		return -ENOMEM;
	quest_image_sources(image, src);
	ret = quest_rehearse_pages(src, n, out);
	kvfree(src);
	return ret;
}
