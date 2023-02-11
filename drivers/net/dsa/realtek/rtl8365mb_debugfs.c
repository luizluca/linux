// SPDX-License-Identifier: GPL-2.0
/* debugfs interface interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022-2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

#include <linux/debugfs.h>

#include "realtek.h"
#include "rtl8365mb_debugfs.h"
#include "rtl8365mb_acl.h"
#include "rtl8365mb_l2.h"
#include "rtl8365mb_vlan.h"
#include "rtl8365mb_table.h"

static u16 user_addr;
static u16 table = 2;

static int rtl8365mb_debugfs_table_show(struct seq_file *file, void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	u16 data[10] = { 0 };
	int ret;

	ret = rtl8365mb_table_query(priv,
				    &(struct rtl8365mb_table_query){
					    .table = table,
					    .op = RTL8365MB_TABLE_OP_READ,
					    .arg.acl_action.addr = user_addr,
				    },
				    data, ARRAY_SIZE(data));
	if (ret)
		return ret;

	seq_printf(file, "%04x %04x %04x %04x %04x %04x %04x %04x %04x %04x\n",
		   data[0], data[1], data[2], data[3], data[4], data[5],
		   data[6], data[7], data[8], data[9]);

	return 0;
}

static int rtl8365mb_debugfs_acl_actions_show(struct seq_file *file,
					      void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_acl_action action = { 0 };
	struct rtl8365mb_acl_rule rule = { 0 };
	int cascade_count;
	int ret;
	int i;

	seq_printf(file, "index\tmode\n");

	for (i = 0; i < RTL8365MB_NUM_ACL_CONFIGS; i++) {
		/* Fistly check if the rule is enabled, since the action is not
		 * interesting if there is no corresponding rule.
		 */
		ret = rtl8365mb_acl_get_rule(priv, i, &rule);
		if (ret)
			return ret;

		if (!rule->enabled)
			continue;

		/* Now get the action */
		ret = rtl8365mb_acl_get_action(priv, i, &action);
		if (ret)
			return ret;

		seq_printf("%d\t%04x\n", i, action->mode);

		/* Empty mode means "cascade into previous action", but there is
		 * a limit of up to 5 cascaded actions.
		 */
		if (!action->mode && cascade_count < 5) {
			cascade_count++;
			seq_printf("\tcascade\n");
			continue;
		} else {
			cascade_count = 1;
		}

		if (action->mode & RTL8365MB_ACTION_MODE_CVLAN) {
			const char *subaction_str;

			switch (action->cvlan.subaction) {
			case RTL8365MB_ACL_CVLAN_SUBACTION_INGRESS:
				subaction_str = "ingress";
				break;
			case RTL8365MB_ACL_CVLAN_SUBACTION_EGRESS:
				subaction_str = "egress";
				break;
			default:
				subaction_str = "?";
				break;
			}

			seq_printf(" \tCVLAN\tsubact %s mcidx %d\n",
				   subaction_str, action->cvlan.mcidx);
		}
	}
}

static int rtl8365mb_debugfs_acl_rules_show(struct seq_file *file, void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_acl_rule rule = { 0 };
	int ret;
	int i;

	seq_printf(file,
		   "index\tenabled\tnegate\ttmpl\twhat\tportmsk\tfields\n");

	for (i = 0; i < RTL8365MB_NUM_ACL_CONFIGS; i++) {
		ret = rtl8365mb_acl_get_rule(priv, i, &rule);
		if (ret)
			return ret;

		if (!rule->enabled)
			continue;

		seq_printf(file, "%d\t%u\t%u\t%u\n", i, rule.enabled,
			   rule.negate, rule.template);
		seq_printf(file,
			   " \t \t \t \tcare\t%04x\t"
			   "%04x %04x %04x %04x %04x %04x %04x %04x\n",
			   rule.care.portmask, rule.care.fields[0],
			   rule.care.fields[1], rule.care.fields[2],
			   rule.care.fields[3], rule.care.fields[4],
			   rule.care.fields[5], rule.care.fields[6],
			   rule.care.fields[7]);
		seq_printf(file,
			   " \t \t \t \tdata\t%04x\t"
			   "%04x %04x %04x %04x %04x %04x %04x %04x\n",
			   rule.data.portmask, rule.data.fields[0],
			   rule.data.fields[1], rule.data.fields[2],
			   rule.data.fields[3], rule.data.fields[4],
			   rule.data.fields[5], rule.data.fields[6],
			   rule.data.fields[7]);
	}

	return 0;
}

static int rtl8365mb_debugfs_vlan_vlan4k_show(struct seq_file *file, void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_vlan4k vlan4k;
	int vid;
	int ret;

	seq_printf(file, "VID\tmember\tuntag\tfid\tIVL\n");

	for (vid = 0; vid < 4096; vid++) {
		ret = rtl8365mb_vlan_get_vlan4k(priv, vid, &vlan4k);
		if (ret)
			return ret;

		if (!vlan4k.member)
			continue;

		seq_printf(file, "%u\t%04x\t%04x\t%u\t%u\n", vlan4k.vid,
			   vlan4k.member, vlan4k.untag, vlan4k.fid,
			   vlan4k.ivl_en);
	}

	return 0;
}

static int rtl8365mb_debugfs_vlan_vlanmc_show(struct seq_file *file,
					      void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_vlanmc vlanmc;
	int i;
	int ret;

	seq_printf(file, "index\tEVID\tmember\tfid\n");

	for (i = 0; i < 32; i++) {
		ret = rtl8365mb_vlan_get_vlanmc(priv, i, &vlanmc);
		if (ret)
			return ret;

		seq_printf(file, "%u\t%u\t%04x\t%u\n", i, vlanmc.evid,
			   vlanmc.member, vlanmc.fid);
	}

	return 0;
}

static int rtl8365mb_debugfs_l2_uc_by_next_addr_show(struct seq_file *file,
						     void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_l2_uc uc = { 0 };
	int addr = user_addr;
	int ret;

	seq_printf(file, "addr\tMAC addr\t\tEFID\tIVL\tVID\tFID\t"
		   "port\tage\tstatic\n");

	ret = rtl8365mb_l2_get_next_uc(priv, &addr, &uc);
	seq_printf(file,
		   "%d\t%pM\t%u\t%d\t%u\t%u\t"
		   "%u\t%u\t%u\n",
		   addr, uc.key.mac_addr, uc.key.efid,
		   uc.key.ivl, uc.key.vid, uc.key.fid, uc.port, uc.age,
		   uc.is_static);

	seq_printf(file, "ret = %d\n", ret);

	return 0;
}

static int rtl8365mb_debugfs_l2_uc_by_addr_show(struct seq_file *file, void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_l2_uc uc = { 0 };
	int addr = user_addr;
	int ret;

	seq_printf(file, "addr\tMAC addr\t\tEFID\tIVL\tVID\tFID\t"
		   "port\tage\tstatic\n");

	ret = rtl8365mb_l2_get_uc_by_addr(priv, addr, &uc);
	seq_printf(file,
		   "%d\t%pM\t%u\t%d\t%u\t%u\t"
		   "%u\t%u\t%u\n",
		   addr, uc.key.mac_addr, uc.key.efid,
		   uc.key.ivl, uc.key.vid, uc.key.fid, uc.port, uc.age,
		   uc.is_static);

	seq_printf(file, "ret = %d\n", ret);

	return 0;
}

static int rtl8365mb_debugfs_l2_uc_all_show(struct seq_file *file, void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_l2_uc uc = { 0 };
	int addr;
	int hit;
	int ret;

	seq_printf(file, "hit\taddr\tMAC addr\t\tEFID\tIVL\tVID\tFID\t"
		   "port\tage\tstatic\n");

	for (addr = 0; addr < 2112; addr++) {
	    ret = rtl8365mb_l2_get_uc_by_addr(priv, addr, &uc);
	    if (ret == -ENOENT)
		    hit = false;
	    else if (ret == -EINVAL)
		    continue; /* Assume multicast and skip */
	    else if (ret)
		    return ret;
	    else
		    hit = true;

	    seq_printf(file,
		       "%c\t%d\t%pM\t%u\t%d\t%u\t%u\t"
		       "%u\t%u\t%u\n",
		       hit ? '*' : ' ', addr, uc.key.mac_addr, uc.key.efid,
		       uc.key.ivl, uc.key.vid, uc.key.fid, uc.port, uc.age,
		       uc.is_static);
	}

	return 0;
}

static int rtl8365mb_debugfs_l2_uc_show(struct seq_file *file, void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_l2_uc uc;
	int first_addr;
	int count = 0;
	int addr = 0;
	int ret;

	seq_printf(file, "addr\tMAC addr\t\tEFID\tIVL\tVID\tFID\t"
		   "port\tage\tstatic\n");

	/* Walk the L2 unicast entries of the switch forwarding database */
	ret = rtl8365mb_l2_get_next_uc(priv, &addr, &uc);
	if (ret == -ENOENT)
		return 0; /* The database is empty - not an error */
	else if (ret)
		return ret;

	/* Mark where we started, so that we don't loop forever */
	first_addr = addr;

	do {
		seq_printf(file,
			   "%d\t%pM\t%u\t%d\t%u\t%u\t"
			   "%u\t%u\t%u\n",
			   addr, uc.key.mac_addr, uc.key.efid, uc.key.ivl,
			   uc.key.vid, uc.key.fid, uc.port, uc.age,
			   uc.is_static);

		addr++;
		ret = rtl8365mb_l2_get_next_uc(priv, &addr, &uc);
		if (ret)
			return ret;
	} while (addr > first_addr && count++ < RTL8365MB_LEARN_LIMIT_MAX);

	seq_printf(file, "%d entries\n", count);

	return 0;
}

static int rtl8365mb_debugfs_l2_mc_show(struct seq_file *file, void *offset)
{
	struct realtek_priv *priv = dev_get_drvdata(file->private);
	struct rtl8365mb_l2_mc mc;
	int first_addr;
	int count = 0;
	int addr = 0;
	int ret;

	seq_printf(file, "addr\tMAC addr\t\tIVL\tVID\t"
		   "member\tstatic\n");

	/* Walk the L2 unicast entries of the switch forwarding database */
	ret = rtl8365mb_l2_get_next_mc(priv, &addr, &mc);
	if (ret == -ENOENT)
		return 0; /* The database is empty - not an error */
	else if (ret)
		return ret;

	/* Mark where we started, so that we don't loop forever */
	first_addr = addr;

	do {
		seq_printf(file,
			   "%d\t%pM\t%d\t%u\t"
			   "0x%04x\t%d\n",
			   addr, mc.key.mac_addr, mc.key.ivl,
			   mc.key.vid, mc.member, mc.is_static);

		addr++;
		ret = rtl8365mb_l2_get_next_mc(priv, &addr, &mc);
		if (ret)
			return ret;
	} while (addr > first_addr && count++ < RTL8365MB_LEARN_LIMIT_MAX);

	seq_printf(file, "%d entries\n", count);

	return 0;
}

struct dentry *rtl8365mb_debugfs_create(struct realtek_priv *priv)
{
	struct dentry *dir = debugfs_create_dir(dev_name(priv->dev), NULL);

	// TODO make a root dir
	debugfs_create_devm_seqfile(priv->dev, "table", dir,
				    rtl8365mb_debugfs_table_show);

	debugfs_create_devm_seqfile(priv->dev, "acl_rules", dir,
				    rtl8365mb_debugfs_acl_rules_show);

	debugfs_create_devm_seqfile(priv->dev, "acl_actions", dir,
				    rtl8365mb_debugfs_acl_actions_show);

	debugfs_create_devm_seqfile(priv->dev, "vlan_vlan4k", dir,
				    rtl8365mb_debugfs_vlan_vlan4k_show);

	debugfs_create_devm_seqfile(priv->dev, "vlan_vlanmc", dir,
				    rtl8365mb_debugfs_vlan_vlanmc_show);

	debugfs_create_devm_seqfile(priv->dev, "l2_uc", dir,
				    rtl8365mb_debugfs_l2_uc_show);

	debugfs_create_u16("l2_uc_addr", 0644, dir, &user_addr);

	debugfs_create_devm_seqfile(priv->dev, "l2_uc_by_next_addr", dir,
				    rtl8365mb_debugfs_l2_uc_by_next_addr_show);

	debugfs_create_devm_seqfile(priv->dev, "l2_uc_by_addr", dir,
				    rtl8365mb_debugfs_l2_uc_by_addr_show);

	debugfs_create_devm_seqfile(priv->dev, "l2_uc_all", dir,
				    rtl8365mb_debugfs_l2_uc_all_show);

	debugfs_create_devm_seqfile(priv->dev, "l2_mc", dir,
				    rtl8365mb_debugfs_l2_mc_show);

	return dir;
}

void rtl8365mb_debugfs_remove(struct dentry *dir)
{
	debugfs_remove_recursive(dir);
}
