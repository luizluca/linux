// SPDX-License-Identifier: GPL-2.0
/* Realtek SMI subdriver for the Realtek RTL8365MB-VC ethernet switch.
 *
 * Copyright (C) 2021 Alvin Šipraga <alsi@bang-olufsen.dk>
 * Copyright (C) 2021 Michael Rasmussen <mir@bang-olufsen.dk>
 *
 * The RTL8365MB-VC is a 4+1 port 10/100/1000M switch controller. It includes 4
 * integrated PHYs for the user facing ports, and an extension interface which
 * can be connected to the CPU - or another PHY - via either MII, RMII, RGMII,
 * SGMII or HSGMII. The switch is configured via the Realtek Simple Management
 * Interface (SMI), which uses the MDIO/MDC lines.
 *
 * Below is a simplified block diagram of the chip and its relevant interfaces.
 *
 *                          .-----------------------------------.
 *                          |                                   |
 *         UTP <---------------> Giga PHY <-> PCS <-> P0 GMAC   |
 *         UTP <---------------> Giga PHY <-> PCS <-> P1 GMAC   |
 *         UTP <---------------> Giga PHY <-> PCS <-> P2 GMAC   |
 *         UTP <---------------> Giga PHY <-> PCS <-> P3 GMAC   |
 *                          |                                   |
 *     CPU/PHY <-MII/RMII/RGMII--->  Extension  <---> Extension |
 *                          |       interface 1        GMAC 1   |
 *                          |                                   |
 *     SMI driver/ <-MDC/SCL---> Management    ~~~~~~~~~~~~~~   |
 *        EEPROM   <-MDIO/SDA--> interface     ~REALTEK ~~~~~   |
 *                          |                  ~RTL8365MB ~~~   |
 *                          |                  ~GXXXC TAIWAN~   |
 *        GPIO <--------------> Reset          ~~~~~~~~~~~~~~   |
 *                          |                                   |
 *      Interrupt  <----------> Link UP/DOWN events             |
 *      controller          |                                   |
 *                          '-----------------------------------'
 *
 * The driver uses DSA to integrate the 4 user and 1 extension ports into the
 * kernel. Netdevices are created for the user ports, as are PHY devices for
 * their integrated PHYs. The device tree firmware should also specify the link
 * partner of the extension port - either via a fixed-link or other phy-handle.
 * See the device tree bindings for more detailed information. Note that the
 * driver has only been tested with a fixed-link, but in principle it should not
 * matter.
 *
 * NOTE: Currently, only the RGMII, SGMII and HSGMII interface are implemented
 * in this driver. MII and RMII are missing.
 *
 * The interrupt line is asserted on link UP/DOWN events. The driver creates a
 * custom irqchip to handle this interrupt and demultiplex the events by reading
 * the status registers via SMI. Interrupts are then propagated to the relevant
 * PHY device.
 *
 * The EEPROM contains initial register values which the chip will read over I2C
 * upon hardware reset. It is also possible to omit the EEPROM. In both cases,
 * the driver will manually reprogram some registers using jam tables to reach
 * an initial state defined by the vendor driver.
 *
 * This Linux driver is written based on an OS-agnostic vendor driver from
 * Realtek. The reference GPL-licensed sources can be found in the OpenWrt
 * source tree under the name rtl8367c. The vendor driver claims to support a
 * number of similar switch controllers from Realtek, but the only hardware we
 * have is the RTL8365MB-VC. Moreover, there does not seem to be any chip under
 * the name RTL8367C. Although one wishes that the 'C' stood for some kind of
 * common hardware revision, there exist examples of chips with the suffix -VC
 * which are explicitly not supported by the rtl8367c driver and which instead
 * require the rtl8367d vendor driver. With all this uncertainty, the driver has
 * been modestly named rtl8365mb. Future implementors may wish to rename things
 * accordingly.
 *
 * In the same family of chips, some carry up to 8 user ports and up to 2
 * extension ports. Where possible this driver tries to make things generic, but
 * more work must be done to support these configurations. According to
 * documentation from Realtek, the family should include the following chips:
 *
 *  - RTL8363NB
 *  - RTL8363NB-VB
 *  - RTL8363SC
 *  - RTL8363SC-VB
 *  - RTL8364NB
 *  - RTL8364NB-VB
 *  - RTL8365MB-VC
 *  - RTL8366SC
 *  - RTL8367RB-VB
 *  - RTL8367SB
 *  - RTL8367S
 *  - RTL8370MB
 *  - RTL8310SR
 *
 * Some of the register logic for these additional chips has been skipped over
 * while implementing this driver. It is therefore not possible to assume that
 * things will work out-of-the-box for other chips, and a careful review of the
 * vendor driver may be needed to expand support. The RTL8365MB-VC seems to be
 * one of the simpler chips.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/mutex.h>
#include <linux/of_irq.h>
#include <linux/regmap.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>

#include "realtek.h"
#include "realtek-smi.h"
#include "realtek-mdio.h"
#include "rtl83xx.h"
#include "rtl8365mb.h"
#include "rtl8365mb_reg.h"
#include "rtl8365mb_data.h"
#include "rtl8365mb_l2.h"
#include "rtl8365mb_vlan.h"

/* Family-specific data and limits */
#define RTL8365MB_PHYADDRMAX		7
#define RTL8365MB_NUM_PHYREGS		32
#define RTL8365MB_PHYREGMAX		(RTL8365MB_NUM_PHYREGS - 1)

/* Valid for the whole family except RTL8370B, which has 4160 entries.
 * RTL8370B is mentioned in vendor code but it might not even belong
 * to the same RTL8367C family.
 */
#define RTL8365MB_LEARN_LIMIT_MAX	2112

/* The LUT table size matches the maximum learning limit */
#define RTL8365MB_L2_TABLE_SIZE		RTL8365MB_LEARN_LIMIT_MAX

/* The DSA callback .get_stats64 runs in atomic context, so we are not allowed
 * to block. On the other hand, accessing MIB counters absolutely requires us to
 * block. The solution is thus to schedule work which polls the MIB counters
 * asynchronously and updates some private data, which the callback can then
 * fetch atomically. Three seconds should be a good enough polling interval.
 */
#define RTL8365MB_STATS_INTERVAL_JIFFIES	(3 * HZ)

/* Opaque vendor register. All set to 7 */
#define RTL8365MB_HSGMII_SCHED_REG0            0x00d0
#define RTL8365MB_HSGMII_SCHED_REG1            0x0399
#define RTL8365MB_HSGMII_SCHED_REG2            0x03fa
/* This value meaning is unknown */
#define RTL8365MB_HSGMII_SCHED_VAL             7

#define RTL8365MB_MAKE_MIB_COUNTER(_offset, _length, _name) \
		[RTL8365MB_MIB_ ## _name] = { _offset, _length, #_name }

/* Always keep rtl8365mb_mib_counter size as RTL8365MB_MIB_END */
static const struct rtl8365mb_mib_counter rtl8365mb_mib_counters_c[RTL8365MB_MIB_END] = {
	RTL8365MB_MAKE_MIB_COUNTER(0, 4, ifInOctets),
	RTL8365MB_MAKE_MIB_COUNTER(4, 2, dot3StatsFCSErrors),
	RTL8365MB_MAKE_MIB_COUNTER(6, 2, dot3StatsSymbolErrors),
	RTL8365MB_MAKE_MIB_COUNTER(8, 2, dot3InPauseFrames),
	RTL8365MB_MAKE_MIB_COUNTER(10, 2, dot3ControlInUnknownOpcodes),
	RTL8365MB_MAKE_MIB_COUNTER(12, 2, etherStatsFragments),
	RTL8365MB_MAKE_MIB_COUNTER(14, 2, etherStatsJabbers),
	RTL8365MB_MAKE_MIB_COUNTER(16, 2, ifInUcastPkts),
	RTL8365MB_MAKE_MIB_COUNTER(18, 2, etherStatsDropEvents),
	RTL8365MB_MAKE_MIB_COUNTER(20, 2, ifInMulticastPkts),
	RTL8365MB_MAKE_MIB_COUNTER(22, 2, ifInBroadcastPkts),
	RTL8365MB_MAKE_MIB_COUNTER(24, 2, inMldChecksumError),
	RTL8365MB_MAKE_MIB_COUNTER(26, 2, inIgmpChecksumError),
	RTL8365MB_MAKE_MIB_COUNTER(28, 2, inMldSpecificQuery),
	RTL8365MB_MAKE_MIB_COUNTER(30, 2, inMldGeneralQuery),
	RTL8365MB_MAKE_MIB_COUNTER(32, 2, inIgmpSpecificQuery),
	RTL8365MB_MAKE_MIB_COUNTER(34, 2, inIgmpGeneralQuery),
	RTL8365MB_MAKE_MIB_COUNTER(36, 2, inMldLeaves),
	RTL8365MB_MAKE_MIB_COUNTER(38, 2, inIgmpLeaves),
	RTL8365MB_MAKE_MIB_COUNTER(40, 4, etherStatsOctets),
	RTL8365MB_MAKE_MIB_COUNTER(44, 2, etherStatsUnderSizePkts),
	RTL8365MB_MAKE_MIB_COUNTER(46, 2, etherOversizeStats),
	RTL8365MB_MAKE_MIB_COUNTER(48, 2, etherStatsPkts64Octets),
	RTL8365MB_MAKE_MIB_COUNTER(50, 2, etherStatsPkts65to127Octets),
	RTL8365MB_MAKE_MIB_COUNTER(52, 2, etherStatsPkts128to255Octets),
	RTL8365MB_MAKE_MIB_COUNTER(54, 2, etherStatsPkts256to511Octets),
	RTL8365MB_MAKE_MIB_COUNTER(56, 2, etherStatsPkts512to1023Octets),
	RTL8365MB_MAKE_MIB_COUNTER(58, 2, etherStatsPkts1024to1518Octets),
	RTL8365MB_MAKE_MIB_COUNTER(60, 4, ifOutOctets),
	RTL8365MB_MAKE_MIB_COUNTER(64, 2, dot3StatsSingleCollisionFrames),
	RTL8365MB_MAKE_MIB_COUNTER(66, 2, dot3StatsMultipleCollisionFrames),
	RTL8365MB_MAKE_MIB_COUNTER(68, 2, dot3StatsDeferredTransmissions),
	RTL8365MB_MAKE_MIB_COUNTER(70, 2, dot3StatsLateCollisions),
	RTL8365MB_MAKE_MIB_COUNTER(72, 2, etherStatsCollisions),
	RTL8365MB_MAKE_MIB_COUNTER(74, 2, dot3StatsExcessiveCollisions),
	RTL8365MB_MAKE_MIB_COUNTER(76, 2, dot3OutPauseFrames),
	RTL8365MB_MAKE_MIB_COUNTER(78, 2, ifOutDiscards),
	RTL8365MB_MAKE_MIB_COUNTER(80, 2, dot1dTpPortInDiscards),
	RTL8365MB_MAKE_MIB_COUNTER(82, 2, ifOutUcastPkts),
	RTL8365MB_MAKE_MIB_COUNTER(84, 2, ifOutMulticastPkts),
	RTL8365MB_MAKE_MIB_COUNTER(86, 2, ifOutBroadcastPkts),
	RTL8365MB_MAKE_MIB_COUNTER(88, 2, outOampduPkts),
	RTL8365MB_MAKE_MIB_COUNTER(90, 2, inOampduPkts),
	RTL8365MB_MAKE_MIB_COUNTER(92, 4, inIgmpJoinsSuccess),
	RTL8365MB_MAKE_MIB_COUNTER(96, 2, inIgmpJoinsFail),
	RTL8365MB_MAKE_MIB_COUNTER(98, 2, inMldJoinsSuccess),
	RTL8365MB_MAKE_MIB_COUNTER(100, 2, inMldJoinsFail),
	RTL8365MB_MAKE_MIB_COUNTER(102, 2, inReportSuppressionDrop),
	RTL8365MB_MAKE_MIB_COUNTER(104, 2, inLeaveSuppressionDrop),
	RTL8365MB_MAKE_MIB_COUNTER(106, 2, outIgmpReports),
	RTL8365MB_MAKE_MIB_COUNTER(108, 2, outIgmpLeaves),
	RTL8365MB_MAKE_MIB_COUNTER(110, 2, outIgmpGeneralQuery),
	RTL8365MB_MAKE_MIB_COUNTER(112, 2, outIgmpSpecificQuery),
	RTL8365MB_MAKE_MIB_COUNTER(114, 2, outMldReports),
	RTL8365MB_MAKE_MIB_COUNTER(116, 2, outMldLeaves),
	RTL8365MB_MAKE_MIB_COUNTER(118, 2, outMldGeneralQuery),
	RTL8365MB_MAKE_MIB_COUNTER(120, 2, outMldSpecificQuery),
	RTL8365MB_MAKE_MIB_COUNTER(122, 2, inKnownMulticastPkts),
	/* RTL8365MB_MAKE_MIB_COUNTER(0x420, 2, dot1dTpLearnEntryDiscard), // global */
};

static const struct rtl8365mb_family_info rtl8365mb_family_info_a = {
	.family_id = RTL8365MB_FAMILY_A,
	.name = "RTL8367",
	.num_ports = 10,
};

static const struct rtl8365mb_family_info rtl8365mb_family_info_b = {
	.family_id = RTL8365MB_FAMILY_B,
	.name = "RTL8367B",
	.num_ports = 8,
};

static const struct rtl8365mb_family_info rtl8365mb_family_info_c = {
	.family_id = RTL8365MB_FAMILY_C,
	.name = "RTL8367C",
	.num_ports = 11,
	.jam_table = rtl8365mb_init_jam_common,
	.jam_size = &rtl8365mb_init_jam_common_size,
	.table_query = rtl8365mb_table_query_c,
	.mib_counters = rtl8365mb_mib_counters_c,
	.mib_port_offset = 0x7c,
	.l2_flush = rtl8365mb_l2_flush_c,
};

static const struct rtl8365mb_family_info rtl8365mb_family_info_d = {
	.family_id = RTL8365MB_FAMILY_D,
	.name = "RTL8367D",
	.num_ports = 8,
	.table_query = rtl8365mb_table_query_c,
};

/* Chip info for each supported switch in the family */
#define PHY_INTF(_mode) (RTL8365MB_PHY_INTERFACE_MODE_ ## _mode)
static const struct rtl8365mb_chip_info rtl8365mb_chip_infos[] = {
	{
		.name = "RTL8365MB-VC",
		.chip_id = 0x6367,
		.chip_ver = 0x0040,
		.family = &rtl8365mb_family_info_c,
		.extints = {
			{ 6, 1, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
		},
		.jam_table = rtl8365mb_init_jam_8365mb_vc,
		.jam_size = &rtl8365mb_init_jam_8365mb_vc_size,
	},
	{
		.name = "RTL8367S",
		.chip_id = 0x6367,
		.chip_ver = 0x00A0,
		.family = &rtl8365mb_family_info_c,
		.extints = {
			{ 6, 1, PHY_INTF(SGMII) | PHY_INTF(HSGMII) },
			{ 7, 2, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
		},
		.jam_table = rtl8365mb_init_jam_8365mb_vc,
		.jam_size = &rtl8365mb_init_jam_8365mb_vc_size,
	},
	{
		.name = "RTL8367SB",
		.chip_id = 0x6367,
		.chip_ver = 0x0010,
		.extints = {
			{ 6, 1, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) |
				PHY_INTF(SGMII) | PHY_INTF(HSGMII) },
			{ 7, 2, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
		},
		.jam_table = rtl8365mb_init_jam_8365mb_vc,
		.jam_size = &rtl8365mb_init_jam_8365mb_vc_size,
	},
	{
		.name = "RTL8367RB-VB",
		.chip_id = 0x6367,
		.chip_ver = 0x0020,
		.family = &rtl8365mb_family_info_c,
		.extints = {
			{ 6, 1, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
			{ 7, 2, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
		},
		.jam_table = rtl8365mb_init_jam_8365mb_vc,
		.jam_size = &rtl8365mb_init_jam_8365mb_vc_size,
	},
};

static int rtl8365mb_phy_poll_busy(struct realtek_priv *priv)
{
	u32 val;

	return regmap_read_poll_timeout(priv->map_nolock,
					RTL8365MB_INDIRECT_ACCESS_STATUS_REG,
					val, !val, 10, 100);
}

static int rtl8365mb_phy_ocp_prepare(struct realtek_priv *priv, int phy,
				     u32 ocp_addr)
{
	u32 val;
	int ret;

	/* Set OCP prefix */
	val = FIELD_GET(RTL8365MB_PHY_OCP_ADDR_PREFIX_MASK, ocp_addr);
	ret = regmap_update_bits(
		priv->map_nolock, RTL8365MB_GPHY_OCP_MSB_0_REG,
		RTL8365MB_GPHY_OCP_MSB_0_CFG_CPU_OCPADR_MASK,
		FIELD_PREP(RTL8365MB_GPHY_OCP_MSB_0_CFG_CPU_OCPADR_MASK, val));
	if (ret)
		return ret;

	/* Set PHY register address */
	val = RTL8365MB_PHY_BASE;
	val |= FIELD_PREP(RTL8365MB_INDIRECT_ACCESS_ADDRESS_PHYNUM_MASK, phy);
	val |= FIELD_PREP(RTL8365MB_INDIRECT_ACCESS_ADDRESS_OCPADR_5_1_MASK,
			  ocp_addr >> 1);
	val |= FIELD_PREP(RTL8365MB_INDIRECT_ACCESS_ADDRESS_OCPADR_9_6_MASK,
			  ocp_addr >> 6);
	ret = regmap_write(priv->map_nolock,
			   RTL8365MB_INDIRECT_ACCESS_ADDRESS_REG, val);
	if (ret)
		return ret;

	return 0;
}

static int rtl8365mb_phy_ocp_read(struct realtek_priv *priv, int phy,
				  u32 ocp_addr, u16 *data)
{
	u32 val;
	int ret;

	rtl83xx_lock(priv);

	ret = rtl8365mb_phy_poll_busy(priv);
	if (ret)
		goto out;

	ret = rtl8365mb_phy_ocp_prepare(priv, phy, ocp_addr);
	if (ret)
		goto out;

	/* Execute read operation */
	val = FIELD_PREP(RTL8365MB_INDIRECT_ACCESS_CTRL_CMD_MASK,
			 RTL8365MB_INDIRECT_ACCESS_CTRL_CMD_VALUE) |
	      FIELD_PREP(RTL8365MB_INDIRECT_ACCESS_CTRL_RW_MASK,
			 RTL8365MB_INDIRECT_ACCESS_CTRL_RW_READ);
	ret = regmap_write(priv->map_nolock, RTL8365MB_INDIRECT_ACCESS_CTRL_REG,
			   val);
	if (ret)
		goto out;

	ret = rtl8365mb_phy_poll_busy(priv);
	if (ret)
		goto out;

	/* Get PHY register data */
	ret = regmap_read(priv->map_nolock,
			  RTL8365MB_INDIRECT_ACCESS_READ_DATA_REG, &val);
	if (ret)
		goto out;

	*data = val & 0xFFFF;

out:
	rtl83xx_unlock(priv);

	return ret;
}

static int rtl8365mb_phy_ocp_write(struct realtek_priv *priv, int phy,
				   u32 ocp_addr, u16 data)
{
	u32 val;
	int ret;

	rtl83xx_lock(priv);

	ret = rtl8365mb_phy_poll_busy(priv);
	if (ret)
		goto out;

	ret = rtl8365mb_phy_ocp_prepare(priv, phy, ocp_addr);
	if (ret)
		goto out;

	/* Set PHY register data */
	ret = regmap_write(priv->map_nolock,
			   RTL8365MB_INDIRECT_ACCESS_WRITE_DATA_REG, data);
	if (ret)
		goto out;

	/* Execute write operation */
	val = FIELD_PREP(RTL8365MB_INDIRECT_ACCESS_CTRL_CMD_MASK,
			 RTL8365MB_INDIRECT_ACCESS_CTRL_CMD_VALUE) |
	      FIELD_PREP(RTL8365MB_INDIRECT_ACCESS_CTRL_RW_MASK,
			 RTL8365MB_INDIRECT_ACCESS_CTRL_RW_WRITE);
	ret = regmap_write(priv->map_nolock, RTL8365MB_INDIRECT_ACCESS_CTRL_REG,
			   val);
	if (ret)
		goto out;

	ret = rtl8365mb_phy_poll_busy(priv);
	if (ret)
		goto out;

out:
	rtl83xx_unlock(priv);

	return ret;
}

static int rtl8365mb_phy_read(struct realtek_priv *priv, int phy, int regnum)
{
	u32 ocp_addr;
	u16 val;
	int ret;

	if (phy > RTL8365MB_PHYADDRMAX)
		return -EINVAL;

	if (regnum > RTL8365MB_PHYREGMAX)
		return -EINVAL;

	ocp_addr = RTL8365MB_PHY_OCP_ADDR_PHYREG_BASE + regnum * 2;

	ret = rtl8365mb_phy_ocp_read(priv, phy, ocp_addr, &val);
	if (ret) {
		dev_err(priv->dev,
			"failed to read PHY%d reg %02x @ %04x, ret %pe\n", phy,
			regnum, ocp_addr, ERR_PTR(ret));
		return ret;
	}

	dev_dbg(priv->dev, "read PHY%d register 0x%02x @ %04x, val <- %04x\n",
		phy, regnum, ocp_addr, val);

	return val;
}

static int rtl8365mb_phy_write(struct realtek_priv *priv, int phy, int regnum,
			       u16 val)
{
	u32 ocp_addr;
	int ret;

	if (phy > RTL8365MB_PHYADDRMAX)
		return -EINVAL;

	if (regnum > RTL8365MB_PHYREGMAX)
		return -EINVAL;

	ocp_addr = RTL8365MB_PHY_OCP_ADDR_PHYREG_BASE + regnum * 2;

	ret = rtl8365mb_phy_ocp_write(priv, phy, ocp_addr, val);
	if (ret) {
		dev_err(priv->dev,
			"failed to write PHY%d reg %02x @ %04x, ret %pe\n", phy,
			regnum, ocp_addr, ERR_PTR(ret));
		return ret;
	}

	dev_dbg(priv->dev, "write PHY%d register 0x%02x @ %04x, val -> %04x\n",
		phy, regnum, ocp_addr, val);

	return 0;
}

static const struct rtl8365mb_extint *
rtl8365mb_get_port_extint(struct realtek_priv *priv, int port)
{
	struct rtl8365mb *mb = priv->chip_data;
	int i;

	for (i = 0; i < RTL8365MB_MAX_NUM_EXTINTS; i++) {
		const struct rtl8365mb_extint *extint =
			&mb->chip_info->extints[i];

		if (!extint->supported_interfaces)
			continue;

		if (extint->port == port)
			return extint;
	}

	return NULL;
}

static enum dsa_tag_protocol
rtl8365mb_get_tag_protocol(struct dsa_switch *ds, int port,
			   enum dsa_tag_protocol mp)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_cpu *cpu;
	struct rtl8365mb *mb;

	mb = priv->chip_data;
	cpu = &mb->cpu;

	if (cpu->position == RTL8365MB_CPU_POS_BEFORE_CRC)
		return DSA_TAG_PROTO_RTL8_4T;

	return DSA_TAG_PROTO_RTL8_4;
}

static int rtl8365mb_sds_indacs_write(struct realtek_priv *priv,
				      unsigned int addr,
				      unsigned int data)
{
	int ret;

	ret = regmap_write(priv->map, RTL8365MB_SDS_INDACS_DATA, data);
	if (ret)
		return ret;

	ret = regmap_write(priv->map, RTL8365MB_SDS_INDACS_ADR, addr);
	if (ret)
		return ret;

	return regmap_write(priv->map, RTL8365MB_SDS_INDACS_CMD,
			    RTL8365MB_SDS_CMD_MASK | RTL8365MB_SDS_RWO_MASK);
}

static int rtl8365mb_sds_indacs_read(struct realtek_priv *priv,
				     unsigned int addr,
				     unsigned int *data)
{
	int ret;
	int val;

	ret = regmap_write(priv->map, RTL8365MB_SDS_INDACS_ADR, addr);
	if (ret)
		return ret;

	ret = regmap_write(priv->map, RTL8365MB_SDS_INDACS_CMD,
			   RTL8365MB_SDS_CMD_MASK);
	if (ret)
		return ret;

	ret = regmap_read_poll_timeout(priv->map,
				       RTL8365MB_SDS_INDACS_CMD,
				       val,
				       !(val & RTL8365MB_SDS_CMD_BUSY_MASK),
				       10, 100);
	if (ret)
		return ret;

	return regmap_read(priv->map, RTL8365MB_SDS_INDACS_DATA, data);
}

static int rtl8365mb_ext_init_sgmii_fw(struct realtek_priv *priv)
{
	const struct firmware *fw;
	int ret;
	int i;

	ret = request_firmware(&fw, RTL8365MB_DW8051_SGMII_INIT_FW, priv->dev);
	if (ret)
		return ret;

	dev_dbg(priv->dev, "loading SGMII firmware to DW8051");

	ret = regmap_set_bits(priv->map, RTL8365MB_CHIP_RESET_REG,
			      RTL8365MB_CHIP_RESET_DW8051);
	if (ret)
		goto release_fw;

	ret = regmap_set_bits(priv->map, RTL8365MB_MISC_CFG0,
			      RTL8365MB_MISC_CFG0_DW8051_EN);
	if (ret)
		goto release_fw;

	ret = regmap_set_bits(priv->map, RTL8365MB_DW8051_RDY,
			      RTL8365MB_DW8051_RDY_ACS_IROM_EN);
	if (ret)
		goto release_fw;

	ret = regmap_clear_bits(priv->map, RTL8365MB_DW8051_RDY,
				RTL8365MB_DW8051_RDY_IROM_MSB);
	if (ret)
		goto release_fw;

	if (fw->size >= RTL8365MB_DW8051_FW_MAX) {
		ret = -E2BIG;
		goto release_fw;
	}

	/* cannot use regmap_bulk_write() as each byte must be written to a
	 * single 16-bit reg
	 */
	for (i = 0; i < fw->size; i++) {
		ret = regmap_write(priv->map, RTL8365MB_DW8051_FW_REG_BASE + i,
				   fw->data[i]);
		if (ret)
			goto release_fw;
	}

	ret = regmap_clear_bits(priv->map, RTL8365MB_DW8051_RDY,
				RTL8365MB_DW8051_RDY_IROM_MSB);
	if (ret)
		goto release_fw;

	ret = regmap_clear_bits(priv->map, RTL8365MB_DW8051_RDY,
				RTL8365MB_DW8051_RDY_ACS_IROM_EN);
	if (ret)
		goto release_fw;

	ret = regmap_clear_bits(priv->map, RTL8365MB_CHIP_RESET_REG,
				RTL8365MB_CHIP_RESET_DW8051);
	if (ret)
		goto release_fw;

release_fw:
	release_firmware(fw);

	return ret;
}

static int rtl8365mb_get_chip_option(struct regmap *map, u32 *id)
{
	int ret;

	ret = regmap_write(map, RTL8365MB_MAGIC_OPT_REG, RTL8365MB_MAGIC_VALUE);
	if (ret)
		return ret;

	ret = regmap_read(map, RTL8365MB_CHIP_OPTION_REG, id);
	if (ret)
		return ret;

	ret = regmap_write(map, RTL8365MB_MAGIC_OPT_REG, 0);
	if (ret)
		return ret;

	return 0;
}

static int rtl8365mb_sgmii_calib(struct realtek_priv *priv, u32 chip_option,
				 const struct rtl8365mb_sds_init **sds_init,
				 size_t *sds_init_len)
{
	if (chip_option == 0) {
		*sds_init = calib_data_SGMII_opt0;
		*sds_init_len = calib_data_SGMII_opt0_size;
	} else {
		*sds_init = calib_data_SGMII_opt1;
		*sds_init_len = calib_data_SGMII_opt1_size;
	}

	return 0;
}

static int rtl8365mb_hsgmii_calib(struct realtek_priv *priv, u32 chip_option,
				  const struct rtl8365mb_sds_init **sds_init,
				  size_t *sds_init_len)
{
	struct rtl8365mb *mb;
	u32 model;

	mb = priv->chip_data;

	if (chip_option == 0) {
		model = FIELD_GET(RTL8365MB_CHIP_VER_MODEL_MASK,
				  mb->chip_info->chip_ver);
		switch (model) {
		case 0x1:
		case 0x5:
		case 0x6:
			*sds_init = calib_data_HSGMII_opt0_A;
			*sds_init_len = calib_data_HSGMII_opt0_A_size;
			break;
		case 0x8:
		case 0x9:
			*sds_init = calib_data_HSGMII_opt0_B;
			*sds_init_len = calib_data_HSGMII_opt0_B_size;
			break;
		default:
			return -EINVAL;
		}
	} else {
		*sds_init = calib_data_HSGMII_opt1;
		*sds_init_len = calib_data_HSGMII_opt1_size;
	}

	return 0;
}

/* This code should possibly support chip_num 0x0276 and 0x0597, but it was
 * only tested on 0x6367
 */
static int rtl8365mb_ext_init_sgmii(struct realtek_priv *priv, int port,
				    phy_interface_t interface)
{
	const struct rtl8365mb_sds_init *sds_init;
	const struct rtl8365mb_extint *extint;
	size_t sds_init_len;
	int interface_mode;
	u32 chip_option;
	int sds_mode;
	int mask;
	int reg;
	int ret;
	int val;
	int i;

	extint = rtl8365mb_get_port_extint(priv, port);
	/* RTL8370MB is the only model in this family that has two
	 * SGMII extint. If that model is added to this driver,
	 * this check might need to be updated as well as the firmware
	 * loading process.
	 */
	if (extint->id != 1)
		return -EINVAL;

	ret = rtl8365mb_get_chip_option(priv->map, &chip_option);
	if (ret)
		return ret;

	dev_dbg(priv->dev, "initializing SGMII (chip option: %d)", chip_option);

	if (interface == PHY_INTERFACE_MODE_SGMII) {
		sds_mode = RTL8365MB_CFG_MAC8_SEL_SGMII;
		interface_mode = RTL8365MB_EXT_PORT_MODE_SGMII;
		ret = rtl8365mb_sgmii_calib(priv, chip_option, &sds_init,
					    &sds_init_len);
		if (ret)
			return ret;

	} else if (interface == PHY_INTERFACE_MODE_2500BASEX) {
		sds_mode = RTL8365MB_CFG_MAC8_SEL_HSGMII;
		interface_mode = RTL8365MB_EXT_PORT_MODE_HSGMII;
		ret = rtl8365mb_hsgmii_calib(priv, chip_option, &sds_init,
					     &sds_init_len);
		if (ret)
			return ret;
	} else {
		return -EINVAL;
	}

	for (i = 0; i < sds_init_len; i++) {
		ret = rtl8365mb_sds_indacs_write(priv, sds_init[i].addr,
						 sds_init[i].data);
		if (ret)
			return ret;
	}

	if (interface == PHY_INTERFACE_MODE_2500BASEX) {
		ret = regmap_write(priv->map, RTL8365MB_HSGMII_SCHED_REG0,
				   RTL8365MB_HSGMII_SCHED_VAL);
		if (ret)
			return ret;
		ret = regmap_write(priv->map, RTL8365MB_HSGMII_SCHED_REG1,
				   RTL8365MB_HSGMII_SCHED_VAL);
		if (ret)
			return ret;
		ret = regmap_write(priv->map, RTL8365MB_HSGMII_SCHED_REG2,
				   RTL8365MB_HSGMII_SCHED_VAL);
		if (ret)
			return ret;
	}

	mask = RTL8365MB_CFG_MAC8_SEL_SGMII | RTL8365MB_CFG_MAC8_SEL_HSGMII;
	ret = regmap_update_bits(priv->map,
				 RTL8365MB_SDS_MISC,
				 mask,
				 sds_mode);
	if (ret)
		return ret;

	reg = RTL8365MB_DIGITAL_INTERFACE_SELECT_REG(extint->id);
	mask = RTL8365MB_DIGITAL_INTERFACE_SELECT_MODE_MASK(extint->id);
	val = interface_mode
		<< RTL8365MB_DIGITAL_INTERFACE_SELECT_MODE_OFFSET(extint->id);
	ret = regmap_update_bits(priv->map, reg, mask, val);
	if (ret)
		return ret;

	ret = regmap_write(priv->map, RTL8365MB_BYPASS_LINE_RATE, 0x0);
	if (ret)
		return ret;

	/* Serdes not reset */
	ret = rtl8365mb_sds_indacs_write(priv, 0x0003, 0x7106);
	if (ret)
		return ret;

	return rtl8365mb_ext_init_sgmii_fw(priv);
}

static int rtl8365mb_ext_sgmii_nway(struct realtek_priv *priv, bool state)
{
	bool dw8051_enabled;
	int ret, ret2;
	u32 val;

	ret = regmap_read(priv->map, RTL8365MB_MISC_CFG0, &val);
	dw8051_enabled = val & RTL8365MB_MISC_CFG0_DW8051_EN;
	if (dw8051_enabled) {
		ret = regmap_clear_bits(priv->map, RTL8365MB_MISC_CFG0,
					RTL8365MB_MISC_CFG0_DW8051_EN);
		if (ret)
			return ret;
	}

	/* There is no clue in vendor docs about 0x0002 semantic */
	ret = rtl8365mb_sds_indacs_read(priv, 0x0002, &val);
	if (ret)
		goto restore_dw8051;

	/* 0x0200 in reg 0x0002 seems to be a reset nway enable flag */
	if (state)
		val |= 0x0200;
	else
		val &= ~0x0200;
	/* this value is set in both cases. nway restart? */
	val |= 0x0100;

	ret = rtl8365mb_sds_indacs_write(priv, 0x0002, val);

restore_dw8051:
	/* only enables if it was enabled before */
	if (dw8051_enabled) {
		ret2 = regmap_set_bits(priv->map, RTL8365MB_MISC_CFG0,
				      RTL8365MB_MISC_CFG0_DW8051_EN);
		/* only returns ret2 if we are not already handling an error */
		if (!ret)
			return ret2;
	}
	return ret;
}

static int rtl8365mb_ext_config_rgmii(struct realtek_priv *priv, int port,
				      phy_interface_t interface)
{
	const struct rtl8365mb_extint *extint =
		rtl8365mb_get_port_extint(priv, port);
	struct dsa_switch *ds = &priv->ds;
	struct device_node *dn;
	struct dsa_port *dp;
	int tx_delay = 0;
	int rx_delay = 0;
	u32 val;
	int ret;

	if (!extint)
		return -ENODEV;

	dp = dsa_to_port(ds, port);
	dn = dp->dn;

	/* Set the RGMII TX/RX delay
	 *
	 * The Realtek vendor driver indicates the following possible
	 * configuration settings:
	 *
	 *   TX delay:
	 *     0 = no delay, 1 = 2 ns delay
	 *   RX delay:
	 *     0 = no delay, 7 = maximum delay
	 *     Each step is approximately 0.3 ns, so the maximum delay is about
	 *     2.1 ns.
	 *
	 * The vendor driver also states that this must be configured *before*
	 * forcing the external interface into a particular mode, which is done
	 * in the rtl8365mb_phylink_mac_link_{up,down} functions.
	 *
	 * Only configure an RGMII TX (resp. RX) delay if the
	 * tx-internal-delay-ps (resp. rx-internal-delay-ps) OF property is
	 * specified. We ignore the detail of the RGMII interface mode
	 * (RGMII_{RXID, TXID, etc.}), as this is considered to be a PHY-only
	 * property.
	 */
	if (!of_property_read_u32(dn, "tx-internal-delay-ps", &val)) {
		val = val / 1000; /* convert to ns */

		if (val == 0 || val == 2)
			tx_delay = val / 2;
		else
			dev_warn(priv->dev,
				 "RGMII TX delay must be 0 or 2 ns\n");
	}

	if (!of_property_read_u32(dn, "rx-internal-delay-ps", &val)) {
		val = DIV_ROUND_CLOSEST(val, 300); /* convert to 0.3 ns step */

		if (val <= 7)
			rx_delay = val;
		else
			dev_warn(priv->dev,
				 "RGMII RX delay must be 0 to 2.1 ns\n");
	}

	ret = regmap_update_bits(
		priv->map, RTL8365MB_EXT_RGMXF_REG(extint->id),
		RTL8365MB_EXT_RGMXF_TXDELAY_MASK |
			RTL8365MB_EXT_RGMXF_RXDELAY_MASK,
		FIELD_PREP(RTL8365MB_EXT_RGMXF_TXDELAY_MASK, tx_delay) |
			FIELD_PREP(RTL8365MB_EXT_RGMXF_RXDELAY_MASK, rx_delay));
	if (ret)
		return ret;

	ret = regmap_update_bits(
		priv->map, RTL8365MB_DIGITAL_INTERFACE_SELECT_REG(extint->id),
		RTL8365MB_DIGITAL_INTERFACE_SELECT_MODE_MASK(extint->id),
		RTL8365MB_EXT_PORT_MODE_RGMII
			<< RTL8365MB_DIGITAL_INTERFACE_SELECT_MODE_OFFSET(
				   extint->id));
	if (ret)
		return ret;

	return 0;
}

static int rtl8365mb_ext_config_forcemode(struct realtek_priv *priv, int port,
					  phy_interface_t interface,
					  bool link, int speed, int duplex,
					  bool tx_pause, bool rx_pause)
{
	const struct rtl8365mb_extint *extint =
		rtl8365mb_get_port_extint(priv, port);
	u32 r_tx_pause;
	u32 r_rx_pause;
	u32 r_duplex;
	u32 r_speed;
	u32 r_link;
	int val;
	int ret;

	if (!extint)
		return -ENODEV;

	if (link) {
		/* Force the link up with the desired configuration */
		r_link = 1;
		r_rx_pause = rx_pause ? 1 : 0;
		r_tx_pause = tx_pause ? 1 : 0;

		if (speed == SPEED_2500) {
			r_speed = RTL8365MB_PORT_SPEED_2500M;
		} else if (speed == SPEED_1000) {
			r_speed = RTL8365MB_PORT_SPEED_1000M;
		} else if (speed == SPEED_100) {
			r_speed = RTL8365MB_PORT_SPEED_100M;
		} else if (speed == SPEED_10) {
			r_speed = RTL8365MB_PORT_SPEED_10M;
		} else {
			dev_err(priv->dev, "unsupported port speed %s\n",
				phy_speed_to_str(speed));
			return -EINVAL;
		}

		if (duplex == DUPLEX_FULL) {
			r_duplex = 1;
		} else if (duplex == DUPLEX_HALF) {
			r_duplex = 0;
		} else {
			dev_err(priv->dev, "unsupported duplex %s\n",
				phy_duplex_to_str(duplex));
			return -EINVAL;
		}
	} else {
		/* Force the link down and reset any programmed configuration */
		r_link = 0;
		r_tx_pause = 0;
		r_rx_pause = 0;
		r_speed = 0;
		r_duplex = 0;
	}

	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_2500BASEX) {
		val = FIELD_PREP(RTL8365MB_CFG_SGMII_FDUP, r_duplex) |
		      FIELD_PREP(RTL8365MB_CFG_SGMII_SPD_MASK, r_speed) |
		      FIELD_PREP(RTL8365MB_CFG_SGMII_LINK, r_link) |
		      FIELD_PREP(RTL8365MB_CFG_SGMII_TXFC, r_tx_pause) |
		      FIELD_PREP(RTL8365MB_CFG_SGMII_RXFC, r_rx_pause);
		ret = regmap_update_bits(priv->map,
					 RTL8365MB_SDS_MISC,
					 RTL8365MB_CFG_SGMII_FDUP |
					 RTL8365MB_CFG_SGMII_SPD_MASK |
					 RTL8365MB_CFG_SGMII_LINK |
					 RTL8365MB_CFG_SGMII_TXFC |
					 RTL8365MB_CFG_SGMII_RXFC,
					 val);
		if (ret)
			return ret;
	}

	val = FIELD_PREP(RTL8365MB_DIGITAL_INTERFACE_FORCE_EN_MASK, 1) |
	      FIELD_PREP(RTL8365MB_DIGITAL_INTERFACE_FORCE_TXPAUSE_MASK,
			 r_tx_pause) |
	      FIELD_PREP(RTL8365MB_DIGITAL_INTERFACE_FORCE_RXPAUSE_MASK,
			 r_rx_pause) |
	      FIELD_PREP(RTL8365MB_DIGITAL_INTERFACE_FORCE_LINK_MASK, r_link) |
	      FIELD_PREP(RTL8365MB_DIGITAL_INTERFACE_FORCE_DUPLEX_MASK,
			 r_duplex) |
	      FIELD_PREP(RTL8365MB_DIGITAL_INTERFACE_FORCE_SPEED_MASK, r_speed);
	ret = regmap_write(priv->map,
			   RTL8365MB_DIGITAL_INTERFACE_FORCE_REG(extint->id),
			   val);
	if (ret)
		return ret;

	return 0;
}

static void rtl8365mb_phylink_get_caps(struct dsa_switch *ds, int port,
				       struct phylink_config *config)
{
	const struct rtl8365mb_extint *extint =
		rtl8365mb_get_port_extint(ds->priv, port);

	config->mac_capabilities = MAC_SYM_PAUSE | MAC_ASYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000FD;

	if (!extint) {
		__set_bit(PHY_INTERFACE_MODE_INTERNAL,
			  config->supported_interfaces);

		/* GMII is the default interface mode for phylib, so
		 * we have to support it for ports with integrated PHY.
		 */
		__set_bit(PHY_INTERFACE_MODE_GMII,
			  config->supported_interfaces);
		return;
	}

	/* Populate according to the modes supported by _this driver_,
	 * not necessarily the modes supported by the hardware, some of
	 * which remain unimplemented.
	 */

	if (extint->supported_interfaces & RTL8365MB_PHY_INTERFACE_MODE_RGMII)
		phy_interface_set_rgmii(config->supported_interfaces);

	if (extint->supported_interfaces & RTL8365MB_PHY_INTERFACE_MODE_SGMII)
		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  config->supported_interfaces);

	if (extint->supported_interfaces &
	    RTL8365MB_PHY_INTERFACE_MODE_HSGMII) {
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  config->supported_interfaces);
		config->mac_capabilities |= MAC_2500FD;
	}
}

static void rtl8365mb_phylink_mac_config(struct phylink_config *config,
					 unsigned int mode,
					 const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct realtek_priv *priv = dp->ds->priv;
	u8 port = dp->index;
	int ret;

	if (mode != MLO_AN_PHY && mode != MLO_AN_FIXED) {
		dev_err(priv->dev,
			"port %d supports only conventional PHY or fixed-link\n",
			port);
		return;
	}

	if (phy_interface_mode_is_rgmii(state->interface)) {
		ret = rtl8365mb_ext_config_rgmii(priv, port, state->interface);
		if (ret)
			dev_err(priv->dev,
				"failed to configure RGMII mode on port %d: %pe\n",
				port, ERR_PTR(ret));
		return;
	} else if (state->interface == PHY_INTERFACE_MODE_SGMII ||
		   state->interface == PHY_INTERFACE_MODE_2500BASEX) {
		ret = rtl8365mb_ext_init_sgmii(priv, port, state->interface);
		if (ret) {
			dev_err(priv->dev,
				"failed to initialize SerDes mode %d on port %d: %pe\n",
				state->interface, port, ERR_PTR(ret));
			return;
		}

		ret = rtl8365mb_ext_sgmii_nway(priv, phylink_autoneg_inband(mode));
		if (ret)
			dev_err(priv->dev,
				"failed to configure SerDes autoneg on port %d: %pe\n",
				port, ERR_PTR(ret));
	}

	/* TODO: Implement MII and RMII modes, which the RTL8365MB-VC also
	 * supports
	 */
}

static void rtl8365mb_phylink_mac_link_down(struct phylink_config *config,
					    unsigned int mode,
					    phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct realtek_priv *priv = dp->ds->priv;
	struct rtl8365mb_port *p;
	struct rtl8365mb *mb;
	u8 port = dp->index;
	int ret;

	mb = priv->chip_data;
	p = &mb->ports[port];
	cancel_delayed_work_sync(&p->mib_work);

	if (phy_interface_mode_is_rgmii(interface) ||
	    interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_2500BASEX) {
		ret = rtl8365mb_ext_config_forcemode(priv, port, interface,
						     false, 0, 0,
						     false, false);
		if (ret)
			dev_err(priv->dev,
				"failed to reset forced mode on port %d: %pe\n",
				port, ERR_PTR(ret));

		return;
	}
}

static void rtl8365mb_phylink_mac_link_up(struct phylink_config *config,
					  struct phy_device *phydev,
					  unsigned int mode,
					  phy_interface_t interface,
					  int speed, int duplex, bool tx_pause,
					  bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct realtek_priv *priv = dp->ds->priv;
	struct rtl8365mb_port *p;
	struct rtl8365mb *mb;
	u8 port = dp->index;
	int ret;

	mb = priv->chip_data;
	p = &mb->ports[port];
	schedule_delayed_work(&p->mib_work, 0);

	if (phy_interface_mode_is_rgmii(interface) ||
	    interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_2500BASEX) {
		ret = rtl8365mb_ext_config_forcemode(priv, port, interface,
						     true, speed,
						     duplex, tx_pause,
						     rx_pause);
		if (ret)
			dev_err(priv->dev,
				"failed to force mode on port %d: %pe\n", port,
				ERR_PTR(ret));

		return;
	}
}

static int rtl8365mb_port_change_mtu(struct dsa_switch *ds, int port,
				     int new_mtu)
{
	struct realtek_priv *priv = ds->priv;
	int frame_size;

	/* When a new MTU is set, DSA always sets the CPU port's MTU to the
	 * largest MTU of the user ports. Because the switch only has a global
	 * RX length register, only allowing CPU port here is enough.
	 */
	if (!dsa_is_cpu_port(ds, port))
		return 0;

	frame_size = new_mtu + VLAN_ETH_HLEN + ETH_FCS_LEN;

	dev_dbg(priv->dev, "changing mtu to %d (frame size: %d)\n",
		new_mtu, frame_size);

	return regmap_update_bits(priv->map, RTL8365MB_CFG0_MAX_LEN_REG,
				  RTL8365MB_CFG0_MAX_LEN_MASK,
				  FIELD_PREP(RTL8365MB_CFG0_MAX_LEN_MASK,
					     frame_size));
}

static int rtl8365mb_port_max_mtu(struct dsa_switch *ds, int port)
{
	return RTL8365MB_CFG0_MAX_LEN_MAX - VLAN_ETH_HLEN - ETH_FCS_LEN;
}

static void rtl8365mb_port_stp_state_set(struct dsa_switch *ds, int port,
					 u8 state)
{
	struct realtek_priv *priv = ds->priv;
	enum rtl8365mb_stp_state val;
	int msti = 0;

	switch (state) {
	case BR_STATE_DISABLED:
		val = RTL8365MB_STP_STATE_DISABLED;
		break;
	case BR_STATE_BLOCKING:
	case BR_STATE_LISTENING:
		val = RTL8365MB_STP_STATE_BLOCKING;
		break;
	case BR_STATE_LEARNING:
		val = RTL8365MB_STP_STATE_LEARNING;
		break;
	case BR_STATE_FORWARDING:
		val = RTL8365MB_STP_STATE_FORWARDING;
		break;
	default:
		dev_err(priv->dev, "invalid STP state: %u\n", state);
		return;
	}

	regmap_update_bits(priv->map, RTL8365MB_MSTI_CTRL_REG(msti, port),
			   RTL8365MB_MSTI_CTRL_PORT_STATE_MASK(port),
			   val << RTL8365MB_MSTI_CTRL_PORT_STATE_OFFSET(port));
}

static int rtl8365mb_port_set_transparent(struct realtek_priv *priv,
					  int igr_port, int egr_port,
					  bool enable)
{
	dev_dbg(priv->dev, "%s transparent VLAN from %d to %d\n",
		enable ? "Enable" : "Disable", igr_port, egr_port);

	/* "Transparent" between the two ports means that packets forwarded by
	 * igr_port and egressed on egr_port will not be filtered by the usual
	 * VLAN membership settings.
	 */
	return regmap_update_bits(priv->map,
			RTL8365MB_VLAN_EGRESS_TRANSPARENT_REG(egr_port),
			BIT(igr_port), enable ? BIT(igr_port) : 0);
}

static int rtl8365mb_port_set_ingress_filtering(struct realtek_priv *priv,
						int port, bool enable)
{
	/* Ingress filtering enabled: Discard VLAN-tagged frames if the port is
	 * not a member of the VLAN with which the packet is associated.
	 * Untagged packets will also be discarded unless the port has a PVID
	 * programmed. Priority-tagged frames are treated as untagged frames.
	 *
	 * Ingress filtering disabled: Accept all tagged and untagged frames.
	 */
	return regmap_update_bits(priv->map, RTL8365MB_VLAN_INGRESS_REG,
			RTL8365MB_VLAN_INGRESS_FILTER_PORT_EN_MASK(port),
			enable ?
			RTL8365MB_VLAN_INGRESS_FILTER_PORT_EN_MASK(port) :
			0);
}

static int
rtl8365mb_port_set_vlan_egress_mode(struct realtek_priv *priv, int port,
				    enum rtl8365mb_vlan_egress_mode mode)
{
	u32 val;

	val = FIELD_PREP(RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_MASK, mode);
	return regmap_update_bits(priv->map,
			RTL8365MB_PORT_MISC_CFG_REG(port),
			RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_MASK, val);
}

static int rtl8365mb_port_vlan_filtering(struct dsa_switch *ds, int port,
					 bool vlan_filtering,
					 struct netlink_ext_ack *extack)
{
	enum rtl8365mb_frame_ingress accepted_frame, prev_accepted_frame;
	enum rtl8365mb_vlan_egress_mode mode;
	struct realtek_priv *priv = ds->priv;
	u32 configured_ports = 0;
	struct dsa_port *dp;
	u16 pvid_vid;
	int ret;

	dev_dbg(priv->dev, "port %d: %s VLAN filtering\n", port,
		vlan_filtering ? "enable" : "disable");

	ret = rtl8365mb_vlan_port_get_framefilter(priv, port,
						  &prev_accepted_frame);
	if (ret) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Failed to get current framefilter");
		return ret;
	}

	/* While filtering, only accepts untagged frames if PVID is enabled */
	if (vlan_filtering) {
		ret = rtl8365mb_vlan_port_get_pvid(priv, port, &pvid_vid);
		if (ret)
			return ret;

		if (pvid_vid)
			accepted_frame = RTL8365MB_FRAME_TYPE_ANY_FRAME;
		else
			accepted_frame = RTL8365MB_FRAME_TYPE_TAGGED_ONLY;
	} else {
		accepted_frame = RTL8365MB_FRAME_TYPE_ANY_FRAME;
	}

	/* When vlan filter is enable/disabled in a bridge, this function is
	 * called for all member ports. We need to enable/disable ingress
	 * VLAN membership check.
	 */
	ret = rtl8365mb_port_set_ingress_filtering(priv, port, vlan_filtering);
	if (ret)
		return ret;

	/* However, we also enable/disable egress filtering because the switch
	 * still consider the egress interface VLAN membership to forward the
	 * traffic. We enable/disable that check disabling/enabling transparent
	 * VLAN between the ingress port and all other available ports.
	 */
	dsa_switch_for_each_available_port(dp, ds) {
		/* port isolation will still keep traffic inside the bridge */
		ret = rtl8365mb_port_set_transparent(priv, port, dp->index,
						     !vlan_filtering);
		if (ret)
			goto undo_transparent;

		configured_ports |= BIT(dp->index);
	}

	if (accepted_frame != prev_accepted_frame) {
		ret = rtl8365mb_vlan_port_set_framefilter(priv, port,
							  accepted_frame);
		if (ret) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Failed to set port framefilter");
			goto undo_transparent;
		}
	}

	/* When VLAN filtering is disabled, preserve frames exactly as received.
	 * Otherwise, the VLAN egress pipeline may still alter tag state
	 * according to VLAN membership and untag configuration.
	 */
	if (vlan_filtering)
		mode = RTL8365MB_VLAN_EGRESS_MODE_ORIGINAL;
	else
		mode = RTL8365MB_VLAN_EGRESS_MODE_REAL_KEEP;

	ret = rtl8365mb_port_set_vlan_egress_mode(priv, port, mode);
	if (ret)
		goto undo_set_framefilter;

	return ret;

undo_set_framefilter:
	if (prev_accepted_frame != accepted_frame)
		rtl8365mb_vlan_port_set_framefilter(priv, port,
						    prev_accepted_frame);
undo_transparent:
	dsa_switch_for_each_port(dp, ds) {
		if (configured_ports & BIT(dp->index))
			rtl8365mb_port_set_transparent(priv, port, dp->index,
						       vlan_filtering);
	}

	rtl8365mb_port_set_ingress_filtering(priv, port, !vlan_filtering);

	return ret;
}

static int rtl8365mb_port_vlan_add(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_port_vlan *vlan,
				   struct netlink_ext_ack *extack)
{
	bool untagged = !!(vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED);
	bool pvid = !!(vlan->flags & BRIDGE_VLAN_INFO_PVID);
	u16 pvid_vid;
	struct realtek_priv *priv = ds->priv;
	int ret;

	dev_dbg(priv->dev, "add VLAN %d on port %d, %s, %s\n",
		vlan->vid, port, untagged ? "untagged" : "tagged",
		pvid ? "PVID" : "no PVID");

	/* VID == 0 is reserved in this driver */
	if (vlan->vid == 0) {
		NL_SET_ERR_MSG_MOD(extack,
				   "VLAN 0 is not supported by this hardware");
		return -EOPNOTSUPP;
	}

	mutex_lock(&priv->vlan_lock);

	ret = rtl8365mb_vlan_port_get_pvid(priv, port, &pvid_vid);
	if (ret)
		goto out_unlock;

	/* Set PVID if needed */
	if (pvid) {
		ret = rtl8365mb_vlan_pvid_port_set(ds, port, vlan->vid,
						   extack);
		if (ret)
			goto out_unlock;
	} else {
		/* or try to unset it if not */
		ret = rtl8365mb_vlan_pvid_port_clear(ds, port, vlan->vid);
		if (ret)
			goto out_unlock;
	}

	/* add port to vlan4k. It knows nothing about PVID */
	ret = rtl8365mb_vlan_4k_port_add(ds, port, vlan, extack);
	if (ret)
		goto undo_set_pvid;

	ret = 0;
	goto out_unlock;

undo_set_pvid:
	/* undo the pvid definition */
	if (pvid != (pvid_vid == vlan->vid)) {
		if (pvid_vid)
			(void)rtl8365mb_vlan_pvid_port_set(ds, port, pvid_vid,
							   NULL);
		else
			(void)rtl8365mb_vlan_pvid_port_clear(ds, port,
							     vlan->vid);
	}
out_unlock:
	mutex_unlock(&priv->vlan_lock);
	return ret;
}

static int rtl8365mb_port_vlan_del(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_port_vlan *vlan)
{
	bool untagged = !!(vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED);
	bool pvid = !!(vlan->flags & BRIDGE_VLAN_INFO_PVID);
	struct realtek_priv *priv = ds->priv;
	int ret1, ret2;

	dev_dbg(priv->dev, "del VLAN %d on port %d, %s, %s\n",
		vlan->vid, port, untagged ? "untagged" : "tagged",
		pvid ? "PVID" : "no PVID");

	mutex_lock(&priv->vlan_lock);
	ret1 = rtl8365mb_vlan_pvid_port_clear(ds, port, vlan->vid);
	ret2 = rtl8365mb_vlan_4k_port_del(ds, port, vlan);
	mutex_unlock(&priv->vlan_lock);

	return ret1 ?: ret2;
}

/* VLAN support is always enabled in the switch.
 *
 * Standalone forwarding relies on transparent VLAN mode combined with per-port
 * isolation masks restricting egress to CPU ports only.
 *
 */
static int rtl8365mb_vlan_setup(struct dsa_switch *ds)
{
	struct realtek_priv *priv = ds->priv;
	struct dsa_port *dp;
	int ret;

	dsa_switch_for_each_available_port(dp, ds) {
		/* Disable vlan-filtering for all ports */
		ret = rtl8365mb_port_vlan_filtering(ds, dp->index, false, NULL);
		if (ret) {
			dev_err(priv->dev,
				"Failed to disable vlan filtering on port %d\n",
				dp->index);
			return ret;
		}
	}

	/* VLAN is always enabled. */
	ret = regmap_update_bits(priv->map, RTL8365MB_VLAN_CTRL_REG,
				 RTL8365MB_VLAN_CTRL_EN_MASK,
				 FIELD_PREP(RTL8365MB_VLAN_CTRL_EN_MASK, 1));
	return ret;
}

static int rtl8365mb_port_set_learning(struct realtek_priv *priv, int port,
				       bool enable)
{
	int limit;

	/* Enable/disable learning by limiting the number of L2 addresses the
	 * port can learn. Realtek documentation states that a limit of zero
	 * disables learning. When enabling learning, set it to the chip's
	 * maximum.
	 */
	limit = enable ? priv->variant->l2_table_size : 0;

	return regmap_write(priv->map, RTL8365MB_LUT_PORT_LEARN_LIMIT_REG(port),
			    limit);
}

static int rtl8365mb_port_set_ucast_flood(struct realtek_priv *priv, int port,
					  bool enable)
{
	/* Frames with unknown unicast DA will be flooded to a programmable
	 * port mask that by default includes all ports. Add or remove
	 * the specified port from this port mask accordingly.
	 */
	return regmap_update_bits(priv->map,
				  RTL8365MB_UNKNOWN_UNICAST_FLOODING_PMASK_REG,
				  BIT(port), enable ? BIT(port) : 0);
}

static int rtl8365mb_port_set_mcast_flood(struct realtek_priv *priv, int port,
					  bool enable)
{
	return regmap_update_bits(priv->map,
			RTL8365MB_UNKNOWN_MULTICAST_FLOODING_PMASK_REG,
			BIT(port), enable ? BIT(port) : 0);
}

static int rtl8365mb_port_set_bcast_flood(struct realtek_priv *priv, int port,
					  bool enable)
{
	return regmap_update_bits(priv->map,
			RTL8365MB_UNKNOWN_BROADCAST_FLOODING_PMASK_REG,
			BIT(port), enable ? BIT(port) : 0);
}

static int rtl8365mb_port_pre_bridge_flags(struct dsa_switch *ds, int port,
					   struct switchdev_brport_flags flags,
					   struct netlink_ext_ack *extack)
{
	struct realtek_priv *priv = ds->priv;

	dev_dbg(priv->dev, "pre_bridge_flags port:%d flags:%lx supported:%lx\n",
		port, flags.mask, RTL8365MB_SUPPORTED_BRIDGE_FLAGS);

	if (flags.mask & ~RTL8365MB_SUPPORTED_BRIDGE_FLAGS)
		return -EINVAL;

	return 0;
}

static int rtl8365mb_port_set_efid(struct realtek_priv *priv, int port,
				   u32 efid)
{
	return regmap_update_bits(priv->map, RTL8365MB_PORT_EFID_REG(port),
				  RTL8365MB_PORT_EFID_MASK(port),
				  efid << RTL8365MB_PORT_EFID_OFFSET(port));
}

/* Port isolation manipulation functions.
 *
 * The port isolation register controls the forwarding mask of a given
 * port. The switch will not forward packets ingressed on a given port
 * to ports which are not enabled in its forwarding mask.
 *
 * The port forwarding mask has the highest priority in forwarding
 * decisions. The only exception to this rule is when the switch
 * receives a packet on its CPU port with ALLOW=0. In that case the TX
 * field of the CPU tag will override the forwarding port mask.
 */
static int rtl8365mb_port_set_isolation(struct realtek_priv *priv, int port,
					u32 mask)
{
	return regmap_write(priv->map, RTL8365MB_PORT_ISOLATION_REG(port),
			    mask);
}

static int rtl8365mb_port_add_isolation(struct realtek_priv *priv, int port,
					u32 mask)
{
	return regmap_update_bits(priv->map, RTL8365MB_PORT_ISOLATION_REG(port),
				  mask, mask);
}

static int rtl8365mb_port_remove_isolation(struct realtek_priv *priv, int port,
					   u32 mask)
{
	return regmap_update_bits(priv->map, RTL8365MB_PORT_ISOLATION_REG(port),
				  mask, 0);
}

static int rtl8365mb_mib_counter_read(struct realtek_priv *priv, int port,
				      const struct rtl8365mb_mib_counter *mib,
				      u64 *mibvalue)
{
	struct rtl8365mb *mb = priv->chip_data;
	u32 offset = mib->offset;
	u64 tmpvalue = 0;
	u32 addr;
	u32 val;
	int ret;
	int i;

	/* The MIB address is an SRAM address. We request a particular address
	 * and then poll the control register before reading the value from some
	 * counter registers.
	 */
	addr = offset + mb->chip_info->family->mib_port_offset * port;
	ret = regmap_write(priv->map, RTL8365MB_MIB_ADDRESS_REG, addr >> 2);
	if (ret)
		return ret;

	/* Poll for completion */
	ret = regmap_read_poll_timeout(priv->map, RTL8365MB_MIB_CTRL0_REG, val,
				       !(val & RTL8365MB_MIB_CTRL0_BUSY_MASK),
				       10, 100);
	if (ret)
		return ret;

	/* Presumably this indicates a MIB counter read failure */
	if (val & RTL8365MB_MIB_CTRL0_RESET_MASK)
		return -EIO;

	/* There are four MIB counter registers each holding a 16 bit word of a
	 * MIB counter. Depending on the offset, we should read from the upper
	 * two or lower two registers. In case the MIB counter is 4 words, we
	 * read from all four registers.
	 */
	if (mib->length == 4)
		offset = 3;
	else
		offset = (offset + 1) % 4;

	/* Read the MIB counter 16 bits at a time */
	for (i = 0; i < mib->length; i++) {
		ret = regmap_read(priv->map,
				  RTL8365MB_MIB_COUNTER_REG(offset - i), &val);
		if (ret)
			return ret;

		tmpvalue = (tmpvalue << 16) | (val & 0xFFFF);
	}

	/* Only commit the result if no error occurred */
	*mibvalue = tmpvalue;

	return 0;
}

static void rtl8365mb_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_port *p = &mb->ports[port];
	int i;

	if (!mb->chip_info->family->mib_counters)
		return;

	spin_lock(&p->stats_lock);
	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		if (mb->chip_info->family->mib_counters[i].length)
			*data++ = p->stats_cache[i];
	}
	spin_unlock(&p->stats_lock);
}

static void rtl8365mb_get_strings(struct dsa_switch *ds, int port, u32 stringset,
				  u8 *data)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	if (!mb->chip_info->family->mib_counters)
		return;

	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		const struct rtl8365mb_mib_counter *mib =
			&mb->chip_info->family->mib_counters[i];

		if (mib->length)
			ethtool_puts(&data, mib->name);
	}
}

static int rtl8365mb_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	int count = 0;
	int i;

	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	if (!mb->chip_info->family->mib_counters)
		return -EOPNOTSUPP;

	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		if (mb->chip_info->family->mib_counters[i].length)
			count++;
	}

	return count;
}

static void rtl8365mb_get_phy_stats(struct dsa_switch *ds, int port,
				    struct ethtool_eth_phy_stats *phy_stats)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_port *p = &mb->ports[port];

	if (!mb->chip_info->family->mib_counters)
		return;

	spin_lock(&p->stats_lock);
	phy_stats->SymbolErrorDuringCarrier =
		p->stats_cache[RTL8365MB_MIB_dot3StatsSymbolErrors];
	spin_unlock(&p->stats_lock);
}

static void rtl8365mb_get_mac_stats(struct dsa_switch *ds, int port,
				    struct ethtool_eth_mac_stats *mac_stats)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_port *p = &mb->ports[port];
	u64 *cache = p->stats_cache;
	u64 v;

	if (!mb->chip_info->family->mib_counters)
		return;

	spin_lock(&p->stats_lock);

	/* The RTL8365MB-VC exposes MIB objects, which we have to translate into
	 * IEEE 802.3 Managed Objects. This is not always completely faithful,
	 * but we try out best. See RFC 3635 for a detailed treatment of the
	 * subject.
	 */

	v = cache[RTL8365MB_MIB_dot3OutPauseFrames];
	mac_stats->FramesTransmittedOK =
		cache[RTL8365MB_MIB_ifOutUcastPkts] +
		cache[RTL8365MB_MIB_ifOutMulticastPkts] +
		cache[RTL8365MB_MIB_ifOutBroadcastPkts] + v -
		cache[RTL8365MB_MIB_ifOutDiscards];

	mac_stats->SingleCollisionFrames =
		cache[RTL8365MB_MIB_dot3StatsSingleCollisionFrames];
	mac_stats->MultipleCollisionFrames =
		cache[RTL8365MB_MIB_dot3StatsMultipleCollisionFrames];

	v = cache[RTL8365MB_MIB_dot3InPauseFrames];
	mac_stats->FramesReceivedOK = cache[RTL8365MB_MIB_ifInUcastPkts] +
				      cache[RTL8365MB_MIB_ifInMulticastPkts] +
				      cache[RTL8365MB_MIB_ifInBroadcastPkts] +
				      v;

	mac_stats->FrameCheckSequenceErrors =
		cache[RTL8365MB_MIB_dot3StatsFCSErrors];
	mac_stats->OctetsTransmittedOK = cache[RTL8365MB_MIB_ifOutOctets] -
					 18 * mac_stats->FramesTransmittedOK;
	mac_stats->FramesWithDeferredXmissions =
		cache[RTL8365MB_MIB_dot3StatsDeferredTransmissions];
	mac_stats->LateCollisions = cache[RTL8365MB_MIB_dot3StatsLateCollisions];
	mac_stats->FramesAbortedDueToXSColls =
		cache[RTL8365MB_MIB_dot3StatsExcessiveCollisions];
	mac_stats->OctetsReceivedOK = cache[RTL8365MB_MIB_ifInOctets] -
				      18 * mac_stats->FramesReceivedOK;
	mac_stats->MulticastFramesXmittedOK =
		cache[RTL8365MB_MIB_ifOutMulticastPkts];
	mac_stats->BroadcastFramesXmittedOK =
		cache[RTL8365MB_MIB_ifOutBroadcastPkts];
	mac_stats->MulticastFramesReceivedOK =
		cache[RTL8365MB_MIB_ifInMulticastPkts];
	mac_stats->BroadcastFramesReceivedOK =
		cache[RTL8365MB_MIB_ifInBroadcastPkts];

	spin_unlock(&p->stats_lock);
}

static void rtl8365mb_get_ctrl_stats(struct dsa_switch *ds, int port,
				     struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_port *p = &mb->ports[port];

	if (!mb->chip_info->family->mib_counters)
		return;

	spin_lock(&p->stats_lock);
	ctrl_stats->UnsupportedOpcodesReceived = p->stats_cache[RTL8365MB_MIB_dot3ControlInUnknownOpcodes];
	spin_unlock(&p->stats_lock);
}

static void rtl8365mb_stats_update(struct realtek_priv *priv, int port)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_port *p = &mb->ports[port];
	u64 tmp_cache[RTL8365MB_MIB_END] = {0};
	int i;

	if (!mb->chip_info->family->mib_counters)
		return;

	dev_dbg(priv->dev, "rtl8365mb_stats_update: port=%d starting update\n", port);

	mutex_lock(&mb->mib_lock);

	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		const struct rtl8365mb_mib_counter *mib =
			&mb->chip_info->family->mib_counters[i];

		if (mib->length)
			rtl8365mb_mib_counter_read(priv, port, mib, &tmp_cache[i]);
	}

	spin_lock(&p->stats_lock);
	memcpy(p->stats_cache, tmp_cache, sizeof(tmp_cache));
	spin_unlock(&p->stats_lock);

	mutex_unlock(&mb->mib_lock);

	dev_dbg(priv->dev, "rtl8365mb_stats_update: port=%d update complete\n", port);
}

static void rtl8365mb_stats_poll(struct work_struct *work)
{
	struct rtl8365mb_port *p = container_of(to_delayed_work(work),
						struct rtl8365mb_port,
						mib_work);
	struct realtek_priv *priv = p->priv;

	rtl8365mb_stats_update(priv, p->index);

	schedule_delayed_work(&p->mib_work, RTL8365MB_STATS_INTERVAL_JIFFIES);
}

static void rtl8365mb_get_stats64(struct dsa_switch *ds, int port,
				  struct rtnl_link_stats64 *s)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_port *p = &mb->ports[port];
	u64 *cache = p->stats_cache;

	if (!mb->chip_info->family->mib_counters)
		return;

	spin_lock(&p->stats_lock);

	s->rx_packets = cache[RTL8365MB_MIB_ifInUcastPkts] +
			cache[RTL8365MB_MIB_ifInMulticastPkts] +
			cache[RTL8365MB_MIB_ifInBroadcastPkts];

	s->tx_packets = cache[RTL8365MB_MIB_ifOutUcastPkts] +
			cache[RTL8365MB_MIB_ifOutMulticastPkts] +
			cache[RTL8365MB_MIB_ifOutBroadcastPkts];

	/* if{In,Out}Octets includes FCS - remove it */
	s->rx_bytes = cache[RTL8365MB_MIB_ifInOctets] - 4 * s->rx_packets;
	s->tx_bytes = cache[RTL8365MB_MIB_ifOutOctets] - 4 * s->tx_packets;

	s->rx_dropped = cache[RTL8365MB_MIB_etherStatsDropEvents];
	s->tx_dropped = cache[RTL8365MB_MIB_ifOutDiscards];

	s->multicast = cache[RTL8365MB_MIB_ifInMulticastPkts];
	s->collisions = cache[RTL8365MB_MIB_etherStatsCollisions];

	s->rx_length_errors = cache[RTL8365MB_MIB_etherStatsFragments] +
			      cache[RTL8365MB_MIB_etherStatsJabbers];
	s->rx_crc_errors = cache[RTL8365MB_MIB_dot3StatsFCSErrors];
	s->rx_errors = s->rx_length_errors + s->rx_crc_errors;

	s->tx_aborted_errors = cache[RTL8365MB_MIB_ifOutDiscards];
	s->tx_window_errors = cache[RTL8365MB_MIB_dot3StatsLateCollisions];
	s->tx_errors = s->tx_aborted_errors + s->tx_window_errors;

	spin_unlock(&p->stats_lock);
}

static int rtl8365mb_stats_setup(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct dsa_switch *ds = &priv->ds;
	struct dsa_port *dp;
	int ret;

	/* Per-chip global mutex to protect MIB counter access, since doing
	 * so requires accessing a series of registers in a particular order.
	 */
	ret = devm_mutex_init(priv->dev, &mb->mib_lock);
	if (ret)
		return ret;

	dsa_switch_for_each_available_port(dp, ds) {
		struct rtl8365mb_port *p = &mb->ports[dp->index];

		/* Per-port spinlock to protect the stats_cache data */
		spin_lock_init(&p->stats_lock);

		/* This work polls the MIB counters and keeps the stats_cache data
		 * up-to-date.
		 */
		INIT_DELAYED_WORK(&p->mib_work, rtl8365mb_stats_poll);
	}

	return 0;
}

static void rtl8365mb_stats_teardown(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct dsa_switch *ds = &priv->ds;
	struct dsa_port *dp;

	dsa_switch_for_each_available_port(dp, ds) {
		struct rtl8365mb_port *p = &mb->ports[dp->index];

		cancel_delayed_work_sync(&p->mib_work);
	}
}

static int rtl8365mb_get_and_clear_status_reg(struct realtek_priv *priv, u32 reg,
					      u32 *val)
{
	int ret;

	ret = regmap_read(priv->map, reg, val);
	if (ret)
		return ret;

	return regmap_write(priv->map, reg, *val);
}

static irqreturn_t rtl8365mb_irq(int irq, void *data)
{
	struct realtek_priv *priv = data;
	unsigned long line_changes = 0;
	u32 stat;
	int line;
	int ret;

	ret = rtl8365mb_get_and_clear_status_reg(priv, RTL8365MB_INTR_STATUS_REG,
						 &stat);
	if (ret)
		goto out_error;

	if (stat & RTL8365MB_INTR_LINK_CHANGE_MASK) {
		u32 linkdown_ind;
		u32 linkup_ind;
		u32 val;

		ret = rtl8365mb_get_and_clear_status_reg(
			priv, RTL8365MB_PORT_LINKUP_IND_REG, &val);
		if (ret)
			goto out_error;

		linkup_ind = FIELD_GET(RTL8365MB_PORT_LINKUP_IND_MASK, val);

		ret = rtl8365mb_get_and_clear_status_reg(
			priv, RTL8365MB_PORT_LINKDOWN_IND_REG, &val);
		if (ret)
			goto out_error;

		linkdown_ind = FIELD_GET(RTL8365MB_PORT_LINKDOWN_IND_MASK, val);

		line_changes = linkup_ind | linkdown_ind;
	}

	if (!line_changes)
		goto out_none;

	for_each_set_bit(line, &line_changes, priv->num_ports) {
		int child_irq = irq_find_mapping(priv->irqdomain, line);

		if (!child_irq)
			continue;

		handle_nested_irq(child_irq);
	}

	return IRQ_HANDLED;

out_error:
	dev_err(priv->dev, "failed to read interrupt status: %pe\n",
		ERR_PTR(ret));

out_none:
	return IRQ_NONE;
}

static struct irq_chip rtl8365mb_irq_chip = {
	.name = "rtl8365mb",
	/* The hardware doesn't support masking IRQs on a per-port basis */
};

static int rtl8365mb_irq_map(struct irq_domain *domain, unsigned int irq,
			     irq_hw_number_t hwirq)
{
	irq_set_chip_data(irq, domain->host_data);
	irq_set_chip_and_handler(irq, &rtl8365mb_irq_chip, handle_simple_irq);
	irq_set_nested_thread(irq, 1);
	irq_set_noprobe(irq);

	return 0;
}

static void rtl8365mb_irq_unmap(struct irq_domain *d, unsigned int irq)
{
	irq_set_nested_thread(irq, 0);
	irq_set_chip_and_handler(irq, NULL, NULL);
	irq_set_chip_data(irq, NULL);
}

static const struct irq_domain_ops rtl8365mb_irqdomain_ops = {
	.map = rtl8365mb_irq_map,
	.unmap = rtl8365mb_irq_unmap,
	.xlate = irq_domain_xlate_onecell,
};

static int rtl8365mb_set_irq_enable(struct realtek_priv *priv, bool enable)
{
	return regmap_update_bits(priv->map, RTL8365MB_INTR_CTRL_REG,
				  RTL8365MB_INTR_LINK_CHANGE_MASK,
				  FIELD_PREP(RTL8365MB_INTR_LINK_CHANGE_MASK,
					     enable ? 1 : 0));
}

static int rtl8365mb_irq_enable(struct realtek_priv *priv)
{
	return rtl8365mb_set_irq_enable(priv, true);
}

static int rtl8365mb_irq_disable(struct realtek_priv *priv)
{
	return rtl8365mb_set_irq_enable(priv, false);
}

static int rtl8365mb_irq_setup(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct dsa_switch *ds = &priv->ds;
	struct device_node *intc;
	struct dsa_port *dp;
	u32 irq_trig;
	int virq;
	int irq;
	u32 val;
	int ret;

	intc = of_get_child_by_name(priv->dev->of_node, "interrupt-controller");
	if (!intc) {
		dev_err(priv->dev, "missing child interrupt-controller node\n");
		return -EINVAL;
	}

	/* rtl8365mb IRQs cascade off this one */
	irq = of_irq_get(intc, 0);
	if (irq <= 0) {
		if (!irq) {
			dev_err(priv->dev, "failed to map IRQ\n");
			ret = -EINVAL;
		} else {
			ret = dev_err_probe(priv->dev, irq,
					    "failed to get parent irq\n");
		}
		goto out_put_node;
	}

	priv->irqdomain = irq_domain_create_linear(of_fwnode_handle(intc), priv->num_ports,
						   &rtl8365mb_irqdomain_ops, priv);
	if (!priv->irqdomain) {
		dev_err(priv->dev, "failed to add irq domain\n");
		ret = -ENOMEM;
		goto out_put_node;
	}

	dsa_switch_for_each_available_port(dp, ds) {
		virq = irq_create_mapping(priv->irqdomain, dp->index);
		if (!virq) {
			dev_err(priv->dev,
				"failed to create irq domain mapping\n");
			ret = -EINVAL;
			goto out_remove_irqdomain;
		}

		irq_set_parent(virq, irq);
	}

	/* Configure chip interrupt signal polarity */
	irq_trig = irq_get_trigger_type(irq);
	switch (irq_trig) {
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_HIGH:
		val = RTL8365MB_INTR_POLARITY_HIGH;
		break;
	case IRQF_TRIGGER_FALLING:
	case IRQF_TRIGGER_LOW:
		val = RTL8365MB_INTR_POLARITY_LOW;
		break;
	default:
		dev_err(priv->dev, "unsupported irq trigger type %u\n",
			irq_trig);
		ret = -EINVAL;
		goto out_remove_irqdomain;
	}

	ret = regmap_update_bits(priv->map, RTL8365MB_INTR_POLARITY_REG,
				 RTL8365MB_INTR_POLARITY_MASK,
				 FIELD_PREP(RTL8365MB_INTR_POLARITY_MASK, val));
	if (ret)
		goto out_remove_irqdomain;

	/* Disable the interrupt in case the chip has it enabled on reset */
	ret = rtl8365mb_irq_disable(priv);
	if (ret)
		goto out_remove_irqdomain;

	/* Clear the interrupt status register */
	ret = regmap_write(priv->map, RTL8365MB_INTR_STATUS_REG,
			   RTL8365MB_INTR_ALL_MASK);
	if (ret)
		goto out_remove_irqdomain;

	ret = request_threaded_irq(irq, NULL, rtl8365mb_irq, IRQF_ONESHOT,
				   "rtl8365mb", priv);
	if (ret) {
		dev_err(priv->dev, "failed to request irq: %pe\n",
			ERR_PTR(ret));
		goto out_remove_irqdomain;
	}

	/* Store the irq so that we know to free it during teardown */
	mb->irq = irq;

	ret = rtl8365mb_irq_enable(priv);
	if (ret)
		goto out_free_irq;

	of_node_put(intc);

	return 0;

out_free_irq:
	free_irq(mb->irq, priv);
	mb->irq = 0;

out_remove_irqdomain:
	dsa_switch_for_each_port(dp, ds) {
		virq = irq_find_mapping(priv->irqdomain, dp->index);

		if (virq)
			irq_dispose_mapping(virq);
	}

	irq_domain_remove(priv->irqdomain);
	priv->irqdomain = NULL;

out_put_node:
	of_node_put(intc);

	return ret;
}

static void rtl8365mb_irq_teardown(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct dsa_switch *ds = &priv->ds;
	struct dsa_port *dp;
	int virq;

	if (mb->irq) {
		free_irq(mb->irq, priv);
		mb->irq = 0;
	}

	if (priv->irqdomain) {
		/* Unused ports with a linked PHY still have an active IRQ
		 * mapping that must be disposed of during teardown. Loop
		 * through all ports.
		 */
		dsa_switch_for_each_port(dp, ds) {
			virq = irq_find_mapping(priv->irqdomain, dp->index);

			if (virq)
				irq_dispose_mapping(virq);
		}

		irq_domain_remove(priv->irqdomain);
		priv->irqdomain = NULL;
	}
}

static int rtl8365mb_cpu_config(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_cpu *cpu = &mb->cpu;
	u32 val;
	int ret;

	ret = regmap_update_bits(priv->map, RTL8365MB_CPU_PORT_MASK_REG,
				 RTL8365MB_CPU_PORT_MASK_MASK,
				 FIELD_PREP(RTL8365MB_CPU_PORT_MASK_MASK,
					    cpu->mask));
	if (ret)
		return ret;

	val = FIELD_PREP(RTL8365MB_CPU_CTRL_EN_MASK, cpu->enable ? 1 : 0) |
	      FIELD_PREP(RTL8365MB_CPU_CTRL_INSERTMODE_MASK, cpu->insert) |
	      FIELD_PREP(RTL8365MB_CPU_CTRL_TAG_POSITION_MASK, cpu->position) |
	      FIELD_PREP(RTL8365MB_CPU_CTRL_RXBYTECOUNT_MASK, cpu->rx_length) |
	      FIELD_PREP(RTL8365MB_CPU_CTRL_TAG_FORMAT_MASK, cpu->format) |
	      FIELD_PREP(RTL8365MB_CPU_CTRL_TRAP_PORT_MASK, cpu->trap_port & 0x7) |
	      FIELD_PREP(RTL8365MB_CPU_CTRL_TRAP_PORT_EXT_MASK,
			 cpu->trap_port >> 3 & 0x1);
	ret = regmap_write(priv->map, RTL8365MB_CPU_CTRL_REG, val);
	if (ret)
		return ret;

	return 0;
}

static int rtl8365mb_change_tag_protocol(struct dsa_switch *ds,
					 enum dsa_tag_protocol proto)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_cpu *cpu;
	struct rtl8365mb *mb;

	mb = priv->chip_data;
	cpu = &mb->cpu;

	switch (proto) {
	case DSA_TAG_PROTO_RTL8_4:
		cpu->format = RTL8365MB_CPU_FORMAT_8BYTES;
		cpu->position = RTL8365MB_CPU_POS_AFTER_SA;
		break;
	case DSA_TAG_PROTO_RTL8_4T:
		cpu->format = RTL8365MB_CPU_FORMAT_8BYTES;
		cpu->position = RTL8365MB_CPU_POS_BEFORE_CRC;
		break;
	/* The switch also supports a 4-byte format, similar to rtl4a but with
	 * the same 0x04 8-bit version and probably 8-bit port source/dest.
	 * There is no public doc about it. Not supported yet and it will probably
	 * never be.
	 */
	default:
		return -EPROTONOSUPPORT;
	}

	return rtl8365mb_cpu_config(priv);
}

static int rtl8365mb_switch_init(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	const struct rtl8365mb_chip_info *ci;
	int ret;
	int i;

	ci = mb->chip_info;

	/* Family init jam */
	if (ci->family->jam_table) {
		for (i = 0; i < *ci->family->jam_size; i++) {
			ret = regmap_write(priv->map, ci->family->jam_table[i].reg,
					   ci->family->jam_table[i].val);
			if (ret)
				return ret;
		}
	}

	/* Chip init jam */
	if (ci->jam_table) {
		for (i = 0; i < *ci->jam_size; i++) {
			ret = regmap_write(priv->map, ci->jam_table[i].reg,
					   ci->jam_table[i].val);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int rtl8365mb_reset_chip(struct realtek_priv *priv)
{
	u32 val;

	priv->write_reg_noack(priv, RTL8365MB_CHIP_RESET_REG,
			      FIELD_PREP(RTL8365MB_CHIP_RESET_HW_MASK, 1));

	/* Realtek documentation says the chip needs 1 second to reset. Sleep
	 * for a while before accessing any registers to prevent ACK timeouts.
	 */
	msleep(100);
	return regmap_read_poll_timeout(priv->map, RTL8365MB_CHIP_RESET_REG, val,
					!(val & RTL8365MB_CHIP_RESET_HW_MASK),
					20000, 1e6);
}

static int rtl8365mb_setup(struct dsa_switch *ds)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_cpu *cpu;
	u32 downports_mask = 0;
	u32 upports_mask = 0;
	struct rtl8365mb *mb;
	struct dsa_port *dp;
	int ret;

	mb = priv->chip_data;
	cpu = &mb->cpu;

	ret = rtl8365mb_reset_chip(priv);
	if (ret) {
		dev_err(priv->dev, "failed to reset chip: %pe\n",
			ERR_PTR(ret));
		goto out_error;
	}

	/* Configure switch to vendor-defined initial state */
	ret = rtl8365mb_switch_init(priv);
	if (ret) {
		dev_err(priv->dev, "failed to initialize switch: %pe\n",
			ERR_PTR(ret));
		goto out_error;
	}

	/* Set up cascading IRQs */
	ret = rtl8365mb_irq_setup(priv);
	if (ret == -EPROBE_DEFER)
		return ret;
	else if (ret)
		dev_info(priv->dev, "no interrupt support\n");

	/* Start with all ports blocked, including unused ports */
	dsa_switch_for_each_port(dp, ds) {
		struct rtl8365mb_port *p = &mb->ports[dp->index];

		if (dsa_port_is_dsa(dp)) {
			/* Cascading (DSA links) is not supported yet.
			 * Historically, the driver has always been broken
			 * without a dedicated CPU port because CPU tagging
			 * would be disabled, rendering the switch entirely
			 * non-functional for DSA operations.
			 */
			dev_err(ds->dev,
				"Cascading (DSA link) not supported\n");
			ret = -EOPNOTSUPP;
			goto out_teardown_irq;
		}

		/* Set the initial STP state of all ports to DISABLED, otherwise
		 * ports will still forward frames to the CPU despite being
		 * administratively down by default.
		 */
		rtl8365mb_port_stp_state_set(ds, dp->index, BR_STATE_DISABLED);

		/* Start with all port completely isolated */
		ret = rtl8365mb_port_set_isolation(priv, dp->index, 0);
		if (ret)
			goto out_teardown_irq;

		/* Set the default EFID 0 for standalone mode */
		ret = rtl8365mb_port_set_efid(priv, dp->index, 0);
		if (ret)
			goto out_teardown_irq;

		/* Disable learning */
		ret = rtl8365mb_port_set_learning(priv, dp->index, false);
		if (ret)
			goto out_teardown_irq;

		/* Enable all types of flooding */
		ret = rtl83xx_setup_port_flood_control(priv, dp->index);
		if (ret)
			goto out_teardown_irq;

		/* Set up per-port private data */
		p->priv = priv;
		p->index = dp->index;

		/* Collect CPU ports. If we support cascade switches, it should
		 * also include the upstream DSA ports.
		 */
		if (!dsa_port_is_cpu(dp))
			continue;

		upports_mask |= BIT(dp->index);
	}

	/* Configure user ports */
	dsa_switch_for_each_port(dp, ds) {
		if (!dsa_port_is_user(dp))
			continue;

		/* Forward only to the CPU */
		ret = rtl8365mb_port_set_isolation(priv, dp->index,
						   upports_mask);
		if (ret)
			goto out_teardown_irq;

		/* If we support cascade switches, it should also include the
		 * downstream DSA ports.
		 */
		downports_mask |= BIT(dp->index);
	}

	/* Configure CPU tagging */
	/* If we support cascade switches, it should also include the upstream
	 * DSA ports.
	 */
	dsa_switch_for_each_cpu_port(dp, ds) {
		/* Use the first CPU port as trap_port */
		if (cpu->trap_port == mb->chip_info->family->num_ports)
			cpu->trap_port = dp->index;

		/* Forward to all user ports */
		ret = rtl8365mb_port_set_isolation(priv, dp->index,
						   downports_mask);
		if (ret)
			goto out_teardown_irq;
	}

	cpu->mask = upports_mask;
	cpu->enable = cpu->mask > 0;

	if (!cpu->enable) {
		dev_err(priv->dev, "no upstream (CPU, Link) port defined\n");
		ret = -EINVAL;
		goto out_teardown_irq;
	}

	ret = rtl8365mb_cpu_config(priv);
	if (ret)
		goto out_teardown_irq;

	ret = rtl8365mb_port_change_mtu(ds, cpu->trap_port, ETH_DATA_LEN);
	if (ret)
		goto out_teardown_irq;

	ds->assisted_learning_on_cpu_port = true;
	ds->fdb_isolation = true;
	/* The EFID is 3 bits, but EFID 0 is reserved for standalone ports */
	ds->max_num_bridges = FIELD_MAX(RTL8365MB_EFID_MASK);
	ds->configure_vlan_while_not_filtering = true;

	/* Set up VLAN */
	ret = rtl8365mb_vlan_setup(ds);
	if (ret)
		goto out_teardown_irq;

	ret = rtl83xx_setup_user_mdio(ds);
	if (ret) {
		dev_err(priv->dev, "could not set up MDIO bus\n");
		goto out_teardown_irq;
	}

	/* Start statistics counter polling */
	ret = rtl8365mb_stats_setup(priv);
	if (ret) {
		dev_err(priv->dev, "failed to setup stats\n");
		goto out_teardown_irq;
	}

	return 0;

out_teardown_irq:
	rtl8365mb_irq_teardown(priv);

out_error:
	return ret;
}

static void rtl8365mb_teardown(struct dsa_switch *ds)
{
	struct realtek_priv *priv = ds->priv;

	rtl8365mb_stats_teardown(priv);
	rtl8365mb_irq_teardown(priv);
}

static int rtl8365mb_get_chip_id_and_ver(struct regmap *map, u32 *id, u32 *ver)
{
	int ret;

	/* For some reason we have to write a magic value to an arbitrary
	 * register whenever accessing the chip ID/version registers.
	 */
	ret = regmap_write(map, RTL8365MB_MAGIC_REG, RTL8365MB_MAGIC_VALUE);
	if (ret)
		return ret;

	ret = regmap_read(map, RTL8365MB_CHIP_ID_REG, id);
	if (ret)
		return ret;

	ret = regmap_read(map, RTL8365MB_CHIP_VER_REG, ver);
	if (ret)
		return ret;

	/* Reset magic register */
	ret = regmap_write(map, RTL8365MB_MAGIC_REG, 0);
	if (ret)
		return ret;

	return 0;
}

static int rtl8365mb_detect(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	u32 chip_id;
	u32 chip_ver;
	int ret;
	int i;

	ret = rtl8365mb_get_chip_id_and_ver(priv->map, &chip_id, &chip_ver);
	if (ret) {
		dev_err(priv->dev, "failed to read chip id and version: %pe\n",
			ERR_PTR(ret));
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(rtl8365mb_chip_infos); i++) {
		const struct rtl8365mb_chip_info *ci = &rtl8365mb_chip_infos[i];

		if (ci->chip_id == chip_id && ci->chip_ver == chip_ver) {
			mb->chip_info = ci;
			break;
		}
	}

	if (!mb->chip_info) {
		dev_err(priv->dev,
			"unrecognized switch (id=0x%04x, ver=0x%04x)", chip_id,
			chip_ver);
		return -ENODEV;
	}

	dev_info(priv->dev, "found an %s switch (%s family)\n",
		 mb->chip_info->name, mb->chip_info->family->name);

	priv->num_ports = mb->chip_info->family->num_ports;
	mb->priv = priv;
	mb->cpu.trap_port = mb->chip_info->family->num_ports;
	mb->cpu.insert = RTL8365MB_CPU_INSERT_TO_ALL;
	mb->cpu.position = RTL8365MB_CPU_POS_AFTER_SA;
	mb->cpu.rx_length = RTL8365MB_CPU_RXLEN_64BYTES;
	mb->cpu.format = RTL8365MB_CPU_FORMAT_8BYTES;

	return 0;
}

static const struct phylink_mac_ops rtl8365mb_phylink_mac_ops = {
	.mac_config = rtl8365mb_phylink_mac_config,
	.mac_link_down = rtl8365mb_phylink_mac_link_down,
	.mac_link_up = rtl8365mb_phylink_mac_link_up,
};

static const struct dsa_switch_ops rtl8365mb_switch_ops = {
	.get_tag_protocol = rtl8365mb_get_tag_protocol,
	.change_tag_protocol = rtl8365mb_change_tag_protocol,
	.setup = rtl8365mb_setup,
	.teardown = rtl8365mb_teardown,
	.phylink_get_caps = rtl8365mb_phylink_get_caps,
	.port_bridge_join = rtl83xx_port_bridge_join,
	.port_bridge_leave = rtl83xx_port_bridge_leave,
	.port_pre_bridge_flags = rtl8365mb_port_pre_bridge_flags,
	.port_bridge_flags = rtl83xx_port_bridge_flags,
	.port_stp_state_set = rtl8365mb_port_stp_state_set,
	.port_fast_age = rtl83xx_port_fast_age,
	.port_fdb_add = rtl83xx_port_fdb_add,
	.port_fdb_del = rtl83xx_port_fdb_del,
	.port_fdb_dump = rtl83xx_port_fdb_dump,
	.port_mdb_add = rtl83xx_port_mdb_add,
	.port_mdb_del = rtl83xx_port_mdb_del,
	.port_vlan_add = rtl8365mb_port_vlan_add,
	.port_vlan_del = rtl8365mb_port_vlan_del,
	.port_vlan_filtering = rtl8365mb_port_vlan_filtering,
	.get_strings = rtl8365mb_get_strings,
	.get_ethtool_stats = rtl8365mb_get_ethtool_stats,
	.get_sset_count = rtl8365mb_get_sset_count,
	.get_eth_phy_stats = rtl8365mb_get_phy_stats,
	.get_eth_mac_stats = rtl8365mb_get_mac_stats,
	.get_eth_ctrl_stats = rtl8365mb_get_ctrl_stats,
	.get_stats64 = rtl8365mb_get_stats64,
	.port_change_mtu = rtl8365mb_port_change_mtu,
	.port_max_mtu = rtl8365mb_port_max_mtu,
	.port_hsr_join = dsa_port_simple_hsr_join,
	.port_hsr_leave = dsa_port_simple_hsr_leave,
};

static const struct realtek_ops rtl8365mb_ops = {
	.detect = rtl8365mb_detect,
	.port_add_isolation = rtl8365mb_port_add_isolation,
	.port_remove_isolation = rtl8365mb_port_remove_isolation,
	.port_set_efid = rtl8365mb_port_set_efid,
	.port_set_learning = rtl8365mb_port_set_learning,
	.port_set_ucast_flood = rtl8365mb_port_set_ucast_flood,
	.port_set_mcast_flood = rtl8365mb_port_set_mcast_flood,
	.port_set_bcast_flood = rtl8365mb_port_set_bcast_flood,
	.l2_add_uc = rtl8365mb_l2_add_uc,
	.l2_del_uc = rtl8365mb_l2_del_uc,
	.l2_get_next_uc = rtl8365mb_l2_get_next_uc,
	.l2_add_mc = rtl8365mb_l2_add_mc,
	.l2_del_mc = rtl8365mb_l2_del_mc,
	.l2_flush = rtl8365mb_l2_flush,
	.phy_read = rtl8365mb_phy_read,
	.phy_write = rtl8365mb_phy_write,
};

const struct realtek_variant rtl8365mb_variant = {
	.ds_ops = &rtl8365mb_switch_ops,
	.ops = &rtl8365mb_ops,
	.phylink_mac_ops = &rtl8365mb_phylink_mac_ops,
	.clk_delay = 10,
	.reset_delay_ms = 100,
	.cmd_read = 0xb9,
	.cmd_write = 0xb8,
	.l2_table_size = RTL8365MB_L2_TABLE_SIZE,
	.chip_data_sz = sizeof(struct rtl8365mb),
};

static const struct of_device_id rtl8365mb_of_match[] = {
	{ .compatible = "realtek,rtl8365mb", .data = &rtl8365mb_variant, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rtl8365mb_of_match);

static struct platform_driver rtl8365mb_smi_driver = {
	.driver = {
		.name = "rtl8365mb-smi",
		.of_match_table = rtl8365mb_of_match,
	},
	.probe  = realtek_smi_probe,
	.remove = realtek_smi_remove,
	.shutdown = realtek_smi_shutdown,
};

static struct mdio_driver rtl8365mb_mdio_driver = {
	.mdiodrv.driver = {
		.name = "rtl8365mb-mdio",
		.of_match_table = rtl8365mb_of_match,
	},
	.probe  = realtek_mdio_probe,
	.remove = realtek_mdio_remove,
	.shutdown = realtek_mdio_shutdown,
};

static int rtl8365mb_init(void)
{
	int ret;

	ret = realtek_mdio_driver_register(&rtl8365mb_mdio_driver);
	if (ret)
		return ret;

	ret = realtek_smi_driver_register(&rtl8365mb_smi_driver);
	if (ret) {
		realtek_mdio_driver_unregister(&rtl8365mb_mdio_driver);
		return ret;
	}

	return 0;
}
module_init(rtl8365mb_init);

static void __exit rtl8365mb_exit(void)
{
	realtek_smi_driver_unregister(&rtl8365mb_smi_driver);
	realtek_mdio_driver_unregister(&rtl8365mb_mdio_driver);
}
module_exit(rtl8365mb_exit);

MODULE_AUTHOR("Alvin Šipraga <alsi@bang-olufsen.dk>");
MODULE_DESCRIPTION("Driver for RTL8365MB-VC ethernet switch");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("REALTEK_DSA");
MODULE_FIRMWARE(RTL8365MB_DW8051_SGMII_INIT_FW);
