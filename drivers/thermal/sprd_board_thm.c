/*
 * Copyright (C) 2016 Spreadtrum Communications Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/io.h>
#include <linux/iio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/uidgid.h>
#include <linux/uaccess.h>
#include "sprd_thm.h"


#define FEATURE_THERMAL_CLEAR
#ifdef FEATURE_THERMAL_CLEAR

/* signal */
#define MAX_LEN	128
#define MAX_LEVEL	3

struct temp_clr_config {
	int level[MAX_LEVEL][2]; /*it has MAX_LEVEL level, every level has set temp and clr temp*/
	int tmc_state[MAX_LEVEL];/*ervry level tmc_state = 1 when set, tmc_state = 0 when clr   */
};

static unsigned int tmc_pid = 0;
static unsigned int tmc_input_pid = 0;
static struct task_struct *ptmc_task = NULL;
static struct temp_clr_config *tmc_config = NULL;
static kuid_t uid = KUIDT_INIT(0);
static kgid_t gid = KGIDT_INIT(1000);

/*
 * use "si_code" for Action identify
 * for tmc_pid (/system/bin/thermald)
 */

static struct temp_clr_config *sprd_temp_clr_parse_dt(struct device *dev)
{
	int ret = 0;
	int temp = 0, i = 0;
	struct temp_clr_config *pconfig;
	struct device_node *np = dev->of_node;

	ret = of_property_read_u32(np, "level0_set", &temp);
	if (ret) {
		dev_err(dev, "fail to get thermal clear temp setting\n");
		return NULL;
	}

	pconfig = devm_kzalloc(dev, sizeof(*pconfig), GFP_KERNEL);
	if (!pconfig)
		return NULL;

	for (i = 0; i < MAX_LEVEL; i++) {
		char str[20] = { 0 };

		snprintf(str, sizeof("level0_set"), "level%d_set", i);
		ret = of_property_read_u32(np, str, &(pconfig->level[i][0]));
		if (ret) {
			dev_err(dev, "fail to get level %d set temp\n", i);
			devm_kfree(dev, pconfig);
			pconfig = NULL;
			return NULL;
		}
		snprintf(str, sizeof("level0_set"), "level%d_clr", i);
		ret = of_property_read_u32(np, str, &(pconfig->level[i][1]));
		if (ret) {
			dev_err(dev, "fail to get level %d clr temp\n", i);
			devm_kfree(dev, pconfig);
			pconfig = NULL;
			return NULL;
		}
		pconfig->tmc_state[i] = 0;
		pr_info("board temp ctrl level%d config is %d, %d\n", i,
			pconfig->level[i][0], pconfig->level[i][1]);
	}
	return pconfig;
}

static int send_tmc_signal(int level)
{
	int ret = 0;
	static int clear_state = 1;

	if (clear_state == level)
		return ret;

	if (tmc_input_pid == 0) {
		pr_info("%s pid is empty\n", __func__);
		ret = -1;
	}

	pr_info(" %s pid is %d, %d; MD_Alert: %d\n", __func__,
							tmc_pid, tmc_input_pid, level);

	if (ret == 0 && tmc_input_pid != tmc_pid) {
		tmc_pid = tmc_input_pid;

		if (ptmc_task != NULL)
			put_task_struct(ptmc_task);
		ptmc_task = get_pid_task(find_vpid(tmc_pid), PIDTYPE_PID);
	}

	if (ret == 0 && ptmc_task) {
		siginfo_t info;

		info.si_signo = SIGIO;
		info.si_errno = 0;
		info.si_code = level;
		info.si_addr = NULL;
		ret = send_sig_info(SIGIO, &info, ptmc_task);
	}

	if (ret != 0)
		pr_info(" %s ret=%d\n", __func__, ret);
	else {
			clear_state = level;
	}

	return ret;
}

static ssize_t tmc_pid_write(struct file *filp, const char __user *buf, size_t count,
				    loff_t *data)
{
	int ret = 0;
	char tmp[MAX_LEN] = { 0 };
	int len = 0;

	len = (count < (MAX_LEN - 1)) ? count : (MAX_LEN - 1);
	/* write data to the buffer */
	if (copy_from_user(tmp, (void *)buf, len))
		return -EFAULT;

	ret = kstrtouint(tmp, 10, &tmc_input_pid);
	if (ret)
		WARN_ON_ONCE(1);

	pr_info("%s %s = %d\n", __func__, tmp, tmc_input_pid);

	return len;
}

static int tmc_pid_read(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", tmc_input_pid);
	pr_info("%s %d\n", __func__, tmc_input_pid);

	return 0;
}

static int tmc_pid_open(struct inode *inode, struct file *file)
{
	return single_open(file, tmc_pid_read, PDE_DATA(inode));
}

static const struct file_operations tmc_pid_fops = {
	.owner = THIS_MODULE,
	.open = tmc_pid_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = tmc_pid_write,
	.release = single_release,
};
#endif

static int sprd_vol_to_temp_tab(int x, int n, struct vol_temp_table *tab)
{
	int index;
	int y;

	if (x >= tab[0].x)
		y = tab[0].y;
	else if (x <= tab[n - 1].x)
		y = tab[n - 1].y;
	else {
		/*  find interval */
		for (index = 1; index < n; index++)
			if (x > tab[index].x)
				break;
		/*  interpolate */
		y = (tab[index - 1].y - tab[index].y) * (x - tab[index].x)
		    * 2 / (tab[index - 1].x - tab[index].x);
		y = (y + 1) / 2;
		y += tab[index].y;
	}
	return y;
}

static int temp_sensor_vol_to_temp(struct sprd_thermal_zone *pzone, int val)
{
	return sprd_vol_to_temp_tab(val, pzone->pthm_config->temp_tab_size,
				    pzone->pthm_config->temp_tab);
}

static uint32_t temp_sensor_adc_to_vol(struct iio_channel *channel)
{
	int err;
	uint32_t val;

	err = iio_read_channel_processed(channel, &val);
	if (err < 0)
		return err;
	return val;
}

static int sprd_temp_sensor_read(struct sprd_thermal_zone *pzone, int *temp)
{
	int vol = 0;
	int sensor_temp = 0;
#ifdef FEATURE_THERMAL_CLEAR
	int i = 0;
#endif

	vol = temp_sensor_adc_to_vol(pzone->pthm_config->channel_temp);
	sensor_temp = temp_sensor_vol_to_temp(pzone, vol);
	*temp = sensor_temp * 100;
	pr_info("sensor id:%d,vol:0x%x, temp:%d\n", pzone->id, vol, *temp);
#ifdef FEATURE_THERMAL_CLEAR
	if (tmc_config != NULL && !strcmp(pzone->name, "bd-tsensor")) {
		for (i = 0; i < MAX_LEVEL; i++) {
			if ((sensor_temp > tmc_config->level[i][0]*10) &&
			     tmc_config->tmc_state[i] == 0) {
				tmc_config->tmc_state[i] = 1;
				send_tmc_signal(i * 2 + 1);/*signal 1/3/5 is level 1/2/3 set*/
			}
			if ((sensor_temp < tmc_config->level[i][1]*10) &&
			     tmc_config->tmc_state[i] == 1) {
				tmc_config->tmc_state[i] = 0;
				send_tmc_signal(i * 2 + 2);/*signal 2/4/6 is level 1/2/3 clr*/
			}
		}
	}
#endif

	return 0;
}

static void sprd_temp_sensor_init(struct sprd_thermal_zone *pzone)
{
	pr_info("sprd_temp_sensor_init\n");
}

static int sprd_temp_sensor_suspend(struct sprd_thermal_zone *pzone)
{
	pr_info("sprd_temp_sensor_suspend\n");
	return 0;
}

static int sprd_temp_sensor_resume(struct sprd_thermal_zone *pzone)
{
	pr_info("sprd_temp_sensor_resume\n");
	return 0;
}

struct thm_handle_ops sprd_boardthm_ops = {
	.hw_init = sprd_temp_sensor_init,
	.read_temp = sprd_temp_sensor_read,
	.suspend = sprd_temp_sensor_suspend,
	.resume = sprd_temp_sensor_resume,
};

static struct temp_sen_config *sprd_temp_sen_parse_dt(struct device *dev)
{
	int ret = 0, i = 0;
	int temp = 0;
	int temp_tab_size = 0;
	struct temp_sen_config *pconfig;
	struct device_node *np = dev->of_node;

	pconfig = devm_kzalloc(dev, sizeof(*pconfig), GFP_KERNEL);
	if (!pconfig)
		return NULL;

	ret = of_property_read_u32(np, "temp-tab-size", &temp_tab_size);
	if (ret) {
		dev_err(dev, "fail to get  temp_tab_size\n");
		return NULL;
	}

	pconfig->temp_tab_size = temp_tab_size;
	pconfig->temp_tab =
	    devm_kzalloc(dev,
			 sizeof(struct vol_temp_table) *
			 pconfig->temp_tab_size - 1, GFP_KERNEL);
	if (!pconfig->temp_tab)
		return NULL;

	for (i = 0; i < pconfig->temp_tab_size - 1; i++) {
		ret = of_property_read_u32_index(np, "temp-tab-val", i,
						 &pconfig->temp_tab[i].x);
		if (ret) {
			dev_err(dev, "fail to get temp-tab-va\n");
			return NULL;
		}
		ret = of_property_read_u32_index(np, "temp-tab-temp", i, &temp);
		if (ret) {
			dev_err(dev, "fail to get temp-tab-temp\n");
			return NULL;
		}
		pconfig->temp_tab[i].y = temp - 1000;
	}

	return pconfig;
}

static int sprd_temp_sensor_probe(struct platform_device *pdev)
{
	int ret = 0, sensor_id = 0;
	struct sprd_thermal_zone *pzone = NULL;
	struct temp_sen_config *pconfig = NULL;
	struct device_node *np = pdev->dev.of_node;
#ifdef FEATURE_THERMAL_CLEAR
	struct proc_dir_entry *entry = NULL;
	struct proc_dir_entry *dir_entry = NULL;
#endif

	if (!np) {
		dev_err(&pdev->dev, "device node not found\n");
		return -EINVAL;
	}

	sensor_id = of_alias_get_id(np, "thm-sensor");
	if (sensor_id == -ENODEV) {
		dev_err(&pdev->dev, "fail to get id\n");
		return -ENODEV;
	}
	pr_info("sprd board sensor probe id %d\n", sensor_id);

	pconfig = sprd_temp_sen_parse_dt(&pdev->dev);
	if (!pconfig) {
		dev_err(&pdev->dev, "not found ptrips\n");
		return -EINVAL;
	}
	pconfig->channel_temp = iio_channel_get(&pdev->dev, "adc_temp");
	if (IS_ERR(pconfig->channel_temp)) {
		dev_err(&pdev->dev, "get iio channel adc temp fail\n");
		return PTR_ERR(pconfig->channel_temp);
	}

	pzone = devm_kzalloc(&pdev->dev, sizeof(*pzone), GFP_KERNEL);
	if (!pzone)
		return -ENOMEM;

	mutex_init(&pzone->th_lock);
	pzone->dev = &pdev->dev;
	pzone->id = sensor_id;
	pzone->ops = &sprd_boardthm_ops;
	pzone->pthm_config = pconfig;
	strlcpy(pzone->name, np->name, sizeof(pzone->name));
#ifdef FEATURE_THERMAL_CLEAR
	if (!strcmp(pzone->name, "bd-tsensor")) {
		tmc_config = sprd_temp_clr_parse_dt(&pdev->dev);
		if (!tmc_config) {
			dev_err(&pdev->dev, "not found thermal clear setting\n");
		} else {
			dir_entry = proc_mkdir("driver/thermal", NULL);
			if (dir_entry == NULL) {
				pr_info("[%s]: mkdir /proc/driver/thermal failed\n", __func__);
				tmc_config = NULL;
			} else {
				entry = proc_create("tmc_pid", S_IRUGO | S_IWUSR | S_IWGRP,
						dir_entry, &tmc_pid_fops);
				if (entry)
				proc_set_user(entry, uid, gid);
			}
		}
	}
#endif
	sprd_temp_sensor_init(pzone);
	ret = sprd_thermal_init(pzone);
	if (ret) {
		dev_err(&pdev->dev,
			"thm sensor sw init error id =%d\n", pzone->id);
		return ret;
	}
	platform_set_drvdata(pdev, pzone);
	pr_info("sprd temp sensor probe start end\n");

	return 0;
}

static int sprd_board_thm_remove(struct platform_device *pdev)
{
	struct sprd_thermal_zone *pzone = platform_get_drvdata(pdev);

	sprd_thermal_remove(pzone);
	return 0;
}

static int sprd_board_thm_suspend(struct platform_device *pdev,
				  pm_message_t state)
{
	struct sprd_thermal_zone *pzone = platform_get_drvdata(pdev);

	pzone->ops->suspend(pzone);
	return 0;
}

static int sprd_board_thm_resume(struct platform_device *pdev)
{
	pr_info("sprd board thm resume\n");
	return 0;
}

static const struct of_device_id thermal_of_match[] = {
	{.compatible = "sprd,board-thermal",},
	{}
};

static struct platform_driver sprd_thm_sensor_driver = {
	.probe = sprd_temp_sensor_probe,
	.suspend = sprd_board_thm_suspend,
	.resume = sprd_board_thm_resume,
	.remove = sprd_board_thm_remove,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "board-thermal",
		   .of_match_table = of_match_ptr(thermal_of_match),
		   },
};

static int __init sprd_board_thermal_init(void)
{
	return platform_driver_register(&sprd_thm_sensor_driver);
}

static void __exit sprd_board_thermal_exit(void)
{
	platform_driver_unregister(&sprd_thm_sensor_driver);
}

device_initcall_sync(sprd_board_thermal_init);
module_exit(sprd_board_thermal_exit);

MODULE_AUTHOR("Freeman Liu <freeman.liu@spreadtrum.com>");
MODULE_DESCRIPTION("sprd board thermal driver");
MODULE_LICENSE("GPL");
