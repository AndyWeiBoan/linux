// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright (C) The Asahi Linux Contributors */

#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_modeset_lock.h>

#include <linux/backlight.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include "linux/jiffies.h"

#include "dcp.h"
#include "dcp-internal.h"

#define MIN_BRIGHTNESS_PART1	2U
#define MAX_BRIGHTNESS_PART1	99U
#define MIN_BRIGHTNESS_PART2	103U
#define MAX_BRIGHTNESS_PART2	510U

/*
 * lookup for display brightness 2 to 99 nits
 * */
static u32 brightness_part1[] = {
	0x0000000, 0x0810038, 0x0f000bd, 0x143011c,
	0x1850165, 0x1bc01a1, 0x1eb01d4, 0x2140200,
	0x2380227, 0x2590249, 0x2770269, 0x2930285,
	0x2ac02a0, 0x2c402b8, 0x2d902cf, 0x2ee02e4,
	0x30102f8, 0x314030b, 0x325031c, 0x335032d,
	0x345033d, 0x354034d, 0x362035b, 0x3700369,
	0x37d0377, 0x38a0384, 0x3960390, 0x3a2039c,
	0x3ad03a7, 0x3b803b3, 0x3c303bd, 0x3cd03c8,
	0x3d703d2, 0x3e103dc, 0x3ea03e5, 0x3f303ef,
	0x3fc03f8, 0x4050400, 0x40d0409, 0x4150411,
	0x41d0419, 0x4250421, 0x42d0429, 0x4340431,
	0x43c0438, 0x443043f, 0x44a0446, 0x451044d,
	0x4570454, 0x45e045b, 0x4640461, 0x46b0468,
	0x471046e, 0x4770474, 0x47d047a, 0x4830480,
	0x4890486, 0x48e048b, 0x4940491, 0x4990497,
	0x49f049c, 0x4a404a1, 0x4a904a7, 0x4ae04ac,
	0x4b304b1, 0x4b804b6, 0x4bd04bb, 0x4c204c0,
	0x4c704c5, 0x4cc04c9, 0x4d004ce, 0x4d504d3,
	0x4d904d7, 0x4de04dc, 0x4e204e0, 0x4e704e4,
	0x4eb04e9, 0x4ef04ed, 0x4f304f1, 0x4f704f5,
	0x4fb04f9, 0x4ff04fd, 0x5030501, 0x5070505,
	0x50b0509, 0x50f050d, 0x5130511, 0x5160515,
	0x51a0518, 0x51e051c, 0x5210520, 0x5250523,
	0x5290527, 0x52c052a, 0x52f052e, 0x5330531,
	0x5360535, 0x53a0538, 0x53d053b, 0x540053f,
	0x5440542, 0x5470545, 0x54a0548, 0x54d054c,
	0x550054f, 0x5530552, 0x5560555, 0x5590558,
	0x55c055b, 0x55f055e, 0x5620561, 0x5650564,
	0x5680567, 0x56b056a, 0x56e056d, 0x571056f,
	0x5740572, 0x5760575, 0x5790578, 0x57c057b,
	0x57f057d, 0x5810580, 0x5840583, 0x5870585,
	0x5890588, 0x58c058b, 0x58f058d
};

static u32 brightness_part12[] = { 0x58f058d, 0x59d058f };

/*
 * lookup table for display brightness 103.3 to 510 nits
 * */
static u32 brightness_part2[] = {
	0x59d058f, 0x5b805ab, 0x5d105c5, 0x5e805dd,
	0x5fe05f3, 0x6120608, 0x625061c, 0x637062e,
	0x6480640, 0x6580650, 0x6680660, 0x677066f,
	0x685067e, 0x693068c, 0x6a00699, 0x6ac06a6,
	0x6b806b2, 0x6c406be, 0x6cf06ca, 0x6da06d5,
	0x6e506df, 0x6ef06ea, 0x6f906f4, 0x70206fe,
	0x70c0707, 0x7150710, 0x71e0719, 0x7260722,
	0x72f072a, 0x7370733, 0x73f073b, 0x7470743,
	0x74e074a, 0x7560752, 0x75d0759, 0x7640760,
	0x76b0768, 0x772076e, 0x7780775, 0x77f077c,
	0x7850782, 0x78c0789, 0x792078f, 0x7980795,
	0x79e079b, 0x7a407a1, 0x7aa07a7, 0x7af07ac,
	0x7b507b2, 0x7ba07b8, 0x7c007bd, 0x7c507c2,
	0x7ca07c8, 0x7cf07cd, 0x7d407d2, 0x7d907d7,
	0x7de07dc, 0x7e307e1, 0x7e807e5, 0x7ec07ea,
	0x7f107ef, 0x7f607f3, 0x7fa07f8, 0x7fe07fc
};


static int dcp_get_brightness(struct backlight_device *bd)
{
	struct apple_dcp *dcp = bl_get_data(bd);

	return dcp->brightness.nits;
}

#define SCALE_FACTOR (1 << 10)

#define EXT_MIN_NITS	4
#define EXT_MAX_NITS	600

static u32 interpolate_u16(int val, int min, int max, const u16 *tbl,
			   size_t tbl_size)
{
	u32 frac;
	u64 low, high;
	u32 interpolated = (tbl_size - 1) * ((val - min) * SCALE_FACTOR) / (max - min);
	size_t index = interpolated / SCALE_FACTOR;

	if (index + 1 >= tbl_size)
		return tbl[tbl_size - 1];

	frac = interpolated & (SCALE_FACTOR - 1);
	low = tbl[index];
	high = tbl[index + 1];

	return ((frac * high) + ((SCALE_FACTOR - frac) * low)) / SCALE_FACTOR;
}

static u32 interpolate(int val, int min, int max, u32 *tbl, size_t tbl_size)
{
	u32 frac;
	u64 low, high;
	u32 interpolated = (tbl_size - 1) * ((val - min) * SCALE_FACTOR) / (max - min);

	size_t index = interpolated / SCALE_FACTOR;

	if (WARN(index + 1 >= tbl_size, "invalid index %zu for brightness %u\n", index, val))
		return tbl[tbl_size / 2];

	frac = interpolated & (SCALE_FACTOR - 1);
	low = tbl[index];
	high = tbl[index + 1];

	return ((frac * high) + ((SCALE_FACTOR - frac) * low)) / SCALE_FACTOR;
}

unsigned int ext_bl_max_nits = 600;
module_param(ext_bl_max_nits, uint, 0644);
/*
 * External Apple DisplayPort panels (Studio Display) take the same IOMFB swap
 * field as the built-in panel: a 16 bit PWM-ish code in the upper half, not
 * nits.  The built-in panel's own table pins the shape of that curve --
 * 2 nits is 0x1000 and 510 nits is 0x7fe0, i.e. nits scales as code^(8/3).
 *
 * This table is that curve stretched to the Studio Display's 4..600 nits,
 * 65 entries evenly spaced in nits.  Verified on hardware: entry ~18
 * (0x4800) reads as a comfortable dark-room level and ~0x6c00 as a normal
 * daytime level, matching the 134 and 386 nits the curve predicts.
 */
static const u16 ext_brightness_code[] = {
	0x1388, 0x1ea9, 0x2568, 0x2a92, 0x2edb, 0x3293, 0x35e2, 0x38e2,
	0x3ba4, 0x3e34, 0x4099, 0x42db, 0x44fe, 0x4706, 0x48f6, 0x4ad2,
	0x4c9a, 0x4e51, 0x4ff9, 0x5193, 0x5320, 0x54a0, 0x5616, 0x5782,
	0x58e4, 0x5a3c, 0x5b8d, 0x5cd6, 0x5e17, 0x5f51, 0x6085, 0x61b2,
	0x62da, 0x63fc, 0x6518, 0x6630, 0x6742, 0x6850, 0x6959, 0x6a5e,
	0x6b5f, 0x6c5c, 0x6d55, 0x6e4b, 0x6f3d, 0x702c, 0x7117, 0x71ff,
	0x72e5, 0x73c7, 0x74a6, 0x7583, 0x765d, 0x7735, 0x780a, 0x78dc,
	0x79ac, 0x7a7a, 0x7b46, 0x7c0f, 0x7cd7, 0x7d9c, 0x7e5f, 0x7f21,
	0x7fe0,
};

unsigned int ext_bl_raw_max;
module_param(ext_bl_raw_max, uint, 0444);
MODULE_PARM_DESC(ext_bl_raw_max,
		 "if set, external panel brightness is the raw IOMFB bl_value "
		 "and this is its maximum (calibration mode)");
bool ext_bl_service = false;
module_param(ext_bl_service, bool, 0444);
MODULE_PARM_DESC(ext_bl_service,
		 "tell DCP to create a backlight service for external panels "
		 "(set at module load, e.g. appledrm.ext_bl_service=1)");
bool ext_bl_force_register = true;
module_param(ext_bl_force_register, bool, 0644);
MODULE_PARM_DESC(ext_bl_force_register,
		 "register a backlight for external DCP panels on hotplug");
MODULE_PARM_DESC(ext_bl_max_nits,
		 "max brightness in nits reported for external DCP panels");

static u32 calculate_dac(struct apple_dcp *dcp, int val)
{
	u32 dac;

	/*
	 * macOS reports an external panel's brightness as nits in 16.16 fixed
	 * point (IOMFBBrightnessLevel = nits * 65536), but that is *not* what
	 * the swap's bl_value takes -- feeding it nits << 16 lands an order of
	 * magnitude below the panel's minimum and the screen stays black.
	 * bl_value is the same 16 bit code as the built-in panel, in the upper
	 * half of the word, so map nits onto that curve instead.
	 */
	if (dcp->brightness.external) {
		u32 code;

		if (ext_bl_raw_max)
			return (u32)val;

		code = interpolate_u16(clamp(val, EXT_MIN_NITS, EXT_MAX_NITS),
				   EXT_MIN_NITS, EXT_MAX_NITS,
				   ext_brightness_code,
				   ARRAY_SIZE(ext_brightness_code));
		return code << 16;
	}

	if (val <= MIN_BRIGHTNESS_PART1)
		return 16 * brightness_part1[0];
	else if (val == MAX_BRIGHTNESS_PART1)
		return 16 * brightness_part1[ARRAY_SIZE(brightness_part1) - 1];
	else if (val == MIN_BRIGHTNESS_PART2)
		return 16 * brightness_part2[0];
	else if (val >= MAX_BRIGHTNESS_PART2)
		return brightness_part2[ARRAY_SIZE(brightness_part2) - 1];

	if (val < MAX_BRIGHTNESS_PART1) {
		dac = interpolate(val, MIN_BRIGHTNESS_PART1, MAX_BRIGHTNESS_PART1,
				  brightness_part1, ARRAY_SIZE(brightness_part1));
	} else if (val > MIN_BRIGHTNESS_PART2) {
		dac = interpolate(val, MIN_BRIGHTNESS_PART2, MAX_BRIGHTNESS_PART2,
				  brightness_part2, ARRAY_SIZE(brightness_part2));
	} else {
		dac = interpolate(val, MAX_BRIGHTNESS_PART1, MIN_BRIGHTNESS_PART2,
				  brightness_part12, ARRAY_SIZE(brightness_part12));
	}

	return 16 * dac;
}

static int drm_crtc_set_brightness(struct apple_dcp *dcp)
{
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc *crtc = &dcp->crtc->base;
	int ret = 0;

	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);
	ret = drm_modeset_lock(&crtc->mutex, &ctx);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		drm_modeset_acquire_fini(&ctx);
		return -EDEADLK;
	} else if (ret == -ERESTARTSYS) {
		drm_modeset_acquire_fini(&ctx);
		return -ERESTARTSYS;
	}

	if (!dcp->brightness.update)
		goto done;

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state) {
		ret = -ENOMEM;
		goto done;
	}

	state->acquire_ctx = &ctx;
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto fail;
	}

	crtc_state->color_mgmt_changed |= true;

	ret = drm_atomic_commit(state);

fail:
	drm_atomic_state_put(state);
done:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}

int dcp_backlight_update(struct apple_dcp *dcp)
{
	/*
	 * Do not actively try to change brightness if no mode is set.
	 * TODO: should this be reflected the in backlight's power property?
	 *       defer this hopefully until it becomes irrelevant due to proper
	 *       drm integrated backlight handling
	 */
	if (!READ_ONCE(dcp->mode_state.valid))
		return 0;

	/* Wait 1 vblank cycle in the hope an atomic swap has already updated
	 * the brightness */
	msleep((1001 + 23) / 24); // 42ms for 23.976 fps

	return drm_crtc_set_brightness(dcp);
}

static int dcp_set_brightness(struct backlight_device *bd)
{
	int ret = 0;
	struct apple_dcp *dcp = bl_get_data(bd);
	struct drm_modeset_acquire_ctx ctx;
	int brightness = backlight_get_brightness(bd);

	drm_modeset_acquire_init(&ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE);
	ret = drm_modeset_lock(&dcp->crtc->base.mutex, &ctx);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		drm_modeset_acquire_fini(&ctx);
		return -EDEADLK;
	} else if (ret == -ERESTARTSYS) {
		drm_modeset_acquire_fini(&ctx);
		return -ERESTARTSYS;
	}

	dcp->brightness.dac = calculate_dac(dcp, brightness);
	dcp->brightness.update = true;

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	ret = dcp_backlight_update(dcp);
	dev_info(dcp->dev,
		 "set_brightness: %d nits -> dac 0x%x, mode_valid %d, update %d, ret %d\n",
		 brightness, dcp->brightness.dac,
		 READ_ONCE(dcp->mode_state.valid), dcp->brightness.update, ret);
	return ret;
}

static const struct backlight_ops dcp_backlight_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.get_brightness = dcp_get_brightness,
	.update_status = dcp_set_brightness,
};

int dcp_backlight_register(struct apple_dcp *dcp)
{
	struct device *dev = dcp->dev;
	struct backlight_device *bl_dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_PLATFORM,
		.brightness = dcp->brightness.nits,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	const char *name = "apple-panel-bl";
	char ext_name[32];

	if (dcp->brightness.external) {
		props.max_brightness = ext_bl_raw_max ? ext_bl_raw_max
						      : EXT_MAX_NITS;
		/*
		 * A machine has one DCP per external output, so the name has
		 * to carry the device name to stay unique.
		 */
		snprintf(ext_name, sizeof(ext_name), "apple-dp-bl-%.12s",
			 dcp->brightness.conn_name[0] ?
				 dcp->brightness.conn_name : dev_name(dev));
		name = ext_name;
	} else {
		props.max_brightness = min(dcp->brightness.maximum,
					   MAX_BRIGHTNESS_PART2 - 1);
	}

	bl_dev = devm_backlight_device_register(dev, name, dev, dcp,
						&dcp_backlight_ops, &props);
	if (IS_ERR(bl_dev))
		return PTR_ERR(bl_dev);

	dcp->brightness.bl_dev = bl_dev;
	dcp->brightness.dac = calculate_dac(dcp, dcp->brightness.nits);

	dev_info(dev, "backlight '%s' registered: %d of %d nits%s (dac 0x%x)\n",
		 name, dcp->brightness.nits, props.max_brightness,
		 dcp->brightness.external ? " [external/DP]" : "",
		 dcp->brightness.dac);

	return 0;
}
