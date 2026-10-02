// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <linux/align.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/iommu.h>
#include <linux/jiffies.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/usb/typec_altmode.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/workqueue.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_module.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "afk.h"
#include "av.h"
#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "parser.h"
#include "trace.h"

#define APPLE_DCP_COPROC_CPU_CONTROL	 0x44
#define APPLE_DCP_COPROC_CPU_CONTROL_RUN BIT(4)

#define DCP_BOOT_TIMEOUT msecs_to_jiffies(1000)

static bool show_notch;
module_param(show_notch, bool, 0644);
MODULE_PARM_DESC(show_notch, "Use the full display height and shows the notch");

bool hdmi_audio;
module_param(hdmi_audio, bool, 0644);
MODULE_PARM_DESC(hdmi_audio, "Enable unstable HDMI audio support");

static bool unstable_edid = true;
module_param(unstable_edid, bool, 0644);
MODULE_PARM_DESC(unstable_edid, "Enable unstable EDID retrival support");

/* copied and simplified from drm_vblank.c */
static void send_vblank_event(struct drm_device *dev,
		struct drm_pending_vblank_event *e,
		u64 seq, ktime_t now)
{
	struct timespec64 tv;

	if (e->event.base.type != DRM_EVENT_FLIP_COMPLETE)
		return;

	tv = ktime_to_timespec64(now);
	e->event.vbl.sequence = seq;
	/*
		* e->event is a user space structure, with hardcoded unsigned
		* 32-bit seconds/microseconds. This is safe as we always use
		* monotonic timestamps since linux-4.15
		*/
	e->event.vbl.tv_sec = tv.tv_sec;
	e->event.vbl.tv_usec = tv.tv_nsec / 1000;

	/*
	 * Use the same timestamp for any associated fence signal to avoid
	 * mismatch in timestamps for vsync & fence events triggered by the
	 * same HW event. Frameworks like SurfaceFlinger in Android expects the
	 * retire-fence timestamp to match exactly with HW vsync as it uses it
	 * for its software vsync modeling.
	 */
	drm_send_event_timestamp_locked(dev, &e->base, now);
}

/**
 * dcp_crtc_send_page_flip_event - helper to send vblank event after pageflip
 *
 * Compensate for unknown slack between page flip and arrival of the
 * swap_complete callback. Minimal observed duration on DCP with HDMI output
 * was around 2.3 ms. If the fb swap was submitted closer to the expected
 * swap_complete it gets a penalty of one frame duration. This is on the border
 * of unreasonable considering that Apple advertises support for 240 Hz (frame
 * duration of 4.167 ms).
 * It is unreasonable considering kwin's kms commit scheduling. Kwin commits
 * 1.5 ms + the mode's vblank time before the expected next page flip
 * completion. This results in presenting at half the display's rate for HDMI
 * outputs.
 * This might be a difference between dcp and dcpext.
 */
static void dcp_crtc_send_page_flip_event(struct apple_crtc *crtc,
					  struct drm_pending_vblank_event *e,
					  ktime_t now, ktime_t start)
{
	struct drm_device *dev = crtc->base.dev;
	u64 seq;
	unsigned int pipe = drm_crtc_index(&crtc->base);
	ktime_t flip;

	seq = 0;
	if (start != KTIME_MIN) {
		s64 delta = ktime_us_delta(now, start);
		if (delta <= 500)
			flip = now;
		else if (delta >= 2500)
			flip = ktime_sub_us(now, 1000);
		else
			flip = ktime_sub_us(now, (delta - 500) / 2);
	} else {
		flip = now;
	}
	e->pipe = pipe;
	send_vblank_event(dev, e, seq, flip);
}

/* HACK: moved here to avoid circular dependency between apple_drv and dcp */
void dcp_drm_crtc_vblank(struct apple_crtc *crtc)
{
	unsigned long flags;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

void dcp_drm_crtc_page_flip(struct apple_dcp *dcp, ktime_t now)
{
	unsigned long flags;
	struct apple_crtc *crtc = dcp->crtc;

	spin_lock_irqsave(&crtc->base.dev->event_lock, flags);
	if (crtc->event) {
		if (crtc->event->event.base.type == DRM_EVENT_FLIP_COMPLETE)
			dcp_crtc_send_page_flip_event(crtc, crtc->event, now, dcp->swap_start);
		else
			drm_crtc_send_vblank_event(&crtc->base, crtc->event);
		crtc->event = NULL;
		dcp->swap_start = KTIME_MIN;
	}
	spin_unlock_irqrestore(&crtc->base.dev->event_lock, flags);
}

void dcp_set_dimensions(struct apple_dcp *dcp)
{
	int i;
	int width_mm = dcp->width_mm;
	int height_mm = dcp->height_mm;

	if (width_mm == 0 || height_mm == 0) {
		width_mm = dcp->panel.width_mm;
		height_mm = dcp->panel.height_mm;
	}

	/* Set the connector info */
	if (dcp->connector) {
		struct drm_connector *connector = &dcp->connector->base;

		mutex_lock(&connector->dev->mode_config.mutex);
		connector->display_info.width_mm = width_mm;
		connector->display_info.height_mm = height_mm;
		mutex_unlock(&connector->dev->mode_config.mutex);
	}

	/*
	 * Fix up any probed modes. Modes are created when parsing
	 * TimingElements, dimensions are calculated when parsing
	 * DisplayAttributes, and TimingElements may be sent first
	 */
	for (i = 0; i < dcp->nr_modes; ++i) {
		dcp->modes[i].mode.width_mm = width_mm;
		dcp->modes[i].mode.height_mm = height_mm;
	}
}

bool dcp_has_panel(struct apple_dcp *dcp)
{
	return dcp->panel.width_mm > 0;
}

int dcp_set_crc(struct drm_crtc *crtc, bool enabled)
{
	struct apple_crtc *ac = to_apple_crtc(crtc);
	struct apple_dcp *dcp = platform_get_drvdata(ac->dcp);

	dcp->crc_enabled = enabled;

	return 0;
}

/*
 * Helper to send a DRM vblank event. We do not know how call swap_submit_dcp
 * without surfaces. To avoid timeouts in drm_atomic_helper_wait_for_vblanks
 * send a vblank event via a workqueue.
 */
static void dcp_delayed_vblank(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, vblank_wq);
	mdelay(5);
	dcp_drm_crtc_vblank(dcp->crtc);
}

static void dcp_recv_msg(void *cookie, u8 endpoint, u64 message)
{
	struct apple_dcp *dcp = cookie;

	trace_dcp_recv_msg(dcp, endpoint, message);

	switch (endpoint) {
	case IOMFB_ENDPOINT:
		return iomfb_recv_msg(dcp, message);
	case AV_ENDPOINT:
		afk_receive_message(dcp->avep, message);
		return;
	case SYSTEM_ENDPOINT:
		afk_receive_message(dcp->systemep, message);
		return;
	case DISP0_ENDPOINT:
		afk_receive_message(dcp->ibootep, message);
		return;
	case DPAVSERV_ENDPOINT:
		afk_receive_message(dcp->dcpavservep, message);
		return;
	case DPTX_ENDPOINT:
		afk_receive_message(dcp->dptxep, message);
		return;
	default:
		WARN(endpoint, "unknown DCP endpoint %hhu\n", endpoint);
	}
}

static void dcp_rtk_crashed(void *cookie, const void *crashlog, size_t crashlog_size)
{
	struct apple_dcp *dcp = cookie;

	dcp->crashed = true;
	dev_err(dcp->dev, "DCP has crashed\n");
	if (dcp->connector) {
		dcp->connector->connected = 0;
		drm_edid_free(dcp->connector->drm_edid);
		dcp->connector->drm_edid = NULL;
		schedule_work(&dcp->connector->hotplug_wq);
	}
	complete(&dcp->start_done);
}

static int dcp_rtk_shmem_setup(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->iova) {
		struct iommu_domain *domain =
			iommu_get_domain_for_dev(dcp->dev);
		phys_addr_t phy_addr;

		if (!domain)
			return -ENOMEM;

		// TODO: get map from device-tree
		phy_addr = iommu_iova_to_phys(domain, bfr->iova);
		if (!phy_addr)
			return -ENOMEM;

		// TODO: verify phy_addr, cache attribute
		bfr->buffer = memremap(phy_addr, bfr->size, MEMREMAP_WB);
		if (!bfr->buffer)
			return -ENOMEM;

		bfr->is_mapped = true;
		dev_info(dcp->dev,
			 "shmem_setup: iova: %lx -> pa: %lx -> iomem: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)phy_addr,
			 (uintptr_t)bfr->buffer);
	} else {
		bfr->buffer = dma_alloc_coherent(dcp->dev, bfr->size,
						 &bfr->iova, GFP_KERNEL);
		if (!bfr->buffer)
			return -ENOMEM;

		dev_info(dcp->dev, "shmem_setup: iova: %lx, buffer: %lx\n",
			 (uintptr_t)bfr->iova, (uintptr_t)bfr->buffer);
	}

	return 0;
}

static void dcp_rtk_shmem_destroy(void *cookie, struct apple_rtkit_shmem *bfr)
{
	struct apple_dcp *dcp = cookie;

	if (bfr->is_mapped)
		memunmap(bfr->buffer);
	else
		dma_free_coherent(dcp->dev, bfr->size, bfr->buffer, bfr->iova);
}

static struct apple_rtkit_ops rtkit_ops = {
	.crashed = dcp_rtk_crashed,
	.recv_message = dcp_recv_msg,
	.shmem_setup = dcp_rtk_shmem_setup,
	.shmem_destroy = dcp_rtk_shmem_destroy,
};

void dcp_send_message(struct apple_dcp *dcp, u8 endpoint, u64 message)
{
	trace_dcp_send_msg(dcp, endpoint, message);
	apple_rtkit_send_message(dcp->rtk, endpoint, message, NULL,
				 true);
}

int dcp_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_crtc_state *crtc_state;
	bool needs_modeset;

	if (dcp->crashed)
		return -EINVAL;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (!dcp->valid_mode) {
		dev_info(dcp->dev, "forcing modeset: no valid output mode\n");
		crtc_state->mode_changed = true;
	}

	needs_modeset = drm_atomic_crtc_needs_modeset(crtc_state);
	if (!needs_modeset && !dcp->connector->connected) {
		dev_err(dcp->dev, "crtc_atomic_check: disconnected but no modeset\n");
		return -EINVAL;
	}

	return 0;
}

int dcp_get_connector_type(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return (dcp->connector_type);
}

#define DPTX_CONNECT_TIMEOUT msecs_to_jiffies(2000)

/*
 * Experiment knobs for bringing up a display behind a Thunderbolt DP tunnel.
 * The DPTX "remote port" target is core[3:0]|atc[7:4]|die[11:8]; the values
 * that work for DP alt mode are not known to be right for a tunnel. -1 keeps
 * the value derived from the device tree.
 */
/*
 * The DCP firmware asks the AP for the display clock rate and refuses to set
 * up pixel clocks when it gets something lower than the mode needs:
 *
 *   IOMFB updateFrequencies EDT ERROR: getClockFrequency(0) (0) < videoClock
 *   67500000! Giving up on frequencies.
 *
 * clk_get_rate() comes from the "clocks" phandle, and every clk_dispext* node
 * in the Asahi device tree carries clock-frequency = <0> (clk_disp0, used by
 * the internal panel, carries the real 237333328). Override it here so the
 * value can be probed without rebuilding the device tree.
 */
bool dcp_allow_virtual_modes = false;
module_param(dcp_allow_virtual_modes, bool, 0644);
MODULE_PARM_DESC(dcp_allow_virtual_modes, "Keep the DCP's virtual (DSC) timing modes");

bool dcp_add_5k_mode = false;
module_param(dcp_add_5k_mode, bool, 0644);
MODULE_PARM_DESC(dcp_add_5k_mode, "Offer a synthetic 5120x2880 mode (DCP timing 43)");

int apple_dpxbar_finish_dp_tunnel(struct mux_control *mux);

bool dcp_finish_dp_tunnel = true;
module_param(dcp_finish_dp_tunnel, bool, 0644);
static bool dcp_finish_early = true;
module_param(dcp_finish_early, bool, 0644);
MODULE_PARM_DESC(dcp_finish_early,
		 "Arm the DP IN adapter before asserting HPD, the way macOS does");
MODULE_PARM_DESC(dcp_finish_dp_tunnel, "Arm DP IN + PHY + crossbar from inside dcp_dptx_connect()");

int dcp_force_timing_id = -1;
module_param(dcp_force_timing_id, int, 0644);
MODULE_PARM_DESC(dcp_force_timing_id, "Override the DCP timing mode id (-1 = off)");

int dcp_force_color_id = -1;
module_param(dcp_force_color_id, int, 0644);
MODULE_PARM_DESC(dcp_force_color_id, "Override the DCP color mode id (-1 = off)");

int dcp_frequency_override = -1;
module_param(dcp_frequency_override, int, 0644);

static int dptx_core_override = -1;
module_param(dptx_core_override, int, 0644);
static int dptx_atc_override = -1;
module_param(dptx_atc_override, int, 0644);
static int dptx_die_override = -1;
module_param(dptx_die_override, int, 0644);

static int dcp_dptx_connect(struct apple_dcp *dcp, u32 port)
{
	int ret = 0;

	if (!dcp->phy && !dcp->xbar) {
		dev_warn(dcp->dev, "dcp_dptx_connect: missing phy\n");
		return -ENODEV;
	}
	dev_info(dcp->dev, "%s(port=%d)\n", __func__, port);

	mutex_lock(&dcp->hpd_mutex);
	if (!dcp->dptxport[port].enabled) {
		dev_warn(dcp->dev, "dcp_dptx_connect: dptx service for port %d not enabled\n", port);
		ret = -ENODEV;
		goto out_unlock;
	}

	if (dcp->dptxport[port].connected) {
		/*
		 * Being told a display showed up while we still think one is
		 * attached means the disconnect never reached us. It is
		 * reported on the controller the Type-C connector names, which
		 * is the one driving the port's PHY rather than this one when
		 * the display came over a tunnel; and a tunnel whose block
		 * simply loses power never reaches the teardown path that
		 * would report it either.
		 *
		 * Trust the event: the old display is gone. Drop it here
		 * rather than silently ignoring the new one, which left every
		 * later hotplug dead until reboot.
		 */
		dev_info(dcp->dev,
			 "connect while still connected, dropping stale display\n");
		dptxport_release_display(dcp->dptxport[port].service);
		dcp->dptxport[port].connected = false;
	}

	/*
	 * Re-apply the crossbar routing here, not just at probe.
	 *
	 * For a display behind a Thunderbolt DP tunnel the Type-C PHY is
	 * switched to Thunderbolt mode long after this driver probes, and that
	 * switch clears the crossbar: macOS traces show every clock and enable
	 * back at its reset value right before it programs the crossbar again.
	 * So whatever probe wrote is gone by the time the display shows up.
	 * macOS programs the crossbar after the PHY switch; do the same.
	 *
	 * Only for the tunnelled case (no Type-C PHY of our own); a DCP that
	 * owns its PHY drives the crossbar output directly and is unaffected.
	 */
	if (dcp->xbar && !dcp->phy) {
		u32 state;

		if (!of_property_read_u32(dcp->dev->of_node, "mux-index", &state)) {
			int xret;

			mux_control_deselect(dcp->xbar);
			xret = mux_control_select(dcp->xbar, state);
			if (xret)
				dev_warn(dcp->dev,
					 "failed to re-select crossbar state %u: %d\n",
					 state, xret);
		}
	}

	reinit_completion(&dcp->dptxport[port].linkcfg_completion);
	dcp->dptxport[port].atcphy = dcp->phy;
	{
		/*
		 * The DPTX "remote port" target selects a core as well as a
		 * PHY. A DCP that owns a Type-C PHY drives core 0; a display
		 * reached over a Thunderbolt DP tunnel needs core 1, and with
		 * core 0 the DPTX firmware finds no sink and gives up with
		 * DEVICE_NOT_RESPONDING. Measured on j314s with a Studio
		 * Display: core 1 is the only value that gets the firmware
		 * past activate into link training.
		 */
		u8 core = dptx_core_override >= 0 ? dptx_core_override :
			  (!dcp->phy && dcp->xbar) ? 1 : 0;
		u8 atc = dptx_atc_override >= 0 ? dptx_atc_override : dcp->dptx_phy;
		u8 die = dptx_die_override >= 0 ? dptx_die_override : dcp->dptx_die;

		/*
		 * macOS validates the connection before opening it, and the
		 * driver has never done so. Captured order on a Studio Display
		 * over a Thunderbolt tunnel:
		 *     validateConnection(target=0x8011, unk1=0x101)
		 *     connectTo(target=0x8011, unk1=0xa0101)
		 *     setPowerState
		 */
		if (!dcp->phy && dcp->xbar) {
			int vret = dptxport_validate_connection(
				dcp->dptxport[port].service, core, atc, die);
			if (vret)
				dev_warn(dcp->dev,
					 "validate_connection failed: %d\n", vret);
		}

		dptxport_connect(dcp->dptxport[port].service, core, atc, die);
	}
	dptxport_request_display(dcp->dptxport[port].service);
	dcp->dptxport[port].connected = true;

	mutex_unlock(&dcp->hpd_mutex);

	/*
	 * For a tunnelled display the HPD notification is what makes the DCP
	 * firmware start configuring the link, so it has to go out before we
	 * wait for the link to come up - not after the wait times out. macOS
	 * sends it twice, immediately after the firmware's ACTIVATE call and
	 * before it asks for link rates:
	 *
	 *     connectTo / setPowerState / GET_SUPPORTS_HPD /
	 *     GET_MAX_LANE_COUNT / ACTIVATE /
	 *     hotPlugDetectChangeOccurred / hotPlugDetectChangeOccurred /
	 *     SET_TILED_DISPLAY_HINTS / GET_MAX_LINK_RATE / ...
	 */
	/*
	 * Arm the hardware before telling the firmware the display is there,
	 * which is the order macOS uses. The other way round the firmware has
	 * nothing to train against: both hotplug calls and the link
	 * configuration that follows then sit there until they time out, which
	 * is four of the six seconds this takes.
	 */
	if (dcp_finish_early && !dcp->phy && dcp->xbar &&
	    dcp->tunnel_pending && dcp_finish_dp_tunnel) {
		int fret = apple_dpxbar_finish_dp_tunnel(dcp->xbar);

		if (fret)
			dev_warn(dcp->dev, "finish_dp_tunnel: %d\n", fret);
		else
			dev_info(dcp->dev, "finish_dp_tunnel: done (early)\n");
	}

	if (!dcp->phy && dcp->xbar) {
		dptxport_set_hpd(dcp->dptxport[port].service, true);
		dptxport_set_hpd(dcp->dptxport[port].service, true);
	}

	ret = wait_for_completion_timeout(&dcp->dptxport[port].linkcfg_completion,
				    DPTX_CONNECT_TIMEOUT);
	if (ret < 0)
		dev_warn(dcp->dev, "dcp_dptx_connect: port %d link complete failed:%d\n",
			 port, ret);
	else
		dev_dbg(dcp->dev, "dcp_dptx_connect: waited %d ms for link\n",
			jiffies_to_msecs(DPTX_CONNECT_TIMEOUT - ret));

	/*
	 * Finish the tunnel here, while the link is still up.
	 *
	 * The firmware brings a tunnelled display up and drops it again roughly
	 * a second later, and every later connect in the same boot stops at
	 * "IOMFB: IOAVVideoInterface published" without ever asserting HPD - the
	 * DPTX link state degrades until the machine is rebooted (the drive
	 * settings it asks for climb 19 -> 108 -> 152 -> 205 across attempts).
	 * So there is exactly one chance per boot and no userspace poll is fast
	 * enough to take it; arming the DP IN adapter, running the PHY's
	 * configureDPTunnelMode writes and enabling the crossbar clocks has to
	 * happen right here.
	 */
	if (!dcp_finish_early && !dcp->phy && dcp->xbar &&
	    dcp->tunnel_pending && dcp_finish_dp_tunnel) {
		int fret = apple_dpxbar_finish_dp_tunnel(dcp->xbar);

		if (fret)
			dev_warn(dcp->dev, "finish_dp_tunnel: %d\n", fret);
		else
			dev_info(dcp->dev, "finish_dp_tunnel: done\n");
	}

	usleep_range(5, 10);

	/* already sent above for the tunnelled case */
	if (dcp->connector_type == DRM_MODE_CONNECTOR_DisplayPort &&
	    !(!dcp->phy && dcp->xbar))
		dptxport_set_hpd(dcp->dptxport[port].service, true);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;

out_unlock:
	mutex_unlock(&dcp->hpd_mutex);
	return ret;
}

static void disconnected_hpd_event(struct apple_connector *con)
{
	if (con && con->connected) {
		con->connected = 0;
		drm_kms_helper_connector_hotplug_event(&con->base);
	}
}

static int dcp_dptx_disconnect(struct apple_dcp *dcp, u32 port)
{
	dev_info(dcp->dev, "%s(port=%d)\n", __func__, port);

	mutex_lock(&dcp->hpd_mutex);
	if (dcp->dptxport[port].enabled && dcp->dptxport[port].connected) {
		dptxport_release_display(dcp->dptxport[port].service);
		dcp->dptxport[port].connected = false;
	}
	/*
	 * Whatever shows up next has to say for itself that it comes over a
	 * tunnel, or it gets the plain DisplayPort treatment.
	 */
	dcp->tunnel_pending = false;
	mutex_unlock(&dcp->hpd_mutex);

	return 0;
}

/*
 * Manual DPTX connect/disconnect trigger, for displays reached over a
 * Thunderbolt DP tunnel. Nothing in the Thunderbolt stack emits a DRM
 * out-of-band hotplug event, so there is no in-kernel path that tells the
 * DCP to drive the link. Until that plumbing exists, drive it by hand:
 *
 *   echo 1 > /sys/devices/platform/soc/28cc00000.dcp/dptx_connect
 */
static ssize_t dptx_connect_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	char verb[16];
	unsigned int port = 0, arg = 0;
	int n, ret = 0;

	n = sscanf(buf, "%15s %u %u", verb, &port, &arg);
	if (n < 1)
		return -EINVAL;
	if (port > 1)
		return -EINVAL;
	if (!dcp->dptxport[port].enabled) {
		dev_warn(dev, "dptx: port %u not enabled\n", port);
		return -ENODEV;
	}

	dev_info(dev, "dptx: %s port=%u arg=%u\n", verb, port, arg);

	if (!strcmp(verb, "connect") || !strcmp(verb, "1"))
		ret = dcp_dptx_connect(dcp, port);
	else if (!strcmp(verb, "disconnect") || !strcmp(verb, "0"))
		ret = dcp_dptx_disconnect(dcp, port);
	else if (!strcmp(verb, "validate"))
		ret = dptxport_validate_connection(dcp->dptxport[port].service,
						   dptx_core_override >= 0 ? dptx_core_override : 0,
						   dptx_atc_override >= 0 ? dptx_atc_override : dcp->dptx_phy,
						   dptx_die_override >= 0 ? dptx_die_override : dcp->dptx_die);
	else if (!strcmp(verb, "request"))
		ret = dptxport_request_display(dcp->dptxport[port].service);
	else if (!strcmp(verb, "release"))
		ret = dptxport_release_display(dcp->dptxport[port].service);
	else if (!strcmp(verb, "hpd"))
		ret = dptxport_set_hpd(dcp->dptxport[port].service, !!arg);
	else if (!strcmp(verb, "xbar")) {
		/*
		 * Re-apply the crossbar routing now. Probe runs at boot, long
		 * before the Type-C PHY is switched to Thunderbolt mode and
		 * ACIO is powered, so the routing set there may not survive.
		 * "xbar" re-selects the DT state, "xbar <n>" picks dispext n.
		 */
		u32 state = arg;

		if (!dcp->xbar)
			return -ENODEV;
		if (n < 3 && of_property_read_u32(dev->of_node, "mux-index", &state))
			return -EINVAL;
		mux_control_deselect(dcp->xbar);
		ret = mux_control_select(dcp->xbar, state);
		dev_info(dev, "dptx: xbar re-select state %u -> %d\n", state, ret);
	}
	else
		return -EINVAL;

	if (ret)
		dev_warn(dev, "dptx: %s returned %d\n", verb, ret);

	return count;
}
static DEVICE_ATTR_WO(dptx_connect);

static struct attribute *dcp_dev_attrs[] = {
	&dev_attr_dptx_connect.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dcp_dev);

/*
 * Nothing in the kernel connects a DCP to a display that arrives over a
 * Thunderbolt DP tunnel: the Thunderbolt stack never emits
 * drm_connector_oob_hotplug_event() (only typec/altmodes/displayport.c does),
 * so dcp_dptx_connect() is only ever reachable from userspace. That makes it
 * impossible to trace a bring-up from a guest with no userspace, so allow the
 * driver to drive it itself: retry every second until the DPTX firmware
 * reports a display.
 */
static int dptx_autoconnect;
module_param(dptx_autoconnect, int, 0644);

static void dcp_autoconnect_work(struct work_struct *work)
{
	struct apple_dcp *dcp = container_of(to_delayed_work(work),
					     struct apple_dcp, autoconnect_wq);

	if (dcp->dptxport[0].connected)
		return;

	if (dcp->autoconnect_tries++ >= dptx_autoconnect) {
		dev_info(dcp->dev, "autoconnect: giving up after %d tries\n",
			 dcp->autoconnect_tries - 1);
		return;
	}

	dev_info(dcp->dev, "autoconnect: try %d\n", dcp->autoconnect_tries);
	dcp_dptx_connect(dcp, 0);

	if (!dcp->dptxport[0].connected)
		schedule_delayed_work(&dcp->autoconnect_wq, HZ);
}

/**
 * dcp_dptx_select_atc() - point this DCP at the port a tunnel arrived on
 * @pdev: the display controller
 * @atc: index of the Type-C port, matching the crossbar order in the DT
 *
 * A tunnelled display can show up on any Type-C port, and which one decides
 * both the display crossbar to route through and the DP transmitter the DCP
 * firmware has to drive. Whoever brings the tunnel up knows that; call this
 * before reporting the display so the connect that follows uses the right one.
 */
int dcp_dptx_select_atc(struct platform_device *pdev, unsigned int atc,
			bool tunnel)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	unsigned int have;

	if (!dcp)
		return -ENODEV;
	if (atc >= DCP_MAX_ATC)
		return -EINVAL;

	/* Nothing to pick from means this one only ever serves a single port */
	have = max(dcp->n_xbars, dcp->n_phys);
	if (!have)
		return 0;
	if (atc >= have)
		return -EINVAL;

	if (dcp->xbars[atc] && dcp->xbar != dcp->xbars[atc]) {
		u32 state;

		mux_control_deselect(dcp->xbar);
		dcp->xbar = dcp->xbars[atc];

		/*
		 * The new crossbar still has to be told what to route. The
		 * tunnelled path redoes this in dcp_dptx_connect() because the
		 * Type-C mode switch clears it, but a controller driving a PHY
		 * of its own never goes through there and would otherwise keep
		 * whatever probe happened to select on a different port's
		 * crossbar.
		 */
		if (!of_property_read_u32(dcp->dev->of_node, "mux-index",
					  &state)) {
			int xret = mux_control_select(dcp->xbar, state);

			if (xret)
				dev_warn(dcp->dev,
					 "ATC %u: crossbar state %u failed: %d\n",
					 atc, state, xret);
		}
	}
	if (dcp->phys[atc])
		dcp->phy = dcp->phys[atc];
	dcp->dptx_phy = atc;
	if (tunnel)
		dcp->tunnel_pending = true;

	dev_info(dcp->dev, "%s display is on ATC %u\n",
		 tunnel ? "tunnelled" : "DisplayPort altmode", atc);
	return 0;
}
EXPORT_SYMBOL_GPL(dcp_dptx_select_atc);

int dcp_dptx_connect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	return dcp_dptx_connect(dcp, port);
}

int dcp_dptx_disconnect_oob(struct platform_device *pdev, u32 port)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	disconnected_hpd_event(dcp->connector);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dcp->dptxport[port].enabled)
		dptxport_set_hpd(dcp->dptxport[port].service, false);

	return dcp_dptx_disconnect(dcp, port);
}

static irqreturn_t dcp_dp2hdmi_hpd(int irq, void *data)
{
	struct apple_dcp *dcp = data;
	bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);

	/* do nothing on disconnect and trust that dcp detects it itself.
	 * Parallel disconnect HPDs result drm disabling the CRTC even when it
	 * should not.
	 * The interrupt should be changed to rising but for now the disconnect
	 * IRQs might be helpful for debugging.
	 */
	dev_info(dcp->dev, "DP2HDMI HPD irq, connected:%d\n", connected);

	if (connected) {
		msleep(500);
		connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "DP2HDMI HPD irq, 500ms debounce: connected:%d\n", connected);
	}

	if (connected)
		dcp_dptx_connect(dcp, 0);

	return IRQ_HANDLED;
}

void dcp_link(struct platform_device *pdev, struct apple_crtc *crtc,
	      struct apple_connector *connector)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	dcp->crtc = crtc;
	dcp->connector = connector;
}


bool dcp_fw_compat_is_12_x(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->fw_compat == DCP_FIRMWARE_V_12_3;
}

unsigned long* dcp_get_iomfb_surfaces(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->iomfb_surfaces;
}

int dcp_start(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	init_completion(&dcp->start_done);

	/* start RTKit endpoints */
	ret = systemep_init(dcp);
	if (ret)
		dev_warn(dcp->dev, "Failed to start system endpoint: %d\n", ret);

	if (unstable_edid && !dcp_has_panel(dcp)) {
		ret = dpavservep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start DPAVSERV endpoint: %d",
				 ret);
	}

	if ((dcp->phy || dcp->xbar) && dcp->fw_compat >= DCP_FIRMWARE_V_13_5) {
		ret = ibootep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start IBOOT endpoint: %d\n",
				 ret);

		ret = dptxep_init(dcp);
		if (ret) {
			dev_warn(dcp->dev, "Failed to start DPTX endpoint: %d\n",
				 ret);
#ifdef DCP_DPTX_DISCONNECT_ON_INIT
		/*
		 * This disconnect / connect cycle on init is only necessary
		 * when using dcp0 on j473, j474s and presumedly j475c.
		 * Since dcp0 is not used at the moment let's avoid this
		 * since it is possibly the cause for startup issues.
		 */
		} else if (dcp->dptxport[0].enabled) {
			bool connected;
			/* force disconnect on start - necessary if the display
			 * is already up from m1n1
			 */
			dptxport_set_hpd(dcp->dptxport[0].service, false);
			dptxport_release_display(dcp->dptxport[0].service);
			usleep_range(10 * USEC_PER_MSEC, 25 * USEC_PER_MSEC);

			connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
			dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

			// necessary on j473/j474 but not on j314c
			if (connected)
				dcp_dptx_connect(dcp, 0);
#endif
		}
	} else if (dcp->phy || dcp->xbar) {
		dev_warn(dcp->dev, "OS firmware incompatible with dptxport EP\n");
	}
	ret = iomfb_start_rtkit(dcp);
	if (ret)
		dev_err(dcp->dev, "Failed to start IOMFB endpoint: %d\n", ret);

	if (dptx_autoconnect > 0 && !dcp->phy && dcp->xbar) {
		INIT_DELAYED_WORK(&dcp->autoconnect_wq, dcp_autoconnect_work);
		dcp->autoconnect_tries = 0;
		schedule_delayed_work(&dcp->autoconnect_wq, 5 * HZ);
	}

#if IS_ENABLED(CONFIG_DRM_APPLE_AUDIO)
	if (hdmi_audio) {
		ret = avep_init(dcp);
		if (ret)
			dev_warn(dcp->dev, "Failed to start AV endpoint: %d", ret);
		ret = 0;
	}
#endif

	return ret;
}

static void _dcp_poweroff(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweroff_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweroff_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

static int dcp_enable_dp2hdmi_hpd(struct apple_dcp *dcp)
{
	// check HPD state before enabling the edge triggered IRQ
	if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		if (connected)
			dcp_dptx_connect(dcp, 0);
		else
			_dcp_poweroff(dcp);
	}

	if (dcp->hdmi_hpd_irq)
		enable_irq(dcp->hdmi_hpd_irq);

	return 0;
}

int dcp_wait_ready(struct platform_device *pdev, u64 timeout)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	int ret;

	if (dcp->crashed)
		return -ENODEV;
	if (dcp->active)
		return dcp_enable_dp2hdmi_hpd(dcp);
	if (timeout <= 0)
		return -ETIMEDOUT;

	ret = wait_for_completion_timeout(&dcp->start_done, timeout);
	if (ret < 0)
		return ret;

	if (dcp->crashed)
		return -ENODEV;

	if (dcp->active)
		dcp_enable_dp2hdmi_hpd(dcp);

	return dcp->active ? 0 : -ETIMEDOUT;
}

static void __maybe_unused dcp_sleep(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_sleep_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_sleep_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

void dcp_poweron(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		dev_info(dcp->dev, "%s: DP2HDMI HPD connected:%d\n", __func__, connected);

		if (connected)
			dcp_dptx_connect(dcp, 0);
	}

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_poweron_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_poweron_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}

	if (dcp->avep)
		av_service_connect(dcp);
}

void dcp_poweroff(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	if (dcp->avep)
		av_service_disconnect(dcp);

	_dcp_poweroff(dcp);

	if (dcp->hdmi_hpd) {
		bool connected = gpiod_get_value_cansleep(dcp->hdmi_hpd);
		if (!connected) {
			disconnected_hpd_event(dcp->connector);
			dcp_dptx_disconnect(dcp, 0);
		}
	}
}

static void dcp_work_register_backlight(struct work_struct *work)
{
	int ret;
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_register_wq);

	mutex_lock(&dcp->bl_register_mutex);
	if (dcp->brightness.bl_dev)
		goto out_unlock;

	/* try to register backlight device, */
	ret = dcp_backlight_register(dcp);
	if (ret) {
		dev_err(dcp->dev, "Unable to register backlight device\n");
		dcp->brightness.maximum = 0;
	}

out_unlock:
	mutex_unlock(&dcp->bl_register_mutex);
}

static void dcp_work_update_backlight(struct work_struct *work)
{
	struct apple_dcp *dcp;

	dcp = container_of(work, struct apple_dcp, bl_update_wq);

	dcp_backlight_update(dcp);
}

static int dcp_create_piodma_iommu_dev(struct apple_dcp *dcp)
{
	int ret;
	struct device_node *node __free(device_node) = of_get_child_by_name(dcp->dev->of_node, "piodma");

	if (!node)
		return dev_err_probe(dcp->dev, -ENODEV,
				     "Failed to get piodma child DT node\n");

	dcp->piodma = of_platform_device_create(node, NULL, dcp->dev);
	if (!dcp->piodma)
		return dev_err_probe(dcp->dev, -ENODEV, "Failed to create piodma pdev for %pOF\n", node);

	ret = dma_set_mask_and_coherent(&dcp->piodma->dev, DMA_BIT_MASK(42));
	if (ret)
		goto err_destroy_pdev;

	ret = of_dma_configure(&dcp->piodma->dev, node, true);
	if (ret) {
		ret = dev_err_probe(dcp->dev, ret,
			"Failed to configure IOMMU child DMA\n");
		goto err_destroy_pdev;
	}

	dcp->iommu_dom = iommu_get_domain_for_dev(&dcp->piodma->dev);
	if (IS_ERR(dcp->iommu_dom)) {
		ret = dev_err_probe(dcp->dev, PTR_ERR(dcp->iommu_dom),
				    "Failed to get default iommu domain for "
				    "piodma device\n");
		dcp->iommu_dom = NULL;
		goto err_destroy_pdev;
	}

	return 0;
err_destroy_pdev:
	of_platform_device_destroy(&dcp->piodma->dev, NULL);
	return ret;
}

static int dcp_get_bw_scratch_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx, offset;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-scratch",
				   "#apple,bw-scratch-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-scratch': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 3) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-scratch' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];
	offset = ph_args.args[2];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-scratch': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_scratch_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-scratch' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	if (offset > resource_size(&dcp->disp_bw_scratch_res) - 4) {
		ret = -EINVAL;
		goto err_of_node_put;
	}

	dcp->disp_registers[disp_idx] = &dcp->disp_bw_scratch_res;
	dcp->disp_bw_scratch_index = disp_idx;
	dcp->disp_bw_scratch_offset = offset;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_bw_doorbell_reg(struct apple_dcp *dcp, u32 expected)
{
	struct of_phandle_args ph_args;
	u32 addr_idx, disp_idx;
	int ret;

	ret = of_parse_phandle_with_args(dcp->dev->of_node, "apple,bw-doorbell",
				   "#apple,bw-doorbell-cells", 0, &ph_args);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to read 'apple,bw-doorbell': %d\n", ret);
		return ret;
	}

	if (ph_args.args_count != 2) {
		dev_err(dcp->dev, "Unexpected 'apple,bw-doorbell' arg count %d\n",
			ph_args.args_count);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	addr_idx = ph_args.args[0];
	disp_idx = ph_args.args[1];

	if (disp_idx != expected || disp_idx >= MAX_DISP_REGISTERS) {
		dev_err(dcp->dev, "Unexpected disp_reg value in 'apple,bw-doorbell': %d\n",
			disp_idx);
		ret = -EINVAL;
		goto err_of_node_put;
	}

	ret = of_address_to_resource(ph_args.np, addr_idx, &dcp->disp_bw_doorbell_res);
	if (ret < 0) {
		dev_err(dcp->dev, "Failed to get 'apple,bw-doorbell' resource %d from %pOF\n",
			addr_idx, ph_args.np);
		goto err_of_node_put;
	}
	dcp->disp_bw_doorbell_index = disp_idx;
	dcp->disp_registers[disp_idx] = &dcp->disp_bw_doorbell_res;
	ret = 0;

err_of_node_put:
	of_node_put(ph_args.np);
	return ret;
}

static int dcp_get_disp_regs(struct apple_dcp *dcp)
{
	struct platform_device *pdev = to_platform_device(dcp->dev);
	int count = pdev->num_resources - 1;
	int i, ret;

	if (count <= 0 || count > MAX_DISP_REGISTERS)
		return -EINVAL;

	for (i = 0; i < count; ++i) {
		dcp->disp_registers[i] =
			platform_get_resource(pdev, IORESOURCE_MEM, 1 + i);
	}

	/* load pmgr bandwidth scratch resource and offset */
	ret = dcp_get_bw_scratch_reg(dcp, count);
	if (ret < 0)
		return ret;
	count += 1;

	/* load pmgr bandwidth doorbell resource if present (only on t8103) */
	if (of_property_present(dcp->dev->of_node, "apple,bw-doorbell")) {
		ret = dcp_get_bw_doorbell_reg(dcp, count);
		if (ret < 0)
			return ret;
		count += 1;
	}

	dcp->nr_disp_registers = count;
	return 0;
}

#define DCP_FW_VERSION_MIN_LEN	3
#define DCP_FW_VERSION_MAX_LEN	5
#define DCP_FW_VERSION_STR_LEN	(DCP_FW_VERSION_MAX_LEN * 4)

static int dcp_read_fw_version(struct device *dev, const char *name,
			       char *version_str)
{
	u32 ver[DCP_FW_VERSION_MAX_LEN];
	int len_str;
	int len;

	len = of_property_read_variable_u32_array(dev->of_node, name, ver,
						  DCP_FW_VERSION_MIN_LEN,
						  DCP_FW_VERSION_MAX_LEN);

	switch (len) {
	case 3:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d", ver[0], ver[1], ver[2]);
		break;
	case 4:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3]);
		break;
	case 5:
		len_str = scnprintf(version_str, DCP_FW_VERSION_STR_LEN,
				    "%d.%d.%d.%d.%d", ver[0], ver[1], ver[2],
				    ver[3], ver[4]);
		break;
	default:
		len_str = strscpy(version_str, "UNKNOWN",
				  DCP_FW_VERSION_STR_LEN);
		if (len >= 0)
			len = -EOVERFLOW;
		break;
	}

	if (len_str >= DCP_FW_VERSION_STR_LEN)
		dev_warn(dev, "'%s' truncated: '%s'\n", name, version_str);

	return len;
}

static enum dcp_firmware_version dcp_check_firmware_version(struct device *dev)
{
	char compat_str[DCP_FW_VERSION_STR_LEN];
	char fw_str[DCP_FW_VERSION_STR_LEN];
	int ret;

	/* firmware version is just informative */
	dcp_read_fw_version(dev, "apple,firmware-version", fw_str);

	ret = dcp_read_fw_version(dev, "apple,firmware-compat", compat_str);
	if (ret < 0) {
		dev_err(dev, "Could not read 'apple,firmware-compat': %d\n", ret);
		return DCP_FIRMWARE_UNKNOWN;
	}

	if (strncmp(compat_str, "12.3.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_12_3;
	/*
	 * m1n1 reports firmware version 13.5 as compatible with 13.3. This is
	 * only true for the iomfb endpoint. The interface for the dptx-port
	 * endpoint changed between 13.3 and 13.5. The driver will only support
	 * firmware 13.5. Check the actual firmware version for compat version
	 * 13.3 until m1n1 reports 13.5 as "firmware-compat".
	 */
	else if ((strncmp(compat_str, "13.3.0", sizeof(compat_str)) == 0) &&
		 (strncmp(fw_str, "13.5.0", sizeof(compat_str)) == 0))
		return DCP_FIRMWARE_V_13_5;
	else if (strncmp(compat_str, "13.5.0", sizeof(compat_str)) == 0)
		return DCP_FIRMWARE_V_13_5;

	dev_err(dev, "DCP firmware-compat %s (FW: %s) is not supported\n",
		compat_str, fw_str);

	return DCP_FIRMWARE_UNKNOWN;
}

static int dcp_comp_bind(struct device *dev, struct device *main, void *data)
{
	struct device_node *panel_np;
	struct apple_dcp *dcp = dev_get_drvdata(dev);
	u32 cpu_ctrl;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(42));
	if (ret)
		return ret;

	dcp->coproc_reg = devm_platform_ioremap_resource_byname(to_platform_device(dev), "coproc");
	if (IS_ERR(dcp->coproc_reg))
		return PTR_ERR(dcp->coproc_reg);

	of_property_read_u32(dev->of_node, "apple,dcp-index",
					   &dcp->index);
	of_property_read_u32(dev->of_node, "apple,dptx-phy",
					   &dcp->dptx_phy);
	of_property_read_u32(dev->of_node, "apple,dptx-die",
					   &dcp->dptx_die);
	if (dcp->index || dcp->dptx_phy || dcp->dptx_die)
		dev_info(dev, "DCP index:%u dptx target phy: %u dptx die: %u\n",
			 dcp->index, dcp->dptx_phy, dcp->dptx_die);
	mutex_init(&dcp->hpd_mutex);

	if (!show_notch)
		ret = of_property_read_u32(dev->of_node, "apple,notch-height",
					   &dcp->notch_height);

	if (dcp->notch_height > MAX_NOTCH_HEIGHT)
		dcp->notch_height = MAX_NOTCH_HEIGHT;
	if (dcp->notch_height > 0)
		dev_info(dev, "Detected display with notch of %u pixel\n", dcp->notch_height);

	/* initialize brightness scale to a sensible default to avoid divide by 0*/
	dcp->brightness.scale = 65536;
	panel_np = of_get_compatible_child(dev->of_node, "apple,panel-mini-led");
	if (panel_np)
		dcp->panel.has_mini_led = true;
	else
		panel_np = of_get_compatible_child(dev->of_node, "apple,panel");

	if (panel_np) {
		const char height_prop[2][16] = { "adj-height-mm", "height-mm" };

		if (of_device_is_available(panel_np)) {
			ret = of_property_read_u32(panel_np, "apple,max-brightness",
						   &dcp->brightness.maximum);
			if (ret)
				dev_err(dev, "Missing property 'apple,max-brightness'\n");
		}

		of_property_read_u32(panel_np, "width-mm", &dcp->panel.width_mm);
		/* use adjusted height as long as the notch is hidden */
		of_property_read_u32(panel_np, height_prop[!dcp->notch_height],
				     &dcp->panel.height_mm);

		of_node_put(panel_np);
		dcp->connector_type = DRM_MODE_CONNECTOR_eDP;
		INIT_WORK(&dcp->bl_register_wq, dcp_work_register_backlight);
		mutex_init(&dcp->bl_register_mutex);
		INIT_WORK(&dcp->bl_update_wq, dcp_work_update_backlight);
	} else if (of_property_match_string(dev->of_node, "apple,connector-type", "HDMI-A") >= 0)
		dcp->connector_type = DRM_MODE_CONNECTOR_HDMIA;
	else if (of_property_match_string(dev->of_node, "apple,connector-type", "DP") >= 0)
		dcp->connector_type = DRM_MODE_CONNECTOR_DisplayPort;
	else if (of_property_match_string(dev->of_node, "apple,connector-type", "USB-C") >= 0)
		dcp->connector_type = DRM_MODE_CONNECTOR_USB;
	else
		dcp->connector_type = DRM_MODE_CONNECTOR_Unknown;

	ret = dcp_create_piodma_iommu_dev(dcp);
	if (ret || !dcp->iommu_dom)
		return dev_err_probe(dev, ret,
				"Failed to created PIODMA iommu child device");

	ret = dcp_get_disp_regs(dcp);
	if (ret) {
		dev_err(dev, "failed to find display registers\n");
		return ret;
	}

	dcp->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(dcp->clk))
		return dev_err_probe(dev, PTR_ERR(dcp->clk),
				     "Unable to find clock\n");

	bitmap_zero(dcp->memdesc_map, DCP_MAX_MAPPINGS);
	// TDOD: mem_desc IDs start at 1, for simplicity just skip '0' entry
	set_bit(0, dcp->memdesc_map);

	INIT_WORK(&dcp->vblank_wq, dcp_delayed_vblank);

	dcp->swapped_out_fbs =
		(struct list_head)LIST_HEAD_INIT(dcp->swapped_out_fbs);

	cpu_ctrl =
		readl_relaxed(dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);
	writel_relaxed(cpu_ctrl | APPLE_DCP_COPROC_CPU_CONTROL_RUN,
		       dcp->coproc_reg + APPLE_DCP_COPROC_CPU_CONTROL);

	dcp->rtk = devm_apple_rtkit_init(dev, dcp, "mbox", 0, &rtkit_ops);
	if (IS_ERR(dcp->rtk))
		return dev_err_probe(dev, PTR_ERR(dcp->rtk),
				     "Failed to initialize RTKit\n");

	ret = apple_rtkit_wake(dcp->rtk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to boot RTKit: %d\n", ret);
	return ret;
}

/*
 * We need to shutdown DCP before tearing down the display subsystem. Otherwise
 * the DCP will crash and briefly flash a green screen of death.
 */
static void dcp_comp_unbind(struct device *dev, struct device *main, void *data)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (!dcp)
		return;

	if (dcp->hdmi_hpd_irq)
		disable_irq(dcp->hdmi_hpd_irq);

	typec_mux_put(dcp->typec_mux);

	if (dcp->avep) {
		av_service_disconnect(dcp);
		afk_shutdown(dcp->avep);
		dcp->avep = NULL;
	}

	if (dcp->dptxep) {
		afk_shutdown(dcp->dptxep);
		dcp->dptxep = NULL;
	}

	if (dcp->ibootep) {
		afk_shutdown(dcp->ibootep);
		dcp->ibootep = NULL;
	}

	if (dcp->systemep) {
		afk_shutdown(dcp->systemep);
		dcp->systemep = NULL;
	}

	if (dcp->dcpavservep) {
		afk_shutdown(dcp->dcpavservep);
		dcp->dcpavservep = NULL;
	}

	if (dcp->shmem)
		iomfb_shutdown(dcp);

	if (dcp->piodma) {
		dcp->iommu_dom = NULL;
		of_platform_device_destroy(&dcp->piodma->dev, NULL);
		dcp->piodma = NULL;
	}

	if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP) {
		cancel_work_sync(&dcp->bl_register_wq);
		cancel_work_sync(&dcp->bl_update_wq);
	}
	cancel_work_sync(&dcp->vblank_wq);

	devm_clk_put(dev, dcp->clk);
	dcp->clk = NULL;
}

static const struct component_ops dcp_comp_ops = {
	.bind	= dcp_comp_bind,
	.unbind	= dcp_comp_unbind,
};

static int dcp_platform_probe(struct platform_device *pdev)
{
	enum dcp_firmware_version fw_compat;
	struct device *dev = &pdev->dev;
	struct apple_dcp *dcp;
	int surf, num_surfs;
	u32 surf_en;
	u32 mux_index;

	fw_compat = dcp_check_firmware_version(dev);
	if (fw_compat == DCP_FIRMWARE_UNKNOWN)
		return -ENODEV;

	/* Check for "apple,bw-scratch" to avoid probing appledrm with outdated
	 * device trees. This prevents replacing simpledrm and ending up without
	 * display.
	 */
	if (!of_property_present(dev->of_node, "apple,bw-scratch"))
		return dev_err_probe(dev, -ENODEV, "Incompatible devicetree! "
			"Use devicetree matching this kernel.\n");

	dcp = devm_kzalloc(dev, sizeof(*dcp), GFP_KERNEL);
	if (!dcp)
		return -ENOMEM;

	dcp->fw_compat = fw_compat;
	dcp->dev = dev;
	dcp->hw = *(struct apple_dcp_hw_data *)of_device_get_match_data(dev);

	platform_set_drvdata(pdev, dcp);

	/*
	 * Either one PHY under the old "dp-phy" name, or one per Type-C port
	 * a display can arrive on. Start out on the one the device tree says
	 * this controller drives.
	 */
	dcp->phy = devm_phy_optional_get(dev, "dp-phy");
	if (IS_ERR(dcp->phy)) {
		dev_err(dev, "Failed to get dp-phy: %ld\n", PTR_ERR(dcp->phy));
		return PTR_ERR(dcp->phy);
	}
	if (!dcp->phy) {
		unsigned int i;

		for (i = 0; i < DCP_MAX_ATC; i++) {
			char name[16];
			struct phy *p;

			snprintf(name, sizeof(name), "dp-phy-atc%u", i);
			p = devm_phy_optional_get(dev, name);
			if (IS_ERR(p)) {
				dev_err(dev, "Failed to get %s: %ld\n", name,
					PTR_ERR(p));
				return PTR_ERR(p);
			}
			if (!p)
				continue;
			dcp->phys[i] = p;
			dcp->n_phys = i + 1;
		}
		if (dcp->n_phys) {
			u32 which = 0;

			of_property_read_u32(dev->of_node, "apple,dptx-phy",
					     &which);
			if (which >= DCP_MAX_ATC || !dcp->phys[which])
				which = 0;
			dcp->phy = dcp->phys[which];
			dev_info(dev, "%u Type-C PHY(s), starting on ATC %u\n",
				 dcp->n_phys, which);
		}
	}

	bitmap_zero(dcp->iomfb_surfaces, DCP_MAX_PLANES);
	if (!of_property_present(dev->of_node, "apple,iomfb-surfaces"))
		num_surfs = 0;
	else
		num_surfs = of_property_count_elems_of_size(dev->of_node,
						    "apple,iomfb-surfaces",
						    sizeof(u32));

	if (num_surfs == 0 || num_surfs == -ENODATA) {
		set_bit(0, dcp->iomfb_surfaces);
		set_bit(1, dcp->iomfb_surfaces);
	} else if (num_surfs < 0) {
		return num_surfs;
	} else if (num_surfs > DCP_MAX_PLANES) {
		dev_err(dev, "Number of iomfb-surfaces (%d) exceeds DCP_MAX_PLANES\n",
			num_surfs);
		return -EINVAL;
	}

	surf = 0;
	of_property_for_each_u32(dev->of_node, "apple,iomfb-surfaces", surf_en) {
		if (surf_en)
			set_bit(surf, dcp->iomfb_surfaces);
		surf++;
	}

	if (dcp->phy) {
		int ret;
		/*
		 * Request DP2HDMI related GPIOs as optional for DP-altmode
		 * compatibility. J180D misses a dp2hdmi-pwren GPIO in the
		 * template ADT. TODO: check device ADT
		 */
		dcp->hdmi_hpd = devm_gpiod_get_optional(dev, "hdmi-hpd", GPIOD_IN);
		if (IS_ERR(dcp->hdmi_hpd))
			return PTR_ERR(dcp->hdmi_hpd);
		if (dcp->hdmi_hpd) {
			int irq = gpiod_to_irq(dcp->hdmi_hpd);
			if (irq < 0) {
				dev_err(dev, "failed to translate HDMI hpd GPIO to IRQ\n");
				return irq;
			}
			dcp->hdmi_hpd_irq = irq;

			ret = devm_request_threaded_irq(dev, dcp->hdmi_hpd_irq,
						NULL, dcp_dp2hdmi_hpd,
						IRQF_ONESHOT | IRQF_NO_AUTOEN |
						IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
						"dp2hdmi-hpd-irq", dcp);
			if (ret < 0) {
				dev_err(dev, "failed to request HDMI hpd irq %d: %d\n",
					irq, ret);
				return ret;
			}
		}

		/*
		 * Power DP2HDMI on as it is required for the HPD irq.
		 * TODO: check if one is sufficient for the hpd to save power
		 *       on battery powered Macbooks.
		 */
		dcp->hdmi_pwren = devm_gpiod_get_optional(dev, "hdmi-pwren", GPIOD_OUT_HIGH);
		if (IS_ERR(dcp->hdmi_pwren))
			return PTR_ERR(dcp->hdmi_pwren);

		dcp->dp2hdmi_pwren = devm_gpiod_get_optional(dev, "dp2hdmi-pwren", GPIOD_OUT_HIGH);
		if (IS_ERR(dcp->dp2hdmi_pwren))
			return PTR_ERR(dcp->dp2hdmi_pwren);

	}

	/*
	 * Select which display crossbar output this DCP feeds.
	 *
	 * This is deliberately outside the dcp->phy check above: the target
	 * can also be one of the DP IN adapters of the USB4 host router, in
	 * which case the signal is tunnelled over Thunderbolt and never
	 * reaches a Type-C PHY at all. Without a source on DP IN,
	 * tb_dp_wait_dprx() never sees DP_COMMON_CAP_DPRX_DONE and the
	 * Thunderbolt stack tears the DP tunnel down again.
	 */
	if (!of_property_read_u32(dev->of_node, "mux-index", &mux_index)) {
		int ret;

		/*
		 * Either one crossbar under the old "dp-xbar" name, or one per
		 * Type-C port a tunnelled display can arrive on. Start out on
		 * the first one; dcp_dptx_select_atc() re-points us when the
		 * Thunderbolt side says where the display actually turned up.
		 */
		dcp->xbars[0] = devm_mux_control_get(dev, "dp-xbar");
		if (!IS_ERR(dcp->xbars[0])) {
			dcp->n_xbars = 1;
		} else {
			unsigned int i;

			for (i = 0; i < DCP_MAX_ATC; i++) {
				char name[16];
				struct mux_control *m;

				snprintf(name, sizeof(name), "dp-xbar-atc%u", i);
				m = devm_mux_control_get(dev, name);
				if (IS_ERR(m))
					break;
				dcp->xbars[i] = m;
				dcp->n_xbars = i + 1;
			}
		}
		if (!dcp->n_xbars) {
			dev_err(dev, "Failed to get any dp-xbar mux control\n");
			return -ENODEV;
		}
		dev_info(dev, "%u display crossbar(s) available\n", dcp->n_xbars);

		dcp->xbar = dcp->xbars[0];
		ret = mux_control_select(dcp->xbar, mux_index);
		if (ret)
			dev_warn(dev, "mux_control_select failed: %d\n", ret);

		/*
		 * Switch atcphy to DP-only. should move to a Macbook Pro
		 * 14-/16-inch specific DP-to-HDMI drm_bridge.
		 *
		 * Only for a DCP that owns a Type-C PHY - forcing DP-only on
		 * a port carrying a Thunderbolt tunnel would tear it down.
		 */
		if (dcp->phy) {
			dcp->typec_mux = fwnode_typec_mux_get(dev_fwnode(dcp->dev));
			if (!IS_ERR_OR_NULL(dcp->typec_mux)) {
				struct typec_altmode alt = {
					.svid = USB_TYPEC_DP_SID,
				};
				struct typec_mux_state state = {
					.alt = &alt,
					.mode = TYPEC_DP_STATE_C,
				};
				int ret = typec_mux_set(dcp->typec_mux, &state);
				dev_info(dev, "typec_mux_set() returned: %d\n", ret);
			} else {
				dev_info(dev, "fwnode_typec_mux_get() returned: %ld\n",
						IS_ERR(dcp->typec_mux) ? PTR_ERR(dcp->typec_mux) : 0);
				dcp->typec_mux = NULL;
			}
		}
	}

	return component_add(&pdev->dev, &dcp_comp_ops);
}

static void dcp_platform_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dcp_comp_ops);
}

static void dcp_platform_shutdown(struct platform_device *pdev)
{
	component_del(&pdev->dev, &dcp_comp_ops);
}

static int dcp_platform_suspend(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (dcp->avep)
		av_service_disconnect(dcp);

	if (dcp->hdmi_hpd_irq) {
		disable_irq(dcp->hdmi_hpd_irq);
		disconnected_hpd_event(dcp->connector);
		dcp_dptx_disconnect(dcp, 0);
	}
	/*
	 * Set the device as a wakeup device, which forces its power
	 * domains to stay on. We need this as we do not support full
	 * shutdown properly yet.
	 */
	device_set_wakeup_path(dev);

	return 0;
}

static int dcp_platform_resume(struct device *dev)
{
	struct apple_dcp *dcp = dev_get_drvdata(dev);

	if (dcp->hdmi_hpd_irq)
		enable_irq(dcp->hdmi_hpd_irq);

	if (dcp->avep)
		av_service_connect(dcp);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dcp_platform_pm_ops,
				dcp_platform_suspend, dcp_platform_resume);


static const struct apple_dcp_hw_data apple_dcp_hw_t6020 = {
	.num_dptx_ports = 1,
};

static const struct apple_dcp_hw_data apple_dcp_hw_t8112 = {
	.num_dptx_ports = 2,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcp = {
	.num_dptx_ports = 0,
};

static const struct apple_dcp_hw_data apple_dcp_hw_dcpext = {
	.num_dptx_ports = 2,
};

static const struct of_device_id of_match[] = {
	{ .compatible = "apple,t6020-dcp", .data = &apple_dcp_hw_t6020,  },
	{ .compatible = "apple,t8112-dcp", .data = &apple_dcp_hw_t8112,  },
	{ .compatible = "apple,dcp",       .data = &apple_dcp_hw_dcp,    },
	{ .compatible = "apple,dcpext",    .data = &apple_dcp_hw_dcpext, },
	{}
};
MODULE_DEVICE_TABLE(of, of_match);

static struct platform_driver apple_platform_driver = {
	.probe		= dcp_platform_probe,
	.remove		= dcp_platform_remove,
	.shutdown	= dcp_platform_shutdown,
	.driver	= {
		.name = "apple-dcp",
		.of_match_table	= of_match,
		.dev_groups = dcp_dev_groups,
		.pm = pm_sleep_ptr(&dcp_platform_pm_ops),
	},
};

void __init dcp_register(void)
{
	platform_driver_register(&apple_platform_driver);
}

void __exit dcp_unregister(void)
{
	platform_driver_unregister(&apple_platform_driver);
}
