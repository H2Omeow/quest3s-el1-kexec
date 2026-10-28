/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QUEST_EL1_PLATFORM_PCI_H
#define QUEST_EL1_PLATFORM_PCI_H

int quest_pci_compat_init(void);
/* After kernel_restart_prepare(), before CPU shutdown or interrupt masking. */
void quest_pci_quiesce(void);

#endif
