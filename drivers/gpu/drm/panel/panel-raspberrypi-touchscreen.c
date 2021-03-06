/*
 * Copyright © 2016 Broadcom Limited
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Portions of this file (derived from panel-simple.c) are:
 *
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * DOC: Raspberry Pi 7" touchscreen panel driver.
 *
 * The 7" touchscreen consists of a DPI LCD panel, a Toshiba
 * TC358762XBG DSI-DPI bridge, and an I2C-connected Atmel ATTINY88-MUR
 * controlling power management, the LCD PWM, and the touchscreen.
 *
 * This driver presents this device as a MIPI DSI panel to the DRM
 * driver, and should expose the touchscreen as a HID device.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/pm.h>

#include <drm/drm_panel.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

struct rpi_touchscreen {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;
	struct i2c_client *bridge_i2c;
	struct backlight_device *backlight;

	bool prepared;
	bool enabled;

	/* Version of the firmware on the bridge chip */
	int atmel_ver;
};

static const struct drm_display_mode rpi_touchscreen_modes[] = {
	{
		/* This is assuming that we'll be running the DSI PLL
		 * at 2Ghz / 3 (since we only get integer dividers),
		 * so a pixel clock of 2Ghz / 3 / 8.
		 */
		.clock = 83333,
		.hdisplay = 800,
		.hsync_start = 800 + 61,
		.hsync_end = 800 + 61 + 2,
		.htotal = 800 + 61 + 2 + 44,
		.vdisplay = 480,
		.vsync_start = 480 + 7,
		.vsync_end = 480 + 7 + 2,
		.vtotal = 480 + 7 + 2 + 21,
		.vrefresh = 60,
	},
};

static struct rpi_touchscreen *panel_to_ts(struct drm_panel *panel)
{
	return container_of(panel, struct rpi_touchscreen, base);
}

struct regdump {
	const char *reg;
	u32 offset;
};

#define REGDUMP(reg) { #reg, reg }

static int rpi_touchscreen_disable(struct drm_panel *panel)
{
	struct rpi_touchscreen *ts = panel_to_ts(panel);
	pr_err("disable\n");

	if (!ts->enabled)
		return 0;

	if (ts->backlight) {
		ts->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ts->backlight);
	}

	ts->enabled = false;

	return 0;
}

static int rpi_touchscreen_unprepare(struct drm_panel *panel)
{
	struct rpi_touchscreen *ts = panel_to_ts(panel);

	if (!ts->prepared)
		return 0;

	ts->prepared = false;

	return 0;
}

static int rpi_touchscreen_prepare(struct drm_panel *panel)
{
	struct rpi_touchscreen *ts = panel_to_ts(panel);

	if (ts->prepared)
		return 0;

	ts->prepared = true;

	return 0;
}

/*
 * Powers on the panel once the DSI link is up.
 *
 * The TC358762 is run in PLLOFF mode, where it usees the MIPI DSI
 * byte clock instead of an external reference clock.  This means that
 * we need the DSI host to be on and transmitting before we start
 * talking to it.
 */
static int rpi_touchscreen_enable(struct drm_panel *panel)
{
	struct rpi_touchscreen *ts = panel_to_ts(panel);

	if (ts->enabled)
		return 0;

	if (ts->backlight) {
		ts->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ts->backlight);
	}

	ts->enabled = true;

	return 0;
}

static int rpi_touchscreen_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct drm_device *drm = panel->drm;
	unsigned int i, num = 0;

	for (i = 0; i < ARRAY_SIZE(rpi_touchscreen_modes); i++) {
		const struct drm_display_mode *m = &rpi_touchscreen_modes[i];
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(drm, m);
		if (!mode) {
			dev_err(drm->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, m->vrefresh);
			continue;
		}

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (i == 0)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	connector->display_info.bpc = 8;
	connector->display_info.width_mm = 217; /* XXX */
	connector->display_info.height_mm = 136; /* XXX */

	return num;
}

static int rpi_touchscreen_backlight_update(struct backlight_device *bl)
{
	int brightness = bl->props.brightness;

	if (bl->props.power != FB_BLANK_UNBLANK ||
	    bl->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;

	return 0;
}

static const struct backlight_ops rpi_touchscreen_backlight_ops = {
	.update_status	= rpi_touchscreen_backlight_update,
};

static const struct drm_panel_funcs rpi_touchscreen_funcs = {
	.disable = rpi_touchscreen_disable,
	.unprepare = rpi_touchscreen_unprepare,
	.prepare = rpi_touchscreen_prepare,
	.enable = rpi_touchscreen_enable,
	.get_modes = rpi_touchscreen_get_modes,
};

static struct i2c_client *rpi_touchscreen_get_i2c(struct device *dev,
						  const char *name)
{
	struct device_node *node;
	struct i2c_client *client;

	node = of_parse_phandle(dev->of_node, name, 0);
	if (!node)
		return ERR_PTR(-ENODEV);

	client = of_find_i2c_device_by_node(node);

	of_node_put(node);

	return client;
}

static int rpi_touchscreen_dsi_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rpi_touchscreen *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	dev_set_drvdata(dev, ts);

	ts->dsi = dsi;
	dsi->mode_flags = (MIPI_DSI_MODE_VIDEO |
			   MIPI_DSI_MODE_VIDEO_SYNC_PULSE);
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->lanes = 1;

	ts->bridge_i2c =
		rpi_touchscreen_get_i2c(dev, "raspberrypi,touchscreen-bridge");
	if (!ts->bridge_i2c) {
		ret = -EPROBE_DEFER;
		return ret;
	}

#if 0
	ts->backlight =
		devm_backlight_device_register(dev,
					       "raspberrypi-touchscreen-backlight",
					       dev, ts,
					       &rpi_touchscreen_backlight_ops,
					       NULL);
	if (IS_ERR(ts->backlight)) {
		DRM_ERROR("failed to register backlight\n");
		return PTR_ERR(ts->backlight);
	}
	ts->backlight->props.max_brightness = RPI_TOUCHSCREEN_MAX_BRIGHTNESS;
	ts->backlight->props.brightness = RPI_TOUCHSCREEN_MAX_BRIGHTNESS;
#endif

	drm_panel_init(&ts->base);
	ts->base.dev = dev;
	ts->base.funcs = &rpi_touchscreen_funcs;

	ret = drm_panel_add(&ts->base);
	if (ret < 0)
		goto err_release_bridge;

	return mipi_dsi_attach(dsi);

err_release_bridge:
	put_device(&ts->bridge_i2c->dev);
	return ret;
}

static int rpi_touchscreen_dsi_remove(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct rpi_touchscreen *ts = dev_get_drvdata(dev);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0) {
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);
		return ret;
	}

	drm_panel_detach(&ts->base);
	drm_panel_remove(&ts->base);

	put_device(&ts->bridge_i2c->dev);

	return 0;
}

static void rpi_touchscreen_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	/* XXX: poweroff */
}

static const struct of_device_id rpi_touchscreen_of_match[] = {
	{ .compatible = "raspberrypi,touchscreen" },
	{ } /* sentinel */
};
MODULE_DEVICE_TABLE(of, rpi_touchscreen_of_match);

static struct mipi_dsi_driver rpi_touchscreen_driver = {
	.driver = {
		.name = "raspberrypi-touchscreen",
		.of_match_table = rpi_touchscreen_of_match,
	},
	.probe = rpi_touchscreen_dsi_probe,
	.remove = rpi_touchscreen_dsi_remove,
	.shutdown = rpi_touchscreen_dsi_shutdown,
};
module_mipi_dsi_driver(rpi_touchscreen_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi 7-inch touchscreen driver");
MODULE_LICENSE("GPL v2");
