// SPDX-License-Identifier: GPL-2.0
/* ACL interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include "rtl8365mb_acl.h"
#include "rtl8365mb_table.h"
#include <linux/regmap.h>

#define RTL8365MB_ACL_ENABLE_REG 0x06D5
#define   RTL8365MB_ACL_ENABLE_MASK 0x07FF

#define RTL8365MB_ACL_UNMATCH_PERMIT 0x06D6
#define   RTL8365MB_ACL_UNMATCH_PERMIT_MASK 0x07FF

#define RTL8365MB_ACL_RESET_REG 0x06D9
#define   RTL8365MB_ACL_RESET_MASK 0x0001

#define RTL8365MB_ACL_ACTION_CTRL_BASE 0x0614
#define RTL8365MB_ACL_ACTION_CTRL_EXT_BASE 0x06F0

#define RTL8365MB_ACL_TEMPLATE_REG_BASE 0x0600
#define RTL8365MB_ACL_TEMPLATE_REG(_t, _f) \
		(RTL8365MB_ACL_TEMPLATE_REG_BASE + ((_t) * 4) + ((_f) >> 1))
#define   RTL8365MB_ACL_TEMPLATE_OFFSET(_x) (8 * ((_x) & 1))
#define   RTL8365MB_ACL_TEMPLATE_MASK(_x) \
		(0x003F << RTL8365MB_ACL_TEMPLATE_OFFSET(_x))

#define RTL8365MB_ACL_FIELDSEL_REG_BASE 0x12E7
#define RTL8365MB_ACL_FIELDSEL_REG(_x) \
		(RTL8365MB_ACL_FIELDSEL_REG_BASE + (_x))
#define   RTL8365MB_ACL_FIELDSEL_TYPE_MASK 0x0700
#define   RTL8365MB_ACL_FIELDSEL_OFFSET_MASK 0x00FF

/* Each register contains the ACL action control for two ACL rules */
#define RTL8365MB_ACL_ACTION_CTRL_REG(_x)                                   \
		((_x) < 64 ? (RTL8365MB_ACL_ACTION_CTRL_BASE + ((_x) >> 1)) \
			   : (RTL8365MB_ACL_ACTION_CTRL_EXT_BASE +          \
			      (((_x) - 64) >> 1)))
#define   RTL8365MB_ACL_ACTION_CTRL_OFFSET(_x)			(8 * ((_x) & 1))
#define   RTL8365MB_ACL_ACTION_CTRL_NEGATE_MASK_BASE		0x0040
#define   RTL8365MB_ACL_ACTION_CTRL_NEGATE_MASK(_x) \
		(0x0040 << RTL8365MB_ACL_ACTION_CTRL_OFFSET(_x))
#define   RTL8365MB_ACL_ACTION_CTRL_MODE_MASK_BASE		0x003F
#define   RTL8365MB_ACL_ACTION_CTRL_MODE_MASK(_x) \
		(0x003F << RTL8365MB_ACL_ACTION_CTRL_OFFSET(_x))
#define     RTL8365MB_ACL_ACTION_CTRL_MODE_INTGPIO_MASK_BASE	0x0020
#define     RTL8365MB_ACL_ACTION_CTRL_MODE_FORWARD_MASK_BASE	0x0010
#define     RTL8365MB_ACL_ACTION_CTRL_MODE_POLICING_MASK_BASE	0x0008
#define     RTL8365MB_ACL_ACTION_CTRL_MODE_PRIORITY_MASK_BASE	0x0004
#define     RTL8365MB_ACL_ACTION_CTRL_MODE_SVLAN_MASK_BASE	0x0002
#define     RTL8365MB_ACL_ACTION_CTRL_MODE_CVLAN_MASK_BASE	0x0001

/* ACL action table entry layout, u16[4] */
#define RTL8365MB_ACL_ACTION_ENTRY_D0_CVLAN_MCIDX_MASK	0x003F
#define RTL8365MB_ACL_ACTION_ENTRY_D0_CVLAN_SUBACT_MASK	0x00C0
/* Further detail omitted as it is not used in this driver */

/* ACL rule table entry layout, u16[10] (ish) */
#define RTL8365MB_ACL_RULE_ENTRY_D0_TEMPLATE_MASK	0x0007
#define RTL8365MB_ACL_RULE_ENTRY_D0_TAGEXIST_MASK	0x00F8
#define RTL8365MB_ACL_RULE_ENTRY_D0_PORTMASK_MASK	0xFF00
#define RTL8365MB_ACL_RULE_ENTRY_D1_FIELD0_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D2_FIELD1_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D3_FIELD2_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D4_FIELD3_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D5_FIELD4_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D6_FIELD5_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D7_FIELD6_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D8_FIELD7_MASK   	0xFFFF
#define RTL8365MB_ACL_RULE_ENTRY_D9_VALID_MASK		0x0001
#define RTL8365MB_ACL_RULE_ENTRY_D9_PORTMASK_EXT_MASK	0x000E

#define RTL8365MB_ACL_RULE_ENTRY_ADDR(_data, _x)      \
		((_x) < 64 ? (((_data) << 6) | (_x)) \
			   : (((_data) << 5) | ((_x) + 64)))
#define RTL8365MB_ACL_RULE_ENTRY_DATA_ADDR(_x) \
	RTL8365MB_ACL_RULE_ENTRY_ADDR(1, (_x))
#define RTL8365MB_ACL_RULE_ENTRY_CARE_ADDR(_x) \
	RTL8365MB_ACL_RULE_ENTRY_ADDR(0, (_x))

#define ACLFTYPE(_x) RTL8365MB_ACL_FIELD_TYPE_ ## _x
#define ACLTMPL(_0, _1, _2, _3, _4, _5, _6, _7)                           \
		{ ACLFTYPE(_0), ACLFTYPE(_1), ACLFTYPE(_2), ACLFTYPE(_3), \
		  ACLFTYPE(_4), ACLFTYPE(_5), ACLFTYPE(_6), ACLFTYPE(_7) }

const struct rtl8365mb_acl_template_config
	rtl8365mb_acl_default_template_config = {
		.t0 = ACLTMPL(DMAC0, DMAC1, DMAC2, SMAC0, SMAC1, SMAC2,
			      ETHERTYPE, FS_07),
		.t1 = ACLTMPL(IPV4_SIP0, IPV4_SIP1, IPV4_DIP0, IPV4_DIP1,
			      L4_SPORT, L4_DPORT, FS_02, FS_07),
		.t2 = ACLTMPL(IPV6_SIP0, IPV6_SIP1, L4_SPORT, L4_DPORT, FS_05,
			      FS_06, FS_00, FS_01),
		.t3 = ACLTMPL(IPV6_DIP0, IPV6_DIP1, L4_SPORT, L4_DPORT, FS_00,
			      FS_03, FS_04, FS_07),
		.t4 = ACLTMPL(FS_01, IPRANGE, FS_02, CTAG, STAG, FS_04, FS_03,
			      FS_07),
};

#define ACLFSTYPE(_t) RTL8365MB_ACL_FIELDSEL_TYPE_ ## _t
#define ACLFS(_t, _offset) { .type = ACLFSTYPE(_t), .offset = _offset }

const struct rtl8365mb_acl_fieldsel_config
	rtl8365mb_acl_default_fieldsel_config = {
		.fieldsels = {
			[0] = ACLFS(IPV6, 0),
			[1] = ACLFS(IPV6, 6),
			[2] = ACLFS(IP_PAYLOAD, 12),
			[3] = ACLFS(IPV4, 12),
			[4] = ACLFS(IP_PAYLOAD, 0),
			[5] = ACLFS(IPV4, 0),
			[6] = ACLFS(IPV4, 8),
			[7] = ACLFS(DEFAULT, 0),
			[8] = ACLFS(DEFAULT, 0),
			[9] = ACLFS(DEFAULT, 0),
			[10] = ACLFS(DEFAULT, 0),
			[11] = ACLFS(DEFAULT, 0),
			[12] = ACLFS(DEFAULT, 0),
			[13] = ACLFS(DEFAULT, 0),
			[14] = ACLFS(DEFAULT, 0),
			[15] = ACLFS(DEFAULT, 0),
		},
};

static int rtl8365mb_acl_set_action_mode(struct realtek_priv *priv, int actidx,
					 u32 mode)
{
	return regmap_update_bits(
		priv->map, RTL8365MB_ACL_ACTION_CTRL_REG(actidx),
		RTL8365MB_ACL_ACTION_CTRL_MODE_MASK(actidx),
		FIELD_PREP(RTL8365MB_ACL_ACTION_CTRL_MODE_MASK_BASE, mode)
			<< RTL8365MB_ACL_ACTION_CTRL_OFFSET(actidx));
}

static int rtl8365mb_acl_set_rule_negate(struct realtek_priv *priv, int ruleidx,
					 bool negate)
{
	return regmap_update_bits(
		priv->map, RTL8365MB_ACL_ACTION_CTRL_REG(ruleidx),
		RTL8365MB_ACL_ACTION_CTRL_NEGATE_MASK(ruleidx),
		FIELD_PREP(RTL8365MB_ACL_ACTION_CTRL_NEGATE_MASK_BASE,
			   (negate ? 1 : 0))
			<< RTL8365MB_ACL_ACTION_CTRL_OFFSET(ruleidx));
}

static int rtl8365mb_acl_get_rule_negate(struct realtek_priv *priv, int ruleidx,
					 bool *negate)
{
	int val;
	int ret;

	ret = regmap_read(priv->map, RTL8365MB_ACL_ACTION_CTRL_REG(ruleidx),
			  &val);
	if (ret)
		return ret;

	*negate = FIELD_GET(RTL8365MB_ACL_ACTION_CTRL_NEGATE_MASK_BASE,
			    val >> RTL8365MB_ACL_ACTION_CTRL_OFFSET(ruleidx));

	return 0;
}

int rtl8365mb_acl_reset(struct realtek_priv *priv)
{
	int ret;
	int i;

	/* Disable ACL for all ports */
	ret = regmap_write(priv->map, RTL8365MB_ACL_ENABLE_REG, 0);

	/* Permit frames unmatched by ACL filters */
	ret = regmap_write(priv->map, RTL8365MB_ACL_UNMATCH_PERMIT,
			   RTL8365MB_ACL_UNMATCH_PERMIT_MASK);

	/* Set the ACL action mode bits to all 1's for all actions,
	 * and the ACL rule negate bit to all 0's for all rules.
	 */
	for (i = 0; i < RTL8365MB_NUM_ACL_CONFIGS; i++) {
		ret = rtl8365mb_acl_set_action_mode(
			priv, i, RTL8365MB_ACL_ACTION_MODE_ALL);
		if (ret)
			return ret;

		ret = rtl8365mb_acl_set_rule_negate(priv, i, false);
		if (ret)
			return ret;
	}

	/* Now this will erase all ACL actions and rules */
	ret = regmap_write(priv->map, RTL8365MB_ACL_RESET_REG,
			   RTL8365MB_ACL_RESET_MASK);
	if (ret)
		return ret;

	return 0;
}

int rtl8365mb_acl_set_template_config(
	struct realtek_priv *priv,
	const struct rtl8365mb_acl_template_config *config)
{
	int ret;
	int val;
	int t; /* template index */
	int f; /* field */

	for (t = 0; t < RTL8365MB_NUM_ACL_TEMPLATES; t++) {
		for (f = 0; f < RTL8365MB_NUM_ACL_FIELDS; f += 2) {
			val = config->templates[t][f] |
			      (config->templates[t][f + 1] << 8);

			dev_info(priv->dev, "XXX %d,%d = %04x, %04x\n", t, f,
				 val, RTL8365MB_ACL_TEMPLATE_REG(t, f));

			ret = regmap_write(priv->map,
					   RTL8365MB_ACL_TEMPLATE_REG(t, f),
					   val);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int rtl8365mb_acl_set_fieldsel_config(
	struct realtek_priv *priv,
	const struct rtl8365mb_acl_fieldsel_config *config)
{
	int ret;
	int i;

	for (i = 0; i < RTL8365MB_NUM_ACL_FIELDSELS; i++) {
		const struct rtl8365mb_acl_fieldsel *fs = &config->fieldsels[i];

		ret = regmap_write(
			priv->map, RTL8365MB_ACL_FIELDSEL_REG(i),
			FIELD_PREP(RTL8365MB_ACL_FIELDSEL_TYPE_MASK, fs->type) |
				FIELD_PREP(RTL8365MB_ACL_FIELDSEL_OFFSET_MASK,
					   fs->offset));
		if (ret)
			return ret;
	}

	return 0;
}

int rtl8365mb_acl_set_port_enable(struct realtek_priv *priv, int port,
				  bool enable)
{
	return regmap_update_bits(priv->map, RTL8365MB_ACL_ENABLE_REG,
				  BIT(port), enable << port);
}

int rtl8365mb_acl_set_action(struct realtek_priv *priv, int actidx,
			     const struct rtl8365mb_acl_action *action)
{
	u16 data[4] = { 0 };
	int ret;

	ret = rtl8365mb_acl_set_action_mode(priv, actidx, action->mode);
	if (ret)
		return ret;

	data[0] = FIELD_PREP(RTL8365MB_ACL_ACTION_ENTRY_D0_CVLAN_MCIDX_MASK,
			     action->cvlan.mcidx) |
		  FIELD_PREP(RTL8365MB_ACL_ACTION_ENTRY_D0_CVLAN_SUBACT_MASK,
			     action->cvlan.subaction);
	/* NOTE: Leave the rest empty since it is unused FIXME */

	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = RTL8365MB_TABLE_ACL_ACTION,
					    .op = RTL8365MB_TABLE_OP_WRITE,
					    .arg.acl_action.addr = actidx,
				    },
				    data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	return 0;
}

int rtl8365mb_acl_set_rule(struct realtek_priv *priv, int ruleidx,
			   const struct rtl8365mb_acl_rule *rule)
{
	u16 data_addr = RTL8365MB_ACL_RULE_ENTRY_DATA_ADDR(ruleidx);
	u16 care_addr = RTL8365MB_ACL_RULE_ENTRY_CARE_ADDR(ruleidx);
	u16 data_data[10] = { 0 };
	u16 care_data[10] = { 0 };
	u16 data_tmp;
	u16 care_tmp;
	int ret;
	int i;

	/* Erase previous data entry to ensure the valid bit is zero */
	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = RTL8365MB_TABLE_ACL_RULE,
					    .op = RTL8365MB_TABLE_OP_WRITE,
					    .arg.acl_rule.addr = data_addr,
				    },
				    data_data, ARRAY_SIZE(data_data));
	if (ret)
		return ret;

	/* Return early if we are just disabling a rule */
	if (!rule->enabled)
		return 0;

	/* Set the negation bit */
	ret = rtl8365mb_acl_set_rule_negate(priv, ruleidx, rule->negate);
	if (ret)
		return ret;

	/* Pack the care- and data-entries */
	care_data[0] = FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D0_TEMPLATE_MASK,
				  RTL8365MB_ACL_RULE_ENTRY_D0_TEMPLATE_MASK) |
		       FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D0_PORTMASK_MASK,
				  rule->care.portmask);
	data_data[0] = FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D0_TEMPLATE_MASK,
				  rule->template) |
		       FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D0_PORTMASK_MASK,
				  rule->data.portmask);

	for (i = 0; i < 8; i++) {
		care_data[i + 1] = rule->care.fields[i];
		data_data[i + 1] = rule->data.fields[i];
	}

	care_data[9] = FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D9_PORTMASK_EXT_MASK,
				  rule->care.portmask >> 8) |
		       FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D9_VALID_MASK,
				  rule->enabled); // TODO remove this enabled?

	// experiment
	care_data[9] = FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D9_PORTMASK_EXT_MASK,
				  rule->care.portmask >> 8) |
		       FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D9_VALID_MASK,
				  0); // TODO remove this enabled?

	data_data[9] = FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D9_PORTMASK_EXT_MASK,
				  rule->data.portmask >> 8);

	/* Some bit twiddling which is apparently necessary before committing */
	for (i = 0; i < 10; i++) {
		care_tmp = care_data[i] & ~data_data[i];
		data_tmp = care_data[i] & data_data[i];
		data_data[i] = data_tmp;
		care_data[i] = care_tmp;

		dev_info(priv->dev,
			 "XXX data_data[%d] = 0x%04x\n"
			 "XXX-care_data[%d] = 0x%04x\n",
			 i, data_data[i], i, care_data[i]);
	}

	/* This comes after as it mustn't get clobbered */
	data_data[9] |= FIELD_PREP(RTL8365MB_ACL_RULE_ENTRY_D9_VALID_MASK,
				   rule->enabled);

	/* Now write the entries, starting with the care entry. The data entry
	 * holds the valid (i.e. enable) bit, hence we should commit it last.
	 */
	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = RTL8365MB_TABLE_ACL_RULE,
					    .op = RTL8365MB_TABLE_OP_WRITE,
					    .arg.acl_rule.addr = care_addr,
				    },
				    care_data, ARRAY_SIZE(care_data));
	if (ret)
		return ret;

	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = RTL8365MB_TABLE_ACL_RULE,
					    .op = RTL8365MB_TABLE_OP_WRITE,
					    .arg.acl_rule.addr = data_addr,
				    },
				    data_data, ARRAY_SIZE(data_data));
	if (ret)
		return ret;

	return 0;
}

int rtl8365mb_acl_get_rule(struct realtek_priv *priv, int ruleidx,
			   struct rtl8365mb_acl_rule *rule)
{
	u16 data_addr = RTL8365MB_ACL_RULE_ENTRY_DATA_ADDR(ruleidx);
	u16 care_addr = RTL8365MB_ACL_RULE_ENTRY_CARE_ADDR(ruleidx);
	u16 data_data[10] = { 0 };
	u16 care_data[10] = { 0 };
	bool negate;
	int ret;
	int i;

	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = RTL8365MB_TABLE_ACL_RULE,
					    .op = RTL8365MB_TABLE_OP_READ,
					    .arg.acl_rule.addr = data_addr,
				    },
				    data_data, ARRAY_SIZE(data_data));
	if (ret)
		return ret;

	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = RTL8365MB_TABLE_ACL_RULE,
					    .op = RTL8365MB_TABLE_OP_READ,
					    .arg.acl_rule.addr = care_addr,
				    },
				    care_data, ARRAY_SIZE(care_data));
	if (ret)
		return ret;

	/* Untwiddle bits */
	for (i = 0; i < 10; i++) {
		care_data[i] = care_data[i] ^ data_data[i];
		/* data_data left verbatim */
	}

	/* Unpack the care- and data-entries */
	rule->template = FIELD_GET(RTL8365MB_ACL_RULE_ENTRY_D0_TEMPLATE_MASK,
				   data_data[0]);
	rule->data.portmask =
		FIELD_GET(RTL8365MB_ACL_RULE_ENTRY_D0_PORTMASK_MASK,
			  data_data[0]) |
		FIELD_GET(RTL8365MB_ACL_RULE_ENTRY_D9_PORTMASK_EXT_MASK,
			  data_data[9]);
	rule->care.portmask =
		FIELD_GET(RTL8365MB_ACL_RULE_ENTRY_D0_PORTMASK_MASK,
			  care_data[0]) |
		FIELD_GET(RTL8365MB_ACL_RULE_ENTRY_D9_PORTMASK_EXT_MASK,
			  care_data[9]);
	rule->enabled =
		FIELD_GET(RTL8365MB_ACL_RULE_ENTRY_D9_VALID_MASK, data_data[9]);

	for (i = 0; i < 8; i++) {
		rule->care.fields[i] = care_data[i + 1];
		rule->data.fields[i] = data_data[i + 1];
	}

	/* And the negate bit */
	ret = rtl8365mb_acl_get_rule_negate(priv, ruleidx, &negate);
	if (ret)
		return ret;

	rule->negate = negate;

	return 0;
}
