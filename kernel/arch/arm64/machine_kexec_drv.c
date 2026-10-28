// SPDX-License-Identifier: GPL-2.0-only
#include "idmap.h"
#include "machine_kexec_compat.h"

int quest_arch_init(void)
{
	int ret = quest_arch_compat_init();

	return ret ? ret : quest_idmap_init();
}

void quest_arch_exit(void)
{
	quest_idmap_exit();
}
