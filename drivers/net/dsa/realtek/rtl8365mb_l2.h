// SPDX-License-Identifier: GPL-2.0
/* Forwarding and multicast database interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#ifndef _REALTEK_RTL8365MB_L2_H
#define _REALTEK_RTL8365MB_L2_H

#include <linux/if_ether.h>
#include <linux/types.h>

#include "realtek.h"

#define RTL8365MB_LEARN_LIMIT_MAX	2112

struct rtl8365mb_l2_uc_key {
	u8 mac_addr[ETH_ALEN];
	u16 efid;
	bool ivl;
	u16 vid; /* IVL */
	u16 fid;
};

struct rtl8365mb_l2_uc {
	struct rtl8365mb_l2_uc_key key;
	u8 port;
	u8 age;
	u8 priority;

	bool sa_block;
	bool da_block;
	bool auth;
	bool is_static;
	bool sa_pri;
	bool fwd_pri;
};

struct rtl8365mb_l2_mc_key {
	u8 mac_addr[ETH_ALEN];
	bool ivl;
	union {
		u16 vid; /* IVL */
		u16 fid; /* SVL */
	};
};

struct rtl8365mb_l2_mc {
	struct rtl8365mb_l2_mc_key key;
	u16 member;
	u8 priority;
	u8 igmpidx;

	bool is_static;
	bool fwd_pri;
	bool igmp_asic;
};

int rtl8365mb_l2_get_uc_by_addr(struct realtek_priv *priv, int addr,
				struct rtl8365mb_l2_uc *uc);
int rtl8365mb_l2_get_mc_by_addr(struct realtek_priv *priv, int addr,
				struct rtl8365mb_l2_mc *mc);

int rtl8365mb_l2_get_next_uc(struct realtek_priv *priv, int *addr,
			     struct rtl8365mb_l2_uc *uc);

/* TODO: I don't think this one needs to be a public API: */
// Then you can remove the key argument perhaps
int rtl8365mb_l2_get_uc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_uc_key *key,
			struct rtl8365mb_l2_uc *uc);

int rtl8365mb_l2_add_uc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_uc *uc);

int rtl8365mb_l2_del_uc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_uc_key *key);

int rtl8365mb_l2_flush(struct realtek_priv *priv, int port, u16 vid);

int rtl8365mb_l2_get_next_mc(struct realtek_priv *priv, int *addr,
			     struct rtl8365mb_l2_mc *mc);

int rtl8365mb_l2_get_mc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_mc_key *key,
			struct rtl8365mb_l2_mc *mc);

int rtl8365mb_l2_add_mc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_mc *mc);

int rtl8365mb_l2_del_mc(struct realtek_priv *priv,
			const struct rtl8365mb_l2_mc_key *key);


#endif /* _REALTEK_RTL8365MB_L2_H */
