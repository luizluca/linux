/* SPDX-License-Identifier: GPL-2.0 */
/* debugfs interface interface for the rtl8365mb switch family
 *
 * Copyright (C) 2022-2023 Alvin Šipraga <alsi@bang-olufsen.dk>
 */

struct dentry;
struct realtek_priv;

struct dentry *rtl8365mb_debugfs_create(struct realtek_priv *priv);
void rtl8365mb_debugfs_remove(struct dentry *dir);
