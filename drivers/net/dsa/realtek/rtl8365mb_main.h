/* SPDX-License-Identifier: GPL-2.0 */
/* Main driver code for the rtl8365mb switch family
 *
 * Copyright (C) 2026 Luiz Angelo Daros de Luca <luizluca@gmail.com>
 *
 */

#ifndef _REALTEK_RTL8365MB_MAIN_H
#define _REALTEK_RTL8365MB_MAIN_H

#include <linux/types.h>

#include "realtek.h"

#define RTL8365MB_MAX_NUM_PORTS		11
#define RTL8365MB_MAX_NUM_EXTINTS	3

/**
 * struct rtl8365mb_extint - external interface info
 * @port: the port with an external interface
 * @id: the external interface ID, which is either 0, 1, or 2
 * @supported_interfaces: a bitmask of supported PHY interface modes
 *
 * Represents a mapping: port -> { id, supported_interfaces }. To be embedded
 * in &struct rtl8365mb_chip_info for every port with an external interface.
 */
struct rtl8365mb_extint {
	int port;
	int id;
	unsigned int supported_interfaces;
};

/**
 * struct rtl8365mb_chip_info - static chip-specific info
 * @name: human-readable chip name
 * @chip_id: chip identifier
 * @chip_ver: chip silicon revision
 * @extints: available external interfaces
 * @jam_table: chip-specific initialization jam table
 * @jam_size: size of the chip's jam table
 *
 * These data are specific to a given chip in the family of switches supported
 * by this driver. When adding support for another chip in the family, a new
 * chip info should be added to the rtl8365mb_chip_infos array.
 */
struct rtl8365mb_chip_info {
	const char *name;
	u32 chip_id;
	u32 chip_ver;
	char family;
	const struct rtl8365mb_extint extints[RTL8365MB_MAX_NUM_EXTINTS];
	const struct rtl8365mb_jam_tbl_entry *jam_table;
	size_t jam_size;
};

enum rtl8365mb_cpu_insert {
	RTL8365MB_CPU_INSERT_TO_ALL = 0,
	RTL8365MB_CPU_INSERT_TO_TRAPPING = 1,
	RTL8365MB_CPU_INSERT_TO_NONE = 2,
};

enum rtl8365mb_cpu_position {
	RTL8365MB_CPU_POS_AFTER_SA = 0,
	RTL8365MB_CPU_POS_BEFORE_CRC = 1,
};

enum rtl8365mb_cpu_format {
	RTL8365MB_CPU_FORMAT_8BYTES = 0,
	RTL8365MB_CPU_FORMAT_4BYTES = 1,
};

enum rtl8365mb_cpu_rxlen {
	RTL8365MB_CPU_RXLEN_72BYTES = 0,
	RTL8365MB_CPU_RXLEN_64BYTES = 1,
};

/**
 * struct rtl8365mb_cpu - CPU port configuration
 * @enable: enable/disable hardware insertion of CPU tag in switch->CPU frames
 * @mask: port mask of ports that parse should parse CPU tags
 * @trap_port: forward trapped frames to this port
 * @insert: CPU tag insertion mode in switch->CPU frames
 * @position: position of CPU tag in frame
 * @rx_length: minimum CPU RX length
 * @format: CPU tag format
 *
 * Represents the CPU tagging and CPU port configuration of the switch. These
 * settings are configurable at runtime.
 */
struct rtl8365mb_cpu {
	bool enable;
	u32 mask;
	u32 trap_port;
	enum rtl8365mb_cpu_insert insert;
	enum rtl8365mb_cpu_position position;
	enum rtl8365mb_cpu_rxlen rx_length;
	enum rtl8365mb_cpu_format format;
};

/**
 * struct rtl8365mb_port - private per-port data
 * @priv: pointer to parent realtek_priv data
 * @index: DSA port index, same as dsa_port::index
 * @stats: link statistics populated by rtl8365mb_stats_poll, ready for atomic
 *         access via rtl8365mb_get_stats64
 * @stats_lock: protect the stats structure during read/update
 * @mib_work: delayed work for polling MIB counters
 */
struct rtl8365mb_port {
	struct realtek_priv *priv;
	unsigned int index;
	struct rtnl_link_stats64 stats;
	spinlock_t stats_lock;
	struct delayed_work mib_work;
};

/**
 * struct rtl8365mb - driver private data
 * @priv: pointer to parent realtek_priv data
 * @irq: registered IRQ or zero
 * @chip_info: chip-specific info about the attached switch
 * @cpu: CPU tagging and CPU port configuration for this chip
 * @mib_lock: prevent concurrent reads of MIB counters
 * @l2_lock: prevent concurrent access to L2 look-up table
 * @ports: per-port data
 *
 * Private data for this driver.
 */
struct rtl8365mb {
	struct realtek_priv *priv;
	int irq;
	const struct rtl8365mb_chip_info *chip_info;
	struct rtl8365mb_cpu cpu;
	struct mutex mib_lock;
	/* l2_lock is used to prevent concurrent modifications of L2 table
	 * entries while another function is reading it. l2_(add,del)_mc
	 * is an example that first read current table entry and then
	 * create/update it. l2_(add|del)_uc uses a single table op and,
	 * internally, it might not need this lock. However, altering FDB
	 * may still collide, as well as l2_flush, with fdb_dump iterating
	 * over FDB.
	 */
	struct mutex l2_lock;
	struct rtl8365mb_port ports[RTL8365MB_MAX_NUM_PORTS];
};

#endif /* _REALTEK_RTL8365MB_MAIN_H */
