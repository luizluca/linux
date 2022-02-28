// SPDX-License-Identifier: GPL-2.0
/* Look-up table query interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#ifndef _REALTEK_RTL8365MB_TABLE_H
#define _REALTEK_RTL8365MB_TABLE_H

#include <linux/if_ether.h>
#include <linux/types.h>

#include "realtek.h"

/**
 * struct rtl8365mb_table - available switch tables
 *
 * @RTL8365MB_TABLE_ACL_RULE - ACL rules
 * @RTL8365MB_TABLE_ACL_ACTION - ACL actions
 * @RTL8365MB_TABLE_CVLAN - VLAN4k configurations
 * @RTL8365MB_TABLE_L2 - filtering database (2K hash table)
 *
 * NOTE: Don't change the enum values. They must concur with the field
 * described by @RTL8365MB_TABLE_CTRL_CMD_TARGET_MASK.
 */
enum rtl8365mb_table {
	RTL8365MB_TABLE_ACL_RULE = 1,
	RTL8365MB_TABLE_ACL_ACTION = 2,
	RTL8365MB_TABLE_CVLAN = 3,
	RTL8365MB_TABLE_L2 = 4,
};

/**
 * enum rtl8365mb_table_op - table query operation
 *
 * @RTL8365MB_TABLE_OP_READ: read en entry from the target table
 * @RTL8365MB_TABLE_OP_WRITE: write en entry to the target table
 *
 * NOTE: Don't change the enum values. They must concur with the field
 * described by @RTL8365MB_TABLE_CTRL_CMD_TYPE_MASK.
 */
enum rtl8365mb_table_op {
	RTL8365MB_TABLE_OP_READ = 0,
	RTL8365MB_TABLE_OP_WRITE = 1,
};

/**
 * enum rtl8365mb_table_l2_method - look-up method for read queries of L2 table
 *
 * @RTL8365MB_TABLE_L2_METHOD_MAC: look-up by source MAC address
 *     input arguments:
 *       - @l2.mac_addr - MAC address to search for
 *     output arguments:
 *       - @l2.mac_addr - same as input (no change)
 *       - @l2.addr - address of the entry with the supplied MAC address
 *
 * @RTL8365MB_TABLE_L2_METHOD_ADDR: look-up by entry address
 *     input arguments:
 *       - @l2.addr - entry address
 *     output arguments:
 *       - @l2.addr - same as input (no change)
 *
 * @RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT: look-up next entry after supplied
 *                                       address
 *     input arguments:
 *       - @l2.addr - starting address
 *     output arguments:
 *       - @l2.addr - address of the next entry after starting address;
 *                    if it's the same as the input address then there are
 *                    no other entries in the table
 *
 * @RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC: same as ADDR_NEXT but search only
 *                                          unicast addresses
 *     input arguments:
 *       - @l2.addr - starting address
 *     output arguments:
 *       - @l2.addr - address of the next entry after starting address;
 *                    if it's the same as the input address then there are
 *                    no other entries in the table
 *
 * @RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC_PORT: same as ADDR_NEXT_UC but
 *                                               search only entries with
 *                                               matching source port
 *     input arguments:
 *       - @l2.addr - starting address
 *       - @l2.port - source port
 *     output arguments:
 *       - @l2.addr - address of the next entry after starting address;
 *                    if it's the same as the input address then there are
 *                    no other entries in the table
 *       - @l2.port - same as input (no change)
 *
 * @RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_MC: same as ADDR_NEXT but search only
 *                                          multicast addresses
 *     input arguments:
 *       - @l2.addr - starting address
 *     output arguments:
 *       - @l2.addr - address of the next entry after starting address;
 *                    if it's the same as the input address then there are
 *                    no other entries in the table
 *
 * It goes without saying that the output arguments are only valid if
 * the look-up is successful.
 */
enum rtl8365mb_table_l2_method {
	RTL8365MB_TABLE_L2_METHOD_MAC,
	RTL8365MB_TABLE_L2_METHOD_ADDR,
	RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT,
	RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC,
	RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_MC,
	RTL8365MB_TABLE_L2_METHOD_ADDR_NEXT_UC_PORT = 7,
};

/**
 * struct rtl8365mb_table_query - query format for accessing switch tables
 *
 * @table: the target table per &enum rtl8365mb_table
 * @op: a read or a write per &enum rtl8365mb_table_op
 * @arg: arguments to the query - data may also be passed back on completion
 *
 * @arg.acl_rule: arguments for querying the ACL rules table
 * @arg.acl_rule.addr: address of the ACL rule to read or write
 *
 * @arg.acl_action: arguments for querying the ACL actions table
 * @arg.acl_action.addr: address of the ACL action to read or write
 *
 * @arg.cvlan: arguments for querying the CVLAN (VLAN4k) table
 * @arg.cvlan.addr: address of the CVLAN entry to read or write
 *
 * @arg.l2: arguments for querying the L2 (forwarding database) table
 * @arg.l2.method: ignored on write; determines the lookup method on
 *                 read (see below and &enum rtl8365mb_table_l2_method)
 * @arg.l2.addr: ignored as an input on write and contains the
 *               address of the written entry on write; also ignored
 *               on read if using method MAC
 * @arg.l2.port: ignored unless reading with method NEXT_UC_PORT
 * @arg.l2.mac_addr: ignored unless reading with method MAC
 */
struct rtl8365mb_table_query {
	enum rtl8365mb_table table;
	enum rtl8365mb_table_op op;
	union {
		struct {
			u32 addr;
		} acl_rule;

		struct {
			u32 addr;
		} acl_action;

		struct {
			u32 addr;
		} cvlan;

		struct {
			enum rtl8365mb_table_l2_method method;
			u32 addr;
			u32 port;
			u8 mac_addr[ETH_ALEN];
		} l2;
	} arg;
};

/**
 * rtl8365mb_table_query() - read from or write to a switch table
 * @priv: driver context
 * @query: the query to make, see &struct rtl8365mb_table_query
 * @data: data to read or write
 * @size: size of data in 16-bit words
 *
 * This function handles accessing the various types of table in the
 * switch. Some tables - like ACL tables or CVLAN - are fairly
 * straightforward indexed tables. The L2 table is a hash table and
 * supports a number of access methods when searching. Fortunately all
 * of these tables follow the same underlying access model, which is
 * abstracted away for the rest of the driver here.
 *
 * This function does not assume any interpretation of the data being
 * read from or written to the table: that is up to the caller.
 *
 * When accessing the L2 table, the address argument of the query is
 * overwritten if an entry was found and read from or written to. For
 * some access methods this will be the same as the input; for others
 * it will be required to continue traversing the table. If an error
 * occurs, the returned address should be considered invalid.
 */
int rtl8365mb_table_query(struct realtek_priv *priv,
			  struct rtl8365mb_table_query *query, u16 *data,
			  size_t size);

#endif /* _REALTEK_RTL8365MB_TABLE_H */
