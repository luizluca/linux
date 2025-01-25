// SPDX-License-Identifier: GPL-2.0
/* Realtek SMI subdriver for the Realtek RTL8365MB-VC ethernet switch.
 *
 * Copyright (C) 2021 Alvin Šipraga <alsi@bang-olufsen.dk>
 * Copyright (C) 2021 Michael Rasmussen <mir@bang-olufsen.dk>
 *
 * The RTL8365MB-VC is a 4+1 port 10/100/1000M switch controller. It includes 4
 * integrated PHYs for the user facing ports, and an extension interface which
 * can be connected to the CPU - or another PHY - via either MII, RMII, or
 * RGMII. The switch is configured via the Realtek Simple Management Interface
 * (SMI), which uses the MDIO/MDC lines.
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
 * NOTE: Currently, only the RGMII interface is implemented in this driver.
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
#include "rtl8365mb_acl.h"
#include "rtl8365mb_l2.h"
#include "rtl8365mb_vlan.h"

/* Family-specific data and limits */
#define RTL8365MB_PHYADDRMAX		7
#define RTL8365MB_NUM_PHYREGS		32
#define RTL8365MB_PHYREGMAX		(RTL8365MB_NUM_PHYREGS - 1)
#define RTL8365MB_MAX_NUM_PORTS		11
#define RTL8365MB_PORTMASK		GENMASK(RTL8365MB_MAX_NUM_PORTS - 1, 0)
#define RTL8365MB_MAX_NUM_EXTINTS	3

/* Chip identification registers */
#define RTL8365MB_CHIP_ID_REG		0x1300

#define RTL8365MB_CHIP_VER_REG		0x1301

#define RTL8365MB_MAGIC_REG		0x13C2
#define   RTL8365MB_MAGIC_VALUE		0x0249

/* Chip reset register */
#define RTL8365MB_CHIP_RESET_REG	0x1322
#define RTL8365MB_CHIP_RESET_SW_MASK	0x0002
#define RTL8365MB_CHIP_RESET_HW_MASK	0x0001

/* Interrupt polarity register */
#define RTL8365MB_INTR_POLARITY_REG	0x1100
#define   RTL8365MB_INTR_POLARITY_MASK	0x0001
#define   RTL8365MB_INTR_POLARITY_HIGH	0
#define   RTL8365MB_INTR_POLARITY_LOW	1

/* Interrupt control/status register - enable/check specific interrupt types */
#define RTL8365MB_INTR_CTRL_REG			0x1101
#define RTL8365MB_INTR_STATUS_REG		0x1102
#define   RTL8365MB_INTR_SLIENT_START_2_MASK	0x1000
#define   RTL8365MB_INTR_SLIENT_START_MASK	0x0800
#define   RTL8365MB_INTR_ACL_ACTION_MASK	0x0200
#define   RTL8365MB_INTR_CABLE_DIAG_FIN_MASK	0x0100
#define   RTL8365MB_INTR_INTERRUPT_8051_MASK	0x0080
#define   RTL8365MB_INTR_LOOP_DETECTION_MASK	0x0040
#define   RTL8365MB_INTR_GREEN_TIMER_MASK	0x0020
#define   RTL8365MB_INTR_SPECIAL_CONGEST_MASK	0x0010
#define   RTL8365MB_INTR_SPEED_CHANGE_MASK	0x0008
#define   RTL8365MB_INTR_LEARN_OVER_MASK	0x0004
#define   RTL8365MB_INTR_METER_EXCEEDED_MASK	0x0002
#define   RTL8365MB_INTR_LINK_CHANGE_MASK	0x0001
#define   RTL8365MB_INTR_ALL_MASK                      \
		(RTL8365MB_INTR_SLIENT_START_2_MASK |  \
		 RTL8365MB_INTR_SLIENT_START_MASK |    \
		 RTL8365MB_INTR_ACL_ACTION_MASK |      \
		 RTL8365MB_INTR_CABLE_DIAG_FIN_MASK |  \
		 RTL8365MB_INTR_INTERRUPT_8051_MASK |  \
		 RTL8365MB_INTR_LOOP_DETECTION_MASK |  \
		 RTL8365MB_INTR_GREEN_TIMER_MASK |     \
		 RTL8365MB_INTR_SPECIAL_CONGEST_MASK | \
		 RTL8365MB_INTR_SPEED_CHANGE_MASK |    \
		 RTL8365MB_INTR_LEARN_OVER_MASK |      \
		 RTL8365MB_INTR_METER_EXCEEDED_MASK |  \
		 RTL8365MB_INTR_LINK_CHANGE_MASK)

/* Per-port interrupt type status registers */
#define RTL8365MB_PORT_LINKDOWN_IND_REG		0x1106
#define   RTL8365MB_PORT_LINKDOWN_IND_MASK	0x07FF

#define RTL8365MB_PORT_LINKUP_IND_REG		0x1107
#define   RTL8365MB_PORT_LINKUP_IND_MASK	0x07FF

/* PHY indirect access registers */
#define RTL8365MB_INDIRECT_ACCESS_CTRL_REG			0x1F00
#define   RTL8365MB_INDIRECT_ACCESS_CTRL_RW_MASK		0x0002
#define   RTL8365MB_INDIRECT_ACCESS_CTRL_RW_READ		0
#define   RTL8365MB_INDIRECT_ACCESS_CTRL_RW_WRITE		1
#define   RTL8365MB_INDIRECT_ACCESS_CTRL_CMD_MASK		0x0001
#define   RTL8365MB_INDIRECT_ACCESS_CTRL_CMD_VALUE		1
#define RTL8365MB_INDIRECT_ACCESS_STATUS_REG			0x1F01
#define RTL8365MB_INDIRECT_ACCESS_ADDRESS_REG			0x1F02
#define   RTL8365MB_INDIRECT_ACCESS_ADDRESS_OCPADR_5_1_MASK	GENMASK(4, 0)
#define   RTL8365MB_INDIRECT_ACCESS_ADDRESS_PHYNUM_MASK		GENMASK(7, 5)
#define   RTL8365MB_INDIRECT_ACCESS_ADDRESS_OCPADR_9_6_MASK	GENMASK(11, 8)
#define   RTL8365MB_PHY_BASE					0x2000
#define RTL8365MB_INDIRECT_ACCESS_WRITE_DATA_REG		0x1F03
#define RTL8365MB_INDIRECT_ACCESS_READ_DATA_REG			0x1F04

/* PHY OCP address prefix register */
#define RTL8365MB_GPHY_OCP_MSB_0_REG			0x1D15
#define   RTL8365MB_GPHY_OCP_MSB_0_CFG_CPU_OCPADR_MASK	0x0FC0
#define RTL8365MB_PHY_OCP_ADDR_PREFIX_MASK		0xFC00

/* The PHY OCP addresses of PHY registers 0~31 start here */
#define RTL8365MB_PHY_OCP_ADDR_PHYREG_BASE		0xA400

/* External interface port mode values - used in DIGITAL_INTERFACE_SELECT */
#define RTL8365MB_EXT_PORT_MODE_DISABLE		0
#define RTL8365MB_EXT_PORT_MODE_RGMII		1
#define RTL8365MB_EXT_PORT_MODE_MII_MAC		2
#define RTL8365MB_EXT_PORT_MODE_MII_PHY		3
#define RTL8365MB_EXT_PORT_MODE_TMII_MAC	4
#define RTL8365MB_EXT_PORT_MODE_TMII_PHY	5
#define RTL8365MB_EXT_PORT_MODE_GMII		6
#define RTL8365MB_EXT_PORT_MODE_RMII_MAC	7
#define RTL8365MB_EXT_PORT_MODE_RMII_PHY	8
#define RTL8365MB_EXT_PORT_MODE_SGMII		9
#define RTL8365MB_EXT_PORT_MODE_HSGMII		10
#define RTL8365MB_EXT_PORT_MODE_1000X_100FX	11
#define RTL8365MB_EXT_PORT_MODE_1000X		12
#define RTL8365MB_EXT_PORT_MODE_100FX		13

/* External interface mode configuration registers 0~1 */
#define RTL8365MB_DIGITAL_INTERFACE_SELECT_REG0		0x1305 /* EXT0,EXT1 */
#define RTL8365MB_DIGITAL_INTERFACE_SELECT_REG1		0x13C3 /* EXT2 */
#define RTL8365MB_DIGITAL_INTERFACE_SELECT_REG(_extint) \
		((_extint) <= 1 ? RTL8365MB_DIGITAL_INTERFACE_SELECT_REG0 : \
		 (_extint) == 2 ? RTL8365MB_DIGITAL_INTERFACE_SELECT_REG1 : \
		 0x0)
#define   RTL8365MB_DIGITAL_INTERFACE_SELECT_MODE_MASK(_extint) \
		(0xF << (((_extint) % 2)))
#define   RTL8365MB_DIGITAL_INTERFACE_SELECT_MODE_OFFSET(_extint) \
		(((_extint) % 2) * 4)

/* External interface RGMII TX/RX delay configuration registers 0~2 */
#define RTL8365MB_EXT_RGMXF_REG0		0x1306 /* EXT0 */
#define RTL8365MB_EXT_RGMXF_REG1		0x1307 /* EXT1 */
#define RTL8365MB_EXT_RGMXF_REG2		0x13C5 /* EXT2 */
#define RTL8365MB_EXT_RGMXF_REG(_extint) \
		((_extint) == 0 ? RTL8365MB_EXT_RGMXF_REG0 : \
		 (_extint) == 1 ? RTL8365MB_EXT_RGMXF_REG1 : \
		 (_extint) == 2 ? RTL8365MB_EXT_RGMXF_REG2 : \
		 0x0)
#define   RTL8365MB_EXT_RGMXF_RXDELAY_MASK	0x0007
#define   RTL8365MB_EXT_RGMXF_TXDELAY_MASK	0x0008

/* External interface port speed values - used in DIGITAL_INTERFACE_FORCE */
#define RTL8365MB_PORT_SPEED_10M	0
#define RTL8365MB_PORT_SPEED_100M	1
#define RTL8365MB_PORT_SPEED_1000M	2

/* External interface force configuration registers 0~2 */
#define RTL8365MB_DIGITAL_INTERFACE_FORCE_REG0		0x1310 /* EXT0 */
#define RTL8365MB_DIGITAL_INTERFACE_FORCE_REG1		0x1311 /* EXT1 */
#define RTL8365MB_DIGITAL_INTERFACE_FORCE_REG2		0x13C4 /* EXT2 */
#define RTL8365MB_DIGITAL_INTERFACE_FORCE_REG(_extint) \
		((_extint) == 0 ? RTL8365MB_DIGITAL_INTERFACE_FORCE_REG0 : \
		 (_extint) == 1 ? RTL8365MB_DIGITAL_INTERFACE_FORCE_REG1 : \
		 (_extint) == 2 ? RTL8365MB_DIGITAL_INTERFACE_FORCE_REG2 : \
		 0x0)
#define   RTL8365MB_DIGITAL_INTERFACE_FORCE_EN_MASK		0x1000
#define   RTL8365MB_DIGITAL_INTERFACE_FORCE_NWAY_MASK		0x0080
#define   RTL8365MB_DIGITAL_INTERFACE_FORCE_TXPAUSE_MASK	0x0040
#define   RTL8365MB_DIGITAL_INTERFACE_FORCE_RXPAUSE_MASK	0x0020
#define   RTL8365MB_DIGITAL_INTERFACE_FORCE_LINK_MASK		0x0010
#define   RTL8365MB_DIGITAL_INTERFACE_FORCE_DUPLEX_MASK		0x0004
#define   RTL8365MB_DIGITAL_INTERFACE_FORCE_SPEED_MASK		0x0003

/* CPU port mask register - controls which ports are treated as CPU ports */
#define RTL8365MB_CPU_PORT_MASK_REG	0x1219
#define   RTL8365MB_CPU_PORT_MASK_MASK	0x07FF

/* CPU control register */
#define RTL8365MB_CPU_CTRL_REG			0x121A
#define   RTL8365MB_CPU_CTRL_TRAP_PORT_EXT_MASK	0x0400
#define   RTL8365MB_CPU_CTRL_TAG_FORMAT_MASK	0x0200
#define   RTL8365MB_CPU_CTRL_RXBYTECOUNT_MASK	0x0080
#define   RTL8365MB_CPU_CTRL_TAG_POSITION_MASK	0x0040
#define   RTL8365MB_CPU_CTRL_TRAP_PORT_MASK	0x0038
#define   RTL8365MB_CPU_CTRL_INSERTMODE_MASK	0x0006
#define   RTL8365MB_CPU_CTRL_EN_MASK		0x0001

/* Maximum packet length register */
#define RTL8365MB_CFG0_MAX_LEN_REG	0x088C
#define   RTL8365MB_CFG0_MAX_LEN_MASK	0x3FFF
#define RTL8365MB_CFG0_MAX_LEN_MAX	0x3FFF

/* Port learning limit registers */
#define RTL8365MB_LUT_PORT_LEARN_LIMIT_BASE		0x0A20
#define RTL8365MB_LUT_PORT_LEARN_LIMIT_REG(_physport) \
		(RTL8365MB_LUT_PORT_LEARN_LIMIT_BASE + (_physport))

/* Port isolation (forwarding mask) registers */
#define RTL8365MB_PORT_ISOLATION_REG_BASE		0x08A2
#define RTL8365MB_PORT_ISOLATION_REG(_physport) \
		(RTL8365MB_PORT_ISOLATION_REG_BASE + (_physport))
#define   RTL8365MB_PORT_ISOLATION_MASK			0x07FF

/* Extended filter ID registers - used to key forwarding database with IVL */
#define RTL8365MB_PORT_EFID_REG_BASE		0x0A32
#define RTL8365MB_PORT_EFID_REG(_p) \
		(RTL8365MB_PORT_EFID_REG_BASE + ((_p) >> 2))
#define   RTL8365MB_PORT_EFID_OFFSET(_p)	(((_p) & 0x3) << 2)
#define   RTL8365MB_PORT_EFID_MASK(_p) (0x7 << RTL8365MB_PORT_EFID_OFFSET(_p))

/* MSTP port state registers - indexed by tree instance */
#define RTL8365MB_MSTI_CTRL_BASE			0x0A00
#define RTL8365MB_MSTI_CTRL_REG(_msti, _physport) \
		(RTL8365MB_MSTI_CTRL_BASE + ((_msti) << 1) + ((_physport) >> 3))
#define   RTL8365MB_MSTI_CTRL_PORT_STATE_OFFSET(_physport) ((_physport) << 1)
#define   RTL8365MB_MSTI_CTRL_PORT_STATE_MASK(_physport) \
		(0x3 << RTL8365MB_MSTI_CTRL_PORT_STATE_OFFSET((_physport)))

/* Unknown unicast DA flooding port mask */
#define RTL8365MB_UNKNOWN_UNICAST_FLOODING_PMASK_REG		0x0890
#define   RTL8365MB_UNKNOWN_UNICAST_FLOODING_PMASK_MASK		0x07FF

/* Unknown multicast DA flooding port mask */
#define RTL8365MB_UNKNOWN_MULTICAST_FLOODING_PMASK_REG		0x0891
#define   RTL8365MB_UNKNOWN_MULTICAST_FLOODING_PMASK_MASK	0x07FF

/* Broadcast flooding port mask */
#define RTL8365MB_UNKNOWN_BROADCAST_FLOODING_PMASK_REG		0x0892
#define   RTL8365MB_UNKNOWN_BROADCAST_FLOODING_PMASK_MASK	0x07FF

/* Port-based VID registers 0~5 - each one holds an MC index for two ports */
#define RTL8365MB_VLAN_PVID_CTRL_BASE			0x0700
#define RTL8365MB_VLAN_PVID_CTRL_REG(_p) \
		(RTL8365MB_VLAN_PVID_CTRL_BASE + ((_p) >> 1))
#define   RTL8365MB_VLAN_PVID_CTRL_PORT0_MCIDX_MASK	0x001F
#define   RTL8365MB_VLAN_PVID_CTRL_PORT1_MCIDX_MASK	0x1F00
#define   RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_OFFSET(_p) \
		(((_p) & 1) << 3)
#define   RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_MASK(_p) \
		(0x1F << RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_OFFSET(_p))

/* Miscellaneous port configuration register, incl. VLAN egress mode */
#define RTL8365MB_PORT_MISC_CFG_REG_BASE			0x000E
#define RTL8365MB_PORT_MISC_CFG_REG(_p) \
		(RTL8365MB_PORT_MISC_CFG_REG_BASE + ((_p) << 5))
#define   RTL8365MB_PORT_MISC_CFG_SMALL_TAG_IPG_MASK		0x8000
#define   RTL8365MB_PORT_MISC_CFG_TX_ITFSP_MODE_MASK		0x4000
#define   RTL8365MB_PORT_MISC_CFG_FLOWCTRL_INDEP_MASK		0x2000
#define   RTL8365MB_PORT_MISC_CFG_DOT1Q_REMARK_ENABLE_MASK	0x1000
#define   RTL8365MB_PORT_MISC_CFG_INGRESSBW_FLOWCTRL_MASK	0x0800
#define   RTL8365MB_PORT_MISC_CFG_INGRESSBW_IFG_MASK		0x0400
#define   RTL8365MB_PORT_MISC_CFG_RX_SPC_MASK			0x0200
#define   RTL8365MB_PORT_MISC_CFG_CRC_SKIP_MASK			0x0100
#define   RTL8365MB_PORT_MISC_CFG_PKTGEN_TX_FIRST_MASK		0x0080
#define   RTL8365MB_PORT_MISC_CFG_MAC_LOOPBACK_MASK		0x0040
#define   RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_MASK		0x0030
#define	    RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_ORIGINAL	0
#define	    RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_KEEP	1
#define	    RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_PRI_TAG	2
#define	    RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_REAL_KEEP	3
#define   RTL8365MB_PORT_MISC_CFG_CONGESTION_SUSTAIN_TIME_MASK	0x000F

/**
 * enum rtl8365mb_vlan_egress_mode - port VLAN engress mode
 * @RTL8365MB_VLAN_EGRESS_MODE_ORIGINAL: follow untag mask in VLAN4k table entry
 * @RTL8365MB_VLAN_EGRESS_MODE_KEEP: the VLAN tag format of egressed packets
 * will remain the same as their ingressed format, but the priority and VID
 * fields may be altered
 * @RTL8365MB_VLAN_EGRESS_MODE_PRI_TAG: always egress with priority tag
 * @RTL8365MB_VLAN_EGRESS_MODE_REAL_KEEP: the VLAN tag format of egressed
 * packets will remain the same as their ingressed format, and neither the
 * priority nor VID fields can be altered
 */
enum rtl8365mb_vlan_egress_mode {
	RTL8365MB_VLAN_EGRESS_MODE_ORIGINAL = 0,
	RTL8365MB_VLAN_EGRESS_MODE_KEEP = 1,
	RTL8365MB_VLAN_EGRESS_MODE_PRI_TAG = 2,
	RTL8365MB_VLAN_EGRESS_MODE_REAL_KEEP = 3,
};

/* VLAN control register */
#define RTL8365MB_VLAN_CTRL_REG			0x07A8
#define   RTL8365MB_VLAN_CTRL_EN_MASK		0x0001

/* VLAN ingress filter register */
#define RTL8365MB_VLAN_INGRESS_REG				0x07A9
#define   RTL8365MB_VLAN_INGRESS_FILTER_PORT_EN_OFFSET(_p)	(_p)
#define   RTL8365MB_VLAN_INGRESS_FILTER_PORT_EN_MASK(_p)	BIT(_p)

/* VLAN "transparent" setting registers */
#define RTL8365MB_VLAN_EGRESS_TRANSPARENT_REG_BASE	0x09D0
#define RTL8365MB_VLAN_EGRESS_TRANSPARENT_REG(_p) \
		(RTL8365MB_VLAN_EGRESS_TRANSPARENT_REG_BASE + (_p))

/* VLAN egress "keep" port mask registers */
#define RTL8365MB_VLAN_EGRESS_KEEP_REG_BASE	0x093B
#define RTL8365MB_VLAN_EGRESS_KEEP_REG(_p) \
		(RTL8365MB_VLAN_EGRESS_KEEP_REG_BASE + ((_p) >> 1))
#define RTL8365MB_VLAN_EGRESS_KEEP_OFFSET(_p)	(((_p) & 1) * 8)
#define RTL8365MB_VLAN_EGRESS_KEEP_MASK		0x00FF

#define RTL8365MB_VLAN_EGRESS_KEEP_EXT_REG_BASE		0x08D8
#define RTL8365MB_VLAN_EGRESS_KEEP_EXT_REG(_p) \
		(RTL8365MB_VLAN_EGRESS_KEEP_EXT_REG_BASE + ((_p) >> 1))
#define   RTL8365MB_VLAN_EGRESS_KEEP_EXT_OFFSET(_p)	(((_p) & 1) * 3)
#define   RTL8365MB_VLAN_EGRESS_KEEP_EXT_MASK		0x0007

/* MIB counter value registers */
#define RTL8365MB_MIB_COUNTER_BASE	0x1000
#define RTL8365MB_MIB_COUNTER_REG(_x)	(RTL8365MB_MIB_COUNTER_BASE + (_x))

/* MIB counter address register */
#define RTL8365MB_MIB_ADDRESS_REG		0x1004
#define   RTL8365MB_MIB_ADDRESS_PORT_OFFSET	0x007C
#define   RTL8365MB_MIB_ADDRESS(_p, _x) \
		(((RTL8365MB_MIB_ADDRESS_PORT_OFFSET) * (_p) + (_x)) >> 2)

#define RTL8365MB_MIB_CTRL0_REG			0x1005
#define   RTL8365MB_MIB_CTRL0_RESET_MASK	0x0002
#define   RTL8365MB_MIB_CTRL0_BUSY_MASK		0x0001

/* The DSA callback .get_stats64 runs in atomic context, so we are not allowed
 * to block. On the other hand, accessing MIB counters absolutely requires us to
 * block. The solution is thus to schedule work which polls the MIB counters
 * asynchronously and updates some private data, which the callback can then
 * fetch atomically. Three seconds should be a good enough polling interval.
 */
#define RTL8365MB_STATS_INTERVAL_JIFFIES	(3 * HZ)

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

#define RTL8365MB_MAKE_MIB_COUNTER(_offset, _length, _name) \
		[RTL8365MB_MIB_ ## _name] = { _offset, _length, #_name }

static struct rtl8365mb_mib_counter rtl8365mb_mib_counters[] = {
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
};

static_assert(ARRAY_SIZE(rtl8365mb_mib_counters) == RTL8365MB_MIB_END);

struct rtl8365mb_jam_tbl_entry {
	u16 reg;
	u16 val;
};

/* Lifted from the vendor driver sources */
static const struct rtl8365mb_jam_tbl_entry rtl8365mb_init_jam_8365mb_vc[] = {
	{ 0x13EB, 0x15BB }, { 0x1303, 0x06D6 }, { 0x1304, 0x0700 },
	{ 0x13E2, 0x003F }, { 0x13F9, 0x0090 }, { 0x121E, 0x03CA },
	{ 0x1233, 0x0352 }, { 0x1237, 0x00A0 }, { 0x123A, 0x0030 },
	{ 0x1239, 0x0084 }, { 0x0301, 0x1000 }, { 0x1349, 0x001F },
	{ 0x18E0, 0x4004 }, { 0x122B, 0x241C }, { 0x1305, 0xC000 },
	{ 0x13F0, 0x0000 },
};

static const struct rtl8365mb_jam_tbl_entry rtl8365mb_init_jam_common[] = {
	{ 0x1200, 0x7FCB }, { 0x0884, 0x0003 }, { 0x06EB, 0x0001 },
	{ 0x03Fa, 0x0007 }, { 0x08C8, 0x00C0 }, { 0x0A30, 0x020E },
	{ 0x0800, 0x0000 }, { 0x0802, 0x0000 }, { 0x09DA, 0x0013 },
	{ 0x1D32, 0x0002 },
};

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
	const struct rtl8365mb_extint extints[RTL8365MB_MAX_NUM_EXTINTS];
	const struct rtl8365mb_jam_tbl_entry *jam_table;
	size_t jam_size;
};

/* Chip info for each supported switch in the family */
#define PHY_INTF(_mode) (RTL8365MB_PHY_INTERFACE_MODE_ ## _mode)
static const struct rtl8365mb_chip_info rtl8365mb_chip_infos[] = {
	{
		.name = "RTL8365MB-VC",
		.chip_id = 0x6367,
		.chip_ver = 0x0040,
		.extints = {
			{ 6, 1, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
		},
		.jam_table = rtl8365mb_init_jam_8365mb_vc,
		.jam_size = ARRAY_SIZE(rtl8365mb_init_jam_8365mb_vc),
	},
	{
		.name = "RTL8367S",
		.chip_id = 0x6367,
		.chip_ver = 0x00A0,
		.extints = {
			{ 6, 1, PHY_INTF(SGMII) | PHY_INTF(HSGMII) },
			{ 7, 2, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
		},
		.jam_table = rtl8365mb_init_jam_8365mb_vc,
		.jam_size = ARRAY_SIZE(rtl8365mb_init_jam_8365mb_vc),
	},
	{
		.name = "RTL8367RB-VB",
		.chip_id = 0x6367,
		.chip_ver = 0x0020,
		.extints = {
			{ 6, 1, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
			{ 7, 2, PHY_INTF(MII) | PHY_INTF(TMII) |
				PHY_INTF(RMII) | PHY_INTF(RGMII) },
		},
		.jam_table = rtl8365mb_init_jam_8365mb_vc,
		.jam_size = ARRAY_SIZE(rtl8365mb_init_jam_8365mb_vc),
	},
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
 * @pvid: port-based VLAN ID, or 0 if unset
 * @stats: link statistics populated by rtl8365mb_stats_poll, ready for atomic
 *         access via rtl8365mb_get_stats64
 * @stats_lock: protect the stats structure during read/update
 * @mib_work: delayed work for polling MIB counters
 */
struct rtl8365mb_port {
	struct realtek_priv *priv;
	unsigned int index;
	u16 pvid;
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
 * @vlanmc_db: VLAN membership configuration database
 * @vlanmc_null: reserved VLAN membership config entry for disabling PVID
 * @vlanmc_unaware: reserved VLAN membership config entry for VLAN-unaware mode
 * @vlanmc_synced: VLAN membership configs that should be synced with
 *                 the VLAN4k table
 * @mib_lock: prevent concurrent reads of MIB counters
 * @l2_lock: prevent concurrent access to L2 look-up table
 * @ports: per-port data
 * @debugfs_dir: debugfs directory
 *
 * Private data for this driver.
 */
struct rtl8365mb {
	struct realtek_priv *priv;
	int irq;
	const struct rtl8365mb_chip_info *chip_info;
	struct rtl8365mb_cpu cpu;
	struct rtl8365mb_vlanmc_db vlanmc_db;
	struct rtl8365mb_vlanmc_entry *vlanmc_null;
	struct rtl8365mb_vlanmc_entry *vlanmc_unaware;
	struct list_head vlanmc_synced;
	struct mutex mib_lock;
	struct mutex l2_lock;
	struct rtl8365mb_port ports[RTL8365MB_MAX_NUM_PORTS];
	struct dentry *debugfs_dir;
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

	return 0;
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
			"failed to read PHY%d reg %02x @ %04x, ret %d\n", phy,
			regnum, ocp_addr, ret);
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
			"failed to write PHY%d reg %02x @ %04x, ret %d\n", phy,
			regnum, ocp_addr, ret);
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

		if (speed == SPEED_1000) {
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
				"failed to configure RGMII mode on port %d: %d\n",
				port, ret);
		return;
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

	if (phy_interface_mode_is_rgmii(interface)) {
		ret = rtl8365mb_ext_config_forcemode(priv, port, false, 0, 0,
						     false, false);
		if (ret)
			dev_err(priv->dev,
				"failed to reset forced mode on port %d: %d\n",
				port, ret);

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

	if (phy_interface_mode_is_rgmii(interface)) {
		ret = rtl8365mb_ext_config_forcemode(priv, port, true, speed,
						     duplex, tx_pause,
						     rx_pause);
		if (ret)
			dev_err(priv->dev,
				"failed to force mode on port %d: %d\n", port,
				ret);

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

static void rtl8365mb_port_fast_age(struct dsa_switch *ds, int port)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb = priv->chip_data;
	int ret;

	mutex_lock(&mb->l2_lock);
	ret = rtl8365mb_l2_flush(priv, port, 0);
	mutex_unlock(&mb->l2_lock);
	if (ret)
		dev_err(priv->dev, "failed to fast age on port %d: %d\n", port,
			ret);
}

static int rtl8365mb_port_set_transparent(struct realtek_priv *priv, int egr_port,
					  int igr_port, bool enable)
{
	/* "Transparent" between the two ports means that packets forwarded by
	 * igr_port and egressed on egr_port will not be filtered by the usual
	 * VLAN membership settings.
	 */
	return regmap_update_bits(
		priv->map, RTL8365MB_VLAN_EGRESS_TRANSPARENT_REG(egr_port),
		BIT(igr_port), enable ? BIT(igr_port) : 0);
}

static int rtl8365mb_port_set_egress_keep(struct realtek_priv *priv,
					  int egr_port, int igr_port,
					  bool enable)
{
	u32 mask;
	u32 reg;

	if (igr_port < 8) {
		reg = RTL8365MB_VLAN_EGRESS_KEEP_REG(egr_port);
		mask = BIT(igr_port)
		       << RTL8365MB_VLAN_EGRESS_KEEP_OFFSET(egr_port);
	} else {
		reg = RTL8365MB_VLAN_EGRESS_KEEP_EXT_REG(egr_port);
		mask = (BIT(igr_port) >> 8)
		       << RTL8365MB_VLAN_EGRESS_KEEP_EXT_OFFSET(egr_port);
	}

	/* Egress "keep" enabled: Packets forwarded by igr_port and egressed on
	 * egr_port will not be subject to the normal egress tagging/untagging
	 * rules of the VLAN configuration. That is to say, the packet will be
	 * egressed on egr_port exactly as it was received on igr_port.
	 *
	 * Egress "keep" disabled: Packets will be subject to the usual egress
	 * tagging rules of the VLAN configuration. E.g. "egress untagged", etc.
	 */
	return regmap_update_bits(priv->map, reg, mask, enable ? mask : 0);
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
	return regmap_update_bits(
		priv->map, RTL8365MB_VLAN_INGRESS_REG,
		RTL8365MB_VLAN_INGRESS_FILTER_PORT_EN_MASK(port),
		(enable ? 1 : 0)
			<< RTL8365MB_VLAN_INGRESS_FILTER_PORT_EN_OFFSET(port));
}

static int rtl8365mb_port_vlan_filtering(struct dsa_switch *ds, int port,
					 bool vlan_filtering,
					 struct netlink_ext_ack *extack)
{
	struct realtek_priv *priv = ds->priv;
	struct dsa_port *dp;
	int ret;

	// DISABLE
	// add this port to the transparent port mask of all ports
	// -> egress filtering will be disabled for all packets forwarded from this port
	// add this port to the keep port mask of all ports
	// -> disables tag/untag logic for all packets forwarded from this port
	// add ACL rule to classify all packets according to MC 1
	// -> MC1 has VID=0 and member=all
	// the port isolation mask should take care of the rest

	// ENABLE
	// remove this port from the transparent port mask of all ports
	// -> egress filtering will be enabled for all packets forwarded from this port
	// remove this port from the keep port mask of all ports
	// -> enables tag/untag logic for all packets forwarded from this port
	// remove ACL rule to classify all packets according to MC 1
	// -> port will classify packets according to its PVID and frame tag

	// TODO: should we only enable keep/transparent for egress ports in the
	// same bridge? or does it not matter, due to port isolation?
	if (vlan_filtering) {
		/* Rely on the membership and tagging rules of the VLAN
		 * configuration already programmed into the switch.
		 */
		dsa_switch_for_each_available_port(dp, ds) {
			ret = rtl8365mb_port_set_transparent(priv, dp->index,
							     port, false);
			if (ret)
				return ret;

			ret = rtl8365mb_port_set_egress_keep(priv, dp->index,
							     port, false);
			if (ret)
				return ret;
		}

		/* Remove port from VLAN-unaware ACL rule by disabling ACL on
		 * this port - this means we don't have to reconfigure the ACL
		 * rule list, which is not a very atomic operation.
		 */
		ret = rtl8365mb_acl_set_port_enable(priv, dp->index, false);
		if (ret)
			return ret;

		/* Filter ingress packets according to VLAN membership */
		ret = rtl8365mb_port_set_ingress_filtering(priv, port, true);
		if (ret)
			return ret;
	} else {
		/* Let packets pass freely from this port to all other ports
		 * unmodified (respecting port isolation settings).
		 */
		dsa_switch_for_each_available_port(dp, ds) {
			ret = rtl8365mb_port_set_transparent(priv, dp->index,
							     port, true);
			if (ret)
				return ret;

			ret = rtl8365mb_port_set_egress_keep(priv, dp->index,
							     port, true);
			if (ret)
				return ret;
		}

		/* Re-enable ACL on this port to make it classify incoming
		 * packets as untagged and hence learn with VID 0.
		 */
		ret = rtl8365mb_acl_set_port_enable(priv, dp->index, true);
		if (ret)
			return ret;

		/* Don't filter incoming packets, i.e. ignore VLAN membership */
		ret = rtl8365mb_port_set_ingress_filtering(priv, port, false);
		if (ret)
			return ret;
	}

	return 0;
}

static struct rtl8365mb_vlanmc_entry *
rtl8365mb_find_synced_vlanmc(struct realtek_priv *priv, u16 vid)
{
	struct rtl8365mb_vlanmc_entry *vlanmc_entry;
	struct rtl8365mb *mb;

	mb = priv->chip_data;

	list_for_each_entry (vlanmc_entry, &mb->vlanmc_synced, list) {
		if (vlanmc_entry->vlanmc.evid == vid)
			return vlanmc_entry;
	}

	return NULL;
}

static struct rtl8365mb_vlanmc_entry *
rtl8365mb_get_synced_vlanmc(struct realtek_priv *priv, u16 vid)
{
	struct rtl8365mb_vlanmc_entry *vlanmc_entry;
	struct rtl8365mb *mb;

	mb = priv->chip_data;

	/* If it already exists, increase the refcount and return it */
	vlanmc_entry = rtl8365mb_find_synced_vlanmc(priv, vid);
	if (vlanmc_entry) {
		refcount_inc(&vlanmc_entry->refcnt);
		return vlanmc_entry;
	}

	/* Otherwise create a new entry, take an initial reference
	 * count, and place it in the list of synced VLAN membership
	 * config entries.
	 */
	vlanmc_entry = rtl8365mb_vlan_alloc_vlanmc_entry(&mb->vlanmc_db);
	if (IS_ERR(vlanmc_entry))
		return vlanmc_entry;

	refcount_inc(&vlanmc_entry->refcnt);
	list_add_tail(&vlanmc_entry->list, &mb->vlanmc_synced);

	/* Only the VID is initialized - so that it can subsequently
	 * be found in the list of synced VLAN membership
	 * configs. Synchronization of VLAN membership config with
	 * the VLAN4k table is handled separately.
	 */
	vlanmc_entry->vlanmc.evid = vid;

	return vlanmc_entry;
}

static void rtl8365mb_put_synced_vlanmc(struct realtek_priv *priv, u16 vid)
{
	struct rtl8365mb_vlanmc_entry *vlanmc_entry;

	vlanmc_entry = rtl8365mb_find_synced_vlanmc(priv, vid);
	if (WARN_ON_ONCE(!vlanmc_entry))
		return;

	/* Decrement the reference counter. If there are no more
	 * interested parties, remove it from the list and free the
	 * entry.
	 */
	if (refcount_dec_and_test(&vlanmc_entry->refcnt)) {
		list_del(&vlanmc_entry->list);
		rtl8365mb_vlan_free_vlanmc_entry(vlanmc_entry);
	}
}

static int rtl8365mb_sync_vlanmc(struct realtek_priv *priv,
				 struct rtl8365mb_vlan4k *vlan4k)
{
	struct rtl8365mb_vlanmc_entry *vlanmc_entry =
		rtl8365mb_find_synced_vlanmc(priv, vlan4k->vid);

	/* If there is no synced VLAN membership config for this VLAN,
	 * then there is nothing to do.
	 */
	if (!vlanmc_entry)
		return 0;

	vlanmc_entry->vlanmc.member = vlan4k->member;
	vlanmc_entry->vlanmc.fid = vlan4k->fid;
	vlanmc_entry->vlanmc.priority = vlan4k->priority;
	vlanmc_entry->vlanmc.priority_en = vlan4k->priority_en;
	vlanmc_entry->vlanmc.policing_en = vlan4k->policing_en;
	vlanmc_entry->vlanmc.meteridx = vlan4k->meteridx;

	return rtl8365mb_vlan_set_vlanmc_entry(priv, vlanmc_entry);
}

static int rtl8365mb_port_set_pvid(struct realtek_priv *priv, int port, u16 vid)
{
	struct rtl8365mb_vlanmc_entry *vlanmc_entry;
	struct rtl8365mb_port *p;
	struct rtl8365mb *mb;
	int ret;

	mb = priv->chip_data;
	p = &mb->ports[port];

	if (p->pvid == vid)
		return 0;

	/* Port-based VLAN IDs are set by specifying a VLAN membership
	 * config index. The PVID will be determined by the settings
	 * in that membership config. Realtek states that VLAN
	 * membership configs are a vestige of older switch chips that
	 * did not use the VLAN4k table. This design still permeates
	 * some aspects of this switch family though, as evidenced
	 * here.
	 */

	/* If a previous PVID was set, signal this port's disinterest
	 * in keeping the VLAN membership config synced.
	 */
	if (p->pvid) {
		rtl8365mb_put_synced_vlanmc(priv, p->pvid);
		p->pvid = 0;
	}

	if (!vid) {
		/* Remove the PVID by selecting the reserved "null"
		 * VLAN membership config. This config is static and
		 * does not require any syncing.
		 */
		vlanmc_entry = mb->vlanmc_null;
	} else {
		/* Program a new PVID by acquiring a synced VLAN
		 * membership config. One will be created if not yet
		 * present. All that matters here is knowing the
		 * membership config index: the config itself will get
		 * synced automatically with the VLAN4k table state.
		 */
		vlanmc_entry = rtl8365mb_get_synced_vlanmc(priv, vid);
		if (IS_ERR(vlanmc_entry))
			return PTR_ERR(vlanmc_entry);
		p->pvid = vid;
	}

	ret = regmap_update_bits(
		priv->map, RTL8365MB_VLAN_PVID_CTRL_REG(port),
		RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_MASK(port),
		vlanmc_entry->index
			<< RTL8365MB_VLAN_PVID_CTRL_PORT_MCIDX_OFFSET(port));
	if (ret)
		return ret;

	return 0;
}

static int rtl8365mb_port_vlan_add(struct dsa_switch *ds, int port,
				   const struct switchdev_obj_port_vlan *vlan,
				   struct netlink_ext_ack *extack)
{
	bool untagged = !!(vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED);
	bool pvid = !!(vlan->flags & BRIDGE_VLAN_INFO_PVID);
	struct rtl8365mb_vlan4k vlan4k = { 0 };
	struct realtek_priv *priv = ds->priv;
	int ret;

	dev_info(priv->dev, "add VLAN %d on port %d, %s, %s\n", vlan->vid, port,
		 untagged ? "untagged" : "tagged", pvid ? "PVID" : "no PVID");

	/* Add this port to the given VLAN in the VLAN4k table */
	ret = rtl8365mb_vlan_get_vlan4k(priv, vlan->vid, &vlan4k);
	if (ret)
		return ret;

	vlan4k.member |= BIT(port);
	vlan4k.untag |= untagged ? BIT(port) : 0;
	vlan4k.ivl_en = true; /* always use Independent VLAN Learning */

	ret = rtl8365mb_vlan_set_vlan4k(priv, &vlan4k);
	if (ret)
		return ret;

	/* Update the PVID */
	ret = rtl8365mb_port_set_pvid(priv, port, pvid ? vlan4k.vid : 0);
	if (ret)
		return ret;

	/* Sync VLAN membership config if necessary */
	ret = rtl8365mb_sync_vlanmc(priv, &vlan4k);
	if (ret)
		return ret;

	return 0;
}

static int rtl8365mb_port_vlan_del(struct dsa_switch *ds, int port,
			    const struct switchdev_obj_port_vlan *vlan)
{
	struct rtl8365mb_vlan4k vlan4k = { 0 };
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_port *p;
	struct rtl8365mb *mb;
	int ret;

	mb = priv->chip_data;
	p = &mb->ports[port];

	dev_info(priv->dev, "del VLAN %d on port %d\n", vlan->vid, port);

	/* Remove this port from the given VLAN in the VLAN4k table */
	ret = rtl8365mb_vlan_get_vlan4k(priv, vlan->vid, &vlan4k);
	if (ret)
		return ret;

	vlan4k.member &= ~BIT(port);
	vlan4k.untag &= ~BIT(port);

	ret = rtl8365mb_vlan_set_vlan4k(priv, &vlan4k);
	if (ret)
		return ret;

	/* Remove the PVID if the corresponding VLAN is being deleted */
	if (p->pvid == vlan->vid) {
		ret = rtl8365mb_port_set_pvid(priv, port, 0);
		if (ret)
			return ret;
	}

	/* Sync VLAN membership config if necessary */
	ret = rtl8365mb_sync_vlanmc(priv, &vlan4k);
	if (ret)
		return ret;

	return 0;
}

static int
rtl8365mb_port_set_vlan_egress_mode(struct realtek_priv *priv, int port,
				    enum rtl8365mb_vlan_egress_mode mode)
{
	return regmap_update_bits(
		priv->map, RTL8365MB_PORT_MISC_CFG_REG(port),
		RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_MASK,
		FIELD_PREP(RTL8365MB_PORT_MISC_CFG_VLAN_EGRESS_MODE_MASK,
			   mode));
}

static int rtl8365mb_vlan_setup(struct realtek_priv *priv)
{
	struct dsa_switch *ds = &priv->ds;
	struct rtl8365mb *mb = priv->chip_data;
	struct rtl8365mb_acl_rule rule = {
		.enabled = 1,
		.negate = 0,
		.template = 0,
		.care = {
			.portmask = RTL8365MB_PORTMASK,
			.fields = { /* don't care about any field bits */ },
		},
		.data = {
			.portmask = RTL8365MB_PORTMASK,
			.fields = { /* ignored since care bits are all 0 */ },
		},
	};
	struct rtl8365mb_acl_action action = {
		.mode = RTL8365MB_ACL_ACTION_MODE_CVLAN,
		.cvlan = {
			.subaction = RTL8365MB_ACL_CVLAN_SUBACTION_INGRESS,
			.mcidx = 1, /* cf. vlanmc_unaware below */
		},
	};
	struct dsa_port *dp;
	int ret;

	INIT_LIST_HEAD(&mb->vlanmc_synced);

	/* Initialize the reserved "null" VLAN membership config. This
	 * is used when programming a port to have no PVID. On switch
	 * reset, all ports should have their PVID determined by the
	 * VLAN membership config with index 0, so for convenience
	 * this is the first entry allocated. The result is no PVID by
	 * default, for all ports.
	 */
	mb->vlanmc_null = rtl8365mb_vlan_alloc_vlanmc_entry(&mb->vlanmc_db);
	if (IS_ERR(mb->vlanmc_null))
		return PTR_ERR(mb->vlanmc_null);

	/* Sanity check just in case changes are made to the allocator */
	WARN_ON(mb->vlanmc_null->index != 0);

	ret = rtl8365mb_vlan_set_vlanmc_entry(priv, mb->vlanmc_null);
	if (ret)
		goto out_free_vlanmc_null;


	/* Initialize the reserved "unaware" VLAN membership config. This is
	 * used by the ACL action defined above in order to reclassify packets
	 * according to this VLAN membership config. Specifically the VID is 0
	 * and hence the rule will force all ingressed packets to be learned as
	 * though the port were VLAN-unaware.
	 *
	 * TODO TODO check whether the next paragraph is true, or if the
	 * transparent setting supercedes it
	 *
	 * As for the membership, in this case all ports are added so that when
	 * a packet is classified according to this VLAN, all ports are a priori
	 * part of the forwarding mask. Of course, the port isolation settings
	 * take precedence over this.
	 */
	mb->vlanmc_unaware = rtl8365mb_vlan_alloc_vlanmc_entry(&mb->vlanmc_db);
	if (IS_ERR(mb->vlanmc_unaware)) {
		ret = PTR_ERR(mb->vlanmc_null);
		goto out_free_vlanmc_null;
	}

	/* Likewise the rule above assumes that vlanmc_unaware has mcidx 1 */
	WARN_ON(mb->vlanmc_unaware->index != 1);

	mb->vlanmc_unaware->vlanmc.member = RTL8365MB_PORTMASK; // TODO maybe not needed, since we use transparent setting to ignore VLAN egress filter

	ret = rtl8365mb_vlan_set_vlanmc_entry(priv, mb->vlanmc_unaware);
	if (ret)
		goto out_free_vlanmc_unaware;

	/* Set up our ACL for VLAN-unaware mode */
	ret = rtl8365mb_acl_reset(priv);
	if (ret)
		goto out_free_vlanmc_unaware;

	ret = rtl8365mb_acl_set_template_config(
		priv, &rtl8365mb_acl_default_template_config);
	if (ret)
		goto out_free_vlanmc_unaware;

	ret = rtl8365mb_acl_set_fieldsel_config(
		priv, &rtl8365mb_acl_default_fieldsel_config);
	if (ret)
		goto out_free_vlanmc_unaware;

	ret = rtl8365mb_acl_set_action(priv, 0, &action);
	if (ret)
		goto out_free_vlanmc_unaware;

	ret = rtl8365mb_acl_set_rule(priv, 0, &rule);
	if (ret)
		goto out_free_vlanmc_unaware;

	/* ACL is now set up. Enable it by default on all ports. It will get
	 * disabled when VLAN filtering is enabled and vice versa.
	 */
	dsa_switch_for_each_available_port(dp, ds) {
		/* TODO explain why we set ORIGINAL */
		ret = rtl8365mb_port_set_vlan_egress_mode(
			priv, dp->index, RTL8365MB_VLAN_EGRESS_MODE_ORIGINAL);
		if (ret)
			return ret;

		ret = rtl8365mb_acl_set_port_enable(priv, dp->index, true);
		if (ret)
			goto out_free_vlanmc_unaware;
	}

	/* Add all ports to VLAN0. Not sure if needed. But at least for
	 * learning, we need to set the ivl_en bit to 1 so that the switch
	 * learns with {VID, MAC, EFID} rather than {FID, MAC, EFID}.
	 */
	// TODO find right place for this
	{
		struct rtl8365mb_vlan4k vlan4k = { 0 };
		ret = rtl8365mb_vlan_get_vlan4k(priv, 0, &vlan4k);
		if (ret)
			return ret;
		vlan4k.member |= RTL8365MB_PORTMASK;
		vlan4k.ivl_en = true;
		ret = rtl8365mb_vlan_set_vlan4k(priv, &vlan4k);
		if (ret)
			return ret;
	}

	/* Enable VLAN functionality on the switch */
	ret = regmap_update_bits(priv->map, RTL8365MB_VLAN_CTRL_REG,
				 RTL8365MB_VLAN_CTRL_EN_MASK,
				 FIELD_PREP(RTL8365MB_VLAN_CTRL_EN_MASK, 1));
	if (ret)
		goto out_free_vlanmc_unaware;

	return 0;

out_free_vlanmc_unaware:
	rtl8365mb_vlan_free_vlanmc_entry(mb->vlanmc_unaware);
	mb->vlanmc_unaware = NULL;
	
out_free_vlanmc_null:
	rtl8365mb_vlan_free_vlanmc_entry(mb->vlanmc_null);
	mb->vlanmc_null = NULL;

	return ret;
}

static void rtl8365mb_vlan_teardown(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	int i;

	for (i = 0; i < priv->num_ports; i++) {
		struct rtl8365mb_port *p = &mb->ports[i];

		if (dsa_is_unused_port(&priv->ds, i))
			continue;

		if (p->pvid) {
			rtl8365mb_put_synced_vlanmc(priv, p->pvid);
			p->pvid = 0;
		}
	}

	rtl8365mb_vlan_free_vlanmc_entry(mb->vlanmc_unaware);
	mb->vlanmc_unaware = NULL;

	rtl8365mb_vlan_free_vlanmc_entry(mb->vlanmc_null);
	mb->vlanmc_null = NULL;
}

static int rtl8365mb_port_fdb_add(struct dsa_switch *ds, int port,
				  const unsigned char *addr, u16 vid,
				  struct dsa_db db)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_l2_uc uc = {
		.key = {
			.efid = db.type == DSA_DB_BRIDGE ? db.bridge.num : 0,
			.ivl = true,
			.vid = vid,
		},
		.port = port,
		.is_static = true,
		.age = 6, // TODO
	};
	struct rtl8365mb *mb = priv->chip_data;
	int ret;

	// TODO: standlone mode adds entrie(s) in DSA_DB_PORT, should efid be 0?
	if (db.type != DSA_DB_PORT && db.type != DSA_DB_BRIDGE)
		return -EOPNOTSUPP;

	dev_info(priv->dev, "fdb_add port %d addr %pM efid %d vid %d\n",
		 port, addr, uc.key.efid, vid);

	memcpy(uc.key.mac_addr, addr, ETH_ALEN);

	mutex_lock(&mb->l2_lock);
	ret = rtl8365mb_l2_add_uc(priv, &uc);
	if (ret)
	  dev_info(priv->dev, "XXX fdb_add ERROR %d\n", ret);
	mutex_unlock(&mb->l2_lock);

	return ret;
}

static int rtl8365mb_port_fdb_del(struct dsa_switch *ds, int port,
				  const unsigned char *addr, u16 vid,
				  struct dsa_db db)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_l2_uc_key key = {
		.efid = db.type == DSA_DB_BRIDGE ? db.bridge.num : 0,
		.ivl = true,
		.vid = vid,
	};
	struct rtl8365mb *mb = priv->chip_data;
	int ret;

	// TODO: standlone mode adds entrie(s) in DSA_DB_PORT, should efid be 0?
	if (db.type != DSA_DB_PORT && db.type != DSA_DB_BRIDGE)
		return -EOPNOTSUPP;

	dev_info(priv->dev, "fdb_del port %d addr %pM efid %d vid %d\n",
		 port, addr, key.efid, vid);

	memcpy(key.mac_addr, addr, ETH_ALEN);

	mutex_lock(&mb->l2_lock);
	ret = rtl8365mb_l2_del_uc(priv, &key);
	mutex_unlock(&mb->l2_lock);

	return ret;
}

static int rtl8365mb_port_fdb_dump(struct dsa_switch *ds, int port,
				   dsa_fdb_dump_cb_t *cb, void *data)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_l2_uc uc;
	int first_addr;
	int addr = 0;
	int ret;

	/* Walk the L2 unicast entries of the switch forwarding database */
	ret = rtl8365mb_l2_get_next_uc(priv, &addr, &uc);
	if (ret == -ENOENT)
		return 0; /* The database is empty - not an error */
	else if (ret)
		return ret;

	/* Mark where we started, so that we don't loop forever */
	first_addr = addr;

	do {
		if (uc.port == port)
			cb(uc.key.mac_addr, uc.key.vid, uc.is_static, data);

		/* Don't overshoot the look-up table size */
		// TODO what if the hardware overshoots? saw this before
		if (++addr >= RTL8365MB_LEARN_LIMIT_MAX)
			break;

		ret = rtl8365mb_l2_get_next_uc(priv, &addr, &uc);
		if (ret)
			return ret;
	} while (addr > first_addr);

	return 0;
}

static int rtl8365mb_port_mdb_add(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_mdb *mdb,
				  struct dsa_db db)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_l2_mc_key key = {
		.ivl = true,
		.vid = mdb->vid,
	};
	struct rtl8365mb_l2_mc mc = { 0 };
	bool new_entry = false;
	int ret;
	struct rtl8365mb *mb = priv->chip_data;

	if (db.type != DSA_DB_PORT && db.type != DSA_DB_BRIDGE)
		return -EOPNOTSUPP;

	memcpy(key.mac_addr, &mdb->addr, ETH_ALEN);

	mutex_lock(&mb->l2_lock);

	ret = rtl8365mb_l2_get_mc(priv, &key, &mc);
	if (ret == -ENOENT)
		new_entry = true;
	else if (ret) {
		mutex_unlock(&mb->l2_lock);
		return ret;
	}

	if (new_entry) {
		memset(&mc, 0, sizeof(mc));
		mc.key = key;
	}

	mc.member |= BIT(port);

	ret = rtl8365mb_l2_add_mc(priv, &mc);
	mutex_unlock(&mb->l2_lock);

	return ret;
}

static int rtl8365mb_port_mdb_del(struct dsa_switch *ds, int port,
				  const struct switchdev_obj_port_mdb *mdb,
				  struct dsa_db db)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_l2_mc_key key = {
		.ivl = true,
		.vid = mdb->vid,
	};
	struct rtl8365mb_l2_mc mc = { 0 };
	int ret;
	struct rtl8365mb *mb = priv->chip_data;

	if (db.type != DSA_DB_PORT && db.type != DSA_DB_BRIDGE)
		return -EOPNOTSUPP;

	memcpy(mc.key.mac_addr, &mdb->addr, ETH_ALEN);

	mutex_lock(&mb->l2_lock);

	ret = rtl8365mb_l2_get_mc(priv, &key, &mc);
	if (ret) {
		mutex_unlock(&mb->l2_lock);
		return ret;
	}

	/* Remove this port from the multicast membership. If there are no more
	 * ports in this multicast group, delete the L2 multicast database entry
	 * altogether.
	 */
	mc.member &= ~BIT(port);
	if (!mc.member) {
		ret = rtl8365mb_l2_del_mc(priv, &key);
		mutex_unlock(&mb->l2_lock);
		return ret;
	}

	/* Otherwise just update */
	/* TODO: should mc be get/set rather than get/add/del? need to debug
	   the behaviour of the LUT to understand how it treats entries which
	   are "dead". notions of deadness:
	    - unicast: age == 0 && is_static == 0
	    - multicast: member == 0, that's all right?
	*/
	ret = rtl8365mb_l2_add_mc(priv, &mc);
	mutex_unlock(&mb->l2_lock);

	return ret;
}

static int rtl8365mb_port_set_learning(struct realtek_priv *priv, int port,
				       bool enable)
{
	/* Enable/disable learning by limiting the number of L2 addresses the
	 * port can learn. Realtek documentation states that a limit of zero
	 * disables learning. When enabling learning, set it to the chip's
	 * maximum.
	 */
	return regmap_write(priv->map, RTL8365MB_LUT_PORT_LEARN_LIMIT_REG(port),
			    enable ? RTL8365MB_LEARN_LIMIT_MAX : 0);
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
	return regmap_update_bits(
		priv->map, RTL8365MB_UNKNOWN_MULTICAST_FLOODING_PMASK_REG,
		BIT(port), enable ? BIT(port) : 0);
}

static int rtl8365mb_port_set_bcast_flood(struct realtek_priv *priv, int port,
					  bool enable)
{
	return regmap_update_bits(
		priv->map, RTL8365MB_UNKNOWN_BROADCAST_FLOODING_PMASK_REG,
		BIT(port), enable ? BIT(port) : 0);
}

static int rtl8365mb_port_pre_bridge_flags(struct dsa_switch *ds, int port,
					   struct switchdev_brport_flags flags,
					   struct netlink_ext_ack *extack)
{
	if (flags.mask &
	    ~(BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD))
		return -EINVAL;

	return 0;
}

static int rtl8365mb_port_bridge_flags(struct dsa_switch *ds, int port,
				       struct switchdev_brport_flags flags,
				       struct netlink_ext_ack *exack)
{
	struct realtek_priv *priv = ds->priv;
	int ret;

	if (flags.mask & BR_LEARNING) {
		bool learning_en = !!(flags.val & BR_LEARNING);

		ret = rtl8365mb_port_set_learning(priv, port, learning_en);
		if (ret)
			return ret;
	}

	if (flags.mask & BR_FLOOD) {
		bool ucast_flood_en = !!(flags.val & BR_FLOOD);

		ret = rtl8365mb_port_set_ucast_flood(priv, port,
						     ucast_flood_en);
		if (ret)
			return ret;
	}

	if (flags.mask & BR_MCAST_FLOOD) {
		bool mcast_flood_en = !!(flags.val & BR_MCAST_FLOOD);

		ret = rtl8365mb_port_set_mcast_flood(priv, port,
						     mcast_flood_en);
		if (ret)
			return ret;
	}

	if (flags.mask & BR_BCAST_FLOOD) {
		bool bcast_flood_en = !!(flags.val & BR_BCAST_FLOOD);

		ret = rtl8365mb_port_set_bcast_flood(priv, port,
						     bcast_flood_en);
		if (ret)
			return ret;
	}

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

static int rtl8365mb_port_bridge_join(struct dsa_switch *ds, int port,
				      struct dsa_bridge bridge,
				      bool *tx_forward_offload,
				      struct netlink_ext_ack *extack)
{
	struct realtek_priv *priv = ds->priv;
	u32 mask;
	int ret;
	int i;

	/* Add this port to the isolation group of every other port
	 * offloading this bridge.
	 */
	for (i = 0; i < priv->num_ports; i++) {
		/* Handle this port after */
		if (i == port)
			continue;

		/* Skip ports that are not in this bridge */
		if (!dsa_port_offloads_bridge(dsa_to_port(ds, i), &bridge))
			continue;

		ret = rtl8365mb_port_add_isolation(priv, i, BIT(port));
		if (ret)
			return ret;

		mask |= BIT(i);
	}

	/* Add those ports to the isolation group of this port */
	ret = rtl8365mb_port_add_isolation(priv, port, mask);
	if (ret)
		return ret;

	/* Use the bridge number as the EFID for this port */
	ret = rtl8365mb_port_set_efid(priv, port, bridge.num);
	if (ret)
		return ret;

	return 0;
}

static void rtl8365mb_port_bridge_leave(struct dsa_switch *ds, int port,
					struct dsa_bridge bridge)
{
	struct realtek_priv *priv = ds->priv;
	u32 mask;
	int i;

	/* Remove this port from the isolation group of every other
	 * port offloading this bridge.
	 */
	for (i = 0; i < priv->num_ports; i++) {
		/* Handle this port after */
		if (i == port)
			continue;

		/* Skip ports that are not in this bridge */
		if (!dsa_port_offloads_bridge(dsa_to_port(ds, i), &bridge))
			continue;

		rtl8365mb_port_remove_isolation(priv, i, BIT(port));

		mask |= BIT(i);
	}

	/* Remove those ports from the isolation group of this port */
	rtl8365mb_port_remove_isolation(priv, port, mask);

	/* Revert to the default EFID 0 for standalone mode */
	rtl8365mb_port_set_efid(priv, port, 0);
}

static int rtl8365mb_mib_counter_read(struct realtek_priv *priv, int port,
				      u32 offset, u32 length, u64 *mibvalue)
{
	u64 tmpvalue = 0;
	u32 val;
	int ret;
	int i;

	/* The MIB address is an SRAM address. We request a particular address
	 * and then poll the control register before reading the value from some
	 * counter registers.
	 */
	ret = regmap_write(priv->map, RTL8365MB_MIB_ADDRESS_REG,
			   RTL8365MB_MIB_ADDRESS(port, offset));
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
	if (length == 4)
		offset = 3;
	else
		offset = (offset + 1) % 4;

	/* Read the MIB counter 16 bits at a time */
	for (i = 0; i < length; i++) {
		ret = regmap_read(priv->map,
				  RTL8365MB_MIB_COUNTER_REG(offset - i), &val);
		if (ret)
			return ret;

		tmpvalue = ((tmpvalue) << 16) | (val & 0xFFFF);
	}

	/* Only commit the result if no error occurred */
	*mibvalue = tmpvalue;

	return 0;
}

static void rtl8365mb_get_ethtool_stats(struct dsa_switch *ds, int port, u64 *data)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb;
	int ret;
	int i;

	mb = priv->chip_data;

	mutex_lock(&mb->mib_lock);
	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		struct rtl8365mb_mib_counter *mib = &rtl8365mb_mib_counters[i];

		ret = rtl8365mb_mib_counter_read(priv, port, mib->offset,
						 mib->length, &data[i]);
		if (ret) {
			dev_err(priv->dev,
				"failed to read port %d counters: %d\n", port,
				ret);
			break;
		}
	}
	mutex_unlock(&mb->mib_lock);
}

static void rtl8365mb_get_strings(struct dsa_switch *ds, int port, u32 stringset, u8 *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		struct rtl8365mb_mib_counter *mib = &rtl8365mb_mib_counters[i];
		ethtool_puts(&data, mib->name);
	}
}

static int rtl8365mb_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return -EOPNOTSUPP;

	return RTL8365MB_MIB_END;
}

static void rtl8365mb_get_phy_stats(struct dsa_switch *ds, int port,
				    struct ethtool_eth_phy_stats *phy_stats)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_mib_counter *mib;
	struct rtl8365mb *mb;

	mb = priv->chip_data;
	mib = &rtl8365mb_mib_counters[RTL8365MB_MIB_dot3StatsSymbolErrors];

	mutex_lock(&mb->mib_lock);
	rtl8365mb_mib_counter_read(priv, port, mib->offset, mib->length,
				   &phy_stats->SymbolErrorDuringCarrier);
	mutex_unlock(&mb->mib_lock);
}

static void rtl8365mb_get_mac_stats(struct dsa_switch *ds, int port,
				    struct ethtool_eth_mac_stats *mac_stats)
{
	u64 cnt[RTL8365MB_MIB_END] = {
		[RTL8365MB_MIB_ifOutOctets] = 1,
		[RTL8365MB_MIB_ifOutUcastPkts] = 1,
		[RTL8365MB_MIB_ifOutMulticastPkts] = 1,
		[RTL8365MB_MIB_ifOutBroadcastPkts] = 1,
		[RTL8365MB_MIB_dot3OutPauseFrames] = 1,
		[RTL8365MB_MIB_ifOutDiscards] = 1,
		[RTL8365MB_MIB_ifInOctets] = 1,
		[RTL8365MB_MIB_ifInUcastPkts] = 1,
		[RTL8365MB_MIB_ifInMulticastPkts] = 1,
		[RTL8365MB_MIB_ifInBroadcastPkts] = 1,
		[RTL8365MB_MIB_dot3InPauseFrames] = 1,
		[RTL8365MB_MIB_dot3StatsSingleCollisionFrames] = 1,
		[RTL8365MB_MIB_dot3StatsMultipleCollisionFrames] = 1,
		[RTL8365MB_MIB_dot3StatsFCSErrors] = 1,
		[RTL8365MB_MIB_dot3StatsDeferredTransmissions] = 1,
		[RTL8365MB_MIB_dot3StatsLateCollisions] = 1,
		[RTL8365MB_MIB_dot3StatsExcessiveCollisions] = 1,

	};
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb *mb;
	int ret;
	int i;

	mb = priv->chip_data;

	mutex_lock(&mb->mib_lock);
	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		struct rtl8365mb_mib_counter *mib = &rtl8365mb_mib_counters[i];

		/* Only fetch required MIB counters (marked = 1 above) */
		if (!cnt[i])
			continue;

		ret = rtl8365mb_mib_counter_read(priv, port, mib->offset,
						 mib->length, &cnt[i]);
		if (ret)
			break;
	}
	mutex_unlock(&mb->mib_lock);

	/* The RTL8365MB-VC exposes MIB objects, which we have to translate into
	 * IEEE 802.3 Managed Objects. This is not always completely faithful,
	 * but we try out best. See RFC 3635 for a detailed treatment of the
	 * subject.
	 */

	mac_stats->FramesTransmittedOK = cnt[RTL8365MB_MIB_ifOutUcastPkts] +
					 cnt[RTL8365MB_MIB_ifOutMulticastPkts] +
					 cnt[RTL8365MB_MIB_ifOutBroadcastPkts] +
					 cnt[RTL8365MB_MIB_dot3OutPauseFrames] -
					 cnt[RTL8365MB_MIB_ifOutDiscards];
	mac_stats->SingleCollisionFrames =
		cnt[RTL8365MB_MIB_dot3StatsSingleCollisionFrames];
	mac_stats->MultipleCollisionFrames =
		cnt[RTL8365MB_MIB_dot3StatsMultipleCollisionFrames];
	mac_stats->FramesReceivedOK = cnt[RTL8365MB_MIB_ifInUcastPkts] +
				      cnt[RTL8365MB_MIB_ifInMulticastPkts] +
				      cnt[RTL8365MB_MIB_ifInBroadcastPkts] +
				      cnt[RTL8365MB_MIB_dot3InPauseFrames];
	mac_stats->FrameCheckSequenceErrors =
		cnt[RTL8365MB_MIB_dot3StatsFCSErrors];
	mac_stats->OctetsTransmittedOK = cnt[RTL8365MB_MIB_ifOutOctets] -
					 18 * mac_stats->FramesTransmittedOK;
	mac_stats->FramesWithDeferredXmissions =
		cnt[RTL8365MB_MIB_dot3StatsDeferredTransmissions];
	mac_stats->LateCollisions = cnt[RTL8365MB_MIB_dot3StatsLateCollisions];
	mac_stats->FramesAbortedDueToXSColls =
		cnt[RTL8365MB_MIB_dot3StatsExcessiveCollisions];
	mac_stats->OctetsReceivedOK = cnt[RTL8365MB_MIB_ifInOctets] -
				      18 * mac_stats->FramesReceivedOK;
	mac_stats->MulticastFramesXmittedOK =
		cnt[RTL8365MB_MIB_ifOutMulticastPkts];
	mac_stats->BroadcastFramesXmittedOK =
		cnt[RTL8365MB_MIB_ifOutBroadcastPkts];
	mac_stats->MulticastFramesReceivedOK =
		cnt[RTL8365MB_MIB_ifInMulticastPkts];
	mac_stats->BroadcastFramesReceivedOK =
		cnt[RTL8365MB_MIB_ifInBroadcastPkts];
}

static void rtl8365mb_get_ctrl_stats(struct dsa_switch *ds, int port,
				     struct ethtool_eth_ctrl_stats *ctrl_stats)
{
	struct realtek_priv *priv = ds->priv;
	struct rtl8365mb_mib_counter *mib;
	struct rtl8365mb *mb;

	mb = priv->chip_data;
	mib = &rtl8365mb_mib_counters[RTL8365MB_MIB_dot3ControlInUnknownOpcodes];

	mutex_lock(&mb->mib_lock);
	rtl8365mb_mib_counter_read(priv, port, mib->offset, mib->length,
				   &ctrl_stats->UnsupportedOpcodesReceived);
	mutex_unlock(&mb->mib_lock);
}

static void rtl8365mb_stats_update(struct realtek_priv *priv, int port)
{
	u64 cnt[RTL8365MB_MIB_END] = {
		[RTL8365MB_MIB_ifOutOctets] = 1,
		[RTL8365MB_MIB_ifOutUcastPkts] = 1,
		[RTL8365MB_MIB_ifOutMulticastPkts] = 1,
		[RTL8365MB_MIB_ifOutBroadcastPkts] = 1,
		[RTL8365MB_MIB_ifOutDiscards] = 1,
		[RTL8365MB_MIB_ifInOctets] = 1,
		[RTL8365MB_MIB_ifInUcastPkts] = 1,
		[RTL8365MB_MIB_ifInMulticastPkts] = 1,
		[RTL8365MB_MIB_ifInBroadcastPkts] = 1,
		[RTL8365MB_MIB_etherStatsDropEvents] = 1,
		[RTL8365MB_MIB_etherStatsCollisions] = 1,
		[RTL8365MB_MIB_etherStatsFragments] = 1,
		[RTL8365MB_MIB_etherStatsJabbers] = 1,
		[RTL8365MB_MIB_dot3StatsFCSErrors] = 1,
		[RTL8365MB_MIB_dot3StatsLateCollisions] = 1,
	};
	struct rtl8365mb *mb = priv->chip_data;
	struct rtnl_link_stats64 *stats;
	int ret;
	int i;

	stats = &mb->ports[port].stats;

	mutex_lock(&mb->mib_lock);
	for (i = 0; i < RTL8365MB_MIB_END; i++) {
		struct rtl8365mb_mib_counter *c = &rtl8365mb_mib_counters[i];

		/* Only fetch required MIB counters (marked = 1 above) */
		if (!cnt[i])
			continue;

		ret = rtl8365mb_mib_counter_read(priv, port, c->offset,
						 c->length, &cnt[i]);
		if (ret)
			break;
	}
	mutex_unlock(&mb->mib_lock);

	/* Don't update statistics if there was an error reading the counters */
	if (ret)
		return;

	spin_lock(&mb->ports[port].stats_lock);

	stats->rx_packets = cnt[RTL8365MB_MIB_ifInUcastPkts] +
			    cnt[RTL8365MB_MIB_ifInMulticastPkts] +
			    cnt[RTL8365MB_MIB_ifInBroadcastPkts] -
			    cnt[RTL8365MB_MIB_ifOutDiscards];

	stats->tx_packets = cnt[RTL8365MB_MIB_ifOutUcastPkts] +
			    cnt[RTL8365MB_MIB_ifOutMulticastPkts] +
			    cnt[RTL8365MB_MIB_ifOutBroadcastPkts];

	/* if{In,Out}Octets includes FCS - remove it */
	stats->rx_bytes = cnt[RTL8365MB_MIB_ifInOctets] - 4 * stats->rx_packets;
	stats->tx_bytes =
		cnt[RTL8365MB_MIB_ifOutOctets] - 4 * stats->tx_packets;

	stats->rx_dropped = cnt[RTL8365MB_MIB_etherStatsDropEvents];
	stats->tx_dropped = cnt[RTL8365MB_MIB_ifOutDiscards];

	stats->multicast = cnt[RTL8365MB_MIB_ifInMulticastPkts];
	stats->collisions = cnt[RTL8365MB_MIB_etherStatsCollisions];

	stats->rx_length_errors = cnt[RTL8365MB_MIB_etherStatsFragments] +
				  cnt[RTL8365MB_MIB_etherStatsJabbers];
	stats->rx_crc_errors = cnt[RTL8365MB_MIB_dot3StatsFCSErrors];
	stats->rx_errors = stats->rx_length_errors + stats->rx_crc_errors;

	stats->tx_aborted_errors = cnt[RTL8365MB_MIB_ifOutDiscards];
	stats->tx_window_errors = cnt[RTL8365MB_MIB_dot3StatsLateCollisions];
	stats->tx_errors = stats->tx_aborted_errors + stats->tx_window_errors;

	spin_unlock(&mb->ports[port].stats_lock);
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
	struct rtl8365mb_port *p;
	struct rtl8365mb *mb;

	mb = priv->chip_data;
	p = &mb->ports[port];

	spin_lock(&p->stats_lock);
	memcpy(s, &p->stats, sizeof(*s));
	spin_unlock(&p->stats_lock);
}

static void rtl8365mb_stats_setup(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct dsa_switch *ds = &priv->ds;
	int i;

	/* Per-chip global mutex to protect MIB counter access, since doing
	 * so requires accessing a series of registers in a particular order.
	 */
	mutex_init(&mb->mib_lock);

	for (i = 0; i < priv->num_ports; i++) {
		struct rtl8365mb_port *p = &mb->ports[i];

		if (dsa_is_unused_port(ds, i))
			continue;

		/* Per-port spinlock to protect the stats64 data */
		spin_lock_init(&p->stats_lock);

		/* This work polls the MIB counters and keeps the stats64 data
		 * up-to-date.
		 */
		INIT_DELAYED_WORK(&p->mib_work, rtl8365mb_stats_poll);
	}
}

static void rtl8365mb_stats_teardown(struct realtek_priv *priv)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct dsa_switch *ds = &priv->ds;
	int i;

	for (i = 0; i < priv->num_ports; i++) {
		struct rtl8365mb_port *p = &mb->ports[i];

		if (dsa_is_unused_port(ds, i))
			continue;

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

		handle_nested_irq(child_irq);
	}

	return IRQ_HANDLED;

out_error:
	dev_err(priv->dev, "failed to read interrupt status: %d\n", ret);

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
	struct device_node *intc;
	u32 irq_trig;
	int virq;
	int irq;
	u32 val;
	int ret;
	int i;

	intc = of_get_child_by_name(priv->dev->of_node, "interrupt-controller");
	if (!intc) {
		dev_err(priv->dev, "missing child interrupt-controller node\n");
		return -EINVAL;
	}

	/* rtl8365mb IRQs cascade off this one */
	irq = of_irq_get(intc, 0);
	if (irq <= 0) {
		if (irq != -EPROBE_DEFER)
			dev_err(priv->dev, "failed to get parent irq: %d\n",
				irq);
		ret = irq ? irq : -EINVAL;
		goto out_put_node;
	}

	priv->irqdomain = irq_domain_add_linear(intc, priv->num_ports,
						&rtl8365mb_irqdomain_ops, priv);
	if (!priv->irqdomain) {
		dev_err(priv->dev, "failed to add irq domain\n");
		ret = -ENOMEM;
		goto out_put_node;
	}

	for (i = 0; i < priv->num_ports; i++) {
		virq = irq_create_mapping(priv->irqdomain, i);
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
		dev_err(priv->dev, "failed to request irq: %d\n", ret);
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
	for (i = 0; i < priv->num_ports; i++) {
		virq = irq_find_mapping(priv->irqdomain, i);
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
	int virq;
	int i;

	if (mb->irq) {
		free_irq(mb->irq, priv);
		mb->irq = 0;
	}

	if (priv->irqdomain) {
		for (i = 0; i < priv->num_ports; i++) {
			virq = irq_find_mapping(priv->irqdomain, i);
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

	/* Do any chip-specific init jam before getting to the common stuff */
	if (ci->jam_table) {
		for (i = 0; i < ci->jam_size; i++) {
			ret = regmap_write(priv->map, ci->jam_table[i].reg,
					   ci->jam_table[i].val);
			if (ret)
				return ret;
		}
	}

	/* Common init jam */
	for (i = 0; i < ARRAY_SIZE(rtl8365mb_init_jam_common); i++) {
		ret = regmap_write(priv->map, rtl8365mb_init_jam_common[i].reg,
				   rtl8365mb_init_jam_common[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static int rtl8365mb_reset_chip(struct realtek_priv *priv)
{
	u32 val;

	priv->write_reg_noack(priv, RTL8365MB_CHIP_RESET_REG,
			      FIELD_PREP(RTL8365MB_CHIP_RESET_HW_MASK, 1));

	/* Realtek documentation says the chip needs 1 second to reset. Sleep
	 * for 100 ms before accessing any registers to prevent ACK timeouts.
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
	struct dsa_port *cpu_dp;
	struct rtl8365mb *mb;
	int ret;
	int i;

	mb = priv->chip_data;
	cpu = &mb->cpu;

	ret = rtl8365mb_reset_chip(priv);
	if (ret) {
		dev_err(priv->dev, "failed to reset chip: %d\n", ret);
		goto out_error;
	}

	/* Configure switch to vendor-defined initial state */
	ret = rtl8365mb_switch_init(priv);
	if (ret) {
		dev_err(priv->dev, "failed to initialize switch: %d\n", ret);
		goto out_error;
	}

	/* Set up cascading IRQs */
	ret = rtl8365mb_irq_setup(priv);
	if (ret == -EPROBE_DEFER)
		return ret;
	else if (ret)
		dev_info(priv->dev, "no interrupt support\n");

	/* Configure CPU tagging */
	dsa_switch_for_each_cpu_port(cpu_dp, ds) {
		cpu->mask |= BIT(cpu_dp->index);

		if (cpu->trap_port == RTL8365MB_MAX_NUM_PORTS)
			cpu->trap_port = cpu_dp->index;
	}
	cpu->enable = cpu->mask > 0;
	ret = rtl8365mb_cpu_config(priv);
	if (ret)
		goto out_teardown_irq;

	/* Configure ports */
	for (i = 0; i < priv->num_ports; i++) {
		struct rtl8365mb_port *p = &mb->ports[i];

		if (dsa_is_unused_port(ds, i))
			continue;

		/* Set the initial STP state of all ports to DISABLED, otherwise
		 * ports will still forward frames to the CPU despite being
		 * administratively down by default.
		 */
		rtl8365mb_port_stp_state_set(ds, i, BR_STATE_DISABLED);

		/* Forward only to the CPU */
		ret = rtl8365mb_port_set_isolation(priv, i, cpu->mask);
		if (ret)
			goto out_teardown_irq;

		/* Disable learning */
		ret = rtl8365mb_port_set_learning(priv, i, false);
		if (ret)
			goto out_teardown_irq;

		/* Enable all types of flooding */
		ret = rtl8365mb_port_set_ucast_flood(priv, i, true);
		if (ret)
			goto out_teardown_irq;

		ret = rtl8365mb_port_set_mcast_flood(priv, i, true);
		if (ret)
			goto out_teardown_irq;

		ret = rtl8365mb_port_set_bcast_flood(priv, i, true);
		if (ret)
			goto out_teardown_irq;

		/* Set up per-port private data */
		p->priv = priv;
		p->index = i;
	}

	ret = rtl8365mb_port_change_mtu(ds, cpu->trap_port, ETH_DATA_LEN);
	
	/* Set up VLAN */
	ret = rtl8365mb_vlan_setup(priv);
	if (ret)
		goto out_teardown_irq;

	/* Set maximum packet length to 1536 bytes */
	ret = regmap_update_bits(priv->map, RTL8365MB_CFG0_MAX_LEN_REG,
				 RTL8365MB_CFG0_MAX_LEN_MASK,
				 FIELD_PREP(RTL8365MB_CFG0_MAX_LEN_MASK, 1536));
	if (ret)
		goto out_teardown_vlan;

	ret = rtl83xx_setup_user_mdio(ds);
	if (ret) {
		dev_err(priv->dev, "could not set up MDIO bus\n");
		goto out_teardown_vlan;
	}

	/* Start statistics counter polling */
	rtl8365mb_stats_setup(priv);

	mutex_init(&mb->l2_lock);

	return 0;

out_teardown_vlan:
	rtl8365mb_vlan_teardown(priv);

out_teardown_irq:
	rtl8365mb_irq_teardown(priv);

out_error:
	return ret;
}

static void rtl8365mb_teardown(struct dsa_switch *ds)
{
	struct realtek_priv *priv = ds->priv;

	rtl8365mb_stats_teardown(priv);
	rtl8365mb_vlan_teardown(priv);
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
		dev_err(priv->dev, "failed to read chip id and version: %d\n",
			ret);
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

	dev_info(priv->dev, "found an %s switch\n", mb->chip_info->name);

	priv->num_ports = RTL8365MB_MAX_NUM_PORTS;
	mb->priv = priv;
	mb->cpu.trap_port = RTL8365MB_MAX_NUM_PORTS;
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
	.port_bridge_join = rtl8365mb_port_bridge_join,
	.port_bridge_leave = rtl8365mb_port_bridge_leave,
	.port_pre_bridge_flags = rtl8365mb_port_pre_bridge_flags,
	.port_bridge_flags = rtl8365mb_port_bridge_flags,
	.port_stp_state_set = rtl8365mb_port_stp_state_set,
	.port_fast_age = rtl8365mb_port_fast_age,
	.port_vlan_filtering = rtl8365mb_port_vlan_filtering,
	.port_vlan_add = rtl8365mb_port_vlan_add,
	.port_vlan_del = rtl8365mb_port_vlan_del,
	.port_fdb_add = rtl8365mb_port_fdb_add,
	.port_fdb_del = rtl8365mb_port_fdb_del,
	.port_fdb_dump = rtl8365mb_port_fdb_dump,
	.port_mdb_add = rtl8365mb_port_mdb_add,
	.port_mdb_del = rtl8365mb_port_mdb_del,
	.get_strings = rtl8365mb_get_strings,
	.get_ethtool_stats = rtl8365mb_get_ethtool_stats,
	.get_sset_count = rtl8365mb_get_sset_count,
	.get_eth_phy_stats = rtl8365mb_get_phy_stats,
	.get_eth_mac_stats = rtl8365mb_get_mac_stats,
	.get_eth_ctrl_stats = rtl8365mb_get_ctrl_stats,
	.get_stats64 = rtl8365mb_get_stats64,
	.port_change_mtu = rtl8365mb_port_change_mtu,
	.port_max_mtu = rtl8365mb_port_max_mtu,
};

static const struct realtek_ops rtl8365mb_ops = {
	.detect = rtl8365mb_detect,
	.phy_read = rtl8365mb_phy_read,
	.phy_write = rtl8365mb_phy_write,
};

const struct realtek_variant rtl8365mb_variant = {
	.ds_ops = &rtl8365mb_switch_ops,
	.ops = &rtl8365mb_ops,
	.phylink_mac_ops = &rtl8365mb_phylink_mac_ops,
	.clk_delay = 10,
	.cmd_read = 0xb9,
	.cmd_write = 0xb8,
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
