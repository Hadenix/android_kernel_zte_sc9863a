#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include "sprd_charge_helper.h"

#include "syv697.h"

#define SYV697_SLAVE_ADDR_WRITE   0xD6
#define SYV697_SLAVE_ADDR_READ    0xD7

static DEFINE_MUTEX(syv697_i2c_access);

static struct syv697 *syv697_data;
static bool otg_enable_flag;

static int charging_hw_init(void);
void syv697_set_chg_current_limit(u32 limit);
static void syv697_dump_regs(void);

static u32 watchdog_en = 0;
static int syv697_en_watchdog(const char *val, struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	pr_info("watchdog enable = 0x%x\n", watchdog_en);
	return 0;
}

module_param_call(watchdog_en, syv697_en_watchdog, param_get_int, &watchdog_en, 0644);


int syv697_write_reg(u8 reg, u8 val)
{
	int ret;

	if (watchdog_en == 0) {
		if (reg == SYV697_REG_01)
			val &= ~SYV_REG01_WDT_RESET_MASK;
	}

	pr_debug("syv697 write reg=0x%x, val=0x%x\n", reg, val);
	mutex_lock(&syv697_i2c_access);
	ret = i2c_smbus_write_byte_data(syv697_data->client, reg, val);
	if (ret < 0)
		dev_err(&syv697_data->client->dev, "error=%d\n", ret);

	mutex_unlock(&syv697_i2c_access);
	return ret;
}

int syv697_read_reg(u8 reg, u8 *dest)
{
	int ret;

	mutex_lock(&syv697_i2c_access);
	ret = i2c_smbus_read_byte_data(syv697_data->client, reg);
	if (ret < 0) {
		dev_err(&syv697_data->client->dev,
			"reg=0x%x, ret=%d\n", reg, ret);
		mutex_lock(&syv697_i2c_access);
		return ret;
	}

	*dest = ret & 0xff;
	mutex_unlock(&syv697_i2c_access);
	return 0;
}

static u8 addr = 0;
static int set_reg_addr(const char *val, struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	pr_info("reg = 0x%x\n", addr);
	return 0;
}

module_param_call(addr, set_reg_addr, param_get_int, &addr, 0644);

static u8 value = 0;
static int set_reg_value(const char *val, struct kernel_param *kp)
{
	int ret = 0;

	ret = param_set_int(val, kp);
	if (ret) {
		pr_err("error setting value %d\n", ret);
		return ret;
	}

	syv697_write_reg(addr, value);
	pr_info("set reg[0x%x]: 0x%x\n", addr, value);
	return 0;
}

static int get_reg_value(const char *val, struct kernel_param *kp)
{
	syv697_read_reg(addr, &value);
	pr_info("get reg[0x%x]: 0x%x\n", addr, value);
	return 0;
}

module_param_call(value, set_reg_value, get_reg_value, &value, 0644);

static void syv697_set_value(u8 reg, u8 reg_bit,
			      u8 reg_shift, u8 val)
{
	u8 tmp;

	syv697_read_reg(reg, &tmp);
	tmp = (tmp & (~reg_bit)) | (val << reg_shift);
	syv697_write_reg(reg, tmp);
}

/* Caution to feed watch dog! it can cause syv697 watchdog expire,
*	This is the bug of syv697 and can be reproduced in it's DEMO,
*	1. One way to avoid it is forbit watchdog and NOT reset watchdog timer,
*	it means NOT to set bit 6 of REG 1
*	2. The other way is after reset watchdog timer, then re-write iusb
*/
void syv697_reset_timer(void)
{
	u8 data;

	if (watchdog_en) {
		syv697_read_reg(SYV697_REG_00, &data);
		pr_info("### syv697 reg[0] 0x%x, now feed watchdog\n", data);

		syv697_set_value(SYV697_REG_01, SYV_REG01_WDT_RESET_MASK,
				  SYV_REG01_WDT_RESET_SHIFT, SYV_REG01_WDT_RESET);

		syv697_set_chg_current_limit(data & SYV_REG00_IINLIM_MASK);
	} else
		pr_info("syv697 watchdog disabled, not feed it!\n");

	syv697_dump_regs();
}

void syv697_sw_reset(void)
{
	syv697_set_value(SYV697_REG_01, SYV_REG01_RESET_MASK,
			  SYV_REG01_RESET_SHIFT, SYV_REG01_RESET);
}

void syv697_set_watchdog(u8 reg_val)
{
	syv697_set_value(SYV697_REG_05, SYV_REG05_WDT_MASK,
			  SYV_REG05_WDT_SHIFT, reg_val);
}

void syv697_set_vindpm(u8 reg_val)
{
	syv697_set_value(SYV697_REG_00, SYV_REG00_VINDPM_MASK,
			  SYV_REG00_VINDPM_SHIFT, reg_val);
}

void syv697_termina_cur_set(u8 reg_val)
{
	syv697_set_value(SYV697_REG_03, SYV_REG03_ITERM_MASK,
			  SYV_REG03_ITERM_SHIFT, reg_val);
}

void syv697_termina_vol_set(u8 reg_val)
{
	syv697_set_value(SYV697_REG_04, SYV_REG04_VREG_MASK,
			  SYV_REG04_VREG_SHIFT, reg_val);
}

void syv697_init(void)
{
	pr_info("syv697 init called!\n");
	syv697_sw_reset();
	charging_hw_init();
}

void syv697_otg_enable(int enable)
{
	if (enable) {
		otg_enable_flag = true;
		syv697_set_value(SYV697_REG_01, SYV_REG01_OTG_CONFIG_MASK,
				  SYV_REG01_OTG_CONFIG_SHIFT, SYV_REG01_OTG_ENABLE);
		schedule_delayed_work(&syv697_data->vbus_detect_work,
				      msecs_to_jiffies(500));
	} else {
		otg_enable_flag = false;
		cancel_delayed_work_sync(&syv697_data->vbus_detect_work);
		syv697_set_value(SYV697_REG_01, SYV_REG01_OTG_CONFIG_MASK,
				  SYV_REG01_OTG_CONFIG_SHIFT, SYV_REG01_OTG_DISABLE);
	}
}

void syv697_stop_charging(u32 flag)
{

	syv697_set_value(SYV697_REG_01, SYV_REG01_CHG_CONFIG_MASK,
			  SYV_REG01_CHG_CONFIG_SHIFT, SYV_REG01_CHG_DISABLE);
}

void syv697_enable_chg(void)
{
	syv697_set_value(SYV697_REG_01, SYV_REG01_CHG_CONFIG_MASK,
			  SYV_REG01_CHG_CONFIG_SHIFT, SYV_REG01_CHG_ENABLE);
}

u8 syv697_get_fault_val(void)
{
	u8 data;

	syv697_read_reg(SYV697_REG_09, &data);
	return data;
}

void syv697_set_chg_current_limit(u32 limit)
{
	syv697_set_value(SYV697_REG_00, SYV_REG00_IINLIM_MASK,
			  SYV_REG00_IINLIM_SHIFT, limit);
}

void syv697_set_chg_current(u8 reg_val)
{
	syv697_set_value(SYV697_REG_02, SYV_REG02_ICHG_MASK,
			  SYV_REG02_ICHG_SHIFT, reg_val);
}

void syv697_set_ship_mode(void)
{
	syv697_set_value(SYV697_REG_05, SYV_REG05_WDT_MASK,
			SYV_REG05_WDT_SHIFT, SYV_REG05_WDT_DISABLE);

	syv697_set_value(SYV697_REG_07, SYV_REG07_BATFET_DIS_MASK,
			SYV_REG07_BATFET_DIS_SHIFT, SYV_REG07_BATFET_OFF);
}

void syv697_set_sys_min(u8 reg_val)
{
	syv697_set_value(SYV697_REG_01, SYV_REG01_SYS_MINV_MASK,
			SYV_REG01_SYS_MINV_SHIFT, reg_val);
}

void syv697_set_iprechg(u8 reg_val)
{
	syv697_set_value(SYV697_REG_03, SYV_REG03_IPRECHG_MASK,
			SYV_REG03_IPRECHG_SHIFT, reg_val);
}

int syv697_get_ship_mode(void)
{
	u8 data = 0;

	syv697_read_reg(SYV697_REG_07, &data);
	data = (data & 0x20) >> SYV_REG07_BATFET_DIS_SHIFT;

	return !data;
}

int syv697_get_charge_status(void)
{
	u8 data = 0;
	u8 bit4 = 0, bit5 = 0;

	syv697_read_reg(SYV697_REG_01, &data);
	bit4 = (data & 0x10) >> SYV_REG01_CHG_CONFIG_SHIFT;
	bit5 = (data & 0x20) >> (SYV_REG01_CHG_CONFIG_SHIFT + 1);
	return (bit4 & !bit5);
}

u8 syv697_get_chgcur(void)
{
	u8 data;

	syv697_read_reg(SYV697_REG_02, &data);
	data = (data & SYV_REG02_ICHG_MASK) >> SYV_REG02_ICHG_SHIFT;

	return data;
}

u8 syv697_get_sys_status(void)
{
	u8 data;

	syv697_read_reg(SYV697_REG_08, &data);
	data = (data & SYV_REG08_CHRG_STAT_MASK) >> SYV_REG08_CHRG_STAT_SHIFT;

	return data;
}

static void syv697_dump_regs(void)
{
	int i;
	u8 value[11];

	for (i = 0; i < ARRAY_SIZE(value); i++) {
		syv697_read_reg(i, &(value[i]));
		if (i == ARRAY_SIZE(value) - 1) {
			pr_info("####### syv697 debug: 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x\n",
				value[0], value[1], value[2], value[3], value[4], value[5],
				value[6], value[7], value[8], value[9], value[10]);
		}
	}
}

static void syv697_vbus_det_work(struct work_struct *work)
{
	if (!syv697_data->vbus_detect) {
		if (!gpiod_get_value(syv697_data->vbus_detect) &&
			otg_enable_flag == true)
			syv697_set_value(SYV697_REG_01, SYV_REG01_OTG_CONFIG_MASK,
					  SYV_REG01_OTG_CONFIG_SHIFT,
					  SYV_REG01_OTG_ENABLE);

		schedule_delayed_work(&syv697_data->vbus_detect_work,
				      msecs_to_jiffies(1500));
	}
}

static ssize_t set_regs_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long set_value;
	int reg, val, ret;

	if (kstrtoul(buf, 16, &set_value))
		return -EINVAL;

	reg = (set_value & 0xff00) >> 8;
	val = set_value & 0xff;
	dev_dbg(dev, "set reg=0x%x, value=%d\n", reg, val);

	ret = syv697_write_reg(reg, val);
	if (ret < 0) {
		dev_err(dev, "set_regs_store error\n");
		return ret;
	}

	return count;
}
static DEVICE_ATTR_WO(set_regs);

static ssize_t dump_regs_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	const int regaddrs[] = {0x00, 0x01, 0x02, 0x03, 0x4,
		0x05, 0x06, 0x07, 0x08, 0x09, 0x0a};
	const char str[] = "0123456789abcdef";
	u8 syv697_regs[0x60];
	int i, index;
	char val = 0;

	for (i = 0; i < 0x60; i++) {
		if ((i % 3) == 2)
			buf[i] = ' ';
		else
			buf[i] = 's';
	}
	buf[0x5d] = '\n';
	buf[0x5e] = 0;
	buf[0x5f] = 0;

	for (i = 0; i < ARRAY_SIZE(regaddrs); i++)
		syv697_read_reg(i, &syv697_regs[i]);

	for (i = 0; i < ARRAY_SIZE(regaddrs); i++) {
		index = regaddrs[i];
		val = syv697_regs[index];
		buf[3 * index] = str[(val & 0xf0) >> 4];
		buf[3 * index + 1] = str[val & 0x0f];
	}

	return 0x60;
}
static DEVICE_ATTR_RO(dump_regs);

static struct attribute *syv697_class_attrs[] = {
	&dev_attr_dump_regs.attr,
	&dev_attr_set_regs.attr,
	NULL,
};
ATTRIBUTE_GROUPS(syv697_class);

static int charging_hw_init(void)
{
	if (watchdog_en)
		syv697_set_watchdog(0x3);	/* reg05, enable watchdog, 160s*/
	else
		syv697_set_watchdog(0x0);	/* reg05, disable watchdog*/
	syv697_set_sys_min(0x3);		/* reg01, Minimum system voltage 3.3V */
	syv697_set_iprechg(0x0);		/* reg03, Precharge current 128mA */
	syv697_termina_cur_set(0x0);	/* reg03, Charge terminal current 128mA */

	return 0;
}

int syv697_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct sprd_ext_ic_operations *syv697_ops = NULL;
	int ret;

	pr_info("syv697 probe enter\n");
	syv697_data = devm_kzalloc(&client->dev, sizeof(struct syv697),
				    GFP_KERNEL);
	if (!syv697_data)
		return -ENOMEM;
	syv697_data->client = client;

	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_SMBUS_BYTE_DATA)) {
		dev_err(&client->dev, "i2c check functionality error.\n");
		kfree(syv697_data);
		return -ENODEV;
	}

	syv697_data->vbus_detect =
			devm_gpiod_get(&client->dev, "vbus-det", 0);
	if (IS_ERR(syv697_data->vbus_detect)) {
		dev_err(&client->dev, "unable to claim vbus-det-gpios\n");
		kfree(syv697_data);
		return PTR_ERR(syv697_data->vbus_detect);
	}

	INIT_DELAYED_WORK(&syv697_data->vbus_detect_work,
			  syv697_vbus_det_work);

	ret = sysfs_create_group(&client->dev.kobj, syv697_class_groups[0]);
	if (ret)
		dev_warn(&client->dev, "failed to create syv697 sysfs attr\n");

	charging_hw_init();

	syv697_ops = sprd_get_syv697_ops();
	if (!syv697_ops) {
		kfree(syv697_data);
		return -EINVAL;
	}

	sprdbat_register_ext_ops(syv697_ops);
	pr_info("syv697 probe success\n");
	return 0;
}

/*
static int syv697_remove(struct i2c_client *client)
{
	flush_scheduled_work();
	sysfs_remove_group(&client->dev.kobj, syv697_class_groups[0]);
	return 0;
}

static const struct i2c_device_id i2c_id_table[] = {
	{ "syv697", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, i2c_id_table);

static const struct of_device_id of_id_table[] = {
	{ .compatible = "silergy,syv697-charger"},
	{},
};
MODULE_DEVICE_TABLE(of, of_id_table);

static struct i2c_driver syv697_i2c_driver = {
	.driver = {
		.name = "syv697",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(of_id_table),
	},
	.probe = syv697_probe,
	.remove = syv697_remove,
	.id_table = i2c_id_table,
};

static int syv697_i2c_init(void)
{
	pr_info("%s enter\n", __func__);
	return i2c_add_driver(&syv697_i2c_driver);
}

static void syv697_i2c_exit(void)
{
	pr_info("%s enter\n", __func__);
	i2c_del_driver(&syv697_i2c_driver);
}

subsys_initcall_sync(syv697_i2c_init);
module_exit(syv697_i2c_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("SYV697 Charger driver");
MODULE_AUTHOR("jiang.zhifeng1@zte.com.cn>");
MODULE_VERSION("1.0.0");
*/

