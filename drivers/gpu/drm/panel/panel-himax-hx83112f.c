// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree.
 * Copyright (c) 2026 Saalim Quadri <danascape@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct hx83112f_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data *supplies;
	struct gpio_desc *reset_gpio;
};

static const struct regulator_bulk_data hx83112f_supplies[] = {
	{ .supply = "vddio" },
	{ .supply = "vsp" },
	{ .supply = "vsn" },
};

static inline struct hx83112f_panel *to_hx83112f_panel(struct drm_panel *panel)
{
	return container_of_const(panel, struct hx83112f_panel, panel);
}

static void hx83112f_reset(struct hx83112f_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
}

static int hx83112f_on(struct hx83112f_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x83, 0x11, 0x2f);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc9, 0x00, 0x1e, 0x0f, 0xe2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb7, 0x00, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 60);
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 0x0fff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY,
				     0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbf, 0xc0, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe4,
				     0x8f, 0x80, 0xaa, 0xa5, 0xab, 0xb0, 0xc8,
				     0xde, 0xff, 0xff, 0xff, 0x03, 0x00, 0x24,
				     0x28, 0x32, 0x47, 0x50, 0x8d, 0xb6, 0xfc,
				     0xff, 0xff, 0x03);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);

	return dsi_ctx.accum_err;
}

static int hx83112f_off(struct hx83112f_panel *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 32);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 64);

	return dsi_ctx.accum_err;
}

static int hx83112f_prepare(struct drm_panel *panel)
{
	struct hx83112f_panel *ctx = to_hx83112f_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(hx83112f_supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	hx83112f_reset(ctx);

	ret = hx83112f_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(hx83112f_supplies), ctx->supplies);
		return ret;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		return ret;
	}

	msleep(28); /* TODO: Is this panel-dependent? */

	return 0;
}

static int hx83112f_unprepare(struct drm_panel *panel)
{
	struct hx83112f_panel *ctx = to_hx83112f_panel(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = hx83112f_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(hx83112f_supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode hx83112f_mode = {
	.clock = (1080 + 52 + 29 + 70) * (2400 + 1300 + 15 + 15) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 52,
	.hsync_end = 1080 + 52 + 29,
	.htotal = 1080 + 52 + 29 + 70,
	.vdisplay = 2400,
	.vsync_start = 2400 + 1300,
	.vsync_end = 2400 + 1300 + 15,
	.vtotal = 2400 + 1300 + 15 + 15,
	.width_mm = 68,
	.height_mm = 150,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int hx83112f_get_modes(struct drm_panel *panel,
				    struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &hx83112f_mode);
}

static const struct drm_panel_funcs hx83112f_panel_funcs = {
	.prepare = hx83112f_prepare,
	.unprepare = hx83112f_unprepare,
	.get_modes = hx83112f_get_modes,
};

static int hx83112f_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct hx83112f_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct hx83112f_panel, panel,
				   &hx83112f_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev,
					    ARRAY_SIZE(hx83112f_supplies),
					    hx83112f_supplies,
					    &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 1 */
	ctx->dsc.slice_height = 20;
	ctx->dsc.slice_width = 540;
	/*
	 * TODO: hdisplay should be read from the selected mode once
	 * it is passed back to drm_panel (in prepare?)
	 */
	WARN_ON(1080 % ctx->dsc.slice_width);
	ctx->dsc.slice_count = 1080 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 8;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void hx83112f_remove(struct mipi_dsi_device *dsi)
{
	struct hx83112f_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id hx83112f_of_match[] = {
	{ .compatible = "tianma,hx83112f-fhd" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hx83112f_of_match);

static struct mipi_dsi_driver hx83112f_driver = {
	.probe = hx83112f_probe,
	.remove = hx83112f_remove,
	.driver = {
		.name = "panel-hx83112f-tianma",
		.of_match_table = hx83112f_of_match,
	},
};
module_mipi_dsi_driver(hx83112f_driver);

MODULE_DESCRIPTION("DRM driver for hx83112_fhd_video_tianma");
MODULE_LICENSE("GPL");
