// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, Linaro Ltd
 */
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/pd_warming.h>

struct pd_warming_device {
	struct thermal_cooling_device *cdev;
	struct device dev;
	int id;
	int max_state;
	int cur_state;
	bool runtime_resumed;
};

static DEFINE_IDA(pd_ida);

static int pd_wdev_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct pd_warming_device *pd_wdev = cdev->devdata;

	*state = pd_wdev->max_state;
	return 0;
}

static int pd_wdev_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	struct pd_warming_device *pd_wdev = cdev->devdata;

	*state = dev_pm_genpd_get_performance_state(&pd_wdev->dev);

	return 0;
}

static int pd_wdev_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	struct pd_warming_device *pd_wdev = cdev->devdata;
	struct device *dev = &pd_wdev->dev;
	int ret;

	ret = dev_pm_genpd_set_performance_state(dev, state);

	if (ret)
		return ret;

	if (state && !pd_wdev->runtime_resumed) {
		ret = pm_runtime_get_sync(dev);
		pd_wdev->runtime_resumed = true;
	} else if (!state && pd_wdev->runtime_resumed) {
		ret = pm_runtime_put(dev);
		pd_wdev->runtime_resumed = false;
	}

	return ret;
}

static struct thermal_cooling_device_ops pd_warming_device_ops = {
	.get_max_state	= pd_wdev_get_max_state,
	.get_cur_state	= pd_wdev_get_cur_state,
	.set_cur_state	= pd_wdev_set_cur_state,
};

static void pd_warming_release(struct device *dev)
{
	struct pd_warming_device *pd_wdev;

	pd_wdev = container_of(dev, struct pd_warming_device, dev);
	kfree(pd_wdev);
}

struct thermal_cooling_device *
of_pd_warming_register(struct device *parent, int pd_id)
{
	struct pd_warming_device *pd_wdev;
	struct of_phandle_args pd_args;
	char cdev_name[THERMAL_NAME_LENGTH];
	int ret;

	pd_wdev = kzalloc(sizeof(*pd_wdev), GFP_KERNEL);
	if (!pd_wdev)
		return ERR_PTR(-ENOMEM);

	dev_set_name(&pd_wdev->dev, "%s_%d_warming_dev",
		     dev_name(parent), pd_id);
	pd_wdev->dev.parent = parent;
	pd_wdev->dev.release = pd_warming_release;

	ret = device_register(&pd_wdev->dev);
	if (ret) {
		put_device(&pd_wdev->dev);
		goto out;
	}

	ret = ida_simple_get(&pd_ida, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto unregister_device;

	pd_wdev->id = ret;

	pd_args.np = parent->of_node;
	pd_args.args[0] = pd_id;
	pd_args.args_count = 1;

	ret = of_genpd_add_device(&pd_args, &pd_wdev->dev);

	if (ret)
		goto remove_ida;

	ret = dev_pm_genpd_performance_state_count(&pd_wdev->dev);
	if (ret < 0)
		goto out_genpd;

	pd_wdev->max_state = ret - 1;
	pm_runtime_enable(&pd_wdev->dev);
	pd_wdev->runtime_resumed = false;

	snprintf(cdev_name, sizeof(cdev_name), "thermal-pd-%d", pd_wdev->id);
	pd_wdev->cdev = thermal_of_cooling_device_register
					(NULL, cdev_name, pd_wdev,
					 &pd_warming_device_ops);
	if (IS_ERR(pd_wdev->cdev)) {
		pr_err("unable to register %s cooling device\n", cdev_name);
		ret = PTR_ERR(pd_wdev->cdev);
		goto out_runtime_disable;
	}

	return pd_wdev->cdev;

out_runtime_disable:
	pm_runtime_disable(&pd_wdev->dev);
out_genpd:
	pm_genpd_remove_device(&pd_wdev->dev);
remove_ida:
	ida_simple_remove(&pd_ida, pd_wdev->id);
unregister_device:
	device_unregister(&pd_wdev->dev);
out:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(of_pd_warming_register);

void pd_warming_unregister(struct thermal_cooling_device *cdev)
{
	struct pd_warming_device *pd_wdev = cdev->devdata;
	struct device *dev = &pd_wdev->dev;

	if (pd_wdev->runtime_resumed) {
		dev_pm_genpd_set_performance_state(dev, 0);
		pm_runtime_put(dev);
		pd_wdev->runtime_resumed = false;
	}
	pm_runtime_disable(dev);
	pm_genpd_remove_device(dev);
	ida_simple_remove(&pd_ida, pd_wdev->id);
	thermal_cooling_device_unregister(cdev);
	device_unregister(dev);
}
EXPORT_SYMBOL_GPL(pd_warming_unregister);
