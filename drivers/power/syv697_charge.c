/* ZTE License-Identifier: GPL-2.0
 *
 * Charger device driver for SYV697
 *
 * Copyright (c) 2018 ZTE.
 */

#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/param.h>
#include <linux/stat.h>
#include <linux/string.h>
#include "sprd_battery.h"
#include "syv697.h"

static struct sprdbat_drivier_data *bat_drv_data;

static void chg_syv697_set_vindpm(int vin)
{
	u8 reg_val;

	if (vin < 3880)
		reg_val = 0x0;
	else if (vin > 5080)
		reg_val = 0x0f;
	else
		reg_val = (vin - 3880) / 80;

	syv697_set_vindpm(reg_val);
}

static void chg_syv697_termina_cur_set(u32 cur)
{
	u8 reg_value;

	if (cur <= 128)
		reg_value = 0x0;
	else if (cur >= 2048)
		reg_value = 0xf;
	else
		reg_value = (cur - 128) / 128;

	syv697_termina_cur_set(reg_value);
}

static void chg_syv697_termina_vol_set(u32 vol)
{
	u8 reg_value;

	if (vol < SYV_REG04_VREG_BASE)
		vol = SYV_REG04_VREG_BASE;
	else if (vol > SYV_REG04_VREG_MAX)
		vol = SYV_REG04_VREG_MAX;

	reg_value = (vol - SYV_REG04_VREG_BASE) / SYV_REG04_VREG_LSB;
	syv697_termina_vol_set(reg_value);
}

static void chg_syv697_reset_timer(void)
{
	syv697_reset_timer();
}

static void chg_syv697_otg_enable(int enable)
{
	syv697_otg_enable(enable);
}

static void chg_syv697_stop_charging(u32 flag)
{
	syv697_stop_charging(flag);
}

static int chg_syv697_get_charge_status(void)
{
	u8 chg_status;

	chg_status = syv697_get_sys_status();

	if (chg_status == SYV_REG08_CHRG_STAT_IDLE)
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	else if (chg_status == SYV_REG08_CHRG_STAT_FASTCHG)
		return POWER_SUPPLY_STATUS_CHARGING;
	else
		return POWER_SUPPLY_STATUS_CHARGING;
}

static u32 chg_syv697_get_chgcur(void)
{
	int cur;
	u8 reg_val = syv697_get_chgcur();

	cur = (reg_val * SYV_REG02_ICHG_LSB);
	return cur;
}

static int chg_syv697_get_charge_fault(void)
{
	u8 reg_val, fault_val;
	u8 ret = 0;

	reg_val = syv697_get_fault_val();

	fault_val = (reg_val & SYV_REG09_FAULT_WDT_MASK) >> SYV_REG09_FAULT_WDT_SHIFT;

	if (fault_val == SYV_REG09_FAULT_WDT)
		ret |= SPRDBAT_CHG_END_UNSPEC;

	fault_val = (reg_val & SYV_REG09_FAULT_BOOST_MASK) >>
			SYV_REG09_FAULT_BOOST_SHIFT;
	if (fault_val == SYV_REG09_FAULT_BOOT)
		ret |= SPRDBAT_CHG_END_OVP_BIT;

	fault_val = (reg_val & SYV_REG09_FAULT_BAT_MASK) >> SYV_REG09_FAULT_BAT_SHIFT;
	if (fault_val == SYV_REG09_FAULT_BAT_OVP)
		ret |= SPRDBAT_CHG_END_BAT_OVP_BIT;

	fault_val = (reg_val & SYV_REG09_FAULT_CHRG_MASK) >> SYV_REG09_FAULT_CHRG_SHIFT;
	switch (fault_val) {
	case SYV_REG09_FAULT_CHRG_TIMER:
		ret |= SPRDBAT_CHG_END_TIMEOUT_BIT;
		break;
	case SYV_REG09_FAULT_CHRG_THERMAL:
		ret |= SPRDBAT_CHG_END_UNSPEC;
		break;
	case SYV_REG09_FAULT_CHRG_INPUT:
		ret |= SPRDBAT_CHG_END_UNSPEC;
		break;
	default:
		ret |= SPRDBAT_CHG_END_NONE_BIT;
		break;
	}

	return ret;
}

static u8 chg_syv697_cur2reg(u32 cur)
{
	u8 reg_val;

	if (cur >= 2048) {
		reg_val = 0x60;
	} else if (cur <= 512) {
		reg_val = 0x0;
	} else {
		reg_val = (cur - SYV_REG02_ICHG_BASE) / SYV_REG02_ICHG_LSB;
		reg_val &= (SYV_REG02_ICHG_MASK >> SYV_REG02_ICHG_SHIFT);
	}

	return reg_val;
}

static void chg_syv697_set_chg_cur(u32 cur)
{
	u8 reg_val;

	reg_val = chg_syv697_cur2reg(cur);
	syv697_set_chg_current(reg_val);
}

static u8 chg_syv697_iusb_cur2reg(u32 cur)
{
	u8 reg_val;

	if (cur >= 3000)
		reg_val = 0x7;
	else if (cur >= 2000)
		reg_val = 0x6;
	else if (cur >= 1500)
		reg_val = 0x5;
	else if (cur >= 1000)
		reg_val = 0x4;
	else if (cur >= 700)
		reg_val = 0x3;
	else if (cur >= 500)
		reg_val = 0x2;
	else if (cur >= 150)
		reg_val = 0x1;
	else
		reg_val = 0x0;
	return reg_val;
}
static void chg_syv697_chgr_cur_limit(u32 limit)
{
	u8 reg_value;

	pr_info("syv697: set cur limit %d\n", limit);
	reg_value = chg_syv697_iusb_cur2reg(limit);
	syv697_set_chg_current_limit(reg_value);
}

static void chg_syv697_enable_chg(void)
{
	syv697_enable_chg();
}

static void chg_syv697_init(struct sprdbat_drivier_data *bdata)
{
	if (bdata == NULL)
		return;
	bat_drv_data = bdata;
	syv697_init();
	chg_syv697_set_vindpm(bat_drv_data->pdata->chg_end_vol_pure);
	chg_syv697_termina_cur_set(bat_drv_data->pdata->chg_end_cur);
	chg_syv697_termina_vol_set(bat_drv_data->pdata->chg_end_vol_pure);
}

static void chg_syv697_set_ship_mode(int enable)
{
	if (enable == 0x0)
		syv697_set_ship_mode();
	else
		pr_info("ship_mode counld not parse input value: %d\n", enable);
}

static int chg_syv697_get_ship_mode(void)
{
	int ret = 0;

	ret = syv697_get_ship_mode();

	return ret;
}

static int chg_syv697_get_enable_status(void)
{
	int ret = 0;

	ret = syv697_get_charge_status();

	return ret;
}

struct sprd_ext_ic_operations syv697_op = {
	.ic_init = chg_syv697_init,
	.charge_start_ext = chg_syv697_enable_chg,
	.set_charge_cur = chg_syv697_set_chg_cur,
	.charge_stop_ext = chg_syv697_stop_charging,
	.get_charge_cur_ext = chg_syv697_get_chgcur,
	.get_charging_status = chg_syv697_get_charge_status,
	.get_charging_fault = chg_syv697_get_charge_fault,
	.timer_callback_ext = chg_syv697_reset_timer,
	.set_termina_cur_ext = chg_syv697_termina_cur_set,
	.set_termina_vol_ext = chg_syv697_termina_vol_set,
	.otg_charge_ext = chg_syv697_otg_enable,
	.set_input_cur_limit = chg_syv697_chgr_cur_limit,
	.set_ship_mode =  chg_syv697_set_ship_mode,
	.get_ship_mode =  chg_syv697_get_ship_mode,
	.get_charge_status = chg_syv697_get_enable_status,
};

struct sprd_ext_ic_operations *sprd_get_syv697_ops(void)
{
	return &syv697_op;
}
