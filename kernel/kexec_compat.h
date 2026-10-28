/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QUEST_EL1_KEXEC_COMPAT_H
#define QUEST_EL1_KEXEC_COMPAT_H

struct mutex;

unsigned long quest_lookup_name(const char *name);
int kexec_compat_load(void);
void kexec_compat_unload(void);
void quest_kernel_restart_prepare(char *cmd);
void quest_migrate_to_reboot_cpu(void);
void quest_machine_shutdown(void);
struct mutex *quest_transition_mutex(void);
int quest_freeze_processes(void);
void quest_thaw_processes(void);
void quest_block_probing(void);
void quest_unblock_probing(void);

int quest_arch_init(void);
void quest_arch_exit(void);

#endif
