// SPDX-License-Identifier: GPL-2.0
/* VLAN configuration interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * VLAN configuration takes place in two separate domains of the switch: the
 * VLAN4k table and the VLAN membership configuration database. While the VLAN4k
 * table is exhaustive and can be fully populated with 4096 VLAN configurations,
 * the same does not hold for the VLAN membership configuration database, which
 * is limited to 32 entries.
 *
 * The switch will normally only use the VLAN4k table when making forwarding
 * decisions. The VLAN membership configuration database is a vestigial ASIC
 * design and is only used for a few specific features in the rtl8365mb
 * family. This means that the limit of 32 entries should not hinder us in
 * programming a huge number of VLANs into the switch.
 *
 * One necessary use of the VLAN membership configuration database is for the
 * programming of a port-based VLAN ID (PVID). The PVID is programmed on a
 * per-port basis via register field, which refers to a specific VLAN membership
 * configuration via an index 0~31. In order to maintain coherent behaviour on a
 * port with a PVID, it is necessary to keep the VLAN configuration synchronized
 * between the VLAN4k table and the VLAN membership configuration database.
 *
 * Since VLAN membership configs are a scarce resource, this interface offers a
 * common way of allocating and freeing such entries. It enables reference
 * counting and emplacement of such entries into a list. It is up to the rest of
 * the users in the driver to make proper use of this interface to prevent
 * exhaustion of the VLAN membership configuration database. For this reason, it
 * is only possible to set the entries via &struct rtl8365mb_vlanmc_entry.
 *
 * With some exceptions, the entries in both the VLAN4k table and the VLAN
 * membership configuration database offer the same configuration options. The
 * differences are as follows:
 *
 * 1. VLAN4k entries can specify whether to use Independent or Shared VLAN
 *    Learning (IVL or SVL respectively). VLAN membership config entries
 *    cannot. This underscores the fact that VLAN membership configs are not
 *    involved in the learning process of the ASIC.
 *
 * 2. VLAN membership config entries use an "enhanced VLAN ID" (efid), which has
 *    a range 0~8191 compared with the standard 0~4095 range of the VLAN4k
 *    table. This underscores the fact that VLAN membership configs can be used
 *    to group ports on a layer beyond the standard VLAN configuration, which
 *    may be useful for ACL rules which specify alternative forwarding
 *    decisions.
 */

#ifndef _REALTEK_RTL8365MB_VLAN_H
#define _REALTEK_RTL8365MB_VLAN_H

#include <linux/types.h>

#include "realtek.h"

/**
 * struct rtl8365mb_vlan4k - VLAN4k table entry
 * @vid: VLAN ID (0~4095)
 * @member: port mask of ports in this VLAN
 * @untag: port mask of ports which untag on egress
 * @fid: filter ID - only used with SVL (unused)
 * @priority: priority classification (unused)
 * @priority_en: enable priority (unused)
 * @policing_en: enable policing (unused)
 * @ivl_en: enable IVL instead of default SVL
 * @meteridx: metering index (unused)
 *
 * This structure is used to get/set entries in the VLAN4k table. The
 * VLAN4k table dictates the VLAN configuration for the switch for the
 * vast majority of features.
 */
struct rtl8365mb_vlan4k {
	u16 vid;
	u16 member;
	u16 untag;
	u8 fid : 4;
	u8 priority : 3;
	u8 priority_en : 1;
	u8 policing_en : 1;
	u8 ivl_en : 1;
	u8 meteridx : 6;
};

/**
 * struct rtl8365mb_vlanmc - VLAN membership config
 * @evid: Enhanced VLAN ID (0~8191)
 * @member: port mask of ports in this VLAN
 * @fid: filter ID - only used with SVL (unused)
 * @priority: priority classification (unused)
 * @priority_en: enable priority (unused)
 * @policing_en: enable policing (unused)
 * @meteridx: metering index (unused)
 *
 * This structure is used to get/set entries in the VLAN membership
 * configuration database. This feature is largely vestigial, but
 * still needed for at least the following features:
 *   - PVID configuration
 *   - ACL configuration
 *   - selection of VLAN by the CPU tag when VSEL=1, although the switch
 *     can also select VLAN based on the VLAN tag if VSEL=0
 *
 * This is a low-level structure and it is recommended to interface with
 * the VLAN membership config database via &struct rtl8365mb_vlanmc_entry.
 */
struct rtl8365mb_vlanmc {
	u16 evid;
	u16 member;
	u8 fid : 4;
	u8 priority : 3;
	u8 priority_en : 1;
	u8 policing_en : 1;
	u8 meteridx : 6;
};

/**
 * struct rtl8365mb_vlanmc_entry - abstract VLAN membership config entry
 * @vlanmc_db: VLAN membership configuration database
 * @index: the index of this VLAN membership config within the database
 * @refcnt: optional refcounter - rtl8365mb_alloc_vlanmc_entry()
 *          initializes this to zero but users can make use of it
 * @list: optional list head for putting these objects in a list
 * @vlanmc: the VLAN membership config itself
 *
 * This structure is the recommended way to interface with the VLAN
 * membership config database. Objects of this type are allocated and
 * freed using rtl8365mb_alloc_vlanmc_entry() and
 * rtl8365mb_free_vlanmc_entry(). The allocator will handle the job of
 * finding an available index in the database and reserve it until the
 * entry is freed. It is of course possible to run out of space in the
 * database, but since the feature is seldom required, this is
 * unlikely to be the case. For the current implementation it is
 * impossible to run out.
 */
struct rtl8365mb_vlanmc_entry {
	struct rtl8365mb_vlanmc_db *vlanmc_db;
	unsigned int index;
	refcount_t refcnt;
	struct list_head list;
	struct rtl8365mb_vlanmc vlanmc;
};

/* Number of VLAN membership configs available */
#define RTL8365MB_NUM_MEMBERCONFIGS 32

/**
 * struct rtl8365mb_vlanmc_db - VLAN membership configuration database
 * @used: array of VLAN membership configuration entries, true iff used
 *
 * This is to be embedded in &struct rtl8365mb and passed to the allocator
 * function for tracking of VLAN membership configuration database usage.
 */
struct rtl8365mb_vlanmc_db {
	bool used[RTL8365MB_NUM_MEMBERCONFIGS];
};

/**
 * rtl8365mb_vlan_get_vlan4k - get a VLAN4k table entry
 * @priv: pointer to realtek_priv driver private data
 * @vid: VLAN ID to get table entry for
 * @vlan4k: VLAN4k table entry data is output here
 */
int rtl8365mb_vlan_get_vlan4k(struct realtek_priv *priv, u16 vid,
			      struct rtl8365mb_vlan4k *vlan4k);

/**
 * rtl8365mb_vlan_set_vlan4k - set a VLAN4k table entry
 * @priv: pointer to realtek_priv driver private data
 * @vlan4k: VLAN4k table entry data
 */
int rtl8365mb_vlan_set_vlan4k(struct realtek_priv *priv,
			      const struct rtl8365mb_vlan4k *vlan4k);

/**
 * rtl8365mb_vlan_alloc_vlanmc_entry - allocate a VLAN membership config entry
 * @vlanmc_db: VLAN membership configuration database to allocate from
 *
 * Note that it is NOT guaranteed that the corresponding in-switch membership
 * config is already zeroed-out. It is up to the user to program the switch
 * membership config accordingly via rtl8365mb_vlan_set_vlanmc_entry().
 *
 * The refcounter is initialized to zero because not all users need refcounting.
 *
 * Return:
 * * pointer to a new VLAN membership config entry - on success
 * * ERR_PTR(-ENOSPC) - if the database is full
 * * ERR_PTR(-ENOMEM) - on memory allocation failure
 */
struct rtl8365mb_vlanmc_entry *
rtl8365mb_vlan_alloc_vlanmc_entry(struct rtl8365mb_vlanmc_db *vlanmc_db);

/**
 * rtl8365mb_vlan_free_vlanmc_entry - free a VLAN membership config entry
 * @vlanmc_entry: the VLAN membership config to free
 */
void rtl8365mb_vlan_free_vlanmc_entry(
	struct rtl8365mb_vlanmc_entry *vlanmc_entry);

/**
 * rtl8365mb_vlan_set_vlanmc_entry - set a VLAN membership config entry
 * @vlanmc_entry: the VLAN membership config to set
 *
 * Commits the contents of @vlanmc_entry to the switch VLAN membership
 * configuration database.
 */
int rtl8365mb_vlan_set_vlanmc_entry(
	struct realtek_priv *priv,
	const struct rtl8365mb_vlanmc_entry *vlanmc_entry);

#endif /* _REALTEK_RTL8365MB_VLAN_H */
