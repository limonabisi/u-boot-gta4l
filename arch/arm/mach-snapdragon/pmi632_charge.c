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
 * screen shows the charger state and the charge in percent: a lightning
 * bolt while charging, "!" when charging stopped, bars for the charge,
 * nothing without a cable.
 */

#define LOG_CATEGORY LOGC_BOARD

#include <cyclic.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <log.h>
#include <power/pmic.h>
#include <video.h>
#include <vsprintf.h>
#include <linux/delay.h>
#include <linux/kernel.h>

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

/*
 * --- state of charge, as the kernel's qcom_qg estimates it at start ---
 *
 * The fuel gauge samples the battery voltage and current; the open circuit
 * voltage (voltage minus the drop over the internal resistance) gives the
 * charge through the stock battery profile, the same table as the kernel
 * device tree.
 */

#define QG_BASE				0x4800
#define QG_PERPH_SUBTYPE		(QG_BASE + 0x05)
#define  QG_ADC_IBAT_5A			0x3
#define QG_LAST_ADC_V_DATA0		(QG_BASE + 0xc0)
#define QG_LAST_ADC_I_DATA0		(QG_BASE + 0xc2)
#define QG_ADC_RESET_VAL		0x8000
#define QG_V_LSB_NV			194637
#define QG_I_LSB_5A_NA			152588
#define QG_I_LSB_10A_NA			305176
#define BATT_RESISTANCE_UOHM		80000
#define SOC_UNKNOWN			-1

/* "P85943-QRD-SCUD-ATL-4V4-7040mAh" at 25 degrees: { OCV in uV, percent } */
static const int ocv_table[][2] = {
	{ 4377700, 100 }, { 4356100, 98 }, { 4332300, 96 }, { 4308500, 94 },
	{ 4284700, 92 }, { 4260800, 90 }, { 4237400, 88 }, { 4214300, 86 },
	{ 4191400, 84 }, { 4168600, 82 }, { 4146200, 80 }, { 4124600, 78 },
	{ 4103500, 76 }, { 4083000, 74 }, { 4062700, 72 }, { 4043200, 70 },
	{ 4024600, 68 }, { 4006700, 66 }, { 3990000, 64 }, { 3975500, 62 },
	{ 3959500, 60 }, { 3936300, 58 }, { 3909600, 56 }, { 3890700, 54 },
	{ 3877400, 52 }, { 3866700, 50 }, { 3858100, 48 }, { 3851000, 46 },
	{ 3843600, 44 }, { 3836000, 42 }, { 3829100, 40 }, { 3823300, 38 },
	{ 3818200, 36 }, { 3813600, 34 }, { 3809800, 32 }, { 3805900, 30 },
	{ 3801900, 28 }, { 3797700, 26 }, { 3791500, 24 }, { 3782300, 22 },
	{ 3771500, 20 }, { 3759800, 18 }, { 3746300, 16 }, { 3729700, 14 },
	{ 3714900, 12 }, { 3706400, 10 }, { 3704000, 9 }, { 3702500, 8 },
	{ 3700600, 7 }, { 3696000, 6 }, { 3671900, 5 }, { 3628200, 4 },
	{ 3570500, 3 }, { 3491700, 2 }, { 3369000, 1 }, { 3000000, 0 },
};

static int rd16(uint reg)
{
	int lo = rd(reg), hi = rd(reg + 1);

	if (lo < 0 || hi < 0)
		return -1;
	return lo | (hi << 8);
}

static int ocv_to_soc(int ocv)
{
	int i, n = ARRAY_SIZE(ocv_table);

	if (ocv >= ocv_table[0][0])
		return 100;
	for (i = 1; i < n; i++) {
		if (ocv >= ocv_table[i][0]) {
			int v0 = ocv_table[i][0], v1 = ocv_table[i - 1][0];
			int c0 = ocv_table[i][1], c1 = ocv_table[i - 1][1];

			return c0 + (ocv - v0) * (c1 - c0) / (v1 - v0);
		}
	}
	return 0;
}

/* Returns 0-100, or SOC_UNKNOWN without a usable sample */
static int chg_soc(void)
{
	static int smooth = SOC_UNKNOWN;
	int sub = rd(QG_PERPH_SUBTYPE);
	int rv = rd16(QG_LAST_ADC_V_DATA0), ri = rd16(QG_LAST_ADC_I_DATA0);
	long long uv, ua;
	int soc;

	if (sub < 0 || rv < 0 || ri < 0 || rv == QG_ADC_RESET_VAL ||
	    ri == QG_ADC_RESET_VAL)
		return smooth;

	uv = (long long)rv * QG_V_LSB_NV / 1000;
	if (uv < 2000000 || uv > 5000000)
		return smooth;

	/* The gauge counts current out of the battery as positive */
	ua = (long long)(s16)ri *
	     (sub == QG_ADC_IBAT_5A ? QG_I_LSB_5A_NA : QG_I_LSB_10A_NA) / 1000;

	/* OCV = V + I*R with I flowing out: charging (negative) pulls it down */
	soc = ocv_to_soc(uv + ua * BATT_RESISTANCE_UOHM / 1000000);

	/* The samples are noisy; smooth them over about eight seconds */
	smooth = smooth == SOC_UNKNOWN ? soc : (smooth * 7 + soc + 4) / 8;

	return smooth;
}

/* --- drawing, in the coordinates of the screen as the console shows it --- */

#define SIGN_W		20	/* bolt or "!" left of the battery */
#define SIGN_GAP	2
#define ICON_W		64	/* body, without the terminal */
#define ICON_H		40
#define ICON_LINE	4
#define NUB_W		6
#define NUB_H		16
#define BARS		4
#define BAR_W		10
#define BAR_STEP	13
#define ICON_MARGIN	16
#define TXT_SCALE	4	/* 5x7 digits drawn 4 times as large */
#define TXT_ADV		(6 * TXT_SCALE)
#define TXT_GAP		10	/* between the percentage and the sign */
#define TXT_W		(4 * TXT_ADV)	/* room for "100%" */

#define COL_BLACK	0x000000
#define COL_WHITE	0xffffff
#define COL_GREEN	0x30d050
#define COL_YELLOW	0xf0c020
#define COL_DIM		0x303030

/* 4x16 exclamation mark, drawn at twice the size */
static const char *const excl[] = {
	".##.", "####", "####", "####", "####", "####", "####", "####",
	".##.", ".##.", ".##.", "....", "....", "####", "####", "####",
};

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

/* 5x7 digits 0-9 and "%" */
static const char *const digits[11][7] = {
	{ ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." },
	{ "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." },
	{ ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" },
	{ "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." },
	{ "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." },
	{ "#####", "#....", "####.", "....#", "....#", "#...#", ".###." },
	{ "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." },
	{ "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." },
	{ ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." },
	{ ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." },
	{ "##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##" },
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

/*
 * The pixels are written straight into the frame buffer, so tell the video
 * uclass which part changed; with CONFIG_VIDEO_DAMAGE only damaged areas are
 * flushed from the cache to the display.
 */
static void damage(struct udevice *vdev, struct video_priv *vp, int lx, int ly,
		   int w, int h)
{
	switch (vp->rot) {
	case 1:
		video_damage(vdev, vp->xsize - (ly + h), lx, h, w);
		break;
	case 2:
		video_damage(vdev, vp->xsize - (lx + w), vp->ysize - (ly + h), w, h);
		break;
	case 3:
		video_damage(vdev, ly, vp->ysize - (lx + w), h, w);
		break;
	default:
		video_damage(vdev, lx, ly, w, h);
		break;
	}
}

static void glyph(struct video_priv *vp, const char *const *g, int w, int h,
		  int x0, int y0, int s, u32 col)
{
	int x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			if (g[y][x] == '#')
				fill(vp, x0 + s * x, y0 + s * y, s, s, col);
}

/* "NN%" right-aligned so that it ends at xe */
static void draw_soc(struct video_priv *vp, int xe, int y0, int soc, u32 col)
{
	char txt[5];
	int i, n, x;

	n = snprintf(txt, sizeof(txt), "%d%%", soc);
	x = xe - n * TXT_ADV + TXT_SCALE;
	for (i = 0; i < n; i++, x += TXT_ADV)
		glyph(vp, digits[txt[i] == '%' ? 10 : txt[i] - '0'], 5, 7, x,
		      y0 + (ICON_H - 7 * TXT_SCALE) / 2, TXT_SCALE, col);
}

/*
 * The charge in percent, a sign and a battery of four bars: bolt while
 * charging, "!" when charging stopped; the bars follow the percentage
 * (all four when the charger reports the battery full).
 */
static void chg_draw(enum chg_state st, int soc)
{
	static int last = -1;
	struct udevice *vdev;
	struct video_priv *vp;
	int lw, x0, y0, bx, tx, i, lit, area;
	u32 col;

	if (uclass_first_device_err(UCLASS_VIDEO, &vdev))
		return;
	vp = dev_get_uclass_priv(vdev);

	/* The menu clears the screen now and then, so redraw every time */
	lw = (vp->rot & 1) ? vp->ysize : vp->xsize;
	x0 = lw - ICON_MARGIN - NUB_W - ICON_W - SIGN_GAP - SIGN_W;
	y0 = ICON_MARGIN;
	bx = x0 + SIGN_W + SIGN_GAP;
	tx = x0 - TXT_GAP - TXT_W;
	area = TXT_W + TXT_GAP + SIGN_W + SIGN_GAP + ICON_W + NUB_W;

	if (st == CHG_NONE) {
		if (last != CHG_NONE) {
			fill(vp, tx, y0, area, ICON_H, COL_BLACK);
			damage(vdev, vp, tx, y0, area, ICON_H);
		}
		last = st;
		video_sync(vdev, false);
		return;
	}
	last = st;

	if (st == CHG_FULL)
		soc = 100;

	col = st == CHG_STOPPED ? COL_YELLOW : COL_GREEN;

	/* percentage */
	fill(vp, tx, y0, TXT_W + TXT_GAP, ICON_H, COL_BLACK);
	if (soc != SOC_UNKNOWN)
		draw_soc(vp, x0 - TXT_GAP, y0, soc, COL_WHITE);

	/* sign */
	fill(vp, x0, y0, SIGN_W, ICON_H, COL_BLACK);
	if (st == CHG_CHARGING)
		glyph(vp, bolt, 10, 16, x0, y0 + (ICON_H - 32) / 2, 2, COL_WHITE);
	else if (st == CHG_STOPPED)
		glyph(vp, excl, 4, 16, x0 + 6, y0 + (ICON_H - 32) / 2, 2, COL_YELLOW);

	/* outline and terminal */
	fill(vp, bx, y0, ICON_W, ICON_LINE, col);
	fill(vp, bx, y0 + ICON_H - ICON_LINE, ICON_W, ICON_LINE, col);
	fill(vp, bx, y0, ICON_LINE, ICON_H, col);
	fill(vp, bx + ICON_W - ICON_LINE, y0, ICON_LINE, ICON_H, col);
	fill(vp, bx + ICON_W, y0 + (ICON_H - NUB_H) / 2, NUB_W, NUB_H, col);

	/* bars: one per started quarter; without a reading, the charger state */
	if (soc != SOC_UNKNOWN)
		lit = clamp((soc + 24) / 25, 1, BARS);
	else
		lit = st == CHG_FULL ? BARS : st == CHG_STOPPED ? 1 : BARS - 1;
	for (i = 0; i < BARS; i++)
		fill(vp, bx + 2 * ICON_LINE + i * BAR_STEP, y0 + 6, BAR_W,
		     ICON_H - 12, i < lit ? col : COL_DIM);

	damage(vdev, vp, tx, y0, area, ICON_H);

	video_sync(vdev, false);
}

static void chg_cyclic_fn(struct cyclic_info *c)
{
	if (!(chg_polls++ % PET_EVERY))
		chg_keep_alive();
	chg_set_icl();
	chg_draw(chg_state(), chg_soc());
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
	chg_draw(chg_state(), chg_soc());
	cyclic_register(&chg_cyclic, chg_cyclic_fn, POLL_US, "pmi632-charge");
}
