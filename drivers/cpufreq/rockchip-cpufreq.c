/*
 * Rockchip CPUFreq Driver
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/reboot.h>
#include <linux/slab.h>

#include "../clk/rockchip/clk.h"

#define MAX_PROP_NAME_LEN	3
#define LEAKAGE_TABLE_END	~1
#define INVALID_VALUE		0xff

#define REBOOT_FREQ		816000 /* kHz */

struct leakage_table {
	int min;
	int max;
	int value;
};

struct cluster_info {
	struct list_head list_head;
	cpumask_t cpus;
	int leakage;
	int lkg_volt_sel;
	int soc_version;
	bool set_opp;
	unsigned int reboot_freq;
	bool rebooting;
};

static LIST_HEAD(cluster_info_list);

static struct cluster_info *rockchip_cluster_info_lookup(int cpu)
{
	struct cluster_info *cluster;

	list_for_each_entry(cluster, &cluster_info_list, list_head) {
		if (cpumask_test_cpu(cpu, &cluster->cpus))
			return cluster;
	}

	return NULL;
}

static int rockchip_efuse_get_one_byte(struct device_node *np, char *porp_name,
				       int *value)
{
	struct nvmem_cell *cell;
	unsigned char *buf;
	size_t len;

	cell = of_nvmem_cell_get(np, porp_name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = (unsigned char *)nvmem_cell_read(cell, &len);

	nvmem_cell_put(cell);

	if (IS_ERR(buf))
		return PTR_ERR(buf);

	if (buf[0] == INVALID_VALUE)
		return -EINVAL;

	*value = buf[0];

	kfree(buf);

	return 0;
}

static int rk3399_get_soc_version(struct device_node *np, int *soc_version)
{
	int ret, version;

	if (of_property_match_string(np, "nvmem-cell-names",
				     "soc_version") < 0)
		return 0;

	ret = rockchip_efuse_get_one_byte(np, "soc_version",
					  &version);
	if (ret)
		return ret;

	*soc_version = (version & 0xf0) >> 4;

	return 0;
}

static const struct of_device_id rockchip_cpufreq_of_match[] = {
	{
		.compatible = "rockchip,rk3399",
		.data = (void *)&rk3399_get_soc_version,
	},
	{},
};

static int rockchip_get_leakage_table(struct device_node *np, char *porp_name,
				      struct leakage_table **table)
{
	struct leakage_table *lkg_table;
	const struct property *prop;
	int count, i;

	prop = of_find_property(np, porp_name, NULL);
	if (!prop)
		return -EINVAL;

	if (!prop->value)
		return -ENODATA;

	count = of_property_count_u32_elems(np, porp_name);
	if (count < 0)
		return -EINVAL;

	if (count % 3)
		return -EINVAL;

	lkg_table = kzalloc(sizeof(*lkg_table) * (count / 3 + 1), GFP_KERNEL);
	if (!lkg_table)
		return -ENOMEM;

	for (i = 0; i < count / 3; i++) {
		of_property_read_u32_index(np, porp_name, 3 * i,
					   &lkg_table[i].min);
		of_property_read_u32_index(np, porp_name, 3 * i + 1,
					   &lkg_table[i].max);
		of_property_read_u32_index(np, porp_name, 3 * i + 2,
					   &lkg_table[i].value);
	}
	lkg_table[i].min = 0;
	lkg_table[i].max = 0;
	lkg_table[i].value = LEAKAGE_TABLE_END;

	*table = lkg_table;

	return 0;
}

static int rockchip_get_leakage_sel(struct device_node *np, char *name,
				    int leakage, int *value)
{
	struct leakage_table *table;
	struct property *prop;
	int i, j = -1, ret;

	if (of_property_match_string(np, "nvmem-cell-names", "cpu_leakage") < 0)
		return 0;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return 0;

	ret = rockchip_get_leakage_table(np, name, &table);
	if (ret)
		return -EINVAL;

	for (i = 0; table[i].value != LEAKAGE_TABLE_END; i++) {
		if (leakage >= table[i].min)
			j = i;
	}
	if (j != -1)
		*value = table[j].value;
	else
		ret = -EINVAL;

	kfree(table);

	return ret;
}

static int rockchip_cpufreq_of_parse_dt(int cpu, struct cluster_info *cluster)
{
	int (*get_soc_version)(struct device_node *np, int *soc_version);
	const struct of_device_id *match;
	struct device_node *node, *np;
	struct clk *clk;
	struct device *dev;
	int ret, lkg_scaling_sel = -1;

	dev = get_cpu_device(cpu);
	if (!dev)
		return -ENODEV;

	ret = dev_pm_opp_of_get_sharing_cpus(dev, &cluster->cpus);
	if (ret)
		return ret;

	np = of_parse_phandle(dev->of_node, "operating-points-v2", 0);
	if (!np) {
		dev_info(dev, "OPP-v2 not supported\n");
		return -EINVAL;
	}

	cluster->soc_version = -1;
	node = of_find_node_by_path("/");
	match = of_match_node(rockchip_cpufreq_of_match, node);
	if (match && match->data) {
		get_soc_version = match->data;
		ret = get_soc_version(np, &cluster->soc_version);
		if (ret) {
			dev_err(dev, "Failed to get chip_version\n");
			return ret;
		}
	}

	if (of_property_read_u32(np, "reboot-freq", &cluster->reboot_freq))
		cluster->reboot_freq = REBOOT_FREQ;

	ret = rockchip_efuse_get_one_byte(np, "cpu_leakage", &cluster->leakage);
	if (ret)
		dev_err(dev, "Failed to get cpu_leakage\n");
	else
		dev_info(dev, "leakage=%d\n", cluster->leakage);

	cluster->lkg_volt_sel = -1;
	ret = rockchip_get_leakage_sel(np, "leakage-voltage-sel",
				       cluster->leakage,
				       &cluster->lkg_volt_sel);
	if (ret) {
		dev_err(dev, "Failed to get voltage-sel\n");
		return ret;
	}

	ret = rockchip_get_leakage_sel(np, "leakage-scaling-sel",
				       cluster->leakage,
				       &lkg_scaling_sel);
	if (ret) {
		dev_err(dev, "Failed to get scaling-sel\n");
		return ret;
	} else if (lkg_scaling_sel >= 0) {
		clk = of_clk_get_by_name(np, NULL);
		if (IS_ERR(clk)) {
			dev_err(dev, "Failed to get opp clk");
			return PTR_ERR(clk);
		}
		ret = rockchip_pll_clk_adaptive_scaling(clk, lkg_scaling_sel);
		if (ret) {
			dev_err(dev, "Failed to adaptive scaling\n");
			return ret;
		}
	}

	return 0;
}

static int rockchip_cpufreq_set_opp_info(int cpu, struct cluster_info *cluster)
{
	struct device *dev;
	char name[MAX_PROP_NAME_LEN];
	int ret, version;

	dev = get_cpu_device(cpu);
	if (!dev)
		return -ENODEV;

	if (cluster->soc_version != -1 && cluster->lkg_volt_sel != -1)
		snprintf(name, MAX_PROP_NAME_LEN, "S%d-L%d",
			 cluster->soc_version,
			 cluster->lkg_volt_sel);
	else if (cluster->soc_version != -1 && cluster->lkg_volt_sel == -1)
		snprintf(name, MAX_PROP_NAME_LEN, "S%d", cluster->soc_version);
	else if (cluster->soc_version == -1 && cluster->lkg_volt_sel != -1)
		snprintf(name, MAX_PROP_NAME_LEN, "L%d", cluster->lkg_volt_sel);
	else
		return 0;

	ret = dev_pm_opp_set_prop_name(dev, name);
	if (ret) {
		dev_err(dev, "Failed to set prop name\n");
		return ret;
	}

	if (cluster->soc_version != -1) {
		version = BIT(cluster->soc_version);
		ret = dev_pm_opp_set_supported_hw(dev, &version, 1);
		if (ret) {
			dev_err(dev, "Failed to set supported hardware\n");
			return ret;
		}
	}

	return 0;
}

static int rockchip_hotcpu_notifier(struct notifier_block *nb,
				    unsigned long action, void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;
	struct cluster_info *cluster;
	cpumask_t cpus;
	int number, ret;

	cluster = rockchip_cluster_info_lookup(cpu);
	if (!cluster)
		return NOTIFY_OK;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_ONLINE:
		if (cluster->set_opp) {
			ret = rockchip_cpufreq_set_opp_info(cpu, cluster);
			if (ret)
				pr_err("Failed to set cpu%d opp_info\n", cpu);
			cluster->set_opp = false;
		}
		break;

	case CPU_POST_DEAD:
		cpumask_and(&cpus, &cluster->cpus, cpu_online_mask);
		number = cpumask_weight(&cpus);
		if (!number)
			cluster->set_opp = true;
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block rockchip_hotcpu_nb = {
	.notifier_call = rockchip_hotcpu_notifier,
};

static int rockchip_reboot_notifier(struct notifier_block *nb,
				    unsigned long action, void *ptr)
{
	int cpu;
	struct cluster_info *cluster;

	list_for_each_entry(cluster, &cluster_info_list, list_head) {
		cpu = cpumask_first_and(&cluster->cpus, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			continue;
		cluster->rebooting = true;
		cpufreq_update_policy(cpu);
	}

	return NOTIFY_OK;
}

static struct notifier_block rockchip_reboot_nb = {
	.notifier_call = rockchip_reboot_notifier,
};

static int rockchip_cpufreq_notifier(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	struct cluster_info *cluster;

	if (event != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	list_for_each_entry(cluster, &cluster_info_list, list_head) {
		if (cluster->rebooting &&
		    cpumask_test_cpu(policy->cpu, &cluster->cpus)) {
			if (cluster->reboot_freq < policy->max)
				policy->max = cluster->reboot_freq;
			policy->min = policy->max;
			pr_info("cpu%d limit freq=%d min=%d max=%d\n",
				policy->cpu, cluster->reboot_freq,
				policy->min, policy->max);
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block rockchip_cpufreq_nb = {
	.notifier_call = rockchip_cpufreq_notifier,
};

static int __init rockchip_cpufreq_driver_init(void)
{
	struct platform_device *pdev;
	struct cluster_info *cluster, *pos;
	int cpu, first_cpu, ret, i = 0;

	for_each_possible_cpu(cpu) {
		cluster = rockchip_cluster_info_lookup(cpu);
		if (cluster)
			continue;

		cluster = kzalloc(sizeof(*cluster), GFP_KERNEL);
		if (!cluster)
			return -ENOMEM;

		ret = rockchip_cpufreq_of_parse_dt(cpu, cluster);
		if (ret) {
			if (ret != -ENOENT) {
				pr_err("Failed to cpu%d parse_dt\n", cpu);
				return ret;
			}

			/*
			 * As the OPP document said, only one OPP binding
			 * should be used per device.
			 * And if there are multiple clusters on rockchip
			 * platforms, we should use operating-points-v2.
			 * So if don't support operating-points-v2, there must
			 * be only one cluster, the list shuold be null.
			 */
			list_for_each_entry(pos, &cluster_info_list, list_head)
				i++;
			if (i)
				return ret;
			/*
			 * If don't support operating-points-v2, there is no
			 * need to register notifiers.
			 */
			goto next;
		}

		first_cpu = cpumask_first_and(&cluster->cpus, cpu_online_mask);
		ret = rockchip_cpufreq_set_opp_info(first_cpu, cluster);
		if (ret) {
			pr_err("Failed to set cpu%d opp_info\n", first_cpu);
			return ret;
		}

		list_add(&cluster->list_head, &cluster_info_list);
	}

	register_hotcpu_notifier(&rockchip_hotcpu_nb);
	register_reboot_notifier(&rockchip_reboot_nb);
	cpufreq_register_notifier(&rockchip_cpufreq_nb,
				  CPUFREQ_POLICY_NOTIFIER);

next:
	pdev = platform_device_register_simple("cpufreq-dt", -1, NULL, 0);

	return PTR_ERR_OR_ZERO(pdev);
}
module_init(rockchip_cpufreq_driver_init);

MODULE_AUTHOR("Finley Xiao <finley.xiao@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip cpufreq driver");
MODULE_LICENSE("GPL v2");
