// SPDX-License-Identifier: GPL-2.0
/* VLAN configuration interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include "rtl8365mb_vlan.h"
#include "rtl8365mb_table.h"
#include <linux/regmap.h>

/* CVLAN (i.e. VLAN4k) table entry layout, u16[4] */
#define RTL8365MB_CVLAN_ENTRY_D0_MBR_MASK		0x00FF
#define RTL8365MB_CVLAN_ENTRY_D0_UNTAG_MASK		0xFF00
#define RTL8365MB_CVLAN_ENTRY_D1_FID_MASK		0x000F
#define RTL8365MB_CVLAN_ENTRY_D1_VBPEN_MASK		0x0010
#define RTL8365MB_CVLAN_ENTRY_D1_VBPRI_MASK		0x00E0
#define RTL8365MB_CVLAN_ENTRY_D1_ENVLANPOL_MASK		0x0100
#define RTL8365MB_CVLAN_ENTRY_D1_METERIDX_MASK		0x3E00
#define RTL8365MB_CVLAN_ENTRY_D1_IVL_SVL_MASK		0x4000
#define RTL8365MB_CVLAN_ENTRY_D2_MBR_EXT_MASK		0x0007
#define RTL8365MB_CVLAN_ENTRY_D2_UNTAG_EXT_MASK		0x0038
#define RTL8365MB_CVLAN_ENTRY_D2_METERIDX_EXT_MASK	0x0040

/* VLAN member configuration registers 0~31, u16[3] */
#define RTL8365MB_VLAN_MC_BASE			0x0728
#define RTL8365MB_VLAN_MC_REG(_x)  \
		(RTL8365MB_VLAN_MC_BASE + (_x) * 4)
#define   RTL8365MB_VLAN_MC_D0_MBR_MASK			0x07FF
#define   RTL8365MB_VLAN_MC_D1_FID_MASK			0x000F
#define   RTL8365MB_VLAN_MC_D2_METERIDX_MASK		0x07E0
#define   RTL8365MB_VLAN_MC_D2_ENVLANPOL_MASK		0x0010
#define   RTL8365MB_VLAN_MC_D2_VBPRI_MASK		0x000E
#define   RTL8365MB_VLAN_MC_D2_VBPEN_MASK		0x0001
#define   RTL8365MB_VLAN_MC_D3_EVID_MASK		0x1FFF

/* Some limits for VLAN4k/VLAN membership config entries */
#define RTL8365MB_PRIORITYMAX	7
#define RTL8365MB_FIDMAX	15
#define RTL8365MB_METERMAX	63

int rtl8365mb_vlan_get_vlan4k(struct realtek_priv *priv, u16 vid,
			      struct rtl8365mb_vlan4k *vlan4k)
{
	u16 data[3];
	int ret;

	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = RTL8365MB_TABLE_CVLAN,
					    .op = RTL8365MB_TABLE_OP_READ,
					    .arg.cvlan.addr = vid,
				    },
				    data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	memset(vlan4k, 0, sizeof(*vlan4k));

	/* Unpack table entry */
	vlan4k->vid = vid;
	vlan4k->member =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D0_MBR_MASK, data[0]) |
		(FIELD_GET(RTL8365MB_CVLAN_ENTRY_D2_MBR_EXT_MASK, data[2])
		 << 8);
	vlan4k->untag =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D0_UNTAG_MASK, data[0]) |
		(FIELD_GET(RTL8365MB_CVLAN_ENTRY_D2_UNTAG_EXT_MASK, data[2])
		 << 8);
	vlan4k->fid = FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_FID_MASK, data[1]);
	vlan4k->priority_en =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_VBPEN_MASK, data[1]);
	vlan4k->priority =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_VBPRI_MASK, data[1]);
	vlan4k->policing_en =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_ENVLANPOL_MASK, data[1]);
	vlan4k->meteridx =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_METERIDX_MASK, data[1]) |
		(FIELD_GET(RTL8365MB_CVLAN_ENTRY_D2_METERIDX_EXT_MASK, data[2])
		 << 5);
	vlan4k->ivl_en =
		FIELD_GET(RTL8365MB_CVLAN_ENTRY_D1_IVL_SVL_MASK, data[1]);

	return 0;
}

int rtl8365mb_vlan_set_vlan4k(struct realtek_priv *priv,
			      const struct rtl8365mb_vlan4k *vlan4k)
{
	u16 data[3] = { 0 };

	if (vlan4k->fid > RTL8365MB_FIDMAX ||
	    vlan4k->priority > RTL8365MB_PRIORITYMAX ||
	    vlan4k->meteridx > RTL8365MB_METERMAX)
		return -EINVAL;

	/* Pack table entry value */
	data[0] |=
		FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D0_MBR_MASK, vlan4k->member);
	data[0] |=
		FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D0_UNTAG_MASK, vlan4k->untag);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_FID_MASK, vlan4k->fid);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_VBPEN_MASK,
			      vlan4k->priority_en);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_VBPRI_MASK,
			      vlan4k->priority);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_ENVLANPOL_MASK,
			      vlan4k->policing_en);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_METERIDX_MASK,
			      vlan4k->meteridx);
	data[1] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D1_IVL_SVL_MASK,
			      vlan4k->ivl_en);
	data[2] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D2_MBR_EXT_MASK,
			      vlan4k->member >> 8);
	data[2] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D2_UNTAG_EXT_MASK,
			      vlan4k->untag >> 8);
	data[2] |= FIELD_PREP(RTL8365MB_CVLAN_ENTRY_D2_METERIDX_EXT_MASK,
			      vlan4k->meteridx >> 5);

	return rtl8365mb_table_query(priv,
				     &(struct rtl8365mb_table_query){
					     .table = RTL8365MB_TABLE_CVLAN,
					     .op = RTL8365MB_TABLE_OP_WRITE,
					     .arg.cvlan.addr = vlan4k->vid,
				     },
				     data, ARRAY_SIZE(data));
}

int rtl8365mb_vlan_get_vlanmc(struct realtek_priv *priv, u32 index,
			      struct rtl8365mb_vlanmc *vlanmc)
{
	u16 data[4] = { 0 };
	int val;
	int ret;
	int i;

	for (i = 0; i < 4; i++) {
		ret = regmap_read(priv->map, RTL8365MB_VLAN_MC_REG(index) + i,
				  &val);
		if (ret)
			return ret;

		data[i] = val;
	}

	vlanmc->member = FIELD_GET(RTL8365MB_VLAN_MC_D0_MBR_MASK, data[0]);
	vlanmc->fid = FIELD_GET(RTL8365MB_VLAN_MC_D1_FID_MASK, data[1]);
	vlanmc->meteridx =
		FIELD_GET(RTL8365MB_VLAN_MC_D2_METERIDX_MASK, data[2]);
	vlanmc->policing_en =
		FIELD_GET(RTL8365MB_VLAN_MC_D2_ENVLANPOL_MASK, data[2]);
	vlanmc->priority = FIELD_GET(RTL8365MB_VLAN_MC_D2_VBPRI_MASK, data[2]);
	vlanmc->priority_en =
		FIELD_GET(RTL8365MB_VLAN_MC_D2_VBPEN_MASK, data[2]);
	vlanmc->evid = FIELD_GET(RTL8365MB_VLAN_MC_D3_EVID_MASK, data[3]);

	return 0;
}

/* Private - use rtl8365mb_vlan_set_vlanmc_entry() */
static int rtl8365mb_vlan_set_vlanmc(struct realtek_priv *priv, u32 index,
				     const struct rtl8365mb_vlanmc *vlanmc)
{
	u16 data[4] = { 0 };
	int ret;
	int i;

	if (index >= RTL8365MB_NUM_MEMBERCONFIGS ||
	    vlanmc->fid > RTL8365MB_FIDMAX ||
	    vlanmc->priority > RTL8365MB_PRIORITYMAX ||
	    vlanmc->meteridx > RTL8365MB_METERMAX)
		return -EINVAL;

	data[0] |= FIELD_PREP(RTL8365MB_VLAN_MC_D0_MBR_MASK, vlanmc->member);
	data[1] |= FIELD_PREP(RTL8365MB_VLAN_MC_D1_FID_MASK, vlanmc->fid);
	data[2] |= FIELD_PREP(RTL8365MB_VLAN_MC_D2_METERIDX_MASK,
			      vlanmc->meteridx);
	data[2] |= FIELD_PREP(RTL8365MB_VLAN_MC_D2_ENVLANPOL_MASK,
			      vlanmc->policing_en);
	data[2] |=
		FIELD_PREP(RTL8365MB_VLAN_MC_D2_VBPRI_MASK, vlanmc->priority);
	data[2] |= FIELD_PREP(RTL8365MB_VLAN_MC_D2_VBPEN_MASK,
			      vlanmc->priority_en);
	data[3] |= FIELD_PREP(RTL8365MB_VLAN_MC_D3_EVID_MASK, vlanmc->evid);

	for (i = 0; i < 4; i++) {
		ret = regmap_write(priv->map, RTL8365MB_VLAN_MC_REG(index) + i,
				   data[i]);
		if (ret)
			return ret;
	}

	return 0;
}

int rtl8365mb_vlan_set_vlanmc_entry(
	struct realtek_priv *priv,
	const struct rtl8365mb_vlanmc_entry *vlanmc_entry)
{
	return rtl8365mb_vlan_set_vlanmc(priv, vlanmc_entry->index,
					 &vlanmc_entry->vlanmc);
}

struct rtl8365mb_vlanmc_entry *
rtl8365mb_vlan_alloc_vlanmc_entry(struct rtl8365mb_vlanmc_db *vlanmc_db)
{
	struct rtl8365mb_vlanmc_entry *vlanmc_entry;
	bool found = false;
	int i;

	/* Look for an available VLAN membership config index */
	for (i = 0; i < RTL8365MB_NUM_MEMBERCONFIGS; i++) {
		if (!vlanmc_db->used[i]) {
			found = true;
			break;
		}
	}

	if (!found)
		return ERR_PTR(-ENOSPC);

	vlanmc_entry = kzalloc(sizeof(*vlanmc_entry), GFP_KERNEL);
	if (!vlanmc_entry)
		return ERR_PTR(-ENOMEM);

	/* Mark it used */
	vlanmc_db->used[i] = true;

	/* Initialize the entry. It is NOT guaranteed that the corresponding
	 * in-switch membership config is already zeroed-out. It is up to the
	 * user to program the switch membership config accordingly.
	 */
	vlanmc_entry->vlanmc_db = vlanmc_db;
	vlanmc_entry->index = i;
	refcount_set(&vlanmc_entry->refcnt, 1);

	return vlanmc_entry;
}

void rtl8365mb_vlan_free_vlanmc_entry(
	struct rtl8365mb_vlanmc_entry *vlanmc_entry)
{
	struct rtl8365mb_vlanmc_db *vlanmc_db;

	if (WARN_ON_ONCE(!vlanmc_entry))
		return;

	WARN_ON_ONCE(refcount_read(&vlanmc_entry->refcnt) > 1);

	vlanmc_db = vlanmc_entry->vlanmc_db;

	/* Mark it free for future use */
	vlanmc_db->used[vlanmc_entry->index] = false;

	kfree(vlanmc_entry);
}
