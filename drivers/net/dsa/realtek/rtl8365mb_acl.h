// SPDX-License-Identifier: GPL-2.0
/* ACL interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022 Alvin Šipraga <alsi@bang-olufsen.dk>
 *
 * I think ACL stands for Access Control List, but this is never explicitly
 * stated by Realtek.
 *
 * When talking about ACL, we typically refer to a grouping of rules with an
 * action. The rules operate on ingress packets and the action is performed if
 * the rules indicate a match. Together these make up what is called an ACL
 * config:
 *
 *   ACL CONFIG
 *      +-------+-----+--------+--------+
 *      | rule0 | ... | rule N | action |  for N = 0 .. 4
 *      +-------+-----+--------+--------+
 *       \____________________/    ^
 *                 |               |
 *                 v               |  perform action
 *   INGRESS +-----------+        /
 *   PACKET  |  payload  |       /
 *           +-----------+      /
 *                 `-----------'
 *         all      \   yes
 *         rules     `--------> try the next ACL config ...
 *         match?        no
 *
 * Each ACL config consists of between 1 and 5 ACL rules, which are applied to
 * the ingress packet payload.
 *
 * ACL rules will perform some kind of bitwise comparison with the packet
 * payload, the result of which determines whether the rule matches or not.
 *
 * ACL actions have several operational modes
 * (cf. &enum rtl8365mb_acl_action_mode):
 *
 *   - CVLAN (aka. VLAN4k or 802.1Q)
 *   - SVLAN (aka. stacked VLAN or 802.1ad)
 *   - priority (QoS)
 *   - policing and logging
 *   - forwarding (e.g. drop, copy to CPU, trap to CPU, etc.)
 *   - interrupt and GPIO (assert an interrupt or GPIO line)
 *
 * These modes aren't mutually exclusive, so an action may operate in multiple
 * modes at once. For instance, an ACL config might specify that certain ingress
 * frames (per the ACL rule) should be reclassified to a particular VLAN (per
 * the ACL action), and that an interrupt should be asserted whenever this
 * happens (per the same ACL action). Note that the modes are optional: it is
 * not a requirement that an action specify every operational mode. The choice
 * of operational modes is selected in a register bitfield for every ACL rule
 * index. For further detail, see &struct rtl8365mb_acl_action.
 *
 * Be advised that the documentation for this feature is severely lacking, so it
 * is unclear exactly what certain combinations of ACL action modes might result
 * in.
 *
 * ACL rules and actions are programmed into the ACL_RULE and ACL_ACTION tables
 * respectively. Although the above description speaks of 1 action per up to 5
 * rules, the size of the ACL_RULE and ACL_ACTION tables is equal. This is
 * because, in principle, each config could consist of exactly one rule and one
 * action. Each table holds up to 96 rules and actions respectively. In general
 * we can speak of an ACL rule and ACL action index, referring to the placement
 * of the given entry in its respective table.
 *
 * The tables are independent of one another, in the sense that the entries
 * themselves do not refer to entries in the other table. Instead, the switch
 * assumes that for each rule with index i in the ACL_RULE table, it should
 * consider the corresponding action with index i in the ACL_ACTION table.
 * In order to facilitate constructing ACL configs consisting of multiple rules,
 * the switch has a concept of a "cascaded" action, which is when the ACL action
 * mode bitfield is zero, meaning "cascade to previous action":
 *
 *              ACL RULE     ACL ACTION       ACL MODE (stored in a register,
 *               TABLE         TABLE          BITFIELD  not in the table)
 *             +-------+    +---------+    +-5 4 3 2 1 0-+
 *           / | rule0 | -> | action0 | -> | 0 0 0 0 0 1 | -> CVLAN <---.
 *   first  |  +-------+    +---------+    +-------------+              |
 *   config |  | rule1 | -> | action1 | -> | 0 0 0 0 0 0 | -> cascade --'
 *          |  +-------+    +---------+    +-------------+              |
 *           \ | rule2 | -> | action2 | -> | 0 0 0 0 0 0 | -> cascade --'
 *             +-------+    +---------+    +-------------+
 *           / | rule3 |    | action3 |    | 1 0 0 0 0 0 | -> INTGPIO <-.
 *   second |  +-------+    +---------+    +-------------+              |
 *   config |  | rule4 |    | action4 |    | 0 0 0 0 0 0 | -> cascade --'
 *          |  +-------+    +---------+    +-------------+
 *   etc.  ... |  ...  |    |   ...   |    |     ...     | ...
 *
 * In the above diagram, we have two configs consisting of multiple rules. If
 * all the rules of the first config match, then action0 is executed and a CVLAN
 * operation is performed. ACL processing of the ingress frame then stops. (TODO
 * check) If any of the rules of the first config do not match, then the switch
 * will continue processing the second config, and so on.
 *
 * Due to the spartan nature of the API offered here, the details above should
 * be carefully considered. It is up to the user(s) in other part(s) of the
 * driver to place the ACL rules and ACL actions into the tables correctly.
 */

#ifndef _REALTEK_RTL8365MB_ACL_H
#define _REALTEK_RTL8365MB_ACL_H

#include <linux/types.h>

#include "realtek.h"

#define RTL8365MB_NUM_ACL_CONFIGS 96

/**
 * enum rtl8365mb_acl_action_mode - available ACL action operational modes
 *
 * NOTE: Don't change the enum values. They must concur with the field
 * described by @RTL8365MB_ACTION_CTRL_MODE_MASK.
 */
enum rtl8365mb_acl_action_mode {
	RTL8365MB_ACL_ACTION_MODE_CVLAN = 0x0001,
	RTL8365MB_ACL_ACTION_MODE_SVLAN = 0x0002,
	RTL8365MB_ACL_ACTION_MODE_PRIORITY = 0x0004,
	RTL8365MB_ACL_ACTION_MODE_POLICING = 0x0008,
	RTL8365MB_ACL_ACTION_MODE_FORWARD = 0x0010,
	RTL8365MB_ACL_ACTION_MODE_INTGPIO = 0x0020,
	RTL8365MB_ACL_ACTION_MODE_ALL = 0x003F,
};

/**
 * enum rtl8365mb_acl_cvlan_subaction - CVLAN ACL subactions
 * @RTL8365MB_ACL_CVLAN_SUBACTION_INGRESS: reclassify packet on ingress
 * (before learning) according to the VLAN MC index in
 * &rtl8365mb_acl_action.cvlan.mcidx.
 * @RTL8365MB_ACL_CVLAN_SUBACTION_EGRESS: reclassify packet on egress (before
 * forwarding) according to the VLAN MC index in
 * &rtl8365mb_acl_action.cvlan.mcidx.
 */
enum rtl8365mb_acl_cvlan_subaction {
	RTL8365MB_ACL_CVLAN_SUBACTION_INGRESS,
	RTL8365MB_ACL_CVLAN_SUBACTION_EGRESS,
};

/**
 * struct rtl8365mb_acl_action - ACL action to be executed when rule(s) match
 * @mode: mask of rtl8365mb_acl_action_mode to indicate which modes of action
 *        are described in this action
 * @cvlan: CVLAN action mode description, valid iff (mode & ACTION_MODE_CVLAN)
 * @cvlan.subaction: what type of CVLAN action to take
 * @cvlan.mcidx: membership config (MC) index to use for CVLAN action mode
 *
 * An action can operate in several modes, namely on CVLANs, SVLANs, PRIORITY,
 * POLICING, FORWARDing, and INTerrupts and GPIOs. These modes are orthogonal,
 * so an action can - for example - manipulate both the CVLAN and also assert a
 * GPIO at the same time.
 *
 * For each mode of operation there is a corresponding nested structure
 * describing the parameters of that mode. The nested structure is ignored by
 * the hardware unless the corresponding mode is enabled in the @mode field.
 * Otherwise it must be valid.
 */
struct rtl8365mb_acl_action {
	u8 mode;

	struct {
		enum rtl8365mb_acl_cvlan_subaction subaction;
		u16 mcidx;
		/* Additional egress tagging features omitted */
	} cvlan;

	/* Other modes of operation omitted */
};

/**
 * enum rtl8365mb_acl_field_type - ASIC-defined field types
 * @RTL8365MB_ACL_FIELD_TYPE_UNUSED: field is not used
 * @RTL8365MB_ACL_FIELD_TYPE_DMAC0: destination MAC address bits 15 .. 0
 * @RTL8365MB_ACL_FIELD_TYPE_DMAC1: destination MAC address bits 31 .. 16
 * @RTL8365MB_ACL_FIELD_TYPE_DMAC2: destination MAC address bits 47 .. 32
 * @RTL8365MB_ACL_FIELD_TYPE_SMAC0: source MAC address bits 15 .. 0
 * @RTL8365MB_ACL_FIELD_TYPE_SMAC1: source MAC address bits 31 .. 16
 * @RTL8365MB_ACL_FIELD_TYPE_SMAC2: source MAC address bits 47 .. 32
 * @RTL8365MB_ACL_FIELD_TYPE_ETHERTYPE: Type/Length
 * @RTL8365MB_ACL_FIELD_TYPE_STAG: 802.1ad tag: PCP(3):DEI(1):SVID(12)
 * @RTL8365MB_ACL_FIELD_TYPE_CTAG: 802.1q tag: PCP(3):DEI(1):CVID(12)
 * @RTL8365MB_ACL_FIELD_TYPE_IPV4_SIP0: source IPv4 address bits 15 .. 0
 * @RTL8365MB_ACL_FIELD_TYPE_IPV4_SIP1:  source IPv4 address bits 31 .. 16
 * @RTL8365MB_ACL_FIELD_TYPE_IPV4_DIP0: destination IPv4 address bits 15 .. 0
 * @RTL8365MB_ACL_FIELD_TYPE_IPV4_DIP1: destination IPv4 address bits 31 .. 16
 * @RTL8365MB_ACL_FIELD_TYPE_IPV6_SIP0: source IPv6 address bits 15 .. 0
 * @RTL8365MB_ACL_FIELD_TYPE_IPV6_SIP1: source IPv6 address bits 31 .. 16
 * @RTL8365MB_ACL_FIELD_TYPE_IPV6_DIP0: destination IPv6 address bits 15 .. 0
 * @RTL8365MB_ACL_FIELD_TYPE_IPV6_DIP1: destination IPv6 address bits 31 .. 16
 * @RTL8365MB_ACL_FIELD_TYPE_L4_DPORT: destination TCP/UDP port
 * @RTL8365MB_ACL_FIELD_TYPE_L4_SPORT: source TCP/UDP port
 * @RTL8365MB_ACL_FIELD_TYPE_VIDRANGE: VID range check(?)
 * @RTL8365MB_ACL_FIELD_TYPE_IPRANGE: IPv4/IPv6 range check mask(?)
 * @RTL8365MB_ACL_FIELD_TYPE_PORTRANGE: port range check(?)
 * @RTL8365MB_ACL_FIELD_TYPE_FIELD_VALID: field selectors valid tag(?)
 * @RTL8365MB_ACL_FIELD_TYPE_FS_n: field selector n (for n = 0 .. 15)
 *
 * For the IPv6 address field types, the switch appears to have space for
 * matching on the rest of the IPv6 source/destination addresses, but this is
 * not documented by Realtek, nor is it tested.
 *
 * NOTE: Don't change the enum values, as they are used to program the switch.
 */
enum rtl8365mb_acl_field_type {
	RTL8365MB_ACL_FIELD_TYPE_UNUSED = 0x00,
	RTL8365MB_ACL_FIELD_TYPE_DMAC0 = 0x01,
	RTL8365MB_ACL_FIELD_TYPE_DMAC1,
	RTL8365MB_ACL_FIELD_TYPE_DMAC2,
	RTL8365MB_ACL_FIELD_TYPE_SMAC0,
	RTL8365MB_ACL_FIELD_TYPE_SMAC1,
	RTL8365MB_ACL_FIELD_TYPE_SMAC2,
	RTL8365MB_ACL_FIELD_TYPE_ETHERTYPE,
	RTL8365MB_ACL_FIELD_TYPE_STAG,
	RTL8365MB_ACL_FIELD_TYPE_CTAG,
	RTL8365MB_ACL_FIELD_TYPE_IPV4_SIP0 = 0x10,
	RTL8365MB_ACL_FIELD_TYPE_IPV4_SIP1,
	RTL8365MB_ACL_FIELD_TYPE_IPV4_DIP0,
	RTL8365MB_ACL_FIELD_TYPE_IPV4_DIP1,
	RTL8365MB_ACL_FIELD_TYPE_IPV6_SIP0 = 0x20,
	RTL8365MB_ACL_FIELD_TYPE_IPV6_SIP1,
	RTL8365MB_ACL_FIELD_TYPE_IPV6_DIP0 = 0x28,
	RTL8365MB_ACL_FIELD_TYPE_IPV6_DIP1,
	RTL8365MB_ACL_FIELD_TYPE_L4_DPORT = 0x2A,
	RTL8365MB_ACL_FIELD_TYPE_L4_SPORT,
	RTL8365MB_ACL_FIELD_TYPE_VIDRANGE = 0x30,
	RTL8365MB_ACL_FIELD_TYPE_IPRANGE,
	RTL8365MB_ACL_FIELD_TYPE_PORTRANGE,
	RTL8365MB_ACL_FIELD_TYPE_FIELD_VALID,
	RTL8365MB_ACL_FIELD_TYPE_FS_00 = 0x40,
	RTL8365MB_ACL_FIELD_TYPE_FS_01,
	RTL8365MB_ACL_FIELD_TYPE_FS_02,
	RTL8365MB_ACL_FIELD_TYPE_FS_03,
	RTL8365MB_ACL_FIELD_TYPE_FS_04,
	RTL8365MB_ACL_FIELD_TYPE_FS_05,
	RTL8365MB_ACL_FIELD_TYPE_FS_06,
	RTL8365MB_ACL_FIELD_TYPE_FS_07,
	RTL8365MB_ACL_FIELD_TYPE_FS_08,
	RTL8365MB_ACL_FIELD_TYPE_FS_09,
	RTL8365MB_ACL_FIELD_TYPE_FS_10,
	RTL8365MB_ACL_FIELD_TYPE_FS_11,
	RTL8365MB_ACL_FIELD_TYPE_FS_12,
	RTL8365MB_ACL_FIELD_TYPE_FS_13,
	RTL8365MB_ACL_FIELD_TYPE_FS_14,
	RTL8365MB_ACL_FIELD_TYPE_FS_15,
};

#define RTL8365MB_NUM_ACL_TEMPLATES 5
#define RTL8365MB_NUM_ACL_FIELDS 8

/**
 * struct rtl8365mb_acl_template_config - switch template configuration
 * @templates: five templates with 8 fields specified
 *
 * This struct is used together with rtl8365mb_acl_set_template_config() to
 * program the available templates' fields. Typically this will only be
 * set once, according to the needs of the ACL rules to be programmed.
 */
struct rtl8365mb_acl_template_config {
	union {
		u8 templates[RTL8365MB_NUM_ACL_TEMPLATES]
			    [RTL8365MB_NUM_ACL_FIELDS];
		struct {
			u8 t0[RTL8365MB_NUM_ACL_FIELDS];
			u8 t1[RTL8365MB_NUM_ACL_FIELDS];
			u8 t2[RTL8365MB_NUM_ACL_FIELDS];
			u8 t3[RTL8365MB_NUM_ACL_FIELDS];
			u8 t4[RTL8365MB_NUM_ACL_FIELDS];
		};
	};
};

/* The recommended default ACL template configuration looks like this:
 *
 * +-----+---------------------------------------------------------------------+
 * |ACL  |                              fields                                 |
 * |tmpl.|------+--------+--------+--------+--------+--------+--------+--------|
 * |idx./  0    |   1    |   2    |   3    |   4    |   5    |   6    |   7    |
 * +---+--------+--------+--------+--------+--------+--------+--------+--------+
 * | 0 | DMAC0  | DMAC1  | DMAC2  | SMAC0  | SMAC1  | SMAC2  |EtherTyp| FS_07  |
 * | 1 |IPV4SIP0|IPV4SIP1|IPV4DIP0|IPV4DIP1|L4SPORT |L4DPORT | FS_02  | FS_07  |
 * | 2 |IPV6SIP0|IPV6SIP1|L4SPORT |L4DPORT | FS_05  | FS_06  | FS_00  | FS_01  |
 * | 3 |IPV6DIP0|IPV6DIP1|L4SPORT |L4DPORT | FS_00  | FS_03  | FS_04  | FS_07  |
 * | 4 | FS_01  |IPRANGE | FS_02  |  CTAG  |  STAG  | FS_04  | FS_03  | FS_07  |
 * +---+--------+--------+--------+--------+--------+--------+--------+--------+
 *
 * This is reflected in rtl8365mb_acl_default_template_config.
 */
extern const struct rtl8365mb_acl_template_config
	rtl8365mb_acl_default_template_config;

/**
 * enum rtl8365mb_acl_fieldsel_type - ACL field selector format types
 * @RTL8365MB_ACL_FIELDSEL_TYPE_DEFAULT: ASIC default
 * @RTL8365MB_ACL_FIELDSEL_TYPE_RAW: raw packet, start after preamble
 * @RTL8365MB_ACL_FIELDSEL_TYPE_LLC: LLC packet
 * @RTL8365MB_ACL_FIELDSEL_TYPE_IPV4: IPv4 packet, begin at IPv4 header
 * @RTL8365MB_ACL_FIELDSEL_TYPE_ARP: ARP packet, begin after EtherType 0x0806
 * @RTL8365MB_ACL_FIELDSEL_TYPE_IPV6: IPV6 packet, begin at IPv6 header
 * @RTL8365MB_ACL_FIELDSEL_TYPE_IP_PAYLOAD: begin at start of IP payload
 * @RTL8365MB_ACL_FIELDSEL_TYPE_L4_PAYLOAD: TCP/UDP: begin after header; ICMP:
 * begin at 4 bytes offieldselet from header
 *
 * Each field selector has a type, which determines the initial packet offset
 * based on packet type. These are the available types.
 *
 * NOTE: Don't change the enum values.
 */
enum rtl8365mb_acl_fieldsel_type {
	RTL8365MB_ACL_FIELDSEL_TYPE_DEFAULT = 0x0,
	RTL8365MB_ACL_FIELDSEL_TYPE_RAW = 0x1,
	RTL8365MB_ACL_FIELDSEL_TYPE_LLC = 0x2,
	RTL8365MB_ACL_FIELDSEL_TYPE_IPV4 = 0x3,
	RTL8365MB_ACL_FIELDSEL_TYPE_ARP = 0x4,
	RTL8365MB_ACL_FIELDSEL_TYPE_IPV6 = 0x5,
	RTL8365MB_ACL_FIELDSEL_TYPE_IP_PAYLOAD = 0x6,
	RTL8365MB_ACL_FIELDSEL_TYPE_L4_PAYLOAD = 0x7,
};

/**
 * struct rtl8365mb_acl_fieldsel - ACL field selector configuration
 * @type: cf. &enum rtl8365mb_acl_fieldsel_type
 * @offset: offset in octets from the beginning specified by @type
 */
struct rtl8365mb_acl_fieldsel {
	u8 type;
	u8 offset;
};

#define RTL8365MB_NUM_ACL_FIELDSELS 16

struct rtl8365mb_acl_fieldsel_config {
	struct rtl8365mb_acl_fieldsel fieldsels[RTL8365MB_NUM_ACL_FIELDSELS];
};

extern const struct rtl8365mb_acl_fieldsel_config
	rtl8365mb_acl_default_fieldsel_config;

/**
 * struct rtl8365mb_acl_rule_part - ACL rule data for the "care" or "data" parts
 * @portmask: port mask
 * @fields: field data
 *
 * See the description of &struct rtl8365mb_acl_rule for more information.
 */
struct rtl8365mb_acl_rule_part {
	u16 portmask;
	u16 fields[RTL8365MB_NUM_ACL_FIELDS];
};

/**
 * struct rtl8365mb_acl_rule - ACL rule
 * @enabled: rule is enabled
 * @negate: negate matching rules
 * @template: interpret this rule's fields using this template index
 * @care: mask to apply before testing, i.e. "ports and field data we care to
 * test"
 * @data: data to test against, i.e. "ports and field data we expect"
 *
 * For a template index @template, the rule will match a frame if all of the
 * following conditions hold:
 *
 *   1. @enabled == 1
 *   2. BIT(igr_port) & @care.portmask & @data.portmask == BIT(igr_port)
 *   3. frame[FIELD(@template, i)] & @care.fields[i] == @data.fields[i],
 *      for all i = 0 .. 7
 *
 * ... where FIELD(@template,i) corresponds to the frame offset specified by the
 * the i'th field type of ACL template index @template, and frame and igr_port
 * are obvious.
 *
 * If @negate is set, then the conditions (2-3) above are negated:
 *
 *   2. BIT(igr_port) & @care.portmask & @data.portmask != BIT(igr_port)
 *   3. frame[FIELD(@template, i)] & @care.fields[i] != @data.fields[i],
 *      for any i = 0 .. 7.
 */
struct rtl8365mb_acl_rule {
	u8 enabled : 1;
	u8 negate : 1;
	u8 template;
	struct rtl8365mb_acl_rule_part care;
	struct rtl8365mb_acl_rule_part data;
};

/**
 * rtl8365mb_acl_reset - reset ACL functionality to well-defined defaults
 * @priv: driver context
 *
 * This function disables ACL on all ports, erases all ACL actions and rules,
 * and permits frames unmatched by any ACL filters to pass. It should be called
 * before (re)configuring ACL functionality.
 */
int rtl8365mb_acl_reset(struct realtek_priv *priv);

/**
 * rtl8365mb_acl_set_template_config - set the switch ACL templates
 * @priv: driver context
 * @config: table of 5 ACL templates
 *
 * ACL rules refer to an ACL template index, so this should be populated before
 * programming any rules.
 */
int rtl8365mb_acl_set_template_config(
	struct realtek_priv *priv,
	const struct rtl8365mb_acl_template_config *config);

/**
 * rtl8365mb_acl_set_fieldsel_config - set the switch ACL field selectors
 * @priv: driver context
 * @config: 16 field selectors
 *
 * ACL template fields may use field selectors, in which case they should be
 * configured suitably with this function.
 */
int rtl8365mb_acl_set_fieldsel_config(
	struct realtek_priv *priv,
	const struct rtl8365mb_acl_fieldsel_config *config);

/**
 * rtl8365mb_acl_set_port_enable - enable or disable ACL on a given port
 * @priv: driver context
 * @port: port index
 * @enable: whether to enable ACL or not
 */
int rtl8365mb_acl_set_port_enable(struct realtek_priv *priv, int port,
				  bool enable);


/**
 * rtl8365mb_acl_set_action - program an ACL action
 * @priv: driver context
 * @actidx: the index of the ACL action table entry to set
 * @action: ACL action description
 *
 * ACL actions and rules must have matching indices to work together.
 */
int rtl8365mb_acl_set_action(struct realtek_priv *priv, int actidx,
			     const struct rtl8365mb_acl_action *action);

/**
 * rtl8365mb_acl_set_rule - program an ACL rule
 * @priv: driver context
 * @ruleidx: the index of the ACL rule table entry to set
 * @rule: ACL rule description
 *
 * ACL actions and rules must have matching indices to work together.
 */
int rtl8365mb_acl_set_rule(struct realtek_priv *priv, int ruleidx,
			   const struct rtl8365mb_acl_rule *rule);

/**
 * rtl8365mb_acl_get_rule - get an ACL rule from the switch
 * @priv: driver context
 * @ruleidx: the index of the ACL rule table entry to get
 * @rule: ACL rule description is output here
 */
int rtl8365mb_acl_get_rule(struct realtek_priv *priv, int ruleidx,
			   struct rtl8365mb_acl_rule *rule);

#endif /* _REALTEK_RTL8365MB_ACL_H */
