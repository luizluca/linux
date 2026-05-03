// SPDX-License-Identifier: GPL-2.0
/* Look-up table query interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include "rtl8365mb_table.h"
#include "rtl8365mb.h"
#include "rtl8365mb_reg.h"
#include <linux/regmap.h>

static int rtl8365mb_table_poll_busy(struct realtek_priv *priv)
{
	u32 val;

	return regmap_read_poll_timeout(priv->map_nolock,
			RTL8365MB_TABLE_STATUS_REG, val,
			!FIELD_GET(RTL8365MB_TABLE_STATUS_BUSY_FLAG_MASK, val),
			10, 10000);
}

int rtl8365mb_table_query_b(struct realtek_priv *priv,
			    enum rtl8365mb_table table,
			    enum rtl8365mb_table_op op, u16 *addr,
			    enum rtl8365mb_table_l2_method method,
			    u16 port, u16 *data, size_t size)
{
	bool addr_as_input = true;
	bool write_data = false;
	int ret = 0;
	u32 cmd;
	u32 val;

	if (!addr) {
		dev_err(priv->dev, "%s: addr is NULL\n", __func__);
		return -EINVAL;
	}

	if (!data) {
		dev_err(priv->dev, "%s: data is NULL\n", __func__);
		return -EINVAL;
	}

	if (size > RTL8365MB_TABLE_ENTRY_MAX_SIZE) {
		dev_err(priv->dev, "%s: size too big: %zu\n", __func__, size);
		return -E2BIG;
	}

	if (size == 0) {
		dev_err(priv->dev, "%s: size is 0\n", __func__);
		return -EINVAL;
	}

	/* Prepare target table and operation (read or write) */
	cmd = 0;
	cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_TABLE_MASK, table);
	cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_OP_MASK, op);
	if (op == RTL8365MB_TABLE_OP_READ && table == RTL8365MB_TABLE_L2) {
		cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_METHOD_MASK, method);
		switch (method) {
		case RTL8365MB_TABLE_L2_METHOD_MAC:
			write_data = true;
			addr_as_input = false;
			break;
		case RTL8365MB_TABLE_L2_METHOD_ADDR:
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT:
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC:
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_MC:
			break;
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC_PORT:
			cmd |= FIELD_PREP(RTL8365MB_B_TABLE_CTRL_PORT_MASK, port);
			break;
		default:
			return -EINVAL;
		}
	} else if (op == RTL8365MB_TABLE_OP_WRITE) {
		write_data = true;
		if (table == RTL8365MB_TABLE_L2)
			addr_as_input = false;
	}

	/* Validate addr only when used as an input */
	if (addr_as_input) {
		if (!FIELD_FIT(RTL8365MB_TABLE_ADDR_MASK, *addr)) {
			dev_err(priv->dev, "%s: addr %u does not fit in MASK\n",
				__func__, *addr);
			return -EINVAL;
		}
	}

	mutex_lock(&priv->map_lock);

	/* Write entry data if writing to the table (or L2_METHOD_MAC) */
	if (write_data) {
		/* bulk write data up to 9th byte */
		ret = regmap_bulk_write(priv->map_nolock,
					RTL8365MB_TABLE_WRITE_BASE,
					data,
					min_t(size_t, size,
					      RTL8365MB_TABLE_ENTRY_MAX_SIZE - 1));
		if (ret)
			goto out;

		/* 10th register uses only 4 less significant bits */
		if (size == RTL8365MB_TABLE_ENTRY_MAX_SIZE) {
			val = FIELD_PREP(RTL8365MB_TABLE_10TH_DATA_MASK,
					 data[size - 1]);
			ret = regmap_update_bits(priv->map_nolock,
						 RTL8365MB_TABLE_WRITE_10TH_REG,
						 RTL8365MB_TABLE_10TH_DATA_MASK,
						 val);
		}

		if (ret)
			goto out;
	}

	/* Write address (if needed) */
	if (addr_as_input) {
		ret = regmap_write(priv->map_nolock,
				   RTL8365MB_TABLE_ACCESS_ADDR_REG,
				   FIELD_PREP(RTL8365MB_TABLE_ADDR_MASK, *addr));
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
	if (table == RTL8365MB_TABLE_L2) {
		ret = regmap_read(priv->map_nolock, RTL8365MB_TABLE_STATUS_REG,
				  &val);
		if (ret)
			goto out;

		if (!(val & RTL8365MB_TABLE_STATUS_HIT_STATUS_MASK)) {
			ret = -ENOENT;
			goto out;
		}

		*addr = val & RTL8365MB_TABLE_STATUS_ADDRESS_MASK;
	}

	/* Finally, get the table entry if we were reading */
	if (op == RTL8365MB_TABLE_OP_READ) {
		ret = regmap_bulk_read(priv->map_nolock,
				       RTL8365MB_TABLE_READ_BASE,
				       data, size);

		if (size == RTL8365MB_TABLE_ENTRY_MAX_SIZE) {
			val = FIELD_GET(RTL8365MB_TABLE_10TH_DATA_MASK,
					data[size - 1]);
			data[size - 1] = val;
		}
	}

out:
	mutex_unlock(&priv->map_lock);
	return ret;
}

int rtl8365mb_table_query_c(struct realtek_priv *priv,
			    enum rtl8365mb_table table,
			    enum rtl8365mb_table_op op, u16 *addr,
			    enum rtl8365mb_table_l2_method method,
			    u16 port, u16 *data, size_t size)
{
	bool addr_as_input = true;
	bool write_data = false;
	int ret = 0;
	u32 cmd;
	u32 val;
	u32 hit;

	/* Prepare target table and operation (read or write) */
	cmd = 0;
	cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_TABLE_MASK, table);
	cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_OP_MASK, op);
	if (op == RTL8365MB_TABLE_OP_READ && table == RTL8365MB_TABLE_L2) {
		cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_METHOD_MASK, method);
		switch (method) {
		case RTL8365MB_TABLE_L2_METHOD_MAC:
			/*
			 * Method MAC requires as input the same L2 table format
			 * you'll get as result. However, it might only use mac
			 * address and FID/VID fields.
			 */
			write_data = true;

			/* METHOD_MAC does not use addr as input, but may return
			 * the matched index.
			 */
			addr_as_input = false;

			break;
		case RTL8365MB_TABLE_L2_METHOD_ADDR:
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT:
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC:
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_MC:
			break;
		case RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC_PORT:
			cmd |= FIELD_PREP(RTL8365MB_TABLE_CTRL_PORT_MASK, port);
			break;
		default:
			return -EINVAL;
		}
	} else if (op == RTL8365MB_TABLE_OP_WRITE) {
		write_data = true;

		/* Writing to L2 does not use addr as input, as the table index
		 * is derived from key fields.
		 */
		if (table == RTL8365MB_TABLE_L2)
			addr_as_input = false;
	}

	/* To prevent concurrent access to the look-up tables, take the regmap
	 * lock manually and access via the map_nolock regmap.
	 */
	mutex_lock(&priv->map_lock);

	/* Protect from a busy table access (i.e. previous access timeouts) */
	ret = rtl8365mb_table_poll_busy(priv);
	if (ret)
		goto out;

	/* Write entry data if writing to the table (or L2_METHOD_MAC) */
	if (write_data) {
		/* bulk write data up to 9th word */
		ret = regmap_bulk_write(priv->map_nolock,
					RTL8365MB_TABLE_WRITE_BASE,
					data,
					min_t(size_t, size,
					      RTL8365MB_TABLE_ENTRY_MAX_SIZE -
						      1));
		if (ret)
			goto out;

		/* 10th register uses only 4 least significant bits */
		if (size == RTL8365MB_TABLE_ENTRY_MAX_SIZE) {
			val = FIELD_PREP(RTL8365MB_TABLE_10TH_DATA_MASK,
					 data[size - 1]);
			ret = regmap_update_bits(priv->map_nolock,
						 RTL8365MB_TABLE_WRITE_10TH_REG,
						 RTL8365MB_TABLE_10TH_DATA_MASK,
						 val);
		}

		if (ret)
			goto out;
	}

	/* Write address (if needed) */
	if (addr_as_input) {
		ret = regmap_write(priv->map_nolock,
				   RTL8365MB_TABLE_ACCESS_ADDR_REG,
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
	if (table == RTL8365MB_TABLE_L2) {
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
		*addr |= FIELD_GET(RTL8365MB_C_TABLE_STATUS_ADDRESS_EXT_MASK, val)
			 << 11;
		/* only set if it is a L3 address */
		*addr |= FIELD_GET(RTL8365MB_TABLE_STATUS_ADDR_TYPE_MASK, val)
			 << 12;
	}

	/* Finally, get the table entry if we were reading */
	if (op == RTL8365MB_TABLE_OP_READ) {
		ret = regmap_bulk_read(priv->map_nolock,
				       RTL8365MB_TABLE_READ_BASE,
				       data, size);
		if (ret)
			goto out;

		/* For the biggest table entries, the uppermost table
		 * entry register has space for only one nibble. Mask
		 * out the remainder bits. Empirically I saw nothing
		 * wrong with omitting this mask, but it may prevent
		 * unwanted behaviour. FYI.
		 */
		if (size == RTL8365MB_TABLE_ENTRY_MAX_SIZE) {
			val = FIELD_GET(RTL8365MB_TABLE_10TH_DATA_MASK,
					data[size - 1]);
			data[size - 1] = val;
		}
	}

out:
	mutex_unlock(&priv->map_lock);

	return ret;
}

int rtl8365mb_table_query(struct realtek_priv *priv,
			  enum rtl8365mb_table table,
			  enum rtl8365mb_table_op op, u16 *addr,
			  enum rtl8365mb_table_l2_method method,
			  u16 port, u16 *data, size_t size)
{
	struct rtl8365mb *mb = priv->chip_data;

	if (mb->chip_info->family->table_query)
		return mb->chip_info->family->table_query(priv, table, op, addr,
							  method, port, data,
							  size);

	return -EPROTONOSUPPORT;
}
