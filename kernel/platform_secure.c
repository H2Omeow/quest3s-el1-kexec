// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only snapshot of RAM which Linux has handed to another VM/secure
 * world and which the next kernel therefore must never touch.
 *
 * The Quest Pro port found that Android only stays up on full RAM once every
 * secure buffer, secure pool page and secure SMMU page table page is reserved
 * as no-map in the target DTB (ion_secmap). On 5.10 GKI Quest 3S the ION
 * heaps are dma-buf heaps and assignment goes through
 * drivers/soc/qcom/secure_buffer.c:hyp_assign_table(), which marks every page
 * that left HLOS with page->private = SECURE_PAGE_MAGIC (0xEEEEEEEE); see
 * page_accessible() in the same file. Scanning struct page for that marker
 * finds buffers, pre-assigned pool pages and anything else assigned through
 * that path without dereferencing any driver private structure and without
 * touching the (inaccessible) memory itself.
 *
 * Not covered, and reported as such: memory assigned with hyp_assign_phys()
 * (static carveouts: those are /reserved-memory regions and are pinned from
 * the "rmem" lines) and memory lent through mem-buf/Gunyah memparcels
 * without hyp_assign_table(). The dma-buf audit below lists every buffer the
 * local VM does not own exclusively, so such cases are at least visible.
 */
#define pr_fmt(fmt) "quest_el1_secure: " fmt

#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "kexec_compat.h"
#include "platform_secure.h"
#include "report.h"

#define QUEST_SECURE_PAGE_MAGIC 0xEEEEEEEEUL
#define QUEST_SECURE_MAX_LINES  4096
#define QUEST_DMABUF_MAX        4096

struct quest_secure_scan {
	struct quest_report *report;
	unsigned long run_start;  /* pfn, valid when run_len */
	unsigned long run_len;
	unsigned long pages;
	unsigned long ranges;
	bool overflow;
};

static void quest_secure_flush(struct quest_secure_scan *s)
{
	if (!s->run_len)
		return;
	s->ranges++;
	if (s->ranges <= QUEST_SECURE_MAX_LINES)
		quest_report_add(s->report, "secure 0x%llx 0x%llx\n",
				 (unsigned long long)PFN_PHYS(s->run_start),
				 (unsigned long long)PFN_PHYS(s->run_len));
	else
		s->overflow = true;
	s->run_len = 0;
}

static int quest_secure_walk(unsigned long start_pfn, unsigned long nr, void *arg)
{
	struct quest_secure_scan *s = arg;
	unsigned long pfn;

	for (pfn = start_pfn; pfn < start_pfn + nr; pfn++) {
		struct page *page;
		bool secure;

		if (!(pfn & 0x3ffff))
			cond_resched();
		if (!pfn_valid(pfn)) {
			quest_secure_flush(s);
			continue;
		}
		page = pfn_to_page(pfn);
		secure = READ_ONCE(page->private) == QUEST_SECURE_PAGE_MAGIC;
		if (!secure) {
			quest_secure_flush(s);
			continue;
		}
		s->pages++;
		if (s->run_len && s->run_start + s->run_len == pfn) {
			s->run_len++;
		} else {
			quest_secure_flush(s);
			s->run_start = pfn;
			s->run_len = 1;
		}
	}
	quest_secure_flush(s);
	return 0;
}

static void quest_secure_scan_pages(struct quest_report *r)
{
	struct quest_secure_scan s = { .report = r };

	walk_system_ram_range(0, PHYS_PFN((phys_addr_t)-1), &s, quest_secure_walk);
	quest_report_add(r, "secure_summary pages=%lu ranges=%lu%s\n", s.pages,
			 s.ranges, s.overflow ? " overflow" : "");
}

/* Collected under db_list.lock; inspected after it is dropped. */
struct quest_dmabuf_list {
	struct dma_buf *bufs[QUEST_DMABUF_MAX];
	unsigned int count;
	unsigned int skipped;
};

static int quest_dmabuf_collect(const struct dma_buf *dmabuf, void *data)
{
	struct quest_dmabuf_list *list = data;

	if (list->count == QUEST_DMABUF_MAX) {
		list->skipped++;
		return 0;
	}
	/* Keep the buffer alive: releasing takes db_list.lock, which we hold. */
	if (!get_file_rcu(dmabuf->file)) {
		list->skipped++;
		return 0;
	}
	list->bufs[list->count++] = (struct dma_buf *)dmabuf;
	return 0;
}

static void __nocfi quest_secure_dmabuf_audit(struct quest_report *r)
{
	int (*each)(int (*)(const struct dma_buf *, void *), void *) =
		(void *)quest_lookup_name("get_each_dmabuf");
	bool (*exclusive)(struct dma_buf *) =
		(void *)quest_lookup_name("mem_buf_dma_buf_exclusive_owner");
	unsigned long attach = quest_lookup_name("mem_buf_dma_buf_attach");
	struct quest_dmabuf_list *list;
	unsigned long total = 0, shared = 0;
	unsigned int i;
	int ret;

	if (!each) {
		quest_report_add(r, "dmabuf_unavailable\n");
		return;
	}
	list = kvzalloc(sizeof(*list), GFP_KERNEL);
	if (!list)
		return;
	ret = each(quest_dmabuf_collect, list);
	for (i = 0; i < list->count; i++) {
		struct dma_buf *buf = list->bufs[i];
		bool memb = attach && buf->ops &&
			    (unsigned long)buf->ops->attach == attach;
		bool excl = true;

		total += buf->size;
		/* to_mem_buf_vmperm() WARNs on foreign buffers: only ask mem_buf ones. */
		if (memb && exclusive)
			excl = exclusive(buf);
		if (!excl) {
			shared += buf->size;
			quest_report_add(r, "dmabuf_shared %s size=0x%zx\n",
					 buf->exp_name ? buf->exp_name : "?",
					 buf->size);
		}
		fput(buf->file);
	}
	quest_report_add(r, "dmabuf_summary ret=%d count=%u skipped=%u bytes=0x%lx shared_bytes=0x%lx\n",
			 ret, list->count, list->skipped, total, shared);
	kvfree(list);
}

void quest_secure_report(struct quest_report *r)
{
	quest_secure_scan_pages(r);
	quest_secure_dmabuf_audit(r);
}
