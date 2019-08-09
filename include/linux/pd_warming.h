// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, Linaro Ltd.
 */
#ifndef __PWR_DOMAIN_WARMING_H__
#define __PWR_DOMAIN_WARMING_H__

#include <linux/pm_domain.h>
#include <linux/thermal.h>

#ifdef CONFIG_PWR_DOMAIN_WARMING_THERMAL
struct thermal_cooling_device *
of_pd_warming_register(struct device *parent, int pd_id);

void pd_warming_unregister(struct thermal_cooling_device *cdev);

#else
static inline struct thermal_cooling_device *
of_pd_warming_register(struct device *parent, int pd_id)
{
	return ERR_PTR(-ENOSYS);
}

static inline void
pd_warming_unregister(struct thermal_cooling_device *cdev)
{
}
#endif /* CONFIG_PWR_DOMAIN_WARMING_THERMAL */
#endif /* __PWR_DOMAIN_WARMING_H__ */
