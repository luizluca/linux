/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _REALTEK_RTL8365MB_H
#define _REALTEK_RTL8365MB_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/if_ether.h>
#include "realtek.h"
#include "rtl8365mb_table.h"

#define RTL8365MB_MAX_NUM_PORTS		11
#define RTL8365MB_MAX_NUM_EXTINTS	3

enum rtl8365mb_phy_interface_mode {
	RTL8365MB_PHY_INTERFACE_MODE_INVAL = 0,
	RTL8365MB_PHY_INTERFACE_MODE_INTERNAL = BIT(0),
	RTL8365MB_PHY_INTERFACE_MODE_MII = BIT(1),
	RTL8365MB_PHY_INTERFACE_MODE_TMII = BIT(2),
	RTL8365MB_PHY_INTERFACE_MODE_RMII = BIT(3),
	RTL8365MB_PHY_INTERFACE_MODE_RGMII = BIT(4),
	RTL8365MB_PHY_INTERFACE_MODE_SGMII = BIT(5),
	RTL8365MB_PHY_INTERFACE_MODE_HSGMII = BIT(6),
};

struct rtl8365mb_extint {
	int port;
	int id;
	unsigned int supported_interfaces;
};

enum rtl8365mb_family_id {
	RTL8365MB_FAMILY_A,
	RTL8365MB_FAMILY_B,
	RTL8365MB_FAMILY_C,
	RTL8365MB_FAMILY_D,
};

enum rtl8365mb_mib_counter_index {
	RTL8365MB_MIB_ifInOctets,
	RTL8365MB_MIB_dot3StatsFCSErrors,
	RTL8365MB_MIB_dot3StatsSymbolErrors,
	RTL8365MB_MIB_dot3InPauseFrames,
	RTL8365MB_MIB_dot3ControlInUnknownOpcodes,
	RTL8365MB_MIB_etherStatsFragments,
	RTL8365MB_MIB_etherStatsJabbers,
	RTL8365MB_MIB_ifInUcastPkts,
	RTL8365MB_MIB_etherStatsDropEvents,
	RTL8365MB_MIB_ifInMulticastPkts,
	RTL8365MB_MIB_ifInBroadcastPkts,
	RTL8365MB_MIB_inMldChecksumError,
	RTL8365MB_MIB_inIgmpChecksumError,
	RTL8365MB_MIB_inMldSpecificQuery,
	RTL8365MB_MIB_inMldGeneralQuery,
	RTL8365MB_MIB_inIgmpSpecificQuery,
	RTL8365MB_MIB_inIgmpGeneralQuery,
	RTL8365MB_MIB_inMldLeaves,
	RTL8365MB_MIB_inIgmpLeaves,
	RTL8365MB_MIB_etherStatsOctets,
	RTL8365MB_MIB_etherStatsUnderSizePkts,
	RTL8365MB_MIB_etherOversizeStats,
	RTL8365MB_MIB_etherStatsPkts64Octets,
	RTL8365MB_MIB_etherStatsPkts65to127Octets,
	RTL8365MB_MIB_etherStatsPkts128to255Octets,
	RTL8365MB_MIB_etherStatsPkts256to511Octets,
	RTL8365MB_MIB_etherStatsPkts512to1023Octets,
	RTL8365MB_MIB_etherStatsPkts1024to1518Octets,
	RTL8365MB_MIB_ifOutOctets,
	RTL8365MB_MIB_dot3StatsSingleCollisionFrames,
	RTL8365MB_MIB_dot3StatsMultipleCollisionFrames,
	RTL8365MB_MIB_dot3StatsDeferredTransmissions,
	RTL8365MB_MIB_dot3StatsLateCollisions,
	RTL8365MB_MIB_etherStatsCollisions,
	RTL8365MB_MIB_dot3StatsExcessiveCollisions,
	RTL8365MB_MIB_dot3OutPauseFrames,
	RTL8365MB_MIB_ifOutDiscards,
	RTL8365MB_MIB_dot1dTpPortInDiscards,
	RTL8365MB_MIB_ifOutUcastPkts,
	RTL8365MB_MIB_ifOutMulticastPkts,
	RTL8365MB_MIB_ifOutBroadcastPkts,
	RTL8365MB_MIB_outOampduPkts,
	RTL8365MB_MIB_inOampduPkts,
	RTL8365MB_MIB_inIgmpJoinsSuccess,
	RTL8365MB_MIB_inIgmpJoinsFail,
	RTL8365MB_MIB_inMldJoinsSuccess,
	RTL8365MB_MIB_inMldJoinsFail,
	RTL8365MB_MIB_inReportSuppressionDrop,
	RTL8365MB_MIB_inLeaveSuppressionDrop,
	RTL8365MB_MIB_outIgmpReports,
	RTL8365MB_MIB_outIgmpLeaves,
	RTL8365MB_MIB_outIgmpGeneralQuery,
	RTL8365MB_MIB_outIgmpSpecificQuery,
	RTL8365MB_MIB_outMldReports,
	RTL8365MB_MIB_outMldLeaves,
	RTL8365MB_MIB_outMldGeneralQuery,
	RTL8365MB_MIB_outMldSpecificQuery,
	RTL8365MB_MIB_inKnownMulticastPkts,
	RTL8365MB_MIB_END,
};

struct rtl8365mb_mib_counter {
	u32 offset;
	u32 length;
	const char *name;
};

struct rtl8365mb_family_info {
	enum rtl8365mb_family_id family_id;
	const char *name;
	int num_ports;
	const struct rtl8365mb_jam_tbl_entry *jam_table;
	const size_t *jam_size;
	int (*table_query)(struct realtek_priv *priv,
			   enum rtl8365mb_table table,
			   enum rtl8365mb_table_op op, u16 *addr,
			   enum rtl8365mb_table_l2_method method,
			   u16 port, u16 *data, size_t size);
	const struct rtl8365mb_mib_counter *mib_counters;
	u32 mib_port_offset;
	int (*l2_flush)(struct realtek_priv *priv, int port, u16 vid);
};

struct rtl8365mb_chip_info {
	const char *name;
	u32 chip_id;
	u32 chip_ver;
	const struct rtl8365mb_family_info *family;
	const struct rtl8365mb_extint extints[RTL8365MB_MAX_NUM_EXTINTS];
	const struct rtl8365mb_jam_tbl_entry *jam_table;
	const size_t *jam_size;
};

enum rtl8365mb_stp_state {
	RTL8365MB_STP_STATE_DISABLED = 0,
	RTL8365MB_STP_STATE_BLOCKING = 1,
	RTL8365MB_STP_STATE_LEARNING = 2,
	RTL8365MB_STP_STATE_FORWARDING = 3,
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

struct rtl8365mb_cpu {
	bool enable;
	u32 mask;
	u32 trap_port;
	enum rtl8365mb_cpu_insert insert;
	enum rtl8365mb_cpu_position position;
	enum rtl8365mb_cpu_rxlen rx_length;
	enum rtl8365mb_cpu_format format;
};

struct rtl8365mb_port {
	struct realtek_priv *priv;
	unsigned int index;
	spinlock_t stats_lock;
	struct delayed_work mib_work;
	u64 stats_cache[RTL8365MB_MIB_END];
};

struct rtl8365mb {
	struct realtek_priv *priv;
	int irq;
	const struct rtl8365mb_chip_info *chip_info;
	struct rtl8365mb_cpu cpu;
	struct mutex mib_lock;
	struct rtl8365mb_port ports[RTL8365MB_MAX_NUM_PORTS];
};

#endif /* _REALTEK_RTL8365MB_H */
