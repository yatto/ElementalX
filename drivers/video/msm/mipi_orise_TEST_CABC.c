/* Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "msm_fb.h"
#include "mipi_dsi.h"
#include "mipi_orise.h"
#include "mdp4.h"

#include "../board-8064.h"
#include <asm/mach-types.h>
#include <linux/pwm.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/gpio.h>
#include <linux/syscore_ops.h>

#define PWM_FREQ_HZ 20000
#define PWM_PERIOD_p1USEC (USEC_PER_SEC * 10 / PWM_FREQ_HZ)
#define PWM_LEVEL 255
#define PWM_DUTY_LEVEL \
	(PWM_PERIOD_p1USEC / PWM_LEVEL)

#define gpio_EN_VDD_BL PM8921_GPIO_PM_TO_SYS(23)
#define gpio_LCD_BL_EN PM8921_GPIO_PM_TO_SYS(30)
#define gpio_PWM PM8921_GPIO_PM_TO_SYS(26)

static struct mipi_dsi_panel_platform_data *mipi_orise_pdata;
extern struct pwm_device *bl_lpm;

static struct dsi_buf orise_tx_buf;
static struct dsi_buf orise_rx_buf;

static char enter_sleep[2] = {0x10, 0x00}; /* DTYPE_DCS_WRITE */
static char exit_sleep[2] = {0x11, 0x00}; /* DTYPE_DCS_WRITE */
static char display_off[2] = {0x28, 0x00}; /* DTYPE_DCS_WRITE */
static char display_on[2] = {0x29, 0x00}; /* DTYPE_DCS_WRITE */
static char write_cabc[] = {0x55, 0x03};


static unsigned int cabc_level = 0;
static unsigned int sre_level = 0;
static bool aco_enabled = false;
static struct dsi_cmd_desc orise_video_on_cmds[] = {
	{DTYPE_DCS_WRITE, 1, 0, 0, 10,
		sizeof(exit_sleep), exit_sleep},
	{DTYPE_DCS_WRITE, 1, 0, 0, 10,
		sizeof(display_on), display_on},
};

static struct dsi_cmd_desc orise_cmd_on_cmds[] = {
	{DTYPE_DCS_WRITE, 1, 0, 0, 10,
		sizeof(exit_sleep), exit_sleep},
	{DTYPE_DCS_WRITE, 1, 0, 0, 10,
		sizeof(display_on), display_on},
};

static struct dsi_cmd_desc orise_display_off_cmds[] = {
	{DTYPE_DCS_WRITE, 1, 0, 0, 10,
		sizeof(display_off), display_off},
	{DTYPE_DCS_WRITE, 1, 0, 0, 120,
		sizeof(enter_sleep), enter_sleep}
};
/*
static char bl_value[2] = {0x51, 0x0};
static struct dsi_cmd_desc backlight_cmd[] = {
	{DTYPE_DCS_WRITE1, 1, 0, 0, 0,
		sizeof(bl_value), bl_value}
};*/

static struct dsi_cmd_desc orise_cabc_cmd[] = {
	{DTYPE_DCS_WRITE1, 1, 0, 0, 0,
		sizeof(write_cabc), write_cabc}
};


static int mipi_orise_lcd_on(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;
	struct mipi_panel_info *mipi;
	struct msm_panel_info *pinfo;
	struct dcs_cmd_req cmdreq;

	mfd = platform_get_drvdata(pdev);
	if (!mfd)
		return -ENODEV;
	if (mfd->key != MFD_KEY)
		return -EINVAL;

	pinfo = &mfd->panel_info;
	mipi  = &mfd->panel_info.mipi;

	memset(&cmdreq, 0, sizeof(cmdreq));
	if (mipi->mode == DSI_VIDEO_MODE) {
		cmdreq.cmds = orise_video_on_cmds;
		cmdreq.cmds_cnt = ARRAY_SIZE(orise_video_on_cmds);
		cmdreq.flags = CMD_REQ_COMMIT;
		cmdreq.rlen = 0;
		cmdreq.cb = NULL;
		mipi_dsi_cmdlist_put(&cmdreq);
	} else {
		cmdreq.cmds = orise_cmd_on_cmds;
		cmdreq.cmds_cnt = ARRAY_SIZE(orise_cmd_on_cmds);
		cmdreq.flags = CMD_REQ_COMMIT;
		cmdreq.rlen = 0;
		cmdreq.cb = NULL;
		mipi_dsi_cmdlist_put(&cmdreq);

		mipi_dsi_cmd_bta_sw_trigger(); /* clean up ack_err_status */
	}

	return 0;
}

static int mipi_orise_lcd_off(struct platform_device *pdev)
{
	
	struct msm_fb_data_type *mfd;
	struct dcs_cmd_req cmdreq;

	mfd = platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;
	if (mfd->key != MFD_KEY)
		return -EINVAL;

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = orise_display_off_cmds;
	cmdreq.cmds_cnt = ARRAY_SIZE(orise_display_off_cmds);
	cmdreq.flags = CMD_REQ_COMMIT;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;
	mipi_dsi_cmdlist_put(&cmdreq);

	return 0;
}
static void orise_command_cabc(void)
{
	struct dcs_cmd_req cmdreq;
	write_cabc[1] = CABC_OFF;

	switch (cabc_level) {
	case 1:
		write_cabc[1] |= CABC_UI;
		break;
	case 2:
		write_cabc[1] |= CABC_IMAGE;
		break;
	case 3:
		write_cabc[1] |= CABC_VIDEO;
		break;
	default:
		break;
	}

	switch (sre_level) {
	case 1:
		write_cabc[1] |= SRE_WEAK;
		break;
	case 2:
		write_cabc[1] |= SRE_MEDIUM;
		break;
	case 3:
		write_cabc[1] |= SRE_STRONG;
		break;
	default:
		break;
	}

	if (aco_enabled)
		write_cabc[1] |= CABC_ACO;

	pr_debug("%s: cabc cmd %d\n", __func__, write_cabc[1]);

	/* mdp4_dsi_cmd_busy_wait: will turn on dsi clock also */
	mipi_dsi_mdp_busy_wait();

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = orise_cabc_cmd;
	cmdreq.cmds_cnt = ARRAY_SIZE(orise_cabc_cmd);
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;
	mipi_dsi_cmdlist_put(&cmdreq);
}

static void mipi_orise_set_cabc(struct platform_device *pdev, int level)
{
	cabc_level = level;
	orise_command_cabc();
}

static int mipi_orise_get_cabc(struct platform_device *pdev) {
	return cabc_level;
}

static void mipi_orise_set_sre(struct platform_device *pdev, int level)
{
	sre_level = level;
	orise_command_cabc();
}

static int mipi_orise_get_sre(struct platform_device *pdev) {
	return sre_level;
}

static void mipi_orise_set_aco(struct platform_device *pdev, bool enabled)
{
	aco_enabled = enabled;
	orise_command_cabc();
}

static int mipi_orise_get_aco(struct platform_device *pdev) {
	return aco_enabled;

	
}

static void mipi_orise_set_backlight(struct msm_fb_data_type *mfd)
{
	int ret;
	static int bl_enable_sleep_control_1 = 0; // msleep(10) only when suspend or resume
	static int bl_enable_sleep_control_2 = 0; // msleep(10) only when suspend or resume

	pr_debug("%s: back light level %d\n", __func__, mfd->bl_level);

	if (bl_lpm) {
		if (mfd->bl_level) {
			gpio_set_value_cansleep(gpio_EN_VDD_BL, 1);
			if(!bl_enable_sleep_control_1) {
				msleep(10);
				bl_enable_sleep_control_1 = 1;
				printk("%s: sleep 1 when resume\n", __func__);
			}
			ret = pwm_config_in_p1us_unit(bl_lpm, PWM_PERIOD_p1USEC *
				mfd->bl_level / PWM_LEVEL, PWM_PERIOD_p1USEC);
			if (ret) {
				pr_err("pwm_config on lpm failed %d\n", ret);
				return;
			}
			ret = pwm_enable(bl_lpm);
			if (ret)
				pr_err("pwm enable/disable on lpm failed"
				"for bl %d\n",	mfd->bl_level);
			if(!bl_enable_sleep_control_2) {
				msleep(10);
				bl_enable_sleep_control_2 = 1;
				printk("%s: sleep 2 when resume\n", __func__);
			}
			gpio_set_value_cansleep(gpio_LCD_BL_EN, 1);

		}
		else {
			gpio_set_value_cansleep(gpio_LCD_BL_EN, 0);
			if(bl_enable_sleep_control_1) {
				msleep(10);
				bl_enable_sleep_control_1 = 0;
				printk("%s: sleep 1 when suspend\n", __func__);
			}
			ret = pwm_config_in_p1us_unit(bl_lpm, PWM_PERIOD_p1USEC *
				mfd->bl_level / PWM_LEVEL, PWM_PERIOD_p1USEC);
			if (ret) {
				pr_err("pwm_config on lpm failed %d\n", ret);
				return;
			}
			pwm_disable(bl_lpm);
			if(bl_enable_sleep_control_2) {
				msleep(10);
				bl_enable_sleep_control_2 = 0;
				printk("%s: sleep 2 when suspend\n", __func__);
			}
			gpio_set_value_cansleep(gpio_EN_VDD_BL, 0);
		}
	}
}

static void mipi_orise_set_recovery_backlight(struct msm_fb_data_type *mfd)
{
	int ret;
	int recovery_backlight = 102;
	static int set_recovery_bl_done = 0;

	if (!set_recovery_bl_done) {
		if (mipi_orise_pdata->recovery_backlight)
			recovery_backlight = mipi_orise_pdata->recovery_backlight;

		pr_info("%s: backlight level %d\n", __func__, recovery_backlight);

		if (bl_lpm) {
			gpio_set_value_cansleep(gpio_EN_VDD_BL, 1);

			msleep(10);

			ret = pwm_config_in_p1us_unit(bl_lpm, PWM_PERIOD_p1USEC *
				recovery_backlight / PWM_LEVEL, PWM_PERIOD_p1USEC);
			if (ret) {
				pr_err("pwm_config on lpm failed %d\n", ret);
				return;
			}
			ret = pwm_enable(bl_lpm);
			if (ret) {
				pr_err("pwm enable/disable on lpm failed"
				"for bl %d\n",	mfd->bl_level);
				return;
			}

			msleep(10);

			gpio_set_value_cansleep(gpio_LCD_BL_EN, 1);

		}
		set_recovery_bl_done = 1;
	}
}

static void mipi_orise_lcd_shutdown(void)
{
	struct dcs_cmd_req cmdreq;

	int ret;

	pr_info("%s+\n", __func__);

	gpio_set_value_cansleep(gpio_LCD_BL_EN, 0);
	msleep_interruptible(10);
	if (bl_lpm) {
			ret = pwm_config(bl_lpm, 0, PWM_PERIOD_p1USEC);
			if (ret)
				pr_err("pwm_config failed %d\n", ret);
			pwm_disable(bl_lpm);
	}
	pr_info("%s, ORISE display shutdown off command+\n", __func__);
	memset(&cmdreq, 0, sizeof(cmdreq));	
	cmdreq.cmds = orise_display_off_cmds;
	cmdreq.cmds_cnt = ARRAY_SIZE(orise_display_off_cmds);
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;
	mipi_dsi_cmdlist_put(&cmdreq);
	pr_info("%s, ORISE display shutdown off command-\n", __func__);

	pr_info("%s: power gpio off\n", __func__);
	msleep(20);
	gpio_set_value_cansleep(gpio_EN_VDD_BL, 0);
	msleep(20);
	gpio_set_value_cansleep(gpio_LCD_BL_EN, 0);
	msleep_interruptible(8);

	pr_info("%s-\n", __func__);
}

struct syscore_ops panel_syscore_ops = {
	.shutdown = mipi_orise_lcd_shutdown,
};


static int __devinit mipi_orise_lcd_probe(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;
	struct mipi_panel_info *mipi;
	struct platform_device *current_pdev;
	static struct mipi_dsi_phy_ctrl *phy_settings;

	printk("%s+\n", __func__);

	if (pdev->id == 0) {
		mipi_orise_pdata = pdev->dev.platform_data;

		if (mipi_orise_pdata
			&& mipi_orise_pdata->phy_ctrl_settings) {
			phy_settings = (mipi_orise_pdata->phy_ctrl_settings);
		}

		return 0;
	}

/*
	// already requested in leds_pm8xxx.c, pm8xxx_led_probe()
	if (mipi_orise_pdata != NULL) {
		bl_lpm = pwm_request(mipi_orise_pdata->gpio[0],
			"backlight");
	}
*/

	if (bl_lpm == NULL || IS_ERR(bl_lpm)) {
		pr_err("%s pwm_request() failed\n", __func__);
		bl_lpm = NULL;
	}
	pr_debug("bl_lpm = %p lpm = %d\n", bl_lpm,
		mipi_orise_pdata->gpio[0]);

	current_pdev = msm_fb_add_device(pdev);
	register_syscore_ops(&panel_syscore_ops);
	if (current_pdev) {
		mfd = platform_get_drvdata(current_pdev);
		if (!mfd)
			return -ENODEV;
		if (mfd->key != MFD_KEY)
			return -EINVAL;

		mipi  = &mfd->panel_info.mipi;

		if (phy_settings != NULL)
			mipi->dsi_phy_db = phy_settings;
	}

	printk("%s-\n", __func__);
	return 0;
}

static struct platform_driver this_driver = {
	.probe  = mipi_orise_lcd_probe,
	.driver = {
		.name   = "mipi_orise",
	},
};

static struct msm_fb_panel_data orise_panel_data = {
	.on		= mipi_orise_lcd_on,
	.off		= mipi_orise_lcd_off,
	.set_backlight = mipi_orise_set_backlight,
	.set_recovery_backlight = mipi_orise_set_recovery_backlight,
	.set_cabc	= mipi_orise_set_cabc,
	.get_cabc	= mipi_orise_get_cabc,
	.set_sre	= mipi_orise_set_sre,
	.get_sre	= mipi_orise_get_sre,
	.set_aco	= mipi_orise_set_aco,
	.get_aco	= mipi_orise_get_aco,
};

static int ch_used[3];

int mipi_orise_device_register(struct msm_panel_info *pinfo,
					u32 channel, u32 panel)
{
	struct platform_device *pdev = NULL;
	int ret;

	if ((channel >= 3) || ch_used[channel])
		return -ENODEV;

	ch_used[channel] = TRUE;

	pdev = platform_device_alloc("mipi_orise", (panel << 8)|channel);
	if (!pdev)
		return -ENOMEM;

	orise_panel_data.panel_info = *pinfo;

	ret = platform_device_add_data(pdev, &orise_panel_data,
		sizeof(orise_panel_data));
	if (ret) {
		printk(KERN_ERR
		  "%s: platform_device_add_data failed!\n", __func__);
		goto err_device_put;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		printk(KERN_ERR
		  "%s: platform_device_register failed!\n", __func__);
		goto err_device_put;
	}

	return 0;

err_device_put:
	platform_device_put(pdev);
	return ret;
}

static int __init mipi_orise_lcd_init(void)
{
	mipi_dsi_buf_alloc(&orise_tx_buf, DSI_BUF_SIZE);
	mipi_dsi_buf_alloc(&orise_rx_buf, DSI_BUF_SIZE);

	return platform_driver_register(&this_driver);
}

module_init(mipi_orise_lcd_init);
