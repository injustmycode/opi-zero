/*
 * Thermal sensor driver for Allwinner SUN8I SoC
 *
 * Copyright (C) 2016 Ondřej Jirman
 * Based on the work of Josef Gajdusek <atx@atx.name>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/printk.h>

#define THS_SUN8I_CTRL0		0x00
#define THS_SUN8I_CTRL2		0x40
#define THS_SUN8I_INT_CTRL	0x44
#define THS_SUN8I_STAT		0x48
#define THS_SUN8I_FILTER	0x70
#define THS_SUN8I_CDATA01	0x74
#define THS_SUN8I_CDATA2	0x78
#define THS_SUN8I_DATA0		0x80
#define THS_SUN8I_DATA1		0x84
#define THS_SUN8I_DATA2		0x88

#define THS_SUN8I_CTRL0_SENSOR_ACQ0(x)		(x)
#define THS_SUN8I_CTRL2_SENSE_EN0		BIT(0)
#define THS_SUN8I_CTRL2_SENSE_EN1		BIT(1)
#define THS_SUN8I_CTRL2_SENSE_EN2		BIT(2)
#define THS_SUN8I_CTRL2_SENSOR_ACQ1(x)		((x) << 16)
#define THS_SUN8I_INT_CTRL_DATA0_IRQ_EN		BIT(8)
#define THS_SUN8I_INT_CTRL_DATA1_IRQ_EN		BIT(9)
#define THS_SUN8I_INT_CTRL_DATA2_IRQ_EN		BIT(10)
#define THS_SUN8I_INT_CTRL_THERMAL_PER(x)	((x) << 12)
#define THS_SUN8I_STAT_DATA0_IRQ_STS		BIT(8)
#define THS_SUN8I_STAT_DATA1_IRQ_STS		BIT(9)
#define THS_SUN8I_STAT_DATA2_IRQ_STS		BIT(10)
#define THS_SUN8I_STAT_CLEAR			0x777
#define THS_SUN8I_FILTER_TYPE(x)		((x) << 0)
#define THS_SUN8I_FILTER_EN			BIT(2)

#define THS_SUN8I_CLK_IN		40000000 /* Hz */
#define THS_SUN8I_DATA_PERIOD		330 /* ms */
#define THS_SUN8I_FILTER_TYPE_VALUE	2 /* average over 2^(n+1) samples */

//XXX: this formula doesn't work for A83T very well
//XXX: A83T is getting slower readings out of this (1s interval?)
//perhaps configure this in sun8i_ths_desc
#define THS_SUN8I_FILTER_DIV		(1 << (THS_SUN8I_FILTER_TYPE_VALUE + 1))
#define THS_SUN8I_INT_CTRL_THERMAL_PER_VALUE \
	(THS_SUN8I_DATA_PERIOD * (THS_SUN8I_CLK_IN / 1000) / \
	 THS_SUN8I_FILTER_DIV / 4096 - 1)

#define THS_SUN8I_CTRL0_SENSOR_ACQ0_VALUE	0x3f /* 16us */
#define THS_SUN8I_CTRL2_SENSOR_ACQ1_VALUE	0x3f

#define SUN8I_THS_MAX_TZDS 3

struct sun8i_ths_sensor_desc {
	u32 data_int_en;
	u32 data_int_flag;
	u32 data_offset;
	u32 sense_en;
};

struct sun8i_ths_desc {
	int num_sensors;
	struct sun8i_ths_sensor_desc *sensors;
	int (*calc_temp)(u32 reg_val);
	bool has_cal1;
};

struct sun8i_ths_tzd {
	struct sun8i_ths_data *data;
	struct thermal_zone_device *tzd;
	u32 temp;
};

struct sun8i_ths_data {
	struct device *dev;
	struct reset_control *reset;
	struct clk *clk;
	struct clk *busclk;
	void __iomem *regs;
	void __iomem *cal_regs;
	struct sun8i_ths_desc *desc;
	struct sun8i_ths_tzd tzds[SUN8I_THS_MAX_TZDS];
};

static int sun8i_ths_calc_temp_h3(u32 reg_val)
{
	uint64_t temp = (uint64_t)reg_val * 1000000ll;

        do_div(temp, 8253);

	return 217000 - (int)temp;
}

static int sun8i_ths_calc_temp_a83t(u32 reg_val)
{
	uint64_t temp = (uint64_t)reg_val * 1000000ll;

        do_div(temp, 14186);

	return 192000 - (int)temp;
}

static int sun8i_ths_get_temp(void *_data, int *out)
{
	struct sun8i_ths_tzd *tzd = _data;
	struct sun8i_ths_data *data = tzd->data;

	if (tzd->temp == 0)
		return -EBUSY;

	*out = data->desc->calc_temp(tzd->temp);
	return 0;
}

static irqreturn_t sun8i_ths_irq_thread(int irq, void *_data)
{
	struct sun8i_ths_data *data = _data;
	struct sun8i_ths_tzd *tzd;
	struct sun8i_ths_sensor_desc *zdesc;
	int i;
	u32 status;

	status = readl(data->regs + THS_SUN8I_STAT);
	writel(THS_SUN8I_STAT_CLEAR, data->regs + THS_SUN8I_STAT);

	for (i = 0; i < data->desc->num_sensors; i++) {
		tzd = &data->tzds[i];
		zdesc = &data->desc->sensors[i];

		if (status & zdesc->data_int_flag) {
			tzd->temp = readl(data->regs + zdesc->data_offset);
			if (tzd->temp)
				thermal_zone_device_update(tzd->tzd,
							   THERMAL_EVENT_TEMP_SAMPLE);
		}
	}

	return IRQ_HANDLED;
}

static void sun8i_ths_init(struct sun8i_ths_data *data)
{
	int i;
	u32 int_ctrl = 0;
	u32 ctrl2 = 0;

	writel(THS_SUN8I_CTRL0_SENSOR_ACQ0(THS_SUN8I_CTRL0_SENSOR_ACQ0_VALUE),
		data->regs + THS_SUN8I_CTRL0);
	writel(THS_SUN8I_FILTER_EN | THS_SUN8I_FILTER_TYPE(THS_SUN8I_FILTER_TYPE_VALUE),
		data->regs + THS_SUN8I_FILTER);

	ctrl2 |= THS_SUN8I_CTRL2_SENSOR_ACQ1(THS_SUN8I_CTRL2_SENSOR_ACQ1_VALUE);
	int_ctrl |= THS_SUN8I_INT_CTRL_THERMAL_PER(THS_SUN8I_INT_CTRL_THERMAL_PER_VALUE);

	for (i = 0; i < data->desc->num_sensors; i++) {
		ctrl2 |= data->desc->sensors[i].sense_en;
		int_ctrl |= data->desc->sensors[i].data_int_en;
	}

	if (data->cal_regs) {
		u32 cal0, cal1;

		cal0 = readl(data->cal_regs);
		if (cal0)
			writel(cal0, data->regs + THS_SUN8I_CDATA01);

		if (data->desc->has_cal1) {
			cal1 = readl(data->cal_regs + 4);
			if (cal1)
				writel(cal1, data->regs + THS_SUN8I_CDATA2);
		}
	}

	writel(ctrl2, data->regs + THS_SUN8I_CTRL2);

	/* enable interrupts */
	writel(int_ctrl, data->regs + THS_SUN8I_INT_CTRL);
}

static const struct thermal_zone_of_device_ops sun8i_ths_thermal_ops = {
	.get_temp = sun8i_ths_get_temp,
};

static int sun8i_ths_probe(struct platform_device *pdev)
{
	struct sun8i_ths_data *data;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int ret, irq, i;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->desc = (struct sun8i_ths_desc *)of_device_get_match_data(dev);
	if (data->desc == NULL)
		return -EINVAL;

	data->dev = dev;
	platform_set_drvdata(pdev, data);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ths");
        if (!res) {
                dev_err(dev, "no memory resources defined\n");
                return -EINVAL;
        }

	data->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->regs)) {
		ret = PTR_ERR(data->regs);
		dev_err(dev, "failed to ioremap THS registers: %d\n", ret);
		return ret;
	}

	/*XXX: use SRAM device in the future, instead of direct access to regs */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "calibration");
        if (res) {
		data->cal_regs = devm_ioremap_resource(dev, res);
		if (IS_ERR(data->cal_regs)) {
			ret = PTR_ERR(data->cal_regs);
			dev_err(dev, "failed to ioremap calibration SRAM: %d\n", ret);
			return ret;
		}
        }

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(dev, "failed to get IRQ: %d\n", irq);
		return irq;
	}

	ret = devm_request_threaded_irq(dev, irq, NULL,
					sun8i_ths_irq_thread, IRQF_ONESHOT,
					dev_name(dev), data);
	if (ret)
		return ret;

	data->busclk = devm_clk_get(dev, "ahb");
	if (IS_ERR(data->busclk)) {
		ret = PTR_ERR(data->busclk);
		if (ret != -ENOENT) {
			dev_err(dev, "failed to get ahb clk: %d\n", ret);
			return ret;
		}

		data->busclk = NULL;
	}

	data->clk = devm_clk_get(dev, "ths");
	if (IS_ERR(data->clk)) {
		ret = PTR_ERR(data->clk);
		if (ret != -ENOENT) {
			dev_err(dev, "failed to get ths clk: %d\n", ret);
			return ret;
		}

		data->clk = NULL;
	}

	data->reset = devm_reset_control_get_optional(dev, "ahb");
	if (IS_ERR(data->reset)) {
		ret = PTR_ERR(data->reset);
		dev_err(dev, "failed to get reset: %d\n", ret);
		return ret;
	}

	ret = reset_control_deassert(data->reset);
	if (ret) {
		dev_err(dev, "reset deassert failed: %d\n", ret);
		return ret;
	}

	if (data->busclk) {
		ret = clk_prepare_enable(data->busclk);
		if (ret) {
			dev_err(dev, "failed to enable bus clk: %d\n", ret);
			goto err_assert_reset;
		}
	}

	if (data->clk) {
		ret = clk_prepare_enable(data->clk);
		if (ret) {
			dev_err(dev, "failed to enable ths clk: %d\n", ret);
			goto err_disable_bus;
		}

		ret = clk_set_rate(data->clk, THS_SUN8I_CLK_IN);
		if (ret)
			goto err_disable_ths;
	}

	for (i = 0; i < data->desc->num_sensors; i++) {
		data->tzds[i].data = data;
		data->tzds[i].tzd =
			devm_thermal_zone_of_sensor_register(dev, i,
							     &data->tzds[i],
							     &sun8i_ths_thermal_ops);
		if (IS_ERR(data->tzds[i].tzd)) {
			ret = PTR_ERR(data->tzds[i].tzd);
			dev_err(dev,
				"failed to register thermal zone: %d\n", ret);
			goto err_disable_ths;
		}
	}

	sun8i_ths_init(data);
	return 0;

err_disable_ths:
	if (data->clk)
		clk_disable_unprepare(data->clk);
err_disable_bus:
	if (data->busclk)
		clk_disable_unprepare(data->busclk);
err_assert_reset:
	reset_control_assert(data->reset);
	return ret;
}

static int sun8i_ths_remove(struct platform_device *pdev)
{
	struct sun8i_ths_data *data = platform_get_drvdata(pdev);

	reset_control_assert(data->reset);
	if (data->clk)
		clk_disable_unprepare(data->clk);
	if (data->busclk)
		clk_disable_unprepare(data->busclk);
	return 0;
}

struct sun8i_ths_sensor_desc sun8i_ths_h3_sensors[] = {
	{
		.data_int_en = THS_SUN8I_INT_CTRL_DATA0_IRQ_EN,
		.data_int_flag = THS_SUN8I_STAT_DATA0_IRQ_STS,
		.data_offset = THS_SUN8I_DATA0,
		.sense_en = THS_SUN8I_CTRL2_SENSE_EN0,
	},
};

struct sun8i_ths_sensor_desc sun8i_ths_a83t_sensors[] = {
	{
		.data_int_en = THS_SUN8I_INT_CTRL_DATA0_IRQ_EN,
		.data_int_flag = THS_SUN8I_STAT_DATA0_IRQ_STS,
		.data_offset = THS_SUN8I_DATA0,
		.sense_en = THS_SUN8I_CTRL2_SENSE_EN0,
	},
	{
		.data_int_en = THS_SUN8I_INT_CTRL_DATA1_IRQ_EN,
		.data_int_flag = THS_SUN8I_STAT_DATA1_IRQ_STS,
		.data_offset = THS_SUN8I_DATA1,
		.sense_en = THS_SUN8I_CTRL2_SENSE_EN1,
	},
	{
		.data_int_en = THS_SUN8I_INT_CTRL_DATA2_IRQ_EN,
		.data_int_flag = THS_SUN8I_STAT_DATA2_IRQ_STS,
		.data_offset = THS_SUN8I_DATA2,
		.sense_en = THS_SUN8I_CTRL2_SENSE_EN2,
	},
};

static const struct sun8i_ths_desc sun8i_ths_h3_desc = {
	.num_sensors = ARRAY_SIZE(sun8i_ths_h3_sensors),
	.sensors = sun8i_ths_h3_sensors,
	.calc_temp = sun8i_ths_calc_temp_h3,
	.has_cal1 = false,
};

static const struct sun8i_ths_desc sun8i_ths_a83t_desc = {
	.num_sensors = ARRAY_SIZE(sun8i_ths_a83t_sensors),
	.sensors = sun8i_ths_a83t_sensors,
	.calc_temp = sun8i_ths_calc_temp_a83t,
	.has_cal1 = true,
};

static const struct of_device_id sun8i_ths_id_table[] = {
	{ .compatible = "allwinner,sun8i-h3-ths", .data = &sun8i_ths_h3_desc },
	{ .compatible = "allwinner,sun8i-a83t-ths", .data = &sun8i_ths_a83t_desc },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, sun8i_ths_id_table);

static struct platform_driver sun8i_ths_driver = {
	.probe = sun8i_ths_probe,
	.remove = sun8i_ths_remove,
	.driver = {
		.name = "sun8i_ths",
		.of_match_table = sun8i_ths_id_table,
	},
};

module_platform_driver(sun8i_ths_driver);

MODULE_AUTHOR("Ondřej Jirman <megous@megous.com>");
MODULE_DESCRIPTION("Thermal sensor driver for Allwinner SUN8I SoCs");
MODULE_LICENSE("GPL v2");
