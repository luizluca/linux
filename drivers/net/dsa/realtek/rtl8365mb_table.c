// SPDX-License-Identifier: GPL-2.0
/* Look-up table query interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include "rtl8365mb_table.h"
#include <linux/regmap.h>

/* Table read/write registers */
#define RTL8365MB_TABLE_READ_BASE	0x0520
#define RTL8365MB_TABLE_READ_REG(_x) \
		(RTL8365MB_TABLE_READ_BASE + (_x))
#define RTL8365MB_TABLE_WRITE_BASE	0x0510
#define RTL8365MB_TABLE_WRITE_REG(_x) \
		(RTL8365MB_TABLE_WRITE_BASE + (_x))

#define RTL8365MB_TABLE_ENTRY_MAX_SIZE	10

/* Table access control register
 *
 * NOTE: PORT_MASK is only 4 bit, which suggests that port-based
 * look-up of the L2 table only works for physical port addresses
 * 0~4. It could be that the Realtek driver is out-of-date and
 * actually the mask is something like 0xFF00, but this is
 * unconfirmed.
 */
#define RTL8365MB_TABLE_CTRL_REG			0x0500
#define   RTL8365MB_TABLE_CTRL_PORT_MASK		0x0F00
#define   RTL8365MB_TABLE_CTRL_TARGET_MASK		0x0007
#define     RTL8365MB_TABLE_CTRL_TARGET_ACLRULE		1
#define     RTL8365MB_TABLE_CTRL_TARGET_ACLACT		2
#define     RTL8365MB_TABLE_CTRL_TARGET_CVLAN		3
#define     RTL8365MB_TABLE_CTRL_TARGET_L2		4
#define     RTL8365MB_TABLE_CTRL_TARGET_IGMP_GROUP	5
#define   RTL8365MB_TABLE_CTRL_CMD_TYPE_MASK		0x0008
#define     RTL8365MB_TABLE_CTRL_CMD_TYPE_READ		0
#define     RTL8365MB_TABLE_CTRL_CMD_TYPE_WRITE		1
#define   RTL8365MB_TABLE_CTRL_METHOD_MASK		0x0070

/* Table access address register */
#define RTL8365MB_TABLE_ADDR_REG			0x0501
#define   RTL8365MB_TABLE_ADDR_MASK			0x1FFF

/* Table status register */
#define RTL8365MB_TABLE_STATUS_REG			0x0502
#define   RTL8365MB_TABLE_STATUS_ADDRESS_EXT_MASK	0x4000
#define   RTL8365MB_TABLE_STATUS_BUSY_FLAG_MASK		0x2000
#define   RTL8365MB_TABLE_STATUS_HIT_STATUS_MASK	0x1000
#define   RTL8365MB_TABLE_STATUS_TYPE_MASK		0x0800
#define   RTL8365MB_TABLE_STATUS_ADDRESS_MASK		0x07FF

static int rtl8365mb_table_poll_busy(struct realtek_priv *priv)
{
	u32 val;

	return regmap_read_poll_timeout(
		priv->map_nolock, RTL8365MB_TABLE_STATUS_REG, val,
		(val & RTL8365MB_TABLE_STATUS_BUSY_FLAG_MASK) == 0, 10, 100);
}

int rtl8365mb_table_query(struct realtek_priv *priv,
			  struct rtl8365mb_table_query *query, u16 *data,
			  size_t size)
{
	u32 *addr;
	u32 cmd;
	u32 val;
	u32 hit;
	int ret;
	int i;

	if (size > RTL8365MB_TABLE_ENTRY_MAX_SIZE)
		return -E2BIG;

	/* Prepare address */
	switch (query->table) {
	case RTL8365MB_TABLE_ACL_RULE:
		addr = &query->arg.acl_rule.addr;
		break;
	case RTL8365MB_TABLE_ACL_ACTION:
		addr = &query->arg.acl_action.addr;
		break;
	case RTL8365MB_TABLE_CVLAN:
		addr = &query->arg.cvlan.addr;
		break;
	case RTL8365MB_TABLE_L2:
		addr = &query->arg.l2.addr;
		break;
	default:
		return -EINVAL;
	}

	/* To prevent concurrent access to the look-up tables, take the regmap
	 * lock manually and access via the map_nolock regmap.
	 */
	mutex_lock(&priv->map_lock);

	/* Prepare target table and operation (read or write) */
	cmd = 0;
	cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_TARGET_MASK, query->table);
	cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_CMD_TYPE_MASK, query->op);

	/* Additional handling for reading the L2 table */
	if (query->op == RTL8365MB_TABLE_OP_READ &&
	    query->table == RTL8365MB_TABLE_L2) {
		/* Prepare the access method */
		cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_METHOD_MASK,
				  query->arg.l2.method);

		/* Prepare source port if using method NEXT_UC_PORT */
		if (query->arg.l2.method ==
		    RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC_PORT) {
			cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_PORT_MASK,
					  query->arg.l2.port);
		}

		/* Write input data to WRITE registers if using method MAC.
		 * Yes, that's how it works.
		 */
		if (query->arg.l2.method == RTL8365MB_TABLE_L2_METHOD_MAC) {
			for (i = 0; i < size; i++) {
				ret = regmap_write(priv->map_nolock,
						   RTL8365MB_TABLE_WRITE_REG(i),
						   data[i]);
				if (ret)
					goto out;
			}
		}
	}

	/* Write entry data if writing to the table */
	if (query->op == RTL8365MB_TABLE_OP_WRITE) {
		for (i = 0; i < size; i++) {
			ret = regmap_write(priv->map_nolock,
					   RTL8365MB_TABLE_WRITE_REG(i),
					   data[i]);
			if (ret)
				goto out;
			if (query->table == RTL8365MB_TABLE_ACL_RULE ||
			    query->table == RTL8365MB_TABLE_ACL_ACTION)
				dev_info(priv->dev, "%04x\n", data[i]);
		}
	}

	/* Write address, except for L2 MAC lookup */
	if (query->table != RTL8365MB_TABLE_L2 ||
	    query->arg.l2.method != RTL8365MB_TABLE_L2_METHOD_MAC) {
		ret = regmap_write(priv->map_nolock, RTL8365MB_TABLE_ADDR_REG,
				   FIELD_PREP(RTL8365MB_TABLE_ADDR_MASK,
					      *addr));
		if (ret)
			goto out;
	}

	/* Execute */
	ret = regmap_write(priv->map_nolock, RTL8365MB_TABLE_CTRL_REG, cmd);
	if (ret)
		goto out;

	/* Poll for completion */
	ret = rtl8365mb_table_poll_busy(priv);
	if (ret)
		goto out;

	/* For both reads and writes to the L2 table, check status */
	if (query->table == RTL8365MB_TABLE_L2) {
		ret = regmap_read(priv->map_nolock, RTL8365MB_TABLE_STATUS_REG,
				  &val);
		if (ret)
			goto out;

		/* Did the query find an entry? */
		hit = FIELD_GET(RTL8365MB_TABLE_STATUS_HIT_STATUS_MASK, val);
		if (!hit) {
			ret = -ENOENT;
			goto out;
		}

		/* If so, extract the address */
		*addr = 0;
		*addr |= FIELD_GET(RTL8365MB_TABLE_STATUS_ADDRESS_MASK, val);
		*addr |= FIELD_GET(RTL8365MB_TABLE_STATUS_ADDRESS_EXT_MASK, val)
			 << 11;
		*addr |= FIELD_GET(RTL8365MB_TABLE_STATUS_TYPE_MASK, val) << 12;
	}

	/* Finally, get the table entry if we were reading */
	if (query->op == RTL8365MB_TABLE_OP_READ) {
		for (i = 0; i < size; i++) {
			ret = regmap_read(priv->map_nolock,
					  RTL8365MB_TABLE_READ_REG(i), &val);
			if (ret)
				goto out;

			/* For the biggest table entries, the uppermost table
			 * entry register has space for only one nibble. Mask
			 * out the remainder bits. Empirically I saw nothing
			 * wrong with ommitting this mask, but it may prevent
			 * unwanted behaviour. FYI.
			 */
			if (i == RTL8365MB_TABLE_ENTRY_MAX_SIZE - 1)
				val &= 0xF;

			data[i] = val;
		}
	}

out:
	mutex_unlock(&priv->map_lock);

	return ret;
}
