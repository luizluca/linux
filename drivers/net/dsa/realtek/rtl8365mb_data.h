/* SPDX-License-Identifier: GPL-2.0 */
/* Jam table data interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 */

#ifndef _REALTEK_RTL8365MB_DATA_H
#define _REALTEK_RTL8365MB_DATA_H

#include <linux/types.h>

struct rtl8365mb_jam_tbl_entry {
	u16 reg;
	u16 val;
};

struct rtl8365mb_sds_init {
	u16 data;
	u16 addr;
};

extern const struct rtl8365mb_jam_tbl_entry rtl8365mb_init_jam_common[];
extern const size_t rtl8365mb_init_jam_common_size;

extern const struct rtl8365mb_jam_tbl_entry rtl8365mb_init_jam_8365mb_vc[];
extern const size_t rtl8365mb_init_jam_8365mb_vc_size;

extern const struct rtl8365mb_sds_init calib_data_SGMII_opt0[];
extern const size_t calib_data_SGMII_opt0_size;

extern const struct rtl8365mb_sds_init calib_data_SGMII_opt1[];
extern const size_t calib_data_SGMII_opt1_size;

extern const struct rtl8365mb_sds_init calib_data_HSGMII_opt0_A[];
extern const size_t calib_data_HSGMII_opt0_A_size;

extern const struct rtl8365mb_sds_init calib_data_HSGMII_opt0_B[];
extern const size_t calib_data_HSGMII_opt0_B_size;

extern const struct rtl8365mb_sds_init calib_data_HSGMII_opt1[];
extern const size_t calib_data_HSGMII_opt1_size;

#endif /* _REALTEK_RTL8365MB_DATA_H */
