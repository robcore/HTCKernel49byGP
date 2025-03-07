/* Copyright (c) 2011-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/wcnss_wlan.h>
#include <linux/platform_data/qcom_wcnss_device.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/gpio.h>
#include <mach/peripheral-loader.h>
#include <linux/io.h>
#include <mach/msm_iomap.h>
#include <mach/subsystem_notif.h>

#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
#include "wcnss_prealloc.h"
#endif

#define DEVICE "wcnss_wlan"
#define VERSION "1.01"
#define WCNSS_PIL_DEVICE "wcnss"

#define WCNSS_CONFIG_UNSPECIFIED (-1)
static int has_48mhz_xo = WCNSS_CONFIG_UNSPECIFIED;
module_param(has_48mhz_xo, int, S_IWUSR | S_IRUGO);
MODULE_PARM_DESC(has_48mhz_xo, "Is an external 48 MHz XO present");

static struct {
	struct platform_device *pdev;
	void		*pil;
	struct resource	*mmio_res;
	struct resource	*tx_irq_res;
	struct resource	*rx_irq_res;
	struct resource	*gpios_5wire;
	const struct dev_pm_ops *pm_ops;
	int		triggered;
	int		smd_channel_ready;
	unsigned int	serial_number;
	int		thermal_mitigation;
	void		(*tm_notify)(struct device *, int);
	struct wcnss_wlan_config wlan_config;
	struct delayed_work wcnss_work;
	void *wcnss_notif_hdle;
	u8 is_shutdown;
} *penv = NULL;

static ssize_t wcnss_serial_number_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!penv)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%08X\n", penv->serial_number);
}

static ssize_t wcnss_serial_number_store(struct device *dev,
		struct device_attribute *attr, const char * buf, size_t count)
{
	unsigned int value;

	if (!penv)
		return -ENODEV;

	if (sscanf(buf, "%08X", &value) != 1)
		return -EINVAL;

	penv->serial_number = value;
	return count;
}

static DEVICE_ATTR(serial_number, S_IRUSR | S_IWUSR,
	wcnss_serial_number_show, wcnss_serial_number_store);


static ssize_t wcnss_thermal_mitigation_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (!penv)
		return -ENODEV;

	return scnprintf(buf, PAGE_SIZE, "%u\n", penv->thermal_mitigation);
}

static ssize_t wcnss_thermal_mitigation_store(struct device *dev,
		struct device_attribute *attr, const char * buf, size_t count)
{
	int value;

	if (!penv)
		return -ENODEV;

	if (sscanf(buf, "%d", &value) != 1)
		return -EINVAL;
	penv->thermal_mitigation = value;
	if (penv->tm_notify)
		(penv->tm_notify)(dev, value);
	return count;
}

static DEVICE_ATTR(thermal_mitigation, S_IRUSR | S_IWUSR,
	wcnss_thermal_mitigation_show, wcnss_thermal_mitigation_store);
void wcnss_reset_intr(void)
{
	pr_info("%s: reset interrupt RIVA\n", __func__);
	wmb();
	__raw_writel(1 << 24, MSM_APCS_GCC_BASE + 0x8);
}
EXPORT_SYMBOL(wcnss_reset_intr);

static int wcnss_create_sysfs(struct device *dev)
{
	int ret;

	if (!dev)
		return -ENODEV;

	ret = device_create_file(dev, &dev_attr_serial_number);
	if (ret)
		return ret;

	ret = device_create_file(dev, &dev_attr_thermal_mitigation);
	if (ret) {
		device_remove_file(dev, &dev_attr_serial_number);
		return ret;
	}
	return 0;
}

static void wcnss_remove_sysfs(struct device *dev)
{
	if (dev) {
		device_remove_file(dev, &dev_attr_serial_number);
		device_remove_file(dev, &dev_attr_thermal_mitigation);
	}
}

static void wcnss_post_bootup(struct work_struct *work)
{
	pr_info("[WCNSS]%s: Cancel APPS vote for Iris & Riva\n", __func__);

	
	wcnss_wlan_power(&penv->pdev->dev, &penv->wlan_config,
		WCNSS_WLAN_SWITCH_OFF);
}

static int
wcnss_gpios_config(struct resource *gpios_5wire, bool enable)
{
	int i, j;
	int rc = 0;

	for (i = gpios_5wire->start; i <= gpios_5wire->end; i++) {
		if (enable) {
			rc = gpio_request(i, gpios_5wire->name);
			if (rc) {
				pr_err("WCNSS gpio_request %d err %d\n", i, rc);
				goto fail;
			}
		} else
			gpio_free(i);
	}

	return rc;

fail:
	for (j = i-1; j >= gpios_5wire->start; j--)
		gpio_free(j);
	return rc;
}

static int __devinit
wcnss_wlan_ctrl_probe(struct platform_device *pdev)
{
	if (!penv)
		return -ENODEV;

	penv->smd_channel_ready = 1;

	pr_info("[WCNSS]%s: SMD ctrl channel up\n", __func__);

	
	INIT_DELAYED_WORK(&penv->wcnss_work, wcnss_post_bootup);
	schedule_delayed_work(&penv->wcnss_work, msecs_to_jiffies(10000));

	return 0;
}

void wcnss_flush_delayed_boot_votes(void)
{
	flush_delayed_work_sync(&penv->wcnss_work);
}
EXPORT_SYMBOL(wcnss_flush_delayed_boot_votes);

static int __devexit
wcnss_wlan_ctrl_remove(struct platform_device *pdev)
{
	if (penv)
		penv->smd_channel_ready = 0;

	pr_info("[WCNSS]%s: SMD ctrl channel down\n", __func__);

	return 0;
}


static struct platform_driver wcnss_wlan_ctrl_driver = {
	.driver = {
		.name	= "WLAN_CTRL",
		.owner	= THIS_MODULE,
	},
	.probe	= wcnss_wlan_ctrl_probe,
	.remove	= __devexit_p(wcnss_wlan_ctrl_remove),
};

struct device *wcnss_wlan_get_device(void)
{
	if (penv && penv->pdev && penv->smd_channel_ready)
		return &penv->pdev->dev;
	return NULL;
}
EXPORT_SYMBOL(wcnss_wlan_get_device);

struct platform_device *wcnss_get_platform_device(void)
{
	if (penv && penv->pdev)
		return penv->pdev;
	return NULL;
}
EXPORT_SYMBOL(wcnss_get_platform_device);

struct wcnss_wlan_config *wcnss_get_wlan_config(void)
{
	if (penv && penv->pdev)
		return &penv->wlan_config;
	return NULL;
}
EXPORT_SYMBOL(wcnss_get_wlan_config);

int wcnss_device_ready(void)
{
       if (!wcnss_device_is_shutdown())
              return 1;
       return 0;
}
EXPORT_SYMBOL(wcnss_device_ready);

int wcnss_device_is_shutdown(void)
{
       if (penv && penv->is_shutdown)
              return 1;
       return 0;
}
EXPORT_SYMBOL(wcnss_device_is_shutdown);

struct resource *wcnss_wlan_get_memory_map(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) && penv->smd_channel_ready)
		return penv->mmio_res;
	return NULL;
}
EXPORT_SYMBOL(wcnss_wlan_get_memory_map);

int wcnss_wlan_get_dxe_tx_irq(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) &&
				penv->tx_irq_res && penv->smd_channel_ready)
		return penv->tx_irq_res->start;
	return WCNSS_WLAN_IRQ_INVALID;
}
EXPORT_SYMBOL(wcnss_wlan_get_dxe_tx_irq);

int wcnss_wlan_get_dxe_rx_irq(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) &&
				penv->rx_irq_res && penv->smd_channel_ready)
		return penv->rx_irq_res->start;
	return WCNSS_WLAN_IRQ_INVALID;
}
EXPORT_SYMBOL(wcnss_wlan_get_dxe_rx_irq);

void wcnss_wlan_register_pm_ops(struct device *dev,
				const struct dev_pm_ops *pm_ops)
{
	if (penv && dev && (dev == &penv->pdev->dev) && pm_ops)
		penv->pm_ops = pm_ops;
}
EXPORT_SYMBOL(wcnss_wlan_register_pm_ops);

void wcnss_wlan_unregister_pm_ops(struct device *dev,
				const struct dev_pm_ops *pm_ops)
{
	if (penv && dev && penv->pdev && (dev == &penv->pdev->dev) && pm_ops && penv->pm_ops) {
		if (pm_ops->suspend != penv->pm_ops->suspend ||
				pm_ops->resume != penv->pm_ops->resume)
			pr_err("[WCNSS]PM APIs dont match with registered APIs\n");
		penv->pm_ops = NULL;
	}
}
EXPORT_SYMBOL(wcnss_wlan_unregister_pm_ops);

void wcnss_register_thermal_mitigation(struct device *dev,
				void (*tm_notify)(struct device *, int))
{
	if (penv && dev && tm_notify)
		penv->tm_notify = tm_notify;
}
EXPORT_SYMBOL(wcnss_register_thermal_mitigation);

void wcnss_unregister_thermal_mitigation(
				void (*tm_notify)(struct device *, int))
{
	if (penv && tm_notify) {
		if (tm_notify != penv->tm_notify)
			pr_err("tm_notify doesn't match registered\n");
		penv->tm_notify = NULL;
	}
}
EXPORT_SYMBOL(wcnss_unregister_thermal_mitigation);

unsigned int wcnss_get_serial_number(void)
{
	if (penv)
		return penv->serial_number;
	return 0;
}
EXPORT_SYMBOL(wcnss_get_serial_number);

static int wcnss_wlan_suspend(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) &&
	    penv->smd_channel_ready &&
	    penv->pm_ops && penv->pm_ops->suspend)
		return penv->pm_ops->suspend(dev);
	return 0;
}

static int wcnss_wlan_resume(struct device *dev)
{
	if (penv && dev && (dev == &penv->pdev->dev) &&
	    penv->smd_channel_ready &&
	    penv->pm_ops && penv->pm_ops->resume)
		return penv->pm_ops->resume(dev);
	return 0;
}

static int
wcnss_trigger_config(struct platform_device *pdev)
{
	int ret;
	struct qcom_wcnss_opts *pdata;

	
	if (penv->triggered)
		return 0;
	penv->triggered = 1;

	
	pdata = pdev->dev.platform_data;
	if (WCNSS_CONFIG_UNSPECIFIED == has_48mhz_xo)
		has_48mhz_xo = pdata->has_48mhz_xo;
	penv->wlan_config.use_48mhz_xo = has_48mhz_xo;

	penv->thermal_mitigation = 0;

	penv->gpios_5wire = platform_get_resource_byname(pdev, IORESOURCE_IO,
							"wcnss_gpios_5wire");

	
	if (!penv->gpios_5wire) {
		dev_err(&pdev->dev, "insufficient IO resources\n");
		ret = -ENOENT;
		goto fail_gpio_res;
	}

	
	ret = wcnss_gpios_config(penv->gpios_5wire, true);
	if (ret) {
		dev_err(&pdev->dev, "WCNSS gpios config failed.\n");
		goto fail_gpio_res;
	}

	
	ret = wcnss_wlan_power(&pdev->dev, &penv->wlan_config,
					WCNSS_WLAN_SWITCH_ON);
	if (ret) {
		dev_err(&pdev->dev, "WCNSS Power-up failed.\n");
		goto fail_power;
	}

	
	penv->pil = pil_get(WCNSS_PIL_DEVICE);
	if (IS_ERR(penv->pil)) {
		dev_err(&pdev->dev, "Peripheral Loader failed on WCNSS.\n");
		ret = PTR_ERR(penv->pil);
		penv->pil = NULL;
		goto fail_pil;
	}

	
	penv->mmio_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"wcnss_mmio");
	penv->tx_irq_res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
							"wcnss_wlantx_irq");
	penv->rx_irq_res = platform_get_resource_byname(pdev, IORESOURCE_IRQ,
							"wcnss_wlanrx_irq");

	if (!(penv->mmio_res && penv->tx_irq_res && penv->rx_irq_res)) {
		dev_err(&pdev->dev, "insufficient resources\n");
		ret = -ENOENT;
		goto fail_res;
	}

	
	ret = wcnss_create_sysfs(&pdev->dev);
	if (ret)
		goto fail_sysfs;

	return 0;

fail_sysfs:
fail_res:
	if (penv->pil)
		pil_put(penv->pil);
fail_pil:
	wcnss_wlan_power(&pdev->dev, &penv->wlan_config,
				WCNSS_WLAN_SWITCH_OFF);
fail_power:
	wcnss_gpios_config(penv->gpios_5wire, false);
fail_gpio_res:
	kfree(penv);
	penv = NULL;
	return ret;
}

#ifndef MODULE
static int wcnss_node_open(struct inode *inode, struct file *file)
{
	struct platform_device *pdev;
	int ret = 0;

	pr_info(DEVICE " triggered by userspace (%p)\n", penv);

	if (penv == NULL) {
		printk("penv == NULL");
		return ENODEV;
	}

	pdev = penv->pdev;
	ret = wcnss_trigger_config(pdev);
	pr_err("wcnss_trigger_config ret = %d\n", ret);
	if(ret != 0) {
		panic("wcnss_trigger_config fail");
		return ret;
	} else {
		return ret;
	}
}

static int wcnss_notif_cb(struct notifier_block *this, unsigned long code,
                          void *ss_handle)
{
  pr_debug("%s: wcnss notification event: %lu\n", __func__, code);
  if (SUBSYS_BEFORE_SHUTDOWN == code) {
    penv->is_shutdown = 1;
    pr_err("%s: before shutdown is_shutdown -> 1", __func__);
  } else if (SUBSYS_AFTER_POWERUP == code)	{
		penv->is_shutdown = 0;
	}
  return NOTIFY_DONE;
}

static struct notifier_block wnb = {
       .notifier_call = wcnss_notif_cb,
};

static const struct file_operations wcnss_node_fops = {
	.owner = THIS_MODULE,
	.open = wcnss_node_open,
};

static struct miscdevice wcnss_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE,
	.fops = &wcnss_node_fops,
};
#endif 

#if defined(CONFIG_PERFLOCK) && !defined(CONFIG_PERFLOCK_HACK)
#include <mach/perflock.h>
struct perf_lock qcom_wlan_perf_lock;
EXPORT_SYMBOL(qcom_wlan_perf_lock);
#endif 

static int __devinit
wcnss_wlan_probe(struct platform_device *pdev)
{
	
	if (penv) {
		dev_err(&pdev->dev, "cannot handle multiple devices.\n");
		return -ENODEV;
	}

#if defined(CONFIG_PERFLOCK) && !defined(CONFIG_PERFLOCK_HACK)
        perf_lock_init(&qcom_wlan_perf_lock, TYPE_PERF_LOCK, PERF_LOCK_HIGHEST, "qcom-wifi-perf");
#endif 

	
	penv = kzalloc(sizeof(*penv), GFP_KERNEL);
	if (!penv) {
		dev_err(&pdev->dev, "cannot allocate device memory.\n");
		return -ENOMEM;
	}
	penv->pdev = pdev;

  
  penv->wcnss_notif_hdle = subsys_notif_register_notifier("riva", &wnb);
  pr_err("%s, wcnss_notif_hdle %p\n", __func__, penv->wcnss_notif_hdle);

#ifdef MODULE

	pr_info(DEVICE " probed in MODULE mode\n");
	return wcnss_trigger_config(pdev);

#else

	pr_info(DEVICE " probed in built-in mode\n");
	return misc_register(&wcnss_misc);

#endif
}

static int __devexit
wcnss_wlan_remove(struct platform_device *pdev)
{
	if (penv->wcnss_notif_hdle)
		subsys_notif_unregister_notifier(penv->wcnss_notif_hdle, &wnb);
	wcnss_remove_sysfs(&pdev->dev);
	return 0;
}


static const struct dev_pm_ops wcnss_wlan_pm_ops = {
	.suspend	= wcnss_wlan_suspend,
	.resume		= wcnss_wlan_resume,
};

static struct platform_driver wcnss_wlan_driver = {
	.driver = {
		.name	= DEVICE,
		.owner	= THIS_MODULE,
		.pm	= &wcnss_wlan_pm_ops,
	},
	.probe	= wcnss_wlan_probe,
	.remove	= __devexit_p(wcnss_wlan_remove),
};

static int __init wcnss_wlan_init(void)
{
	int ret = 0;
	platform_driver_register(&wcnss_wlan_driver);
	platform_driver_register(&wcnss_wlan_ctrl_driver);

#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
	ret = wcnss_prealloc_init();
	if (ret < 0)
		pr_err("wcnss: pre-allocation failed\n");
#endif

	return ret;
}

static void __exit wcnss_wlan_exit(void)
{
	if (penv) {
		if (penv->pil)
			pil_put(penv->pil);


		kfree(penv);
		penv = NULL;
	}

	platform_driver_unregister(&wcnss_wlan_ctrl_driver);
	platform_driver_unregister(&wcnss_wlan_driver);
#ifdef CONFIG_WCNSS_MEM_PRE_ALLOC
	wcnss_prealloc_deinit();
#endif
}

module_init(wcnss_wlan_init);
module_exit(wcnss_wlan_exit);

MODULE_LICENSE("GPL v2");
MODULE_VERSION(VERSION);
MODULE_DESCRIPTION(DEVICE "Driver");
