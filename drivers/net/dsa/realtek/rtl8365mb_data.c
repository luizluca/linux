// SPDX-License-Identifier: GPL-2.0
/* Realtek jam tables extracted from vendor API code
 *
 * Copyright (C) 2026 Luiz Angelo Daros de Luca <luizluca@gmail.com>
 *
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/array_size.h>

#include "rtl8365mb_reg.h"
#include "rtl8365mb_data.h"

/* Lifted from the vendor driver sources */
const struct rtl8365mb_jam_tbl_entry rtl8365mb_init_jam_common[] = {
	/* Set Old max packet length to 16K */
	{ RTL8365MB_OLD_MAX_LENGTH_LIMIT_IPG_REG,
	  /* vendor only set these bits below */
	  FIELD_PREP_CONST(RTL8365MB_OLD_MAX_LENGTH_LIMIT_IPG_MAX_LEN_MASK,
			   RTL8365MB_OLD_MAX_PKT_LEN_16K) |
	  FIELD_PREP_CONST(RTL8365MB_OLD_MAX_LENGTH_LIMIT_IPG_PAGES_BEFORE_FC_MASK,
			   0x7F) |
	  FIELD_PREP_CONST(RTL8365MB_OLD_MAX_LENGTH_LIMIT_IPG_LIMIT_IPG_CFG_MASK,
			   0x0B) },
	{ RTL8365MB_OLD_MAX_LEN_RX_TX_REG, RTL8365MB_OLD_MAX_PKT_LEN_16K },
	/* ACL Mode */
	{ RTL8365MB_ACL_ACCESS_MODE_REG, 0x0001 },
	/* Max rate */
	{ RTL8365MB_LINE_RATE_HSG_H_REG, 0x0007 },
	/* Change unknown DA to per port setting */
	{ RTL8365MB_PORT_SECURITY_CTRL_REG,
	  FIELD_PREP_CONST(RTL8365MB_PORT_SECURITY_CTRL_UNKNOWN_SA_BEHAVE_MASK,
			   RTL8365MB_PORT_SEC_SA_FLOOD) |
	  FIELD_PREP_CONST(RTL8365MB_PORT_SECURITY_CTRL_UNMATCHED_SA_BEHAVE_MASK,
			   RTL8365MB_PORT_SEC_SA_FLOOD) |
	  FIELD_PREP_CONST(RTL8365MB_PORT_SECURITY_CTRL_LUT_LEARN_OVER_ACT_MASK,
			   RTL8365MB_PORT_SEC_LEARN_OVER_ACT_NORMAL) |
	  /* vendor only set these bits below */
	  FIELD_PREP_CONST(RTL8365MB_PORT_SECURITY_CTRL_UNK_UNICAST_DA_MASK,
			   RTL8365MB_PORT_SEC_DA_FLOOD_ALL) },
	/* LUT lookup OP = 1 */
	{ RTL8365MB_LUT_CFG_REG,
	  FIELD_PREP_CONST(RTL8365MB_LUT_CFG_AGE_SPEED_MASK,
			   RTL8365MB_LUT_AGE_SPEED_NORMAL) |
	  FIELD_PREP_CONST(RTL8365MB_LUT_CFG_AGE_TIMER_MASK,
			   6) |
	  /* vendor only set these bits below */
	  RTL8365MB_LUT_CFG_IPMC_LOOKUP_OP },
	/* Set all RMA fields to zero */
	{ RTL8365MB_RMA_CTRL00_REG, 0x0000 },
	{ RTL8365MB_RMA_CTRL02_REG, 0x0000 },
	/* Enable TX Mirror isolation leaky */
	{ RTL8365MB_MIRROR_CTRL2_REG,
	  RTL8365MB_MIRROR_CTRL2_REALKEEP_EN |
	  RTL8365MB_MIRROR_CTRL2_RX_VLAN_LEAKY |
	  RTL8365MB_MIRROR_CTRL2_TX_VLAN_LEAKY },
	/* Enable interrupt PIN */
	{ RTL8365MB_IO_MISC_FUNC_REG, RTL8365MB_IO_MISC_FUNC_INT_EN },
};
const size_t rtl8365mb_init_jam_common_size = ARRAY_SIZE(rtl8365mb_init_jam_common);

const struct rtl8365mb_jam_tbl_entry rtl8365mb_init_jam_8365mb_vc[] = {
	{ RTL8365MB_UTP_FIB_DET_REG,
	  FIELD_PREP_CONST(RTL8365MB_UTP_FIB_DET_FIB_FINAL_TIMER_MASK, 1) |
	  FIELD_PREP_CONST(RTL8365MB_UTP_FIB_DET_FIB_LINK_TIMER_MASK, 1) |
	  FIELD_PREP_CONST(RTL8365MB_UTP_FIB_DET_FIB_SDET_TIMER_MASK, 1) |
	  FIELD_PREP_CONST(RTL8365MB_UTP_FIB_DET_UTP_LINK_TIMER_MASK, 2) |
	  FIELD_PREP_CONST(RTL8365MB_UTP_FIB_DET_UTP_SDET_TIMER_MASK, 3) |
	  RTL8365MB_UTP_FIB_DET_FORCE_AUTODET |
	  RTL8365MB_UTP_FIB_DET_UTP_FIRST |
	  RTL8365MB_UTP_FIB_DET_DISAUTODET },
	{ RTL8365MB_CHIP_DEBUG0_REG,
	  RTL8365MB_CHIP_DEBUG0_SEL33_EXT2 |
	  RTL8365MB_CHIP_DEBUG0_SEL33_EXT1 |
	  RTL8365MB_CHIP_DEBUG0_DRI_OTHER |
	  RTL8365MB_CHIP_DEBUG0_DRI_EXT1_RG |
	  RTL8365MB_CHIP_DEBUG0_DRI_EXT1 |
	  RTL8365MB_CHIP_DEBUG0_SLR_OTHER |
	  RTL8365MB_CHIP_DEBUG0_SLR_EXT1 },
	{ RTL8365MB_CHIP_DEBUG1_REG,
	  FIELD_PREP_CONST(RTL8365MB_CHIP_DEBUG1_RG1_DN_MASK, 0) |
	  FIELD_PREP_CONST(RTL8365MB_CHIP_DEBUG1_RG1_DP_MASK, 7) |
	  FIELD_PREP_CONST(RTL8365MB_CHIP_DEBUG1_RG0_DN_MASK, 0) |
	  FIELD_PREP_CONST(RTL8365MB_CHIP_DEBUG1_RG0_DP_MASK, 0) },
	{ RTL8365MB_CHIP_DEBUG2_REG,
	  FIELD_PREP_CONST(RTL8365MB_CHIP_DEBUG2_RG2_DN_MASK, 0) |
	  FIELD_PREP_CONST(RTL8365MB_CHIP_DEBUG2_RG2_DP_MASK, 7) |
	  RTL8365MB_CHIP_DEBUG2_DRI_EXT2_RG |
	  RTL8365MB_CHIP_DEBUG2_DRI_EXT2 |
	  RTL8365MB_CHIP_DEBUG2_SLR_EXT2 },
	{ RTL8365MB_EXT_TXC_DLY_REG,
	  FIELD_PREP_CONST(RTL8365MB_EXT_TXC_DLY_EXT2_RGMII_TX_DELAY_MASK, 2) |
	  FIELD_PREP_CONST(RTL8365MB_EXT_TXC_DLY_EXT1_RGMII_TX_DELAY_MASK, 2) },
	{ RTL8365MB_FLOWCTRL_ALL_ON_REG, 0x03CA },
	{ RTL8365MB_FLOWCTRL_JUMBO_SYS_ON_REG, 0x0352 },
	{ RTL8365MB_FLOWCTRL_JUMBO_PORT_ON_REG, 0x00A0 },
	{ RTL8365MB_FLOWCTRL_JUMBO_PORT_PRIV_OFF_REG, 0x0030 },
	{ RTL8365MB_FLOWCTRL_JUMBO_PORT_PRIV_ON_REG, 0x0084 },
	{ RTL8365MB_SCHEDULE_WFQ_BURST_SIZE_REG, 0x1000 },
	{ RTL8365MB_BYPASS_ABLTY_LOCK_REG,
	  FIELD_PREP_CONST(RTL8365MB_BYPASS_ABLTY_LOCK_MASK, 0x1F) },
	{ RTL8365MB_RLDP_CTRL0_REG,
	  RTL8365MB_RLDP_CTRL0_TRIGGER_MODE |
	  RTL8365MB_RLDP_CTRL0_COMP_ID },
	{ RTL8365MB_RRCP_CTRL0_REG,
	  RTL8365MB_RRCP_CTRL0_CRS_SEL |
	  RTL8365MB_RRCP_CTRL0_RRCPV3_SECURITY_CRC |
	  RTL8365MB_RRCP_CTRL0_VLANLEAKY |
	  RTL8365MB_RRCP_CTRL0_RRCPV1_SECURITY_CRC_GET |
	  RTL8365MB_RRCP_CTRL0_RRCPV1_SECURITY_CRC_SET },
	{ RTL8365MB_DIGITAL_INTERFACE_SELECT_REG0,
	  RTL8365MB_DIGITAL_INTERFACE_SELECT_ORG_COL |
	  RTL8365MB_DIGITAL_INTERFACE_SELECT_ORG_CRS },
	{ RTL8365MB_CHIP_ECO_REG, 0x0000 },
};
const size_t rtl8365mb_init_jam_8365mb_vc_size = ARRAY_SIZE(rtl8365mb_init_jam_8365mb_vc);

/* calib_data is a table of parameters for the DW8051. There are 5 possible
 * table for each model/mode. All current supported models uses opt1 variants.
 *
 * They were extracted from vendor library/driver rtl8367c, file
 * rtl8367c_asicdrv_port.c, function rtl8367c_setAsicPortExtMode()
 *
 */

/* Used for mode SGMII and RTL8365MB_CHIP_OPTION_REG=0 (also known as redData
 * in vendor code)
 */
const struct rtl8365mb_sds_init calib_data_SGMII_opt0[] = {
	{0x04D7, 0x0480}, {0xF994, 0x0481}, {0x21A2, 0x0482}, {0x6960, 0x0483},
	{0x9728, 0x0484}, {0x9D85, 0x0423}, {0xD810, 0x0424}, {0x83F2, 0x002E}
};
const size_t calib_data_SGMII_opt0_size = ARRAY_SIZE(calib_data_SGMII_opt0);

/* Used for mode SGMII and RTL8365MB_CHIP_OPTION_REG!=0, possibly only 1 (also
 * known as redDataSB in vendor code)
 */
const struct rtl8365mb_sds_init calib_data_SGMII_opt1[] = {
	{0x04D7, 0x0480}, {0xF994, 0x0481}, {0x31A2, 0x0482}, {0x6960, 0x0483},
	{0x9728, 0x0484}, {0x9D85, 0x0423}, {0xD810, 0x0424}, {0x83F2, 0x002E}
};
const size_t calib_data_SGMII_opt1_size = ARRAY_SIZE(calib_data_SGMII_opt1);

/* For mode HSGMII and RTL8365MB_CHIP_OPTION_REG=0, we have two variants of
 * redData, arbitrary called A and B without any specific order. The selection
 * of each variant is based on the RTL8365MB_CHIP_VER_MODEL_MASK value.
 * Variant A is for 0x1, 0x5 and 0x6, while variant B is for 0x8 and 0x9.
 * None of the currently supported chip models match theses cases.
 */

/* Variant A is known in vendor code as redData1, redData5 and redData6. */
const struct rtl8365mb_sds_init calib_data_HSGMII_opt0_A[] = {
	{0x82F1, 0x0500}, {0xF195, 0x0501}, {0x31A2, 0x0502}, {0x796C, 0x0503},
	{0x9728, 0x0504}, {0x9D85, 0x0423}, {0xD810, 0x0424}, {0x0F80, 0x0001},
	{0x83F2, 0x002E}
};
const size_t calib_data_HSGMII_opt0_A_size = ARRAY_SIZE(calib_data_HSGMII_opt0_A);

/* variant B is known in vendor code as redData8 and redData9. */
const struct rtl8365mb_sds_init calib_data_HSGMII_opt0_B[] = {
	{0x82F1, 0x0500}, {0xF995, 0x0501}, {0x31A2, 0x0502}, {0x796C, 0x0503},
	{0x9728, 0x0504}, {0x9D85, 0x0423}, {0xD810, 0x0424}, {0x0F80, 0x0001},
	{0x83F2, 0x002E}
};
const size_t calib_data_HSGMII_opt0_B_size = ARRAY_SIZE(calib_data_HSGMII_opt0_B);

/* Used for mode HSGMII and RTL8365MB_CHIP_OPTION_REG!=0, possibly just 1 (also
 * known as redDataHB in vendor code)
 */
const struct rtl8365mb_sds_init calib_data_HSGMII_opt1[] = {
	{0x82F0, 0x0500}, {0xF195, 0x0501}, {0x31A2, 0x0502}, {0x7960, 0x0503},
	{0x9728, 0x0504}, {0x9D85, 0x0423}, {0xD810, 0x0424}, {0x0F80, 0x0001},
	{0x83F2, 0x002E}
};
const size_t calib_data_HSGMII_opt1_size = ARRAY_SIZE(calib_data_HSGMII_opt1);
