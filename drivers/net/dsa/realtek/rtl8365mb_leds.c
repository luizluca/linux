// SPDX-License-Identifier: GPL-2.0
/* LED support for the Realtek RTL8365MB-VC switch
 *
 * Copyright (C) 2026 Luiz Angelo Daros de Luca <luizluca@gmail.com>
 *
 */

#include <linux/bitops.h>
#include <linux/regmap.h>
#include <linux/leds.h>
#include <linux/property.h>
#include <net/dsa.h>

#include "rtl83xx.h"
#include "rtl8365mb.h"
#include "rtl8365mb_reg.h"

#define RTL8365MB_LED_FORCE_BLINK	1
#define RTL8365MB_LED_FORCE_OFF		2
#define RTL8365MB_LED_FORCE_ON		3

static int rtl8365mb_led_set_group_mode(struct realtek_priv *priv, u8 led_group,
				       enum rtl8365mb_ledgroup_mode mode)
{
	int ret;

	if (led_group >= RTL8365MB_MAX_NUM_LED_GROUPS)
		return -EINVAL;

	if (mode >= __RTL8365MB_LEDGROUP_MODE_MAX)
		return -EINVAL;

	/* Ensure we are in LED configuration mode, not data mode */
	ret = regmap_update_bits(priv->map, RTL8365MB_REG_LED_CONFIGURATION,
				 RTL8365MB_LED_CONFIG_SEL_MASK, 0);
	if (ret)
		return ret;

	return regmap_update_bits(priv->map, RTL8365MB_REG_LED_CONFIGURATION,
				  0xF << (led_group * 4),
				  mode << (led_group * 4));
}

static int rtl8365mb_led_get_port(struct rtl8365mb_led *led)
{
	struct realtek_priv *priv = led->priv;
	u32 reg = RTL8365MB_REG_CPU_FORCE_LED_OFF(led->led_group, led->port_num);
	u32 shift = (led->port_num % 8) * 2;
	u32 val;
	int ret;

	ret = regmap_read(priv->map, reg, &val);
	if (ret)
		return ret;

	return (val >> shift) & 0x3;
}

static int rtl8365mb_led_set_port(struct rtl8365mb_led *led, bool enable)
{
	struct realtek_priv *priv = led->priv;
	u32 reg = RTL8365MB_REG_CPU_FORCE_LED_OFF(led->led_group, led->port_num);
	u32 shift = (led->port_num % 8) * 2;
	u32 val = enable ? RTL8365MB_LED_FORCE_ON : RTL8365MB_LED_FORCE_OFF;
	int ret;

	/* Change the LED group to manual controlled LEDs */
	ret = rtl8365mb_led_set_group_mode(priv, led->led_group,
					  RTL8365MB_LEDGROUP_FORCE);
	if (ret)
		return ret;

	return regmap_update_bits(priv->map, reg, 0x3 << shift, val << shift);
}

static int rtl8365mb_led_brightness_set_blocking(struct led_classdev *cdev,
						 enum led_brightness brightness)
{
	struct rtl8365mb_led *led = container_of(cdev, struct rtl8365mb_led, cdev);

	return rtl8365mb_led_set_port(led, brightness == LED_ON);
}

static int rtl8365mb_led_enable_parallel_io(struct realtek_priv *priv, int port,
					   int group)
{
	struct rtl8365mb *mb = priv->chip_data;
	u32 reg;
	u32 mask;

	if (port >= mb->chip_info->family->num_ports)
		return -EINVAL;

	if (port < 8) {
		reg = RTL8365MB_REG_PARA_LED_IO_EN(group);
		mask = RTL8365MB_PARA_LED_IO_EN_MASK(group, port);
	} else {
		reg = RTL8365MB_REG_PARA_LED_IO_EN3;
		if (mb->chip_info->family->family_id == RTL8365MB_FAMILY_C)
			mask = RTL8365MB_C_PARA_LED_IO_EN3_MASK(group, port);
		else
			mask = RTL8365MB_PARA_LED_IO_EN3_MASK(group, port);
	}

	return regmap_update_bits(priv->map, reg, mask, mask);
}

static int rtl8365mb_led_setup_one(struct realtek_priv *priv,
				   struct dsa_port *dp,
				   struct fwnode_handle *led_fwnode)
{
	struct rtl8365mb *mb = priv->chip_data;
	struct led_init_data init_data = { };
	enum led_default_state state;
	struct rtl8365mb_led *led;
	u32 led_group;
	int ret;

	ret = fwnode_property_read_u32(led_fwnode, "reg", &led_group);
	if (ret)
		return ret;

	if (led_group >= RTL8365MB_MAX_NUM_LED_GROUPS) {
		dev_warn(priv->dev, "Invalid LED reg %d defined for port %d\n",
			 led_group, dp->index);
		return -EINVAL;
	}

	ret = rtl8365mb_led_enable_parallel_io(priv, dp->index, led_group);
	if (ret)
		return ret;

	led = &mb->ports[dp->index].leds[led_group];
	led->port_num = dp->index;
	led->led_group = led_group;
	led->priv = priv;

	state = led_init_default_state_get(led_fwnode);
	switch (state) {
	case LEDS_DEFSTATE_ON:
		led->cdev.brightness = 1;
		rtl8365mb_led_set_port(led, 1);
		break;
	case LEDS_DEFSTATE_KEEP:
		ret = rtl8365mb_led_get_port(led);
		if (ret < 0)
			return ret;
		led->cdev.brightness = (ret == RTL8365MB_LED_FORCE_ON) ? 1 : 0;
		break;
	case LEDS_DEFSTATE_OFF:
	default:
		led->cdev.brightness = 0;
		rtl8365mb_led_set_port(led, 0);
	}

	led->cdev.max_brightness = 1;
	led->cdev.brightness_set_blocking = rtl8365mb_led_brightness_set_blocking;
	init_data.fwnode = led_fwnode;
	init_data.devname_mandatory = true;

	init_data.devicename = kasprintf(GFP_KERNEL, "Realtek-%d:%02d:%d",
					 dp->ds->index, dp->index, led_group);
	if (!init_data.devicename)
		return -ENOMEM;

	ret = devm_led_classdev_register_ext(priv->dev, &led->cdev, &init_data);
	kfree(init_data.devicename);
	if (ret) {
		dev_warn(priv->dev, "Failed to init LED %d for port %d\n",
			 led_group, dp->index);
		return ret;
	}

	return 0;
}

static int rtl8365mb_led_disable_all(struct realtek_priv *priv)
{
	int ret;
	int i;

	for (i = 0; i < RTL8365MB_MAX_NUM_LED_GROUPS; i++) {
		ret = rtl8365mb_led_set_group_mode(priv, i,
						  RTL8365MB_LEDGROUP_OFF);
		if (ret)
			return ret;
	}

	return 0;
}

int rtl8365mb_led_setup(struct realtek_priv *priv)
{
	struct dsa_switch *ds = &priv->ds;
	struct device_node *leds_np;
	struct dsa_port *dp;
	int ret = 0;

	if (priv->leds_disabled)
		return rtl8365mb_led_disable_all(priv);

	dsa_switch_for_each_port(dp, ds) {
		if (!dp->dn)
			continue;

		leds_np = of_get_child_by_name(dp->dn, "leds");
		if (!leds_np)
			continue;

		for_each_child_of_node_scoped(leds_np, led_np) {
			ret = rtl8365mb_led_setup_one(priv, dp,
						      of_fwnode_handle(led_np));
			if (ret)
				break;
		}

		of_node_put(leds_np);
		if (ret)
			return ret;
	}

	return 0;
}

void rtl8365mb_led_teardown(struct realtek_priv *priv)
{
	rtl8365mb_led_disable_all(priv);
}
