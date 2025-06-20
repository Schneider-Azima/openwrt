/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>


#include  "./rtl8367c/include/rtk_switch.h"
#include  "./rtl8367c/include/port.h"
#include  "./rtl8367c/include/vlan.h"
#include  "./rtl8367c/include/rtl8367c_asicdrv_port.h"
#include  "./rtl8367c/include/rtl8367c_asicdrv_mii_mgr.h"

struct rtk_gsw {
 	struct device           *dev;
 	struct mii_bus          *bus;
	int reset_pin;
};

static struct rtk_gsw *_gsw;

#define RTL8367S_PHY_LINK_CHECK_INTERVAL	(1000) /* ms */
#define PORT_MAX_NUM	5
static int rtl8368s_link_status[PORT_MAX_NUM] = {0};
static char rtl8367s_port_bind[PORT_MAX_NUM][IFNAMSIZ] = {0};

int split_string(const char *input, int max_token_num)
{
	int i = 0, j = 0, port = 0;
	if (!input || max_token_num < 1)
		return -EINVAL;

	while (input[i] != '\0' && port < max_token_num) {
		if (input[i] == ',') {
			port++;
			j = 0;
		} else if (j+1 == IFNAMSIZ) {
			pr_err("ifname too long\n");
			return -EINVAL;
		} else {
			rtl8367s_port_bind[port][j] = input[i];
			j++;
		}
		i++;
	}

	return 0;
}

int rtl8367s_phy_link_change_notify(void)
{
	rtk_api_ret_t ret;
	rtk_port_linkStatus_t link_status = 0;
	rtk_port_speed_t link_speed = 0;
	rtk_port_duplex_t link_duplex = 0;
	int speed = 0;
	int p;

	for (p = 0; p < PORT_MAX_NUM; p++) {
		struct net_device *port_dev = dev_get_by_name(&init_net, rtl8367s_port_bind[p]);
		ret = rtk_port_phyStatus_get(p, &link_status, &link_speed, &link_duplex);
		if (ret != RT_ERR_OK)
			pr_err("rtk_port_phyStatus_get failed...ret=%d\n", ret);

		if (port_dev && ((port_dev->flags & IFF_UP) ^ link_status)) {
			rtnl_lock();
			dev_change_flags(port_dev, port_dev->flags ^ IFF_UP, NULL);
			rtnl_unlock();
			dev_put(port_dev);
		}

		if (link_status != rtl8368s_link_status[p]) {
			rtl8368s_link_status[p] = link_status;

			switch (link_speed)
			{
			case PORT_SPEED_10M:
				speed = 10;
				break;
			case PORT_SPEED_100M:
				speed = 100;
				break;
			case PORT_SPEED_1000M:
				speed = 1000;
				break;
			case PORT_SPEED_500M:
				speed = 500;
				break;
			case PORT_SPEED_2500M:
				speed = 2500;
				break;
			default:
				pr_err("failed to get port%d link speed\n", p);
				return 0;
			}

			pr_info("UTP_PORT%d link %s speed %d\r\n", p, link_status ? "up" : "down", speed);
		}
	}
	return 0;
}

static void rtl8367s_phy_link_work_cb(struct work_struct *work)
{
	rtl8367s_phy_link_change_notify();
	schedule_delayed_work((struct delayed_work *)work,
						msecs_to_jiffies(RTL8367S_PHY_LINK_CHECK_INTERVAL));
}

static DECLARE_DELAYED_WORK(rtl8367s_phy_link_delayed_work, rtl8367s_phy_link_work_cb);

static void rtl8367s_phy_link_timer_start(void)
{
	schedule_delayed_work(&rtl8367s_phy_link_delayed_work,
						msecs_to_jiffies(RTL8367S_PHY_LINK_CHECK_INTERVAL));
}

/*mii_mgr_read/mii_mgr_write is the callback API for rtl8367 driver*/
unsigned int mii_mgr_read(unsigned int phy_addr,unsigned int phy_register,unsigned int *read_data)
{
	struct mii_bus *bus = _gsw->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	*read_data = bus->read(bus, phy_addr, phy_register);

	mutex_unlock(&bus->mdio_lock);

	return 0;
}

unsigned int mii_mgr_write(unsigned int phy_addr,unsigned int phy_register,unsigned int write_data)
{
	struct mii_bus *bus =  _gsw->bus;

	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	bus->write(bus, phy_addr, phy_register, write_data);

	mutex_unlock(&bus->mdio_lock);

	return 0;
}

static int rtl8367s_hw_reset(void)
{
	struct rtk_gsw *gsw = _gsw;

	if (gsw->reset_pin < 0)
		return 0;

	gpio_direction_output(gsw->reset_pin, 0);

	usleep_range(1000, 1100);

	gpio_set_value(gsw->reset_pin, 1);

	mdelay(500);

	return 0;
}

static int rtl8367s_vlan_config(int want_at_p0)
{
	rtk_vlan_cfg_t vlan1, vlan2;

	/* Set LAN/WAN VLAN partition */
	memset(&vlan1, 0x00, sizeof(rtk_vlan_cfg_t));

	RTK_PORTMASK_PORT_SET(vlan1.mbr, EXT_PORT0);
	RTK_PORTMASK_PORT_SET(vlan1.mbr, UTP_PORT1);
	RTK_PORTMASK_PORT_SET(vlan1.mbr, UTP_PORT2);
	RTK_PORTMASK_PORT_SET(vlan1.mbr, UTP_PORT3);
	RTK_PORTMASK_PORT_SET(vlan1.untag, EXT_PORT0);
	RTK_PORTMASK_PORT_SET(vlan1.untag, UTP_PORT1);
	RTK_PORTMASK_PORT_SET(vlan1.untag, UTP_PORT2);
	RTK_PORTMASK_PORT_SET(vlan1.untag, UTP_PORT3);

	 if (want_at_p0) {
		RTK_PORTMASK_PORT_SET(vlan1.mbr, UTP_PORT4);
		RTK_PORTMASK_PORT_SET(vlan1.untag, UTP_PORT4);
        } else {
		RTK_PORTMASK_PORT_SET(vlan1.mbr, UTP_PORT0);
		RTK_PORTMASK_PORT_SET(vlan1.untag, UTP_PORT0);
        }

	vlan1.ivl_en = 1;

	rtk_vlan_set(1, &vlan1);

	memset(&vlan2, 0x00, sizeof(rtk_vlan_cfg_t));

	RTK_PORTMASK_PORT_SET(vlan2.mbr, EXT_PORT1);
	RTK_PORTMASK_PORT_SET(vlan2.untag, EXT_PORT1);

	if (want_at_p0) {
		RTK_PORTMASK_PORT_SET(vlan2.mbr, UTP_PORT0);
		RTK_PORTMASK_PORT_SET(vlan2.untag, UTP_PORT0);
	} else {
		RTK_PORTMASK_PORT_SET(vlan2.mbr, UTP_PORT4);
		RTK_PORTMASK_PORT_SET(vlan2.untag, UTP_PORT4);
	}

	vlan2.ivl_en = 1;
	rtk_vlan_set(2, &vlan2);

	rtk_vlan_portPvid_set(EXT_PORT0, 1, 0);
	rtk_vlan_portPvid_set(UTP_PORT1, 1, 0);
	rtk_vlan_portPvid_set(UTP_PORT2, 1, 0);
	rtk_vlan_portPvid_set(UTP_PORT3, 1, 0);
	rtk_vlan_portPvid_set(EXT_PORT1, 2, 0);

	if (want_at_p0) {
		rtk_vlan_portPvid_set(UTP_PORT0, 2, 0);
		rtk_vlan_portPvid_set(UTP_PORT4, 1, 0);
	} else {
		rtk_vlan_portPvid_set(UTP_PORT0, 1, 0);
		rtk_vlan_portPvid_set(UTP_PORT4, 2, 0);
	}

	return 0;
}

static int rtl8367s_hw_init(void)
{

	rtl8367s_hw_reset();

	if(rtk_switch_init())
	        return -1;

	mdelay(500);

	if (rtk_vlan_reset())
	        return -1;

	if (rtk_vlan_init())
	        return -1;

	return 0;
}

static void set_rtl8367s_sgmii(void)
{
	rtk_port_mac_ability_t mac_cfg;
	rtk_mode_ext_t mode;

	mode = MODE_EXT_HSGMII;
	mac_cfg.forcemode = MAC_FORCE;
	mac_cfg.speed = PORT_SPEED_2500M;
	mac_cfg.duplex = PORT_FULL_DUPLEX;
	mac_cfg.link = PORT_LINKUP;
	mac_cfg.nway = DISABLED;
	mac_cfg.txpause = ENABLED;
	mac_cfg.rxpause = ENABLED;
	rtk_port_macForceLinkExt_set(EXT_PORT0, mode, &mac_cfg);
	rtk_port_sgmiiNway_set(EXT_PORT0, DISABLED);
	rtk_port_phyEnableAll_set(ENABLED);

}

static void set_rtl8367s_rgmii(void)
{
	rtk_port_mac_ability_t mac_cfg;
	rtk_mode_ext_t mode;

	mode = MODE_EXT_RGMII;
	mac_cfg.forcemode = MAC_FORCE;
	mac_cfg.speed = PORT_SPEED_1000M;
	mac_cfg.duplex = PORT_FULL_DUPLEX;
	mac_cfg.link = PORT_LINKUP;
	mac_cfg.nway = DISABLED;
	mac_cfg.txpause = ENABLED;
	mac_cfg.rxpause = ENABLED;
	rtk_port_macForceLinkExt_set(EXT_PORT1, mode, &mac_cfg);
	rtk_port_rgmiiDelayExt_set(EXT_PORT1, 1, 3);
	rtk_port_phyEnableAll_set(ENABLED);

}

static void init_gsw(void)
{
	rtl8367s_hw_init();
	set_rtl8367s_sgmii();
	set_rtl8367s_rgmii();
}

// below are platform driver
static const struct of_device_id rtk_gsw_match[] = {
	{ .compatible = "mediatek,rtk-gsw" },
	{},
};

MODULE_DEVICE_TABLE(of, rtk_gsw_match);

static int rtk_gsw_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *mdio;
	struct mii_bus *mdio_bus;
	struct rtk_gsw *gsw;
	const char *pm;
	int ret;

	mdio = of_parse_phandle(np, "mediatek,mdio", 0);

	if (!mdio)
		return -EINVAL;

	mdio_bus = of_mdio_find_bus(mdio);

	if (!mdio_bus)
		return -EPROBE_DEFER;

	gsw = devm_kzalloc(&pdev->dev, sizeof(struct rtk_gsw), GFP_KERNEL);

	if (!gsw)
		return -ENOMEM;

	gsw->dev = &pdev->dev;

	gsw->bus = mdio_bus;

	gsw->reset_pin = of_get_named_gpio(np, "mediatek,reset-pin", 0);
	if (gsw->reset_pin >= 0) {
		ret = devm_gpio_request(gsw->dev, gsw->reset_pin, "mediatek,reset-pin");
		if (ret)
			printk("fail to devm_gpio_request\n");
	}

	_gsw = gsw;

	init_gsw();

	//init default vlan or init swconfig
	if(!of_property_read_string(pdev->dev.of_node,
						"mediatek,port_map", &pm)) {

		if (!strcasecmp(pm, "wllll"))
			rtl8367s_vlan_config(1);
		else
			rtl8367s_vlan_config(0);

		} else {
#ifdef CONFIG_SWCONFIG
		rtl8367s_swconfig_init(&init_gsw);
#else
		rtl8367s_vlan_config(0);
#endif
	}

	gsw_debug_proc_init();

	of_property_read_string(pdev->dev.of_node, "portbind", &pm);
	if (pm && !split_string(pm, 5))
		rtl8367s_phy_link_timer_start();

	platform_set_drvdata(pdev, gsw);

	return 0;

}

static void rtk_gsw_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);
	gsw_debug_proc_exit();
}

static struct platform_driver gsw_driver = {
	.probe = rtk_gsw_probe,
	.remove_new = rtk_gsw_remove,
	.driver = {
		.name = "rtk-gsw",
		.of_match_table = rtk_gsw_match,
	},
};

module_platform_driver(gsw_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Lee <marklee0201@gmail.com>");
MODULE_DESCRIPTION("rtl8367c switch driver for MT7622");

