// SPDX-License-Identifier: GPL-2.0-only
/*
 * Remote processors (ADSP/CDSP/...) keep running their firmware across a
 * plain device_shutdown(): drivers/remoteproc/qcom_q6v5_pas.c has no
 * .shutdown callback. A running DSP keeps DMA-ing into its carveouts and
 * shared buffers, and keeps SMP2P/GLINK state the next kernel cannot adopt.
 * The Quest Pro port only became stable once ADSP/CDSP were stopped before
 * the jump. Stop the selected processors through the regular remoteproc
 * core (sysmon shutdown request, then PAS shutdown in TZ) while the system
 * is still fully alive.
 *
 * Selection (module parameter / rehearsal argument):
 *   ""        nothing (default)
 *   "pas"     every RUNNING processor whose parent driver is qcom_q6v5_pas
 *   "a,b"     processors whose rproc->name matches exactly
 */
#define pr_fmt(fmt) "quest_el1_rproc: " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/remoteproc.h>
#include <linux/string.h>

#include "kexec_compat.h"
#include "platform_rproc.h"
#include "report.h"

#define QUEST_MAX_RPROCS 16
#define QUEST_RPROC_MAX_PUTS 8

struct quest_rproc_entry {
	struct rproc *rproc;
	int puts; /* rproc_shutdown() calls that must be undone on cancel */
};

static DEFINE_MUTEX(quest_rproc_lock);
static struct quest_rproc_entry stopped[QUEST_MAX_RPROCS];
static unsigned int nr_stopped;

static char *stop_remoteprocs = "";
module_param(stop_remoteprocs, charp, 0600);
MODULE_PARM_DESC(stop_remoteprocs,
		 "Remote processors to stop before handoff: '' (none), 'pas', or comma list of names");

static const char *const quest_rproc_states[] = {
	"offline", "suspended", "running", "crashed", "deleted", "detached",
};

static const char *quest_rproc_state(int state)
{
	if (state >= 0 && state < ARRAY_SIZE(quest_rproc_states))
		return quest_rproc_states[state];
	return "invalid";
}

static struct class *quest_rproc_class(void)
{
	return (struct class *)quest_lookup_name("rproc_class");
}

/* Reject devices of the class which are not struct rproc::dev. */
static struct rproc *quest_dev_to_rproc(struct device *dev)
{
	unsigned long type = quest_lookup_name("rproc_type");

	if (!type || (unsigned long)dev->type != type)
		return NULL;
	return container_of(dev, struct rproc, dev);
}

static const char *quest_rproc_parent_driver(struct rproc *rproc)
{
	struct device *parent = rproc->dev.parent;

	return parent && parent->driver ? parent->driver->name : "-";
}

static bool quest_name_in_list(const char *name, const char *list)
{
	size_t len = strlen(name);
	const char *p = list;

	while (p && *p) {
		const char *comma = strchr(p, ',');
		size_t item = comma ? comma - p : strlen(p);

		if (item == len && !strncmp(p, name, len))
			return true;
		p = comma ? comma + 1 : NULL;
	}
	return false;
}

static bool quest_rproc_selected(struct rproc *rproc, const char *spec)
{
	if (!spec || !*spec)
		return false;
	if (!strcmp(spec, "pas"))
		return !strcmp(quest_rproc_parent_driver(rproc), "qcom_q6v5_pas");
	return rproc->name && quest_name_in_list(rproc->name, spec);
}

struct quest_rproc_walk {
	const char *spec;
	struct quest_report *report;
	struct rproc *found[QUEST_MAX_RPROCS];
	unsigned int count;
	int error;
};

static int quest_rproc_visit(struct device *dev, void *data)
{
	struct quest_rproc_walk *walk = data;
	struct rproc *rproc = quest_dev_to_rproc(dev);

	if (!rproc)
		return 0;
	if (walk->report) {
		quest_report_add(walk->report,
				 "rproc %s state=%s power=%d parent=%s driver=%s\n",
				 rproc->name ? rproc->name : "?",
				 quest_rproc_state(READ_ONCE(rproc->state)),
				 atomic_read(&rproc->power),
				 rproc->dev.parent ? dev_name(rproc->dev.parent) : "-",
				 quest_rproc_parent_driver(rproc));
		return 0;
	}
	if (!quest_rproc_selected(rproc, walk->spec) ||
	    READ_ONCE(rproc->state) != RPROC_RUNNING)
		return 0;
	if (walk->count == QUEST_MAX_RPROCS) {
		walk->error = -E2BIG;
		return 1;
	}
	get_device(&rproc->dev);
	walk->found[walk->count++] = rproc;
	return 0;
}

static int quest_rproc_walk(struct quest_rproc_walk *walk)
{
	struct class *cls = quest_rproc_class();

	if (!cls)
		return -ENOENT;
	class_for_each_device(cls, NULL, walk, quest_rproc_visit);
	return walk->error;
}

void quest_rproc_report(struct quest_report *r)
{
	struct quest_rproc_walk walk = { .report = r };

	if (quest_rproc_walk(&walk) == -ENOENT)
		quest_report_add(r, "rproc_unavailable\n");
	quest_report_add(r, "stop_remoteprocs '%s'\n",
			 stop_remoteprocs ? stop_remoteprocs : "");
}

static void __nocfi quest_rproc_restart_locked(void)
{
	int (*boot)(struct rproc *) = (void *)quest_lookup_name("rproc_boot");

	while (nr_stopped) {
		struct quest_rproc_entry *e = &stopped[--nr_stopped];
		int ret = 0;

		for (; e->puts > 0 && boot; e->puts--) {
			ret = boot(e->rproc);
			if (ret)
				break;
		}
		pr_info("restart %s: %d (state %s)\n", e->rproc->name, ret,
			quest_rproc_state(READ_ONCE(e->rproc->state)));
		put_device(&e->rproc->dev);
		e->rproc = NULL;
	}
}

void quest_rproc_restart(void)
{
	mutex_lock(&quest_rproc_lock);
	quest_rproc_restart_locked();
	mutex_unlock(&quest_rproc_lock);
}

/*
 * Stop every selected RUNNING processor. rproc_shutdown() only stops the
 * processor when the last power reference goes, so repeat it (bounded) and
 * remember how many references to give back with rproc_boot() on cancel.
 */
int __nocfi quest_rproc_stop(const char *spec)
{
	void (*shutdown)(struct rproc *) = (void *)quest_lookup_name("rproc_shutdown");
	struct quest_rproc_walk walk = { .spec = spec };
	unsigned int i;
	int ret;

	if (!spec)
		spec = stop_remoteprocs;
	walk.spec = spec;
	if (!spec || !*spec)
		return 0;
	if (!shutdown || !quest_lookup_name("rproc_boot"))
		return -ENOENT;
	mutex_lock(&quest_rproc_lock);
	if (nr_stopped) {
		mutex_unlock(&quest_rproc_lock);
		return -EBUSY;
	}
	ret = quest_rproc_walk(&walk);
	for (i = 0; i < walk.count; i++) {
		struct rproc *rproc = walk.found[i];
		struct quest_rproc_entry *e;

		if (ret) {
			put_device(&rproc->dev);
			continue;
		}
		e = &stopped[nr_stopped++];
		e->rproc = rproc;
		e->puts = 0;
		while (READ_ONCE(rproc->state) == RPROC_RUNNING &&
		       atomic_read(&rproc->power) > 0 &&
		       e->puts < QUEST_RPROC_MAX_PUTS) {
			shutdown(rproc);
			e->puts++;
		}
		pr_info("stop %s: %d shutdown call(s), state %s\n", rproc->name,
			e->puts, quest_rproc_state(READ_ONCE(rproc->state)));
		if (READ_ONCE(rproc->state) == RPROC_RUNNING)
			ret = -EBUSY;
	}
	if (ret)
		quest_rproc_restart_locked();
	mutex_unlock(&quest_rproc_lock);
	return ret;
}

/* Successful handoff: forget the references, the old kernel is gone. */
void quest_rproc_commit(void)
{
	mutex_lock(&quest_rproc_lock);
	nr_stopped = 0;
	mutex_unlock(&quest_rproc_lock);
}
