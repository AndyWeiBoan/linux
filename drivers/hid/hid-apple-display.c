// SPDX-License-Identifier: GPL-2.0-only
/*
 * Backlight control for Apple displays that expose it over HID.
 *
 * A Studio Display reached over a Thunderbolt PCIe tunnel presents five HID
 * interfaces. One of them carries the standard HID monitor control pages
 * (0x80 Monitor, 0x82 VESA VCP) with a single 32-bit feature report, report
 * ID 2, holding the backlight level in hundredths of a nit. Everything else
 * about the display - speakers, microphone, camera, ambient light sensor -
 * is handled by the generic drivers already.
 *
 * Copyright The Asahi Linux Contributors
 */

#include <linux/backlight.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "hid-ids.h"

#define USB_DEVICE_ID_APPLE_STUDIO_DISPLAY	0x1114

/* Report ID 2: one 32-bit VESA VCP field, then a 16-bit field we do not use. */
#define ASD_BRIGHTNESS_REPORT_ID	2
#define ASD_BRIGHTNESS_REPORT_LEN	7

/* Hundredths of a nit, straight out of the report descriptor. */
#define ASD_BRIGHTNESS_MIN		400
#define ASD_BRIGHTNESS_MAX		60000

struct apple_display {
	struct hid_device *hdev;
	struct backlight_device *bl;
};

static int apple_display_get(struct hid_device *hdev, u32 *level)
{
	u8 *buf;
	int ret;

	buf = kzalloc(ASD_BRIGHTNESS_REPORT_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = ASD_BRIGHTNESS_REPORT_ID;
	ret = hid_hw_raw_request(hdev, ASD_BRIGHTNESS_REPORT_ID, buf,
				 ASD_BRIGHTNESS_REPORT_LEN, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret == ASD_BRIGHTNESS_REPORT_LEN) {
		*level = get_unaligned_le32(buf + 1);
		ret = 0;
	} else if (ret >= 0) {
		ret = -EIO;
	}

	kfree(buf);
	return ret;
}

static int apple_display_set(struct hid_device *hdev, u32 level)
{
	u8 *buf;
	int ret;

	level = clamp_t(u32, level, ASD_BRIGHTNESS_MIN, ASD_BRIGHTNESS_MAX);

	buf = kzalloc(ASD_BRIGHTNESS_REPORT_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = ASD_BRIGHTNESS_REPORT_ID;
	put_unaligned_le32(level, buf + 1);
	ret = hid_hw_raw_request(hdev, ASD_BRIGHTNESS_REPORT_ID, buf,
				 ASD_BRIGHTNESS_REPORT_LEN, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);
	if (ret == ASD_BRIGHTNESS_REPORT_LEN)
		ret = 0;
	else if (ret >= 0)
		ret = -EIO;

	kfree(buf);
	return ret;
}

static int apple_display_update_status(struct backlight_device *bl)
{
	struct apple_display *disp = bl_get_data(bl);

	return apple_display_set(disp->hdev, backlight_get_brightness(bl));
}

static int apple_display_get_brightness(struct backlight_device *bl)
{
	struct apple_display *disp = bl_get_data(bl);
	u32 level;
	int ret;

	ret = apple_display_get(disp->hdev, &level);
	if (ret)
		return ret;

	return level;
}

static const struct backlight_ops apple_display_bl_ops = {
	.options	= BL_CORE_SUSPENDRESUME,
	.update_status	= apple_display_update_status,
	.get_brightness	= apple_display_get_brightness,
};

static int apple_display_probe(struct hid_device *hdev,
			       const struct hid_device_id *id)
{
	struct backlight_properties props = {};
	struct apple_display *disp;
	u32 level;
	int ret;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	/*
	 * Only one of the display's interfaces carries this report; the rest
	 * are for the camera, the sensors and Apple's own vendor pages, and
	 * belong to the drivers that already handle them.
	 */
	if (!hdev->report_enum[HID_FEATURE_REPORT]
			.report_id_hash[ASD_BRIGHTNESS_REPORT_ID])
		return -ENODEV;

	disp = devm_kzalloc(&hdev->dev, sizeof(*disp), GFP_KERNEL);
	if (!disp)
		return -ENOMEM;

	disp->hdev = hdev;
	hid_set_drvdata(hdev, disp);

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret)
		goto err_stop;

	ret = apple_display_get(hdev, &level);
	if (ret) {
		hid_err(hdev, "cannot read the backlight level: %d\n", ret);
		goto err_close;
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = ASD_BRIGHTNESS_MAX;
	props.brightness = level;

	disp->bl = devm_backlight_device_register(&hdev->dev, "apple-display",
						  &hdev->dev, disp,
						  &apple_display_bl_ops,
						  &props);
	if (IS_ERR(disp->bl)) {
		ret = PTR_ERR(disp->bl);
		goto err_close;
	}

	hid_info(hdev, "backlight at %u of %u\n", level, ASD_BRIGHTNESS_MAX);
	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void apple_display_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id apple_display_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_APPLE,
			 USB_DEVICE_ID_APPLE_STUDIO_DISPLAY) },
	{ }
};
MODULE_DEVICE_TABLE(hid, apple_display_devices);

static struct hid_driver apple_display_driver = {
	.name		= "apple-display",
	.id_table	= apple_display_devices,
	.probe		= apple_display_probe,
	.remove		= apple_display_remove,
};
module_hid_driver(apple_display_driver);

MODULE_DESCRIPTION("Backlight control for Apple displays over HID");
MODULE_LICENSE("GPL");
