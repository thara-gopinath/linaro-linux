// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Linaro Ltd
 */
#include <linux/platform_device.h>
#include <linux/interconnect.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/of_device.h>
#include <linux/module.h>
#include <linux/pm_opp.h>
#include <linux/delay.h>

#define HW_TIMER_HZ     			19200000

#define BWMON_GLOBAL_IRQ_STATUS			0x0
#define BWMON_GLOBAL_IRQ_CLEAR			0x8
#define BWMON_GLOBAL_IRQ_ENABLE			0xc

#define BWMON_IRQ_STATUS			0x100
#define BWMON_IRQ_CLEAR				0x108
#define BWMON_IRQ_ENABLE			0x10c

#define BWMON_ENABLE				0x2a0
#define BWMON_CLEAR				0x2a4
#define BWMON_SAMPLE_WINDOW			0x2a8
#define BWMON_THRESHOLD_HIGH			0x2ac
#define BWMON_THRESHOLD_MED			0x2b0
#define BWMON_THRESHOLD_LOW			0x2b4
#define BWMON_ZONE_ACTIONS			0x2b8
#define BWMON_THRESHOLD_COUNT			0x2bc
#define BWMON_ZONE_COUNT			0x2d8
#define BWMON_ZONE_MAX(zone)			(0x2e0 + 4 * (zone))

#define BWMON_GLOBAL_IRQ_ENABLE_ENABLE		BIT(0)

#define BWMON_IRQ_ENABLE_ZONE1_SHIFT		5
#define BWMON_IRQ_ENABLE_ZONE3_SHIFT    	7

#define BWMON_ENABLE_ENABLE			BIT(0)

#define BWMON_CLEAR_CLEAR			BIT(0)

#define BWMON_ZONE_ACTIONS_DEFAULT		0x95250901

#define BWMON_THRESHOLD_COUNT_ZONE1_SHIFT	8
#define BWMON_THRESHOLD_COUNT_ZONE2_SHIFT	16
#define BWMON_THRESHOLD_COUNT_ZONE3_SHIFT	24
#define BWMON_THRESHOLD_COUNT_ZONE0_DEFAULT	0xFF
#define BWMON_THRESHOLD_COUNT_ZONE2_DEFAULT     0xFF

struct icc_bwmon_data {
	unsigned int sample_ms;
	unsigned int default_highbw_mbps;
	unsigned int default_medbw_mbps;
	unsigned int default_lowbw_mbps;
	u8 zone1_thres_count;
	u8 zone3_thres_count;
};

struct icc_bwmon {
	struct device *dev;
	void __iomem *base;
	int irq;

	unsigned int sample_ms;
	unsigned int count_shift;
	unsigned int max_bw_mbps;
	unsigned int min_bw_mbps;
	unsigned int target_mbps;
	unsigned int current_mbps;
};

static void bwmon_clear(struct icc_bwmon *bwmon)
{
	/* Clear zone and global interrupts */
	writel(0xa0, bwmon->base + BWMON_IRQ_CLEAR);
	writel(BIT(0), bwmon->base + BWMON_GLOBAL_IRQ_CLEAR);
	
	/* Clear counters */
	writel(BWMON_CLEAR_CLEAR, bwmon->base + BWMON_CLEAR);
}

static void bwmon_disable(struct icc_bwmon *bwmon)
{
	/* Disable interrupts */
	writel(0x0, bwmon->base + BWMON_GLOBAL_IRQ_ENABLE);
	writel(0x0, bwmon->base + BWMON_IRQ_ENABLE);

	/*Disable bwmon */
	writel(0x0, bwmon->base + BWMON_ENABLE);
}

static void bwmon_enable(struct icc_bwmon *bwmon, unsigned int irq_enable)
{
	/* Enable interrupts */
	writel(BWMON_GLOBAL_IRQ_ENABLE_ENABLE, bwmon->base + BWMON_GLOBAL_IRQ_ENABLE);
	writel(irq_enable, bwmon->base + BWMON_IRQ_ENABLE);

	/*Enable bwmon */
	writel(BWMON_ENABLE_ENABLE, bwmon->base + BWMON_ENABLE);
}

static void bwmon_set_threshold(struct icc_bwmon *bwmon, unsigned int reg, unsigned int mbps)
{	
	unsigned int thres;

	thres = mult_frac(mbps, bwmon->sample_ms, MSEC_PER_SEC);
	writel(thres, bwmon->base + reg);
}

static void bwmon_start(struct icc_bwmon *bwmon, const struct icc_bwmon_data *data)
{
	unsigned int thres_count, irq_enable = 0;
	int window;

	bwmon_clear(bwmon);

	window = mult_frac(bwmon->sample_ms, HW_TIMER_HZ, MSEC_PER_SEC);
	writel(window, bwmon->base + BWMON_SAMPLE_WINDOW);

	bwmon_set_threshold(bwmon, BWMON_THRESHOLD_HIGH, data->default_highbw_mbps);
	bwmon_set_threshold(bwmon, BWMON_THRESHOLD_MED, data->default_medbw_mbps);
	bwmon_set_threshold(bwmon, BWMON_THRESHOLD_LOW, data->default_lowbw_mbps);

	thres_count = data->zone3_thres_count << BWMON_THRESHOLD_COUNT_ZONE3_SHIFT |
			BWMON_THRESHOLD_COUNT_ZONE2_DEFAULT << BWMON_THRESHOLD_COUNT_ZONE2_SHIFT |
			data->zone1_thres_count << BWMON_THRESHOLD_COUNT_ZONE1_SHIFT |
			BWMON_THRESHOLD_COUNT_ZONE0_DEFAULT;
	writel(thres_count, bwmon->base + BWMON_THRESHOLD_COUNT);
	writel(BWMON_ZONE_ACTIONS_DEFAULT, bwmon->base + BWMON_ZONE_ACTIONS);

	irq_enable = 1 << BWMON_IRQ_ENABLE_ZONE1_SHIFT | 1 << BWMON_IRQ_ENABLE_ZONE3_SHIFT;
	bwmon_clear(bwmon);
	bwmon_enable(bwmon, irq_enable);
}

static irqreturn_t bwmon_intr(int irq, void *dev_id)
{
	struct icc_bwmon *bwmon = dev_id;
	unsigned int status;
	unsigned long max;
	int zone;

	status = readl(bwmon->base + BWMON_IRQ_STATUS);
	
	if (!status)
		return IRQ_NONE;

	bwmon_disable(bwmon);

	zone = get_bitmask_order(status >> 4) - 1;
	max = readl(bwmon->base + BWMON_ZONE_MAX(zone)) << bwmon->count_shift;
        bwmon->target_mbps = mult_frac(max, MSEC_PER_SEC, bwmon->sample_ms) / SZ_1M;

	return IRQ_WAKE_THREAD;
}

static irqreturn_t bwmon_intr_thread(int irq, void *dev_id)
{
	struct icc_bwmon *bwmon = dev_id;
	struct dev_pm_opp *opp, *target_opp;
	unsigned long bw_kbps;
	unsigned int up_mbps, down_mbps, irq_enable = 0;

	bw_kbps = bwmon->target_mbps * 1000;

	trace_printk("bwmon_intr_thread %d\n", bwmon->target_mbps);
	target_opp = dev_pm_opp_find_bw_ceil(bwmon->dev, &bw_kbps, 0);
        if (IS_ERR(target_opp) && PTR_ERR(target_opp) == -ERANGE)
                target_opp = dev_pm_opp_find_bw_floor(bwmon->dev, &bw_kbps, 0);

	bwmon->target_mbps = bw_kbps / 1000;

	bw_kbps--;
	opp = dev_pm_opp_find_bw_floor(bwmon->dev, &bw_kbps, 0);
	if (IS_ERR(opp) && PTR_ERR(opp) == -ERANGE)
	//	dev_pm_opp_find_bw_ceil(bwmon->dev, &bw_kbps, 0);
		down_mbps = bwmon->target_mbps;
	else
		down_mbps = bw_kbps / 1000;

	up_mbps = bwmon->target_mbps + 1;

	if (bwmon->target_mbps >= bwmon->max_bw_mbps)
		irq_enable = 1 << BWMON_IRQ_ENABLE_ZONE1_SHIFT;
	else if (bwmon->target_mbps <= bwmon->min_bw_mbps)
		irq_enable = 1 << BWMON_IRQ_ENABLE_ZONE3_SHIFT;
	else
		irq_enable = 1 << BWMON_IRQ_ENABLE_ZONE1_SHIFT | 1 << BWMON_IRQ_ENABLE_ZONE3_SHIFT;

	trace_printk("bwmon_intr_thread, %d %d %d 0x%x\n", bwmon->target_mbps, down_mbps, up_mbps, irq_enable);

	bwmon_set_threshold(bwmon, BWMON_THRESHOLD_HIGH, up_mbps);
	bwmon_set_threshold(bwmon, BWMON_THRESHOLD_MED, down_mbps);
	bwmon_clear(bwmon);
	bwmon_enable(bwmon, irq_enable);

	if (bwmon->target_mbps == bwmon->current_mbps)
		goto out;
	
	dev_pm_opp_set_opp(bwmon->dev, target_opp);
	bwmon->current_mbps = bwmon->target_mbps;
out:
        dev_pm_opp_put(target_opp);
	if (!IS_ERR(opp))
	        dev_pm_opp_put(opp);
	return IRQ_HANDLED;
}

static int bwmon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct icc_bwmon *bwmon;
	const struct icc_bwmon_data *data;
	unsigned int bw;
	int ret;

	pr_err("bwmon_probe\n");
	bwmon = devm_kzalloc(dev, sizeof(*bwmon), GFP_KERNEL);
	if (!bwmon)
		return -ENOMEM;

	data = of_device_get_match_data(dev);
	if (!data) {
		dev_err(dev, "No matching driver data found\n");
		return -EINVAL;
	}

	bwmon->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bwmon->base)) {
		dev_err(dev, "failed to map bwmon registers\n");
		return -EINVAL;
	}

	bwmon->irq = platform_get_irq(pdev, 0);
	if (bwmon->irq < 0) {
		dev_err(dev, "unable to acquire bwmon IRQ\n");
		return bwmon->irq;
	}

	ret = of_property_read_u32(np, "qcom,bwmon-max-bw-mpbs", &bw);
	if (ret) {
		dev_err(dev, "missing qcom,bwmon-max-bw-mpbs property\n");
		return ret;
	}
	bwmon->max_bw_mbps = bw;

	ret = of_property_read_u32(np, "qcom,bwmon-min-bw-mpbs", &bw);
	if (ret) {
		dev_err(dev, "missing qcom,bwmon-min-bw-mpbs property\n");
		return ret;
	}
	bwmon->min_bw_mbps = bw;

	ret = dev_pm_opp_of_add_table(dev);
	if (ret) {
		dev_err(dev, "Failed to add OPP table\n");
		return ret;
	}

	bwmon->sample_ms = data->sample_ms;
	bwmon->count_shift = order_base_2(SZ_1M);
	bwmon->dev = dev;
	
	bwmon_disable(bwmon);
	ret = devm_request_threaded_irq(dev, bwmon->irq, bwmon_intr, bwmon_intr_thread,	IRQF_ONESHOT, dev_name(dev), bwmon);
	if (ret < 0) {
		dev_err(dev, "failed to request IRQ: %d\n", ret);
		return ret;
	}

	bwmon_start(bwmon, data);
	return 0;
}

static const struct icc_bwmon_data sdm845_bwmon_data = {
	.sample_ms = 4,
	.default_highbw_mbps = 4800,
	.default_medbw_mbps = 512,
	.default_lowbw_mbps = 0,
	.zone1_thres_count = 0x10,
	.zone3_thres_count = 0x1,
};

static const struct of_device_id bwmon_of_match[] = {
	{ .compatible = "qcom,sdm845-cpu-bwmon", .data = &sdm845_bwmon_data },
	{ . compatible = "qcom, sdm845-cdsp-bwmon" },
	{}
};
MODULE_DEVICE_TABLE(of, bwmon_of_match);

static struct platform_driver bwmon_driver = {
	.probe = bwmon_probe,
	.driver = {
		.name = "qcom-bwmon",
		.of_match_table = bwmon_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(bwmon_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QCOM BWMON driver");
