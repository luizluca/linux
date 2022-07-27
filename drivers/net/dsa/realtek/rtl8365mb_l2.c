// SPDX-License-Identifier: GPL-2.0
/* Forwarding and multicast database interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/etherdevice.h>

#include "rtl8365mb_l2.h"
#include "rtl8365mb_table.h"
#include <linux/regmap.h>

#define RTL8365MB_L2_UC_D0_MAC5_MASK 0x00FF
#define RTL8365MB_L2_UC_D0_MAC4_MASK 0xFF00
#define RTL8365MB_L2_UC_D1_MAC3_MASK 0x00FF
#define RTL8365MB_L2_UC_D1_MAC2_MASK 0xFF00
#define RTL8365MB_L2_UC_D2_MAC1_MASK 0x00FF
#define RTL8365MB_L2_UC_D2_MAC0_MASK 0xFF00
#define RTL8365MB_L2_UC_D3_VID_MASK 0x0FFF
#define RTL8365MB_L2_UC_D3_IVL_MASK 0x2000
#define RTL8365MB_L2_UC_D3_PORT_EXT_MASK 0x8000
#define RTL8365MB_L2_UC_D4_EFID_MASK 0x0007
#define RTL8365MB_L2_UC_D4_FID_MASK 0x0078
#define RTL8365MB_L2_UC_D4_SA_PRI_MASK 0x0080
#define RTL8365MB_L2_UC_D4_PORT_MASK 0x0700
#define RTL8365MB_L2_UC_D4_AGE_MASK 0x3800
#define RTL8365MB_L2_UC_D4_AUTH_MASK 0x4000
#define RTL8365MB_L2_UC_D4_SA_BLOCK_MASK 0x8000
#define RTL8365MB_L2_UC_D5_DA_BLOCK_MASK 0x0001
#define RTL8365MB_L2_UC_D5_PRIORITY_MASK 0x000E
#define RTL8365MB_L2_UC_D5_FWD_PRI_MASK 0x0010
#define RTL8365MB_L2_UC_D5_STATIC_MASK 0x0020
// TODO rename other macros ..._Dx_..._MASK instead of ..._ENTRY_Dx_..._MASK
// it saves much more screen space

#define RTL8365MB_L2_MC_MAC5_MASK 0x00FF /* D0 */
#define RTL8365MB_L2_MC_MAC4_MASK 0xFF00 /* D0 */
#define RTL8365MB_L2_MC_MAC3_MASK 0x00FF /* D1 */
#define RTL8365MB_L2_MC_MAC2_MASK 0xFF00 /* D1 */
#define RTL8365MB_L2_MC_MAC1_MASK 0x00FF /* D2 */
#define RTL8365MB_L2_MC_MAC0_MASK 0xFF00 /* D2 */
#define RTL8365MB_L2_MC_VID_MASK 0x0FFF /* D3 */
#define RTL8365MB_L2_MC_IVL_MASK 0x2000 /* D3 */
#define RTL8365MB_L2_MC_MBR_EXT1_MASK 0xC000 /* D3 */

#define RTL8365MB_L2_MC_MBR_MASK 0x00FF /* D4 */
#define RTL8365MB_L2_MC_IGMPIDX_MASK 0xFF00 /* D4 */

#define RTL8365MB_L2_MC_IGMP_ASIC_MASK 0x0001 /* D5 */
#define RTL8365MB_L2_MC_PRIORITY_MASK 0x000E /* D5 */
#define RTL8365MB_L2_MC_FWD_PRI_MASK 0x0010 /* D5 */
#define RTL8365MB_L2_MC_STATIC_MASK 0x0020 /* D5 */
#define RTL8365MB_L2_MC_MBR_EXT2_MASK 0x0080 /* D5 */

/* Port flush command registers - writing a 1 to the port's MASK bit will
 * initiate the flush procedure. Completion is signalled when the corresponding
 * BUSY bit is 0.
 */
#define RTL8365MB_L2_FLUSH_PORT_REG 0x0A36
#define   RTL8365MB_L2_FLUSH_PORT_MASK_MASK 0x00FF
#define   RTL8365MB_L2_FLUSH_PORT_BUSY_MASK 0xFF00

#define RTL8365MB_L2_FLUSH_PORT_EXT_REG 0x0A35
#define   RTL8365MB_L2_FLUSH_PORT_EXT_MASK_MASK 0x0007
#define   RTL8365MB_L2_FLUSH_PORT_EXT_BUSY_MASK 0x0038

#define RTL8365MB_L2_FLUSH_CTRL1_REG 0x0A37
#define   RTL8365MB_L2_FLUSH_CTRL1_VID_MASK 0x0FFF
#define   RTL8365MB_L2_FLUSH_CTRL1_FID_MASK 0xF000

#define RTL8365MB_L2_FLUSH_CTRL2_REG 0x0A38
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_MASK 0x0003
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT 0
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT_VID 1
#define   RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT_FID 2
#define   RTL8365MB_L2_FLUSH_CTRL2_TYPE_MASK 0x0004
#define   RTL8365MB_L2_FLUSH_CTRL2_TYPE_DYNAMIC 0
#define   RTL8365MB_L2_FLUSH_CTRL2_TYPE_BOTH 0

/* TODO: This flushes the entire LUT, reading it back it will turn 0 when the operation is complete */
#define RTL8365MB_L2_FLUSH_CTRL3_REG 0x0A39
#define   RTL8365MB_L2_FLUSH_CTRL3_MASK 0x1

static void rtl8365mb_l2_data_to_uc(const u16 *data, struct rtl8365mb_l2_uc *uc)
{
	uc->key.mac_addr[5] = FIELD_GET(RTL8365MB_L2_UC_D0_MAC5_MASK, data[0]);
	uc->key.mac_addr[4] = FIELD_GET(RTL8365MB_L2_UC_D0_MAC4_MASK, data[0]);
	uc->key.mac_addr[3] = FIELD_GET(RTL8365MB_L2_UC_D1_MAC3_MASK, data[1]);
	uc->key.mac_addr[2] = FIELD_GET(RTL8365MB_L2_UC_D1_MAC2_MASK, data[1]);
	uc->key.mac_addr[1] = FIELD_GET(RTL8365MB_L2_UC_D2_MAC1_MASK, data[2]);
	uc->key.mac_addr[0] = FIELD_GET(RTL8365MB_L2_UC_D2_MAC0_MASK, data[2]);
	uc->key.efid = FIELD_GET(RTL8365MB_L2_UC_D4_EFID_MASK, data[4]);
	uc->key.vid = FIELD_GET(RTL8365MB_L2_UC_D3_VID_MASK, data[3]); // TODO
	uc->key.ivl = FIELD_GET(RTL8365MB_L2_UC_D3_IVL_MASK, data[3]);
	uc->key.fid = FIELD_GET(RTL8365MB_L2_UC_D4_FID_MASK, data[4]);
	uc->age = FIELD_GET(RTL8365MB_L2_UC_D4_AGE_MASK, data[4]);
	uc->auth = FIELD_GET(RTL8365MB_L2_UC_D4_AUTH_MASK, data[4]);
	uc->port = FIELD_GET(RTL8365MB_L2_UC_D4_PORT_MASK, data[4]) |
		   (FIELD_GET(RTL8365MB_L2_UC_D3_PORT_EXT_MASK, data[3]) << 3);
	uc->sa_pri = FIELD_GET(RTL8365MB_L2_UC_D4_SA_PRI_MASK, data[4]);
	uc->fwd_pri = FIELD_GET(RTL8365MB_L2_UC_D5_FWD_PRI_MASK, data[5]);
	uc->sa_block = FIELD_GET(RTL8365MB_L2_UC_D4_SA_BLOCK_MASK, data[4]);
	uc->da_block = FIELD_GET(RTL8365MB_L2_UC_D5_DA_BLOCK_MASK, data[5]);
	uc->priority = FIELD_GET(RTL8365MB_L2_UC_D5_PRIORITY_MASK, data[5]);
	uc->is_static = FIELD_GET(RTL8365MB_L2_UC_D5_STATIC_MASK, data[5]);
}

static void rtl8365mb_l2_uc_to_data(const struct rtl8365mb_l2_uc *uc, u16 *data)
{
	memset(data, 0, 12);
	data[0] |=
		FIELD_PREP(RTL8365MB_L2_UC_D0_MAC5_MASK, uc->key.mac_addr[5]);
	data[0] |=
		FIELD_PREP(RTL8365MB_L2_UC_D0_MAC4_MASK, uc->key.mac_addr[4]);
	data[1] |=
		FIELD_PREP(RTL8365MB_L2_UC_D1_MAC3_MASK, uc->key.mac_addr[3]);
	data[1] |=
		FIELD_PREP(RTL8365MB_L2_UC_D1_MAC2_MASK, uc->key.mac_addr[2]);
	data[2] |=
		FIELD_PREP(RTL8365MB_L2_UC_D2_MAC1_MASK, uc->key.mac_addr[1]);
	data[2] |=
		FIELD_PREP(RTL8365MB_L2_UC_D2_MAC0_MASK, uc->key.mac_addr[0]);
	data[3] |= FIELD_PREP(RTL8365MB_L2_UC_D3_VID_MASK, uc->key.vid); // TODO
	data[3] |= FIELD_PREP(RTL8365MB_L2_UC_D3_IVL_MASK, uc->key.ivl);
	data[3] |= FIELD_PREP(RTL8365MB_L2_UC_D3_PORT_EXT_MASK, uc->port >> 3);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_FID_MASK, uc->key.fid);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_EFID_MASK, uc->key.efid);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_AGE_MASK, uc->age);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_AUTH_MASK, uc->auth);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_PORT_MASK, uc->port);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_SA_PRI_MASK, uc->sa_pri);
	data[4] |= FIELD_PREP(RTL8365MB_L2_UC_D4_SA_BLOCK_MASK, uc->sa_block);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_FWD_PRI_MASK, uc->fwd_pri);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_DA_BLOCK_MASK, uc->da_block);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_PRIORITY_MASK, uc->priority);
	data[5] |= FIELD_PREP(RTL8365MB_L2_UC_D5_STATIC_MASK, uc->is_static);
}

static void rtl8365mb_l2_data_to_mc(const u16 *data, struct rtl8365mb_l2_mc *mc)
{
	mc->key.mac_addr[5] = FIELD_GET(RTL8365MB_L2_MC_MAC5_MASK, data[0]);
	mc->key.mac_addr[4] = FIELD_GET(RTL8365MB_L2_MC_MAC4_MASK, data[0]);
	mc->key.mac_addr[3] = FIELD_GET(RTL8365MB_L2_MC_MAC3_MASK, data[1]);
	mc->key.mac_addr[2] = FIELD_GET(RTL8365MB_L2_MC_MAC2_MASK, data[1]);
	mc->key.mac_addr[1] = FIELD_GET(RTL8365MB_L2_MC_MAC1_MASK, data[2]);
	mc->key.mac_addr[0] = FIELD_GET(RTL8365MB_L2_MC_MAC0_MASK, data[2]);
	mc->key.vid = FIELD_GET(RTL8365MB_L2_MC_VID_MASK, data[3]); // TODO
	mc->key.ivl = FIELD_GET(RTL8365MB_L2_MC_IVL_MASK, data[3]);
	mc->priority = FIELD_GET(RTL8365MB_L2_MC_PRIORITY_MASK, data[5]);
	mc->fwd_pri = FIELD_GET(RTL8365MB_L2_MC_FWD_PRI_MASK, data[5]);
	mc->is_static = FIELD_GET(RTL8365MB_L2_MC_STATIC_MASK, data[5]);
	mc->member = FIELD_GET(RTL8365MB_L2_MC_MBR_MASK, data[4]) |
		     (FIELD_GET(RTL8365MB_L2_MC_MBR_EXT1_MASK, data[3]) << 8) |
		     (FIELD_GET(RTL8365MB_L2_MC_MBR_EXT2_MASK, data[5]) << 8);
	mc->igmpidx = FIELD_GET(RTL8365MB_L2_MC_IGMPIDX_MASK, data[4]);
	mc->igmp_asic = FIELD_GET(RTL8365MB_L2_MC_IGMP_ASIC_MASK, data[5]);
}

static void rtl8365mb_l2_mc_to_data(const struct rtl8365mb_l2_mc *mc, u16 *data)
{
	memset(data, 0, 12);
	data[0] |= FIELD_PREP(RTL8365MB_L2_MC_MAC5_MASK, mc->key.mac_addr[5]);
	data[0] |= FIELD_PREP(RTL8365MB_L2_MC_MAC4_MASK, mc->key.mac_addr[4]);
	data[1] |= FIELD_PREP(RTL8365MB_L2_MC_MAC3_MASK, mc->key.mac_addr[3]);
	data[1] |= FIELD_PREP(RTL8365MB_L2_MC_MAC2_MASK, mc->key.mac_addr[2]);
	data[2] |= FIELD_PREP(RTL8365MB_L2_MC_MAC1_MASK, mc->key.mac_addr[1]);
	data[2] |= FIELD_PREP(RTL8365MB_L2_MC_MAC0_MASK, mc->key.mac_addr[0]);
	data[3] |= FIELD_PREP(RTL8365MB_L2_MC_VID_MASK, mc->key.vid);
	data[3] |= FIELD_PREP(RTL8365MB_L2_MC_IVL_MASK, mc->key.ivl);
	data[3] |= FIELD_PREP(RTL8365MB_L2_MC_MBR_EXT1_MASK, mc->member >> 8);
	data[4] |= FIELD_PREP(RTL8365MB_L2_MC_MBR_MASK, mc->member);
	data[4] |= FIELD_PREP(RTL8365MB_L2_MC_IGMPIDX_MASK, mc->igmpidx);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_IGMP_ASIC_MASK, mc->igmp_asic);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_PRIORITY_MASK, mc->priority);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_FWD_PRI_MASK, mc->fwd_pri);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_STATIC_MASK, 1);
	data[5] |= FIELD_PREP(RTL8365MB_L2_MC_MBR_EXT2_MASK, mc->member >> 10);
}

int rtl8365mb_l2_get_uc_by_addr(struct realtek_priv *priv, int addr,
				struct rtl8365mb_l2_uc *uc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.op = RTL8365MB_TABLE_OP_READ,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_ADDR,
		.arg.l2.addr = addr,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Search for the next valid L2 unicast table entry, starting from the
	 * supplied table entry address. The table query function will return
	 * the address of that table entry into query.arg.l2.addr.
	 */
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* Opportunistically assume it is a unicast address and convert */
	rtl8365mb_l2_data_to_uc(data, uc);

	/* If the entry is multicast, we are returning junk - say so */
	if (is_multicast_ether_addr(uc->key.mac_addr))
		return -EINVAL;

	return 0;
}

int rtl8365mb_l2_get_mc_by_addr(struct realtek_priv *priv, int addr,
				struct rtl8365mb_l2_mc *mc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.op = RTL8365MB_TABLE_OP_READ,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_ADDR,
		.arg.l2.addr = addr,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Search for the next valid L2 unicast table entry, starting from the
	 * supplied table entry address. The table query function will return
	 * the address of that table entry into query.arg.l2.addr.
	 */
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* Opportunistically assume it is a multicast address and convert */
	rtl8365mb_l2_data_to_mc(data, mc);

	/* If the entry isn't multicast, we are returning junk - say so */
	if (!is_multicast_ether_addr(mc->key.mac_addr))
		return -EINVAL;

	return 0;
}

int rtl8365mb_l2_get_next_uc(struct realtek_priv *priv, int *addr,
			     struct rtl8365mb_l2_uc *uc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.op = RTL8365MB_TABLE_OP_READ,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC,
		.arg.l2.addr = *addr,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Search for the next valid L2 unicast table entry, starting from the
	 * supplied table entry address. The table query function will return
	 * the address of that table entry into query.arg.l2.addr.
	 */
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* Success. We now return the entry and its address to the user. */
	rtl8365mb_l2_data_to_uc(data, uc);
	*addr = query.arg.l2.addr;

	return 0;
}

int rtl8365mb_l2_get_uc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_uc_key *key,
			struct rtl8365mb_l2_uc *uc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.op = RTL8365MB_TABLE_OP_READ,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_MAC,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Prepare key */
	memset(uc, 0, sizeof(*uc));
	memcpy(&uc->key, key, sizeof(*key));
	rtl8365mb_l2_uc_to_data(uc, data);

	/* Perform query */
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* Return entry */
	rtl8365mb_l2_data_to_uc(data, uc);

	return 0;
}

int rtl8365mb_l2_add_uc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_uc *uc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_MAC,
	};
	bool new_entry = false;
	u16 data[6] = { 0 };
	int ret;

	/* First we check if an entry with the given key (MAC address, etc.)
	 * exists in the table. If so, we are just going to update it. Otherwise
	 * we are adding a new entry, in which case it is necessary to check
	 * whether or not the operation succeeded. TODO expound on this
	 * TODO in sja driver a warning is emitted when another entry has to be
	 * evicted, just fyi
	 */
	query.op = RTL8365MB_TABLE_OP_READ;
	rtl8365mb_l2_uc_to_data(uc, data); // lookup should only care about key

	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret == -ENOENT)
		new_entry = true;
	else if (ret)
		return ret;

	/* TODO: This whole song and dance seems too much. Needs testing. Can't
	we just write and check the return code (based on hit status bit)? */
	query.op = RTL8365MB_TABLE_OP_WRITE;
	rtl8365mb_l2_uc_to_data(uc, data);
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* so apparently we have to check it worked by reading back? */
	/* TODO: the above WRITE query should have returned an error surely?
	TODO Test this theory */
	if (new_entry) {
		query.op = RTL8365MB_TABLE_OP_READ;
		ret = rtl8365mb_table_query(priv, &query, data,
					    ARRAY_SIZE(data));
		if (ret == -ENOENT)
			return -ENOSPC;
		else if (ret)
			return ret;

		dev_info(priv->dev, "XXX added addr=%d\n", query.arg.l2.addr);
	}

	return 0;
}

int rtl8365mb_l2_del_uc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_uc_key *key)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_MAC,
	};
	struct rtl8365mb_l2_uc uc = {
		.key = *key,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Check if an entry with this key exists */
	query.op = RTL8365MB_TABLE_OP_READ;
	rtl8365mb_l2_uc_to_data(&uc, data);

	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* If it exists, then delete it by writing all zeros besides the key */
	query.op = RTL8365MB_TABLE_OP_WRITE;
	memset(&uc, 0, sizeof(uc));
	uc.key = *key;
	rtl8365mb_l2_uc_to_data(&uc, data);

	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	return 0;
}

int rtl8365mb_l2_flush(struct realtek_priv *priv, int port, u16 vid)
{
	int mode = vid ? RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT_VID :
			 RTL8365MB_L2_FLUSH_CTRL2_MODE_PORT;
	u32 val;
	int ret;

	mutex_lock(&priv->map_lock);

	/* Configure flushing mode; only flush dynamic entries */
	ret = regmap_write(
		priv->map_nolock, RTL8365MB_L2_FLUSH_CTRL2_REG,
		FIELD_PREP(RTL8365MB_L2_FLUSH_CTRL2_MODE_MASK, mode) |
			FIELD_PREP(RTL8365MB_L2_FLUSH_CTRL2_TYPE_MASK,
				   RTL8365MB_L2_FLUSH_CTRL2_TYPE_DYNAMIC));
	if (ret)
		goto out;

	ret = regmap_write(priv->map_nolock, RTL8365MB_L2_FLUSH_CTRL1_REG,
			   FIELD_PREP(RTL8365MB_L2_FLUSH_CTRL1_VID_MASK, vid));

	/* Now issue the flush command and wait for its completion. There are
	 * two registers for this purpose, and which one to use depends on the
	 * port number. The _EXT register is for ports 8 or higher.
	 */
	if (port < 8) {
		ret = regmap_write(priv->map_nolock,
				   RTL8365MB_L2_FLUSH_PORT_REG,
				   FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_MASK_MASK,
					      BIT(port) & 0xFF));
		if (ret)
			goto out;

		ret = regmap_read_poll_timeout(
			priv->map_nolock, RTL8365MB_L2_FLUSH_PORT_REG, val,
			!(val & FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_BUSY_MASK,
					   BIT(port) & 0xFF)),
			10, 100);
		if (ret)
			goto out;
	} else {
		ret = regmap_write(
			priv->map_nolock, RTL8365MB_L2_FLUSH_PORT_EXT_REG,
			FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_EXT_MASK_MASK,
				   BIT(port) >> 8));
		if (ret)
			goto out;

		ret = regmap_read_poll_timeout(
			priv->map_nolock, RTL8365MB_L2_FLUSH_PORT_EXT_REG, val,
			!(val &
			  FIELD_PREP(RTL8365MB_L2_FLUSH_PORT_EXT_BUSY_MASK,
				     BIT(port) >> 8)),
			10, 100);
		if (ret)
			goto out;
	}

out:
	mutex_unlock(&priv->map_lock);

	return ret;
}

int rtl8365mb_l2_get_next_mc(struct realtek_priv *priv, int *addr,
			     struct rtl8365mb_l2_mc *mc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.op = RTL8365MB_TABLE_OP_READ,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_MC,
		.arg.l2.addr = *addr,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Search for the next valid L2 multicast table entry, starting from the
	 * supplied table entry address. The table query function will return
	 * the address of that table entry into query.arg.l2.addr.
	 */
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* Success. We now return the entry and its address to the user. */
	rtl8365mb_l2_data_to_mc(data, mc);
	*addr = query.arg.l2.addr;

	return 0;
}


int rtl8365mb_l2_get_mc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_mc_key *key,
			struct rtl8365mb_l2_mc *mc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.op = RTL8365MB_TABLE_OP_READ,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_MAC,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Prepare key */
	memset(mc, 0, sizeof(*mc));
	memcpy(&mc->key, key, sizeof(*key));
	rtl8365mb_l2_mc_to_data(mc, data);

	/* Perform query */
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* Return entry */
	rtl8365mb_l2_data_to_mc(data, mc);

	return 0;
}

int rtl8365mb_l2_add_mc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_mc *mc)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_MAC,
	};
	bool new_entry = false;
	u16 data[6] = { 0 };
	int ret;

	/* First we check if an entry with the given key (MAC address, etc.)
	 * exists in the table. If so, we are just going to update it. Otherwise
	 * we are adding a new entry, in which case it is necessary to check
	 * whether or not the operation succeeded. TODO expound on this
	 * TODO in sja driver a warning is emitted when another entry has to be
	 * evicted, just fyi
	 */
	query.op = RTL8365MB_TABLE_OP_READ;
	rtl8365mb_l2_mc_to_data(mc, data); // lookup should only care about key

	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret == -ENOENT)
		new_entry = true;
	else if (ret)
		return ret;

	/* TODO: This whole song and dance seems too much. Needs testing. Can't
	we just write and check the return code (based on hit status bit)? */
	query.op = RTL8365MB_TABLE_OP_WRITE;
	rtl8365mb_l2_mc_to_data(mc, data);
	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* so apparently we have to check it worked by reading back? */
	/* TODO: the above WRITE query should have returned an error surely?
	TODO Test this theory */
	if (new_entry) {
		query.op = RTL8365MB_TABLE_OP_READ;
		ret = rtl8365mb_table_query(priv, &query, data,
					    ARRAY_SIZE(data));
		if (ret == -ENOENT)
			return -ENOSPC;
		else if (ret)
			return ret;
	}

	return 0;
}

int rtl8365mb_l2_del_mc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_mc_key *key)
{
	struct rtl8365mb_table_query query = {
		.table = RTL8365MB_TABLE_L2,
		.arg.l2.method = RTL8365MB_TABLE_L2_METHOD_MAC,
	};
	struct rtl8365mb_l2_mc mc = {
		.key = *key,
	};
	u16 data[6] = { 0 };
	int ret;

	/* Check if an entry with this key exists */
	query.op = RTL8365MB_TABLE_OP_READ;
	rtl8365mb_l2_mc_to_data(&mc, data);

	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	/* If it exists, then delete it by writing all zeros besides the key */
	query.op = RTL8365MB_TABLE_OP_WRITE;
	memset(&mc, 0, sizeof(mc));
	mc.key = *key;
	rtl8365mb_l2_mc_to_data(&mc, data);

	ret = rtl8365mb_table_query(priv, &query, data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	return 0;
}
