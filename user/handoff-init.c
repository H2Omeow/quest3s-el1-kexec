/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * handoff-init - the /init of the first initramfs a handed-off kernel runs.
 *
 * The Quest 3S has no UART and the handed-off kernel has no bootloader
 * console, so the only evidence a first attempt can produce is what survives
 * into the ramoops zone that the stock kernel reads back afterwards
 * (/sys/fs/pstore).  This program therefore does three things and nothing
 * that can block:
 *
 *   1. announce itself on /dev/kmsg as early as possible, so the console zone
 *      proves userspace was reached;
 *   2. copy a compact machine description into /dev/pmsg0, a *separate*
 *      ramoops zone that the kernel log cannot overrun;
 *   3. hand the machine back with a warm restart, so the DDR contents holding
 *      those zones survive into the stock kernel.
 *
 * It is a static binary with no libc startup requirements beyond the normal
 * ones, no shell, and no dependency on any module.
 *
 * Command line (kernel cmdline, read from /proc/cmdline):
 *   handoff.hold=1      stay alive instead of restarting (interactive bring-up)
 *   handoff.wait=<sec>  seconds to wait before restarting (default 5)
 *   handoff.deadman=<s> re-arm the kernel deadman to <s> while we work
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define PMSG_TAG "quest-handoff: "

static int kmsg_fd = -1;
static int pmsg_fd = -1;

static void emit(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	if ((size_t)n >= sizeof(buf))
		n = sizeof(buf) - 1;
	if (kmsg_fd >= 0) {
		char line[1200];
		int m = snprintf(line, sizeof(line), "<4>" PMSG_TAG "%s", buf);
		if (m > 0)
			(void)!write(kmsg_fd, line, (size_t)m);
	}
	if (pmsg_fd >= 0) {
		char line[1200];
		int m = snprintf(line, sizeof(line), PMSG_TAG "%s\n", buf);
		if (m > 0)
			(void)!write(pmsg_fd, line, (size_t)m);
	}
}

/* Read at most len-1 bytes of a procfs/sysfs file; always NUL terminated. */
static ssize_t slurp(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t got = 0;

	buf[0] = '\0';
	if (fd < 0)
		return -1;
	while (got + 1 < len) {
		ssize_t r = read(fd, buf + got, len - got - 1);

		if (r < 0) {
			close(fd);
			return -1;
		}
		if (r == 0)
			break;
		got += (size_t)r;
	}
	buf[got] = '\0';
	close(fd);
	return (ssize_t)got;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = write(fd, text, strlen(text));
	close(fd);
	return r < 0 ? -1 : 0;
}

/* Emit a file with every line prefixed, dropping lines that match nothing. */
static void emit_file(const char *label, const char *path, const char *want)
{
	static char buf[65536];
	char *line, *save;

	if (slurp(path, buf, sizeof(buf)) < 0) {
		emit("%s: unreadable", label);
		return;
	}
	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (want && !strstr(line, want))
			continue;
		emit("%s %s", label, line);
	}
}

static int cmdline_uint(const char *cmdline, const char *key, int fallback)
{
	const char *p = strstr(cmdline, key);
	int v = 0;

	if (!p)
		return fallback;
	p += strlen(key);
	if (*p < '0' || *p > '9')
		return fallback;
	while (*p >= '0' && *p <= '9')
		v = v * 10 + (*p++ - '0');
	return v;
}

int main(void)
{
	static char cmdline[4096];
	static char scratch[4096];
	int wait_s, hold, deadman;

	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	mkdir("/dev", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	/* CONFIG_DEVTMPFS gives us /dev/kmsg and /dev/pmsg0 without mknod. */
	if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0)
		mount("tmpfs", "/dev", "tmpfs", 0, NULL);

	kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	pmsg_fd = open("/dev/pmsg0", O_WRONLY | O_CLOEXEC);

	emit("init reached userspace (pmsg=%s)",
	     pmsg_fd >= 0 ? "yes" : "NO - pstore/pmsg missing");

	slurp("/proc/cmdline", cmdline, sizeof(cmdline));
	hold = cmdline_uint(cmdline, "handoff.hold=", 0);
	wait_s = cmdline_uint(cmdline, "handoff.wait=", 5);
	deadman = cmdline_uint(cmdline, "handoff.deadman=", 0);

	if (deadman > 0) {
		snprintf(scratch, sizeof(scratch), "%d", deadman);
		emit("deadman -> %ds: %s", deadman,
		     write_text("/sys/module/quest_handoff/parameters/deadman",
				scratch) == 0 ? "ok" : "unavailable");
	}

	slurp("/proc/version", scratch, sizeof(scratch));
	emit("version %s", scratch);
	emit("cmdline %s", cmdline);
	emit_file("mem", "/proc/meminfo", "MemTotal");
	emit_file("ram", "/proc/iomem", "System RAM");
	emit_file("rsv", "/proc/iomem", "reserved");
	emit_file("cpu", "/proc/cpuinfo", "processor");
	emit_file("stat", "/proc/stat", "cpu");
	emit_file("ic", "/proc/interrupts", "GIC");

	if (hold) {
		emit("handoff.hold=1: staying alive, deadman left as configured");
		for (;;)
			sleep(3600);
	}

	emit("waiting %ds, then warm restart so pstore can be read from stock",
	     wait_s);
	if (wait_s > 0)
		sleep((unsigned int)wait_s);
	emit("restarting now");
	sync();
	/*
	 * reboot=w on the kernel command line turns this into PSCI
	 * SYSTEM_RESET2, which keeps DDR (and therefore ramoops) intact.
	 */
	reboot(RB_AUTOBOOT);
	for (;;)
		pause();
}
