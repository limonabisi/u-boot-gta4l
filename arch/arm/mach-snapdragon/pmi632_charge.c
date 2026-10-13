// SPDX-License-Identifier: GPL-2.0+
/*
 * Keep a PMI632 charging while U-Boot runs, and show it on screen.
 *
 * The boot firmware leaves the charger's watchdog armed; nothing pets it
 * while U-Boot sits in its menu, so charging switches off about two
 * minutes after the cable goes in. Pet it, switch charging back on if it
 * was bitten already, and let a wall charger draw 1.5 A instead of the
 * USB 2.0 default of 500 mA (the stock settings of the Galaxy Tab A7).
 *
 * A battery symbol in the top right corner of the (possibly rotated)
 * screen shows the charger state: a lightning bolt while charging, filled
 * when the charger reports the battery full, nothing without a cable.
 */

#define LOG_CATEGORY LOGC_BOARD

#include <cyclic.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <log.h>
#include <power/pmic.h>
#include <video.h>
#include <linux/delay.h>

#define CHGR_STATUS_1			0x1006
#define  CHGR_STATUS_MASK		GENMASK(2, 0)
#define  CHGR_STATUS_TERMINATE		5
#define  CHGR_STATUS_INHIBIT		6
#define  CHGR_STATUS_DISABLED		7
#define CHGR_CHARGING_ENABLE_CMD	0x1042
#define  CHARGING_ENABLE_CMD_BIT	BIT(0)
#define DCDC_CMD_OTG			0x1140
#define  OTG_EN_BIT			BIT(0)
#define USBIN_INPUT_STATUS		0x1306
#define  USBIN_PRESENT_MASK		GENMASK(1, 0)
#define APSD_STATUS			0x1307
#define  APSD_DTC_DONE_BIT		BIT(0)
#define APSD_RESULT_STATUS		0x1308
#define  APSD_SDP_BIT			BIT(0)
#define  APSD_OCP_BIT			BIT(1)
#define  APSD_CDP_BIT			BIT(2)
#define  APSD_DCP_BIT			BIT(3)
#define  APSD_FLOAT_BIT			BIT(4)
#define  APSD_QC2_BIT			BIT(5)
#define  APSD_QC3_BIT			BIT(6)
#define USBIN_INT_RT_STS		0x1310
#define  USBIN_PLUGIN_RT_STS_BIT	BIT(4)
#define CMD_ICL_OVERRIDE		0x1342
#define  ICL_OVERRIDE_BIT		BIT(0)
#define USBIN_OPTIONS_2_CFG		0x1363
#define  FLOAT_OPTIONS_MASK		GENMASK(2, 0)
#define USBIN_LOAD_CFG			0x1365
#define  ICL_OVERRIDE_AFTER_APSD_BIT	BIT(4)
#define USBIN_ICL_OPTIONS		0x1366
#define  USBIN_MODE_CHG_BIT		BIT(0)
#define USBIN_CURRENT_LIMIT_CFG		0x1370
#define  ICL_WALL			(1500 / 50)	/* 1.5 A in 50 mA steps */
#define MISC_WDOG_PET			0x1643
#define  WDOG_PET_BIT			BIT(0)

#define POLL_US				(1000 * 1000)
#define PET_EVERY			10	/* polls */

enum chg_state { CHG_NONE, CHG_CHARGING, CHG_FULL, CHG_STOPPED };

static struct cyclic_info chg_cyclic;
static struct udevice *chg_pmic;
static unsigned int chg_polls;
static bool chg_icl_done;

static int rd(uint reg)
{
	return pmic_reg_read(chg_pmic, reg);
}

static void wr(uint reg, uint val)
{
	pmic_reg_write(chg_pmic, reg, val);
}

static void upd(uint reg, uint mask, uint val)
{
	int old = rd(reg);

	if (old >= 0)
		wr(reg, (old & ~mask) | (val & mask));
}

/* A wall charger may draw 1.5 A; a computer port stays at 500 mA */
static void chg_set_icl(void)
{
	int rt = rd(USBIN_INT_RT_STS), otg = rd(DCDC_CMD_OTG);
	int apsd_st, apsd, mode;

	if (rt < 0 || otg < 0 || !(rt & USBIN_PLUGIN_RT_STS_BIT) ||
	    (otg & OTG_EN_BIT)) {
		chg_icl_done = false;
		return;
	}

	apsd_st = rd(APSD_STATUS);
	if (apsd_st < 0 || !(apsd_st & APSD_DTC_DONE_BIT))
		return;
	apsd = rd(APSD_RESULT_STATUS);
	if (apsd < 0 || (apsd & (APSD_SDP_BIT | APSD_CDP_BIT)))
		return;
	if (!(apsd & (APSD_DCP_BIT | APSD_FLOAT_BIT | APSD_OCP_BIT |
		      APSD_QC2_BIT | APSD_QC3_BIT)))
		return;

	mode = rd(USBIN_ICL_OPTIONS);
	if (chg_icl_done && mode >= 0 && (mode & USBIN_MODE_CHG_BIT))
		return;

	upd(USBIN_OPTIONS_2_CFG, FLOAT_OPTIONS_MASK, 0);
	wr(USBIN_CURRENT_LIMIT_CFG, ICL_WALL);
	upd(CMD_ICL_OVERRIDE, ICL_OVERRIDE_BIT, 0);
	upd(USBIN_LOAD_CFG, ICL_OVERRIDE_AFTER_APSD_BIT, 0);
	upd(USBIN_ICL_OPTIONS, USBIN_MODE_CHG_BIT, USBIN_MODE_CHG_BIT);
	log_info("charger: wall charger (apsd 0x%02x), input limit 1500 mA\n", apsd);
	chg_icl_done = true;
}

static void chg_keep_alive(void)
{
	int status, usbin, en;

	wr(MISC_WDOG_PET, WDOG_PET_BIT);

	status = rd(CHGR_STATUS_1);
	usbin = rd(USBIN_INPUT_STATUS);
	en = rd(CHGR_CHARGING_ENABLE_CMD);
	if (status < 0 || usbin < 0 || en < 0)
		return;

	/* Bitten before we got here: a cable is in, yet charging is off */
	if ((status & CHGR_STATUS_MASK) == CHGR_STATUS_DISABLED &&
	    (usbin & USBIN_PRESENT_MASK) && (en & CHARGING_ENABLE_CMD_BIT)) {
		log_info("charger: switched off by its watchdog, restarting it\n");
		upd(CHGR_CHARGING_ENABLE_CMD, CHARGING_ENABLE_CMD_BIT, 0);
		mdelay(10);
		upd(CHGR_CHARGING_ENABLE_CMD, CHARGING_ENABLE_CMD_BIT,
		    CHARGING_ENABLE_CMD_BIT);
	}
}

static enum chg_state chg_state(void)
{
	int rt = rd(USBIN_INT_RT_STS), status = rd(CHGR_STATUS_1);

	if (rt < 0 || status < 0 || !(rt & USBIN_PLUGIN_RT_STS_BIT))
		return CHG_NONE;

	switch (status & CHGR_STATUS_MASK) {
	case CHGR_STATUS_TERMINATE:
	case CHGR_STATUS_INHIBIT:
		return CHG_FULL;
	case CHGR_STATUS_DISABLED:
		return CHG_STOPPED;
	default:
		return CHG_CHARGING;
	}
}

/* --- drawing, in the coordinates of the screen as the console shows it --- */

#define ICON_W		80	/* body, without the terminal */
#define ICON_H		44
#define ICON_LINE	4
#define NUB_W		6
#define NUB_H		18
#define ICON_MARGIN	16

#define COL_BLACK	0x000000
#define COL_WHITE	0xffffff
#define COL_GREEN	0x30d050
#define COL_YELLOW	0xf0c020

/* 10x16 lightning bolt, drawn at twice the size */
static const char *const bolt[] = {
	"......####",
	".....####.",
	"....####..",
	"...####...",
	"..####....",
	".########.",
	"#########.",
	".########.",
	"...####...",
	"..####....",
	"..###.....",
	".###......",
	".##.......",
	"##........",
	"#.........",
	"..........",
};

static void put_px(struct video_priv *vp, int lx, int ly, u32 col)
{
	int x, y;

	switch (vp->rot) {
	case 1:
		x = vp->xsize - 1 - ly;
		y = lx;
		break;
	case 2:
		x = vp->xsize - 1 - lx;
		y = vp->ysize - 1 - ly;
		break;
	case 3:
		x = ly;
		y = vp->ysize - 1 - lx;
		break;
	default:
		x = lx;
		y = ly;
		break;
	}

	if (x < 0 || y < 0 || x >= vp->xsize || y >= vp->ysize)
		return;

	if (vp->bpix == VIDEO_BPP32)
		*(u32 *)(vp->fb + y * vp->line_length + x * 4) = 0xff000000 | col;
	else if (vp->bpix == VIDEO_BPP16)
		*(u16 *)(vp->fb + y * vp->line_length + x * 2) =
			((col >> 8) & 0xf800) | ((col >> 5) & 0x07e0) |
			((col >> 3) & 0x001f);
}

static void fill(struct video_priv *vp, int x0, int y0, int w, int h, u32 col)
{
	int x, y;

	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++)
			put_px(vp, x, y, col);
}

static void chg_draw(enum chg_state st)
{
	static int last = -1;
	struct udevice *vdev;
	struct video_priv *vp;
	int lw, x0, y0, x, y, bx, by;
	u32 col;

	if (uclass_first_device_err(UCLASS_VIDEO, &vdev))
		return;
	vp = dev_get_uclass_priv(vdev);

	/* The menu clears the screen now and then, so redraw every time */
	lw = (vp->rot & 1) ? vp->ysize : vp->xsize;
	x0 = lw - ICON_MARGIN - NUB_W - ICON_W;
	y0 = ICON_MARGIN;

	if (st == CHG_NONE) {
		if (last != CHG_NONE)
			fill(vp, x0, y0, ICON_W + NUB_W, ICON_H, COL_BLACK);
		last = st;
		video_sync(vdev, false);
		return;
	}
	last = st;

	col = st == CHG_STOPPED ? COL_YELLOW : COL_GREEN;

	/* outline and terminal */
	fill(vp, x0, y0, ICON_W, ICON_LINE, col);
	fill(vp, x0, y0 + ICON_H - ICON_LINE, ICON_W, ICON_LINE, col);
	fill(vp, x0, y0, ICON_LINE, ICON_H, col);
	fill(vp, x0 + ICON_W - ICON_LINE, y0, ICON_LINE, ICON_H, col);
	fill(vp, x0 + ICON_W, y0 + (ICON_H - NUB_H) / 2, NUB_W, NUB_H, col);

	/* inside: filled when full, a bolt while charging */
	fill(vp, x0 + ICON_LINE, y0 + ICON_LINE, ICON_W - 2 * ICON_LINE,
	     ICON_H - 2 * ICON_LINE, st == CHG_FULL ? col : COL_BLACK);

	if (st != CHG_FULL) {
		bx = x0 + (ICON_W - 20) / 2;
		by = y0 + (ICON_H - 32) / 2 + 1;
		for (y = 0; y < 16; y++)
			for (x = 0; x < 10; x++)
				if (bolt[y][x] == '#')
					fill(vp, bx + 2 * x, by + 2 * y, 2, 2,
					     st == CHG_STOPPED ? COL_YELLOW : COL_WHITE);
	}

	video_sync(vdev, false);
}

static void chg_cyclic_fn(struct cyclic_info *c)
{
	if (!(chg_polls++ % PET_EVERY))
		chg_keep_alive();
	chg_set_icl();
	chg_draw(chg_state());
}

void qcom_pmi632_charge_init(void)
{
	struct udevice *dev;
	struct uclass *uc;

	uclass_id_foreach_dev(UCLASS_PMIC, dev, uc) {
		if (ofnode_device_is_compatible(dev_ofnode(dev), "qcom,pmi632")) {
			chg_pmic = dev;
			break;
		}
	}
	if (!chg_pmic || device_probe(chg_pmic)) {
		log_debug("charger: no PMI632 found\n");
		chg_pmic = NULL;
		return;
	}

	chg_keep_alive();
	chg_set_icl();
	chg_draw(chg_state());
	cyclic_register(&chg_cyclic, chg_cyclic_fn, POLL_US, "pmi632-charge");
}
