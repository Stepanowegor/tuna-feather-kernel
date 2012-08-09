/*
 * OMAP SmartReflex Voltage Control
 *
 * Author: Thara Gopinath	<thara@ti.com>
 *
 * Copyright (C) 2010 Texas Instruments, Inc.
 * Thara Gopinath <thara@ti.com>
 *
 * Copyright (C) 2008 Nokia Corporation
 * Kalle Jokiniemi
 *
 * Copyright (C) 2007 Texas Instruments, Inc.
 * Lesly A M <x0080970@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>

#include <plat/common.h>

#include "pm.h"
#include "dvfs.h"
#include "smartreflex.h"
#include "voltage.h"

#define SMARTREFLEX_NAME_LEN	16
#define NVALUE_NAME_LEN		40
#define SR_DISABLE_TIMEOUT	200

struct omap_sr {
	int				srid;
	int				ip_type;
	int				nvalue_count;
	bool				autocomp_active;
	u32				clk_length;
	u32				err_weight;
	u32				err_minlimit;
	u32				err_maxlimit;
	u32				accum_data;
	u32				senn_avgweight;
	u32				senp_avgweight;
	u32				senp_mod;
	u32				senn_mod;
	unsigned int			irq;
	bool				irq_enabled;
	void __iomem			*base;
	struct platform_device		*pdev;
	struct list_head		node;
	struct omap_sr_nvalue_table	*nvalue_table;
	struct voltagedomain		*voltdm;
	/* Managed by class driver as needed */
	void				*voltdm_cdata;
	struct dentry			*dbg_dir;
};

/* sr_list contains all the instances of smartreflex module */
static LIST_HEAD(sr_list);

static struct omap_sr_class_data *sr_class;
static struct omap_sr_pmic_data *sr_pmic_data;
static struct dentry		*sr_dbg_dir;

static u32 mpumin = OMAP4_VP_MPU_VLIMITTO_VDDMIN;
static u32 ivamin = OMAP4_VP_IVA_VLIMITTO_VDDMIN;
static u32 coremin = OMAP4_VP_CORE_VLIMITTO_VDDMIN;
bool enable_highvolt_sr = 0;

#define LOWFLOOR 700000
#define LOWCEILING 1000000

static inline void sr_write_reg(struct omap_sr *sr, unsigned offset, u32 value)
{
	__raw_writel(value, (sr->base + offset));
}

static inline void sr_modify_reg(struct omap_sr *sr, unsigned offset, u32 mask,
					u32 value)
{
	u32 reg_val;

	/*
	 * Smartreflex error config register is special as it contains
	 * certain status bits which if written a 1 into means a clear
	 * of those bits. So in order to make sure no accidental write of
	 * 1 happens to those status bits, do a clear of them in the read
	 * value. This mean this API doesn't rewrite values in these bits
	 * if they are currently set, but does allow the caller to write
	 * those bits.
	 */
	if (sr->ip_type == SR_TYPE_V1 && offset == ERRCONFIG_V1)
		mask |= ERRCONFIG_STATUS_V1_MASK;
	else if (sr->ip_type == SR_TYPE_V2 && offset == ERRCONFIG_V2)
		mask |= ERRCONFIG_VPBOUNDINTST_V2;

	reg_val = __raw_readl(sr->base + offset);
	reg_val &= ~mask;

	value &= mask;

	reg_val |= value;

	__raw_writel(reg_val, (sr->base + offset));
}

static inline u32 sr_read_reg(struct omap_sr *sr, unsigned offset)
{
	return __raw_readl(sr->base + offset);
}

static struct omap_sr *_sr_lookup(struct voltagedomain *voltdm)
{
	struct omap_sr *sr_info;

	if (!voltdm) {
		pr_err("%s: Null voltage domain passed!\n", __func__);
		return ERR_PTR(-EINVAL);
	}

	list_for_each_entry(sr_info, &sr_list, node) {
		if (voltdm == sr_info->voltdm)
			return sr_info;
	}

	return ERR_PTR(-ENODATA);
}

static inline u32 notifier_to_irqen_v1(u8 notify_flags)
{
	u32 val;

	val = (notify_flags & SR_NOTIFY_MCUACCUM) ?
		ERRCONFIG_MCUACCUMINTEN : 0;
	val |= (notify_flags & SR_NOTIFY_MCUVALID) ?
		ERRCONFIG_MCUVALIDINTEN : 0;
	val |= (notify_flags & SR_NOTIFY_MCUBOUND) ?
		ERRCONFIG_MCUBOUNDINTEN : 0;
	val |= (notify_flags & SR_NOTIFY_MCUDISACK) ?
		ERRCONFIG_MCUDISACKINTEN : 0;

	return val;
}

static inline u32 notifier_to_irqen_v2(u8 notify_flags)
{
	u32 val;

	val = (notify_flags & SR_NOTIFY_MCUACCUM) ?
		IRQENABLE_MCUACCUMINT : 0;
	val |= (notify_flags & SR_NOTIFY_MCUVALID) ?
		IRQENABLE_MCUVALIDINT : 0;
	val |= (notify_flags & SR_NOTIFY_MCUBOUND) ?
		IRQENABLE_MCUBOUNDSINT : 0;
	val |= (notify_flags & SR_NOTIFY_MCUDISACK) ?
		IRQENABLE_MCUDISABLEACKINT : 0;

	return val;
}

static inline u8 irqstat_to_notifier_v1(u32 status)
{
	u8 val;

	val = (status & ERRCONFIG_MCUACCUMINTST) ?
		SR_NOTIFY_MCUACCUM : 0;
	val |= (status & ERRCONFIG_MCUVALIDINTEN) ?
		SR_NOTIFY_MCUVALID : 0;
	val |= (status & ERRCONFIG_MCUBOUNDINTEN) ?
		SR_NOTIFY_MCUBOUND : 0;
	val |= (status & ERRCONFIG_MCUDISACKINTEN) ?
		SR_NOTIFY_MCUDISACK : 0;

	return val;
}

static inline u8 irqstat_to_notifier_v2(u32 status)
{
	u8 val;

	val = (status & IRQENABLE_MCUACCUMINT) ?
		SR_NOTIFY_MCUACCUM : 0;
	val |= (status & IRQENABLE_MCUVALIDINT) ?
		SR_NOTIFY_MCUVALID : 0;
	val |= (status & IRQENABLE_MCUBOUNDSINT) ?
		SR_NOTIFY_MCUBOUND : 0;
	val |= (status & IRQENABLE_MCUDISABLEACKINT) ?
		SR_NOTIFY_MCUDISACK : 0;

	return val;
}


static irqreturn_t sr_interrupt(int irq, void *data)
{
	struct omap_sr *sr_info = (struct omap_sr *)data;
	u32 status = 0;
	u32 value = 0;

	if (sr_info->ip_type == SR_TYPE_V1) {
		/* Status bits are one bit before enable bits in v1 */
		value = notifier_to_irqen_v1(sr_class->notify_flags) >> 1;

		/* Read the status bits */
		status = sr_read_reg(sr_info, ERRCONFIG_V1);
		status &= value;

		/* Clear them by writing back */
		sr_modify_reg(sr_info, ERRCONFIG_V1, value, status);

		value = irqstat_to_notifier_v1(status);
	} else if (sr_info->ip_type == SR_TYPE_V2) {
		value = notifier_to_irqen_v2(sr_class->notify_flags);
		/* Read the status bits */
		status = sr_read_reg(sr_info, IRQSTATUS);
		status &= value;

		/* Clear them by writing back */
		sr_write_reg(sr_info, IRQSTATUS, status);
		value = irqstat_to_notifier_v2(status);
	}

	/* Attempt some resemblance of recovery! */
	if (!value) {
		dev_err(&sr_info->pdev->dev, "%s: Spurious interrupt!"
			"status = 0x%08x. Disabling to prevent spamming!!\n",
			__func__, status);
		disable_irq_nosync(sr_info->irq);
		sr_info->irq_enabled = false;
	} else {
		/* If the caller reports inability to handle, disable as well */
		if (sr_class->notify && sr_class->notify(sr_info->voltdm,
						sr_info->voltdm_cdata, value)) {
			dev_err(&sr_info->pdev->dev, "%s: Callback cant handle!"
				"status=0x%08x. Disabling to prevent spam!!\n",
				__func__, status);
			disable_irq_nosync(sr_info->irq);
			sr_info->irq_enabled = false;
		}

	}

	return IRQ_HANDLED;
}

static void sr_set_clk_length(struct omap_sr *sr)
{
	struct clk *sys_ck;
	u32 sys_clk_speed;

	if (cpu_is_omap34xx())
		sys_ck = clk_get(NULL, "sys_ck");
	else
		sys_ck = clk_get(NULL, "sys_clkin_ck");

	if (IS_ERR(sys_ck)) {
		dev_err(&sr->pdev->dev, "%s: unable to get sys clk\n",
			__func__);
		return;
	}
	sys_clk_speed = clk_get_rate(sys_ck);
	clk_put(sys_ck);

	switch (sys_clk_speed) {
	case 12000000:
		sr->clk_length = SRCLKLENGTH_12MHZ_SYSCLK;
		break;
	case 13000000:
		sr->clk_length = SRCLKLENGTH_13MHZ_SYSCLK;
		break;
	case 19200000:
		sr->clk_length = SRCLKLENGTH_19MHZ_SYSCLK;
		break;
	case 26000000:
		sr->clk_length = SRCLKLENGTH_26MHZ_SYSCLK;
		break;
	case 38400000:
		sr->clk_length = SRCLKLENGTH_38MHZ_SYSCLK;
		break;
	default:
		dev_err(&sr->pdev->dev, "%s: Invalid sysclk value: %d\n",
			__func__, sys_clk_speed);
		break;
	}
}

static void sr_set_regfields(struct omap_sr *sr)
{
	/*
	 * For time being these values are defined in smartreflex.h
	 * and populated during init. May be they can be moved to board
	 * file or pmic specific data structure. In that case these structure
	 * fields will have to be populated using the pdata or pmic structure.
	 */
	if (cpu_is_omap34xx() || cpu_is_omap44xx()) {
		sr->err_weight = OMAP3430_SR_ERRWEIGHT;
		sr->err_maxlimit = OMAP3430_SR_ERRMAXLIMIT;
		sr->accum_data = OMAP3430_SR_ACCUMDATA;
		if (!(strcmp(sr->voltdm->name, "mpu"))) {
			sr->senn_avgweight = OMAP3430_SR1_SENNAVGWEIGHT;
			sr->senp_avgweight = OMAP3430_SR1_SENPAVGWEIGHT;
		} else {
			sr->senn_avgweight = OMAP3430_SR2_SENNAVGWEIGHT;
			sr->senp_avgweight = OMAP3430_SR2_SENPAVGWEIGHT;
		}
	}
}

static void sr_start_vddautocomp(struct omap_sr *sr)
{
	int r;
	if (!sr_class || !(sr_class->enable) || !(sr_class->configure)) {
		dev_warn(&sr->pdev->dev,
			"%s: smartreflex class driver not registered\n",
			__func__);
		return;
	}

	/* pause dvfs from interfereing with our operations */
	mutex_lock(&omap_dvfs_lock);

	if (sr_class->init &&
	    sr_class->init(sr->voltdm, &sr->voltdm_cdata,
		    sr_class->class_priv_data)) {
		dev_err(&sr->pdev->dev,
			"%s: SRClass initialization failed\n", __func__);
		mutex_unlock(&omap_dvfs_lock);
		return;
	}

	r = sr_class->enable(sr->voltdm,  sr->voltdm_cdata,
			     omap_voltage_get_curr_vdata(sr->voltdm));
	if (!r)
		sr->autocomp_active = true;
	mutex_unlock(&omap_dvfs_lock);
}

static void sr_stop_vddautocomp(struct omap_sr *sr)
{
	if (!sr_class || !(sr_class->disable)) {
		dev_warn(&sr->pdev->dev,
			"%s: smartreflex class driver not registered\n",
			__func__);
		return;
	}

	if (sr->autocomp_active) {
		/* Pause dvfs from interfereing with our operations */
		mutex_lock(&omap_dvfs_lock);
		sr_class->disable(sr->voltdm, sr->voltdm_cdata,
				  omap_voltage_get_curr_vdata(sr->voltdm), 1);
		if (sr_class->deinit &&
		    sr_class->deinit(sr->voltdm, &sr->voltdm_cdata,
			    sr_class->class_priv_data)) {
			dev_err(&sr->pdev->dev,
				"%s: SR[%d]Class deinitialization failed\n",
				__func__, sr->srid);
		}
		sr->autocomp_active = false;
		mutex_unlock(&omap_dvfs_lock);
	}
}

/*
 * This function handles the intializations which have to be done
 * only when both sr device and class driver regiter has
 * completed. This will be attempted to be called from both sr class
 * driver register and sr device intializtion API's. Only one call
 * will ultimately succeed.
 *
 * Currently this function registers interrrupt handler for a particular SR
 * if smartreflex class driver is already registered and has
 * requested for interrupts and the SR interrupt line in present.
 */
static int sr_late_init(struct omap_sr *sr_info)
{
	char *name;
	struct omap_sr_data *pdata = sr_info->pdev->dev.platform_data;
	struct resource *mem;
	int ret = 0;

	if (sr_class->notify && sr_class->notify_flags && sr_info->irq) {
		name = kasprintf(GFP_KERNEL, "sr_%s", sr_info->voltdm->name);
		if (name == NULL) {
			ret = -ENOMEM;
			goto error;
		}
		ret = request_irq(sr_info->irq, sr_interrupt,
				0, name, (void *)sr_info);
		if (ret)
			goto error;
		disable_irq(sr_info->irq);
	}

	if (pdata && pdata->enable_on_init)
		sr_start_vddautocomp(sr_info);

	return ret;

error:
	iounmap(sr_info->base);
	mem = platform_get_resource(sr_info->pdev, IORESOURCE_MEM, 0);
	release_mem_region(mem->start, resource_size(mem));
	list_del(&sr_info->node);
	dev_err(&sr_info->pdev->dev, "%s: ERROR in registering"
		"interrupt handler. Smartreflex will"
		"not function as desired\n", __func__);
	kfree(name);
	kfree(sr_info);
	return ret;
}

static void sr_v1_disable(struct omap_sr *sr)
{
	int timeout = 0;
	int errconf_val = ERRCONFIG_MCUACCUMINTST | ERRCONFIG_MCUVALIDINTST |
			ERRCONFIG_MCUBOUNDINTST;

	/* Enable MCUDisableAcknowledge interrupt */
	sr_modify_reg(sr, ERRCONFIG_V1,
			ERRCONFIG_MCUDISACKINTEN, ERRCONFIG_MCUDISACKINTEN);

	/* SRCONFIG - disable SR */
	sr_modify_reg(sr, SRCONFIG, SRCONFIG_SRENABLE, 0x0);

	/* Disable all other SR interrupts and clear the status as needed */
	if (sr_read_reg(sr, ERRCONFIG_V1) & ERRCONFIG_VPBOUNDINTST_V1)
		errconf_val |= ERRCONFIG_VPBOUNDINTST_V1;
	sr_modify_reg(sr, ERRCONFIG_V1,
			(ERRCONFIG_MCUACCUMINTEN | ERRCONFIG_MCUVALIDINTEN |
			ERRCONFIG_MCUBOUNDINTEN | ERRCONFIG_VPBOUNDINTEN_V1),
			errconf_val);

	/*
	 * Wait for SR to be disabled.
	 * wait until ERRCONFIG.MCUDISACKINTST = 1. Typical latency is 1us.
	 */
	omap_test_timeout((sr_read_reg(sr, ERRCONFIG_V1) &
			ERRCONFIG_MCUDISACKINTST), SR_DISABLE_TIMEOUT,
			timeout);

	if (timeout >= SR_DISABLE_TIMEOUT)
		dev_warn(&sr->pdev->dev, "%s: Smartreflex disable timedout\n",
			__func__);

	/* Disable MCUDisableAcknowledge interrupt & clear pending interrupt */
	sr_modify_reg(sr, ERRCONFIG_V1, ERRCONFIG_MCUDISACKINTEN,
			ERRCONFIG_MCUDISACKINTST);
}

static void sr_v2_disable(struct omap_sr *sr)
{
	int timeout = 0;

	/* Enable MCUDisableAcknowledge interrupt */
	sr_write_reg(sr, IRQENABLE_SET, IRQENABLE_MCUDISABLEACKINT);

	/* SRCONFIG - disable SR */
	sr_modify_reg(sr, SRCONFIG, SRCONFIG_SRENABLE, 0x0);

	/*
	 * Disable all other SR interrupts and clear the status
	 * write to status register ONLY on need basis - only if status
	 * is set.
	 */
	if (sr_read_reg(sr, ERRCONFIG_V2) & ERRCONFIG_VPBOUNDINTST_V2)
		sr_modify_reg(sr, ERRCONFIG_V2, ERRCONFIG_VPBOUNDINTEN_V2,
			ERRCONFIG_VPBOUNDINTST_V2);
	else
		sr_modify_reg(sr, ERRCONFIG_V2, ERRCONFIG_VPBOUNDINTEN_V2,
				0x0);
	sr_write_reg(sr, IRQSTATUS, (IRQSTATUS_MCUACCUMINT |
			IRQSTATUS_MCVALIDINT |
			IRQSTATUS_MCBOUNDSINT));
	sr_write_reg(sr, IRQENABLE_CLR, (IRQENABLE_MCUACCUMINT |
			IRQENABLE_MCUVALIDINT |
			IRQENABLE_MCUBOUNDSINT));

	/*
	 * Wait for SR to be disabled.
	 * wait until IRQSTATUS.MCUDISACKINTST = 1. Typical latency is 1us.
	 */
	omap_test_timeout((sr_read_reg(sr, IRQSTATUS) &
			IRQSTATUS_MCUDISABLEACKINT), SR_DISABLE_TIMEOUT,
			timeout);

	if (timeout >= SR_DISABLE_TIMEOUT)
		dev_warn(&sr->pdev->dev, "%s: Smartreflex disable timedout\n",
			__func__);

	/* Disable MCUDisableAcknowledge interrupt & clear pending interrupt */
	sr_write_reg(sr, IRQSTATUS, IRQSTATUS_MCUDISABLEACKINT);
	sr_write_reg(sr, IRQENABLE_CLR, IRQENABLE_MCUDISABLEACKINT);
}

static u32 sr_retrieve_nvalue(struct omap_sr *sr, u32 efuse_offs)
{
	int i;

	if (!sr->nvalue_table) {
		dev_warn(&sr->pdev->dev, "%s: Missing ntarget value table\n",
			__func__);
		return 0;
	}

	for (i = 0; i < sr->nvalue_count; i++) {
		if (sr->nvalue_table[i].efuse_offs == efuse_offs)
			return sr->nvalue_table[i].nvalue;
	}

	return 0;
}

/* Public Functions */

/**
 * is_sr_enabled() - is Smart reflex enabled for this domain?
 * @voltdm: voltage domain to check
 *
 * Returns 0 if SR is enabled for this domain, else returns err
 */
bool is_sr_enabled(struct voltagedomain *voltdm)
{
	struct omap_sr *sr;
	if (IS_ERR_OR_NULL(voltdm)) {
		pr_warning("%s: invalid param voltdm\n", __func__);
		return false;
	}
	sr = _sr_lookup(voltdm);
	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return false;
	}
	return sr->autocomp_active;
}

/**
 * sr_configure_errgen() - Configures the smrtreflex to perform AVS using the
 *			 error generator module.
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 *
 * This API is to be called from the smartreflex class driver to
 * configure the error generator module inside the smartreflex module.
 * SR settings if using the ERROR module inside Smartreflex.
 * SR CLASS 3 by default uses only the ERROR module where as
 * SR CLASS 2 can choose between ERROR module and MINMAXAVG
 * module. Returns 0 on success and error value in case of failure.
 */
int sr_configure_errgen(struct voltagedomain *voltdm)
{
	u32 sr_config, sr_errconfig, errconfig_offs, vpboundint_en;
	u32 vpboundint_st, senp_en = 0, senn_en = 0;
	u8 senp_shift, senn_shift;
	struct omap_sr *sr = _sr_lookup(voltdm);

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return -EINVAL;
	}

	if (!sr->clk_length)
		sr_set_clk_length(sr);

	senp_en = sr->senp_mod;
	senn_en = sr->senn_mod;

	sr_config = (sr->clk_length << SRCONFIG_SRCLKLENGTH_SHIFT) |
		SRCONFIG_SENENABLE | SRCONFIG_ERRGEN_EN;

	if (sr->ip_type == SR_TYPE_V1) {
		sr_config |= SRCONFIG_DELAYCTRL;
		senn_shift = SRCONFIG_SENNENABLE_V1_SHIFT;
		senp_shift = SRCONFIG_SENPENABLE_V1_SHIFT;
		errconfig_offs = ERRCONFIG_V1;
		vpboundint_en = ERRCONFIG_VPBOUNDINTEN_V1;
		vpboundint_st = ERRCONFIG_VPBOUNDINTST_V1;
	} else if (sr->ip_type == SR_TYPE_V2) {
		senn_shift = SRCONFIG_SENNENABLE_V2_SHIFT;
		senp_shift = SRCONFIG_SENPENABLE_V2_SHIFT;
		errconfig_offs = ERRCONFIG_V2;
		vpboundint_en = ERRCONFIG_VPBOUNDINTEN_V2;
		vpboundint_st = ERRCONFIG_VPBOUNDINTST_V2;
	} else {
		dev_err(&sr->pdev->dev, "%s: Trying to Configure smartreflex"
			"module without specifying the ip\n", __func__);
		return -EINVAL;
	}

	sr_config |= ((senn_en << senn_shift) | (senp_en << senp_shift));
	sr_write_reg(sr, SRCONFIG, sr_config);
	sr_errconfig = (sr->err_weight << ERRCONFIG_ERRWEIGHT_SHIFT) |
		(sr->err_maxlimit << ERRCONFIG_ERRMAXLIMIT_SHIFT) |
		(sr->err_minlimit <<  ERRCONFIG_ERRMINLIMIT_SHIFT);
	sr_modify_reg(sr, errconfig_offs, (SR_ERRWEIGHT_MASK |
		SR_ERRMAXLIMIT_MASK | SR_ERRMINLIMIT_MASK),
		sr_errconfig);

	/* Enabling the interrupts if the ERROR module is used */
	sr_modify_reg(sr, errconfig_offs, (vpboundint_en | vpboundint_st),
		      vpboundint_en);

	return 0;
}

/**
 * sr_disable_errgen() - Disables SmartReflex AVS module's errgen component
 * @voltdm: voltagedomain pointer to which the SR module to be configured belongs to.
 *
 * This API is to be called from the smartreflex class driver to
 * disable the error generator module inside the smartreflex module.
 *
 * Returns 0 on success and error value in case of failure.
 */
int sr_disable_errgen(struct voltagedomain *voltdm)
{
	u32 errconfig_offs, vpboundint_en;
	u32 vpboundint_st;
	struct omap_sr *sr = _sr_lookup(voltdm);

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return -EINVAL;
	}

	if (sr->ip_type == SR_TYPE_V1) {
		errconfig_offs = ERRCONFIG_V1;
		vpboundint_en = ERRCONFIG_VPBOUNDINTEN_V1;
		vpboundint_st = ERRCONFIG_VPBOUNDINTST_V1;
	} else if (sr->ip_type == SR_TYPE_V2) {
		errconfig_offs = ERRCONFIG_V2;
		vpboundint_en = ERRCONFIG_VPBOUNDINTEN_V2;
		vpboundint_st = ERRCONFIG_VPBOUNDINTST_V2;
	} else {
		dev_err(&sr->pdev->dev, "%s: Trying to Configure smartreflex"
			"module without specifying the ip\n", __func__);
		return -EINVAL;
	}

	/* Disable the interrupts of ERROR module */
	sr_modify_reg(sr, errconfig_offs, vpboundint_en | vpboundint_st, 0);

	/* Disable the Sensor and errorgen */
	sr_modify_reg(sr, SRCONFIG, SRCONFIG_SENENABLE | SRCONFIG_ERRGEN_EN, 0);

	return 0;
}

/**
 * sr_configure_minmax() - Configures the smrtreflex to perform AVS using the
 *			 minmaxavg module.
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 *
 * This API is to be called from the smartreflex class driver to
 * configure the minmaxavg module inside the smartreflex module.
 * SR settings if using the ERROR module inside Smartreflex.
 * SR CLASS 3 by default uses only the ERROR module where as
 * SR CLASS 2 can choose between ERROR module and MINMAXAVG
 * module. Returns 0 on success and error value in case of failure.
 */
int sr_configure_minmax(struct voltagedomain *voltdm)
{
	u32 sr_config, sr_avgwt;
	u32 senp_en = 0, senn_en = 0;
	u8 senp_shift, senn_shift;
	struct omap_sr *sr = _sr_lookup(voltdm);

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return -EINVAL;
	}

	if (!sr->clk_length)
		sr_set_clk_length(sr);

	senp_en = sr->senp_mod;
	senn_en = sr->senn_mod;

	sr_config = (sr->clk_length << SRCONFIG_SRCLKLENGTH_SHIFT) |
		SRCONFIG_SENENABLE |
		(sr->accum_data << SRCONFIG_ACCUMDATA_SHIFT);

	if (sr->ip_type == SR_TYPE_V1) {
		sr_config |= SRCONFIG_DELAYCTRL;
		senn_shift = SRCONFIG_SENNENABLE_V1_SHIFT;
		senp_shift = SRCONFIG_SENPENABLE_V1_SHIFT;
	} else if (sr->ip_type == SR_TYPE_V2) {
		senn_shift = SRCONFIG_SENNENABLE_V2_SHIFT;
		senp_shift = SRCONFIG_SENPENABLE_V2_SHIFT;
	} else {
		dev_err(&sr->pdev->dev, "%s: Trying to Configure smartreflex"
			"module without specifying the ip\n", __func__);
		return -EINVAL;
	}

	sr_config |= ((senn_en << senn_shift) | (senp_en << senp_shift));
	sr_write_reg(sr, SRCONFIG, sr_config);
	sr_avgwt = (sr->senp_avgweight << AVGWEIGHT_SENPAVGWEIGHT_SHIFT) |
		(sr->senn_avgweight << AVGWEIGHT_SENNAVGWEIGHT_SHIFT);
	sr_write_reg(sr, AVGWEIGHT, sr_avgwt);

	/*
	 * Enabling the interrupts if MINMAXAVG module is used.
	 * TODO: check if all the interrupts are mandatory
	 */
	if (sr->ip_type == SR_TYPE_V1) {
		sr_modify_reg(sr, ERRCONFIG_V1,
			(ERRCONFIG_MCUACCUMINTEN | ERRCONFIG_MCUVALIDINTEN |
			ERRCONFIG_MCUBOUNDINTEN),
			(ERRCONFIG_MCUACCUMINTEN | ERRCONFIG_MCUACCUMINTST |
			 ERRCONFIG_MCUVALIDINTEN | ERRCONFIG_MCUVALIDINTST |
			 ERRCONFIG_MCUBOUNDINTEN | ERRCONFIG_MCUBOUNDINTST));
	} else if (sr->ip_type == SR_TYPE_V2) {
		sr_write_reg(sr, IRQSTATUS,
			IRQSTATUS_MCUACCUMINT | IRQSTATUS_MCVALIDINT |
			IRQSTATUS_MCBOUNDSINT | IRQSTATUS_MCUDISABLEACKINT);
		sr_write_reg(sr, IRQENABLE_SET,
			IRQENABLE_MCUACCUMINT | IRQENABLE_MCUVALIDINT |
			IRQENABLE_MCUBOUNDSINT | IRQENABLE_MCUDISABLEACKINT);
	}

	return 0;
}

/**
 * sr_enable() - Enables the smartreflex module.
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 * @volt_data:	The voltage at which the Voltage domain associated with
 *		the smartreflex module is operating at.
 *		This is required only to program the correct Ntarget value.
 *
 * This API is to be called from the smartreflex class driver to
 * enable a smartreflex module. Returns 0 on success. Returns error
 * value if the voltage passed is wrong or if ntarget value is wrong.
 */
int sr_enable(struct voltagedomain *voltdm, struct omap_volt_data *volt_data)
{
	u32 nvalue_reciprocal;
	struct omap_sr *sr = _sr_lookup(voltdm);
	int ret;

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(volt_data)) {
		dev_warn(&sr->pdev->dev, "%s: bad voltage data\n", __func__);
		return -EINVAL;
	}

	nvalue_reciprocal = sr_retrieve_nvalue(sr, volt_data->sr_efuse_offs);

	if (!nvalue_reciprocal) {
		dev_warn(&sr->pdev->dev, "%s: NVALUE = 0 at voltage %ld\n",
			__func__, omap_get_operation_voltage(volt_data));
		return -ENODATA;
	}

	/* errminlimit is opp dependent and hence linked to voltage */
	sr->err_minlimit = volt_data->sr_errminlimit;

	pm_runtime_get_sync(&sr->pdev->dev);

	/* Check if SR is already enabled. If yes do nothing */
	if (sr_read_reg(sr, SRCONFIG) & SRCONFIG_SRENABLE)
		return 0;

	/* Configure SR */
	ret = sr_class->configure(voltdm, sr->voltdm_cdata);
	if (ret)
		return ret;

	sr_write_reg(sr, NVALUERECIPROCAL, nvalue_reciprocal);

	/* SRCONFIG - enable SR */
	sr_modify_reg(sr, SRCONFIG, SRCONFIG_SRENABLE, SRCONFIG_SRENABLE);
	return 0;
}

/**
 * sr_disable() - Disables the smartreflex module.
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 *
 * This API is to be called from the smartreflex class driver to
 * disable a smartreflex module.
 */
void sr_disable(struct voltagedomain *voltdm)
{
	struct omap_sr *sr = _sr_lookup(voltdm);

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return;
	}

	/* Check if SR clocks are already disabled. If yes do nothing */
	if (pm_runtime_suspended(&sr->pdev->dev))
		return;

	/*
	 * Disable SR if only it is indeed enabled. Else just
	 * disable the clocks.
	 */
	if (sr_read_reg(sr, SRCONFIG) & SRCONFIG_SRENABLE) {
		if (sr->ip_type == SR_TYPE_V1)
			sr_v1_disable(sr);
		else if (sr->ip_type == SR_TYPE_V2)
			sr_v2_disable(sr);
	}

	pm_runtime_put_sync_suspend(&sr->pdev->dev);
}

/**
 * sr_notifier_control() - control the notifier mechanism
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 * @enable:	true to enable notifiers and false to disable the same
 *
 * SR modules allow an MCU interrupt mechanism that vary based on the IP
 * revision, we allow the system to generate interrupt if the class driver
 * has capability to handle the same. it is upto the class driver to ensure
 * the proper sequencing and handling for a clean implementation. returns
 * 0 if all goes fine, else returns failure results
 */
int sr_notifier_control(struct voltagedomain *voltdm, bool enable)
{
	struct omap_sr *sr = _sr_lookup(voltdm);
	u32 value = 0;

	if (!sr) {
		pr_warning("%s: sr corresponding to domain not found\n",
				__func__);
		return -EINVAL;
	}
	if (!sr->autocomp_active)
		return -EINVAL;

	/* If I could never register an ISR, why bother?? */
	if (!(sr_class && sr_class->notify && sr_class->notify_flags &&
			sr->irq)) {
		dev_warn(&sr->pdev->dev,
			"%s: unable to setup IRQ without handling mechanism\n",
			__func__);
		return -EINVAL;
	}


	switch (sr->ip_type) {
	case SR_TYPE_V1:
		value = notifier_to_irqen_v1(sr_class->notify_flags);
		break;
	case SR_TYPE_V2:
		value = notifier_to_irqen_v2(sr_class->notify_flags);
		break;
	default:
		 dev_warn(&sr->pdev->dev, "%s: unknown type of sr??\n",
				 __func__);
		return -EINVAL;
	}

	if (!enable)
		sr_write_reg(sr, IRQSTATUS, value);

	switch (sr->ip_type) {
	case SR_TYPE_V1:
		sr_modify_reg(sr, ERRCONFIG_V1, value,
				(enable) ? value : 0);
		break;
	case SR_TYPE_V2:
		sr_write_reg(sr, (enable) ? IRQENABLE_SET : IRQENABLE_CLR,
				value);
		break;
	}

	if (enable != sr->irq_enabled) {
		if (enable)
			enable_irq(sr->irq);
		else
			disable_irq(sr->irq);
		sr->irq_enabled = enable;
	}

	return 0;
}

/**
 * sr_register_class() - API to register a smartreflex class parameters.
 * @class_data:	The structure containing various sr class specific data.
 *
 * This API is to be called by the smartreflex class driver to register itself
 * with the smartreflex driver during init. Returns 0 on success else the
 * error value.
 */
int sr_register_class(struct omap_sr_class_data *class_data)
{
	struct omap_sr *sr_info;

	if (!class_data) {
		pr_warning("%s:, Smartreflex class data passed is NULL\n",
			__func__);
		return -EINVAL;
	}

	if (sr_class) {
		pr_warning("%s: Smartreflex class driver already registered\n",
			__func__);
		return -EBUSY;
	}

	sr_class = class_data;

	/*
	 * Call into late init to do intializations that require
	 * both sr driver and sr class driver to be initiallized.
	 */
	list_for_each_entry(sr_info, &sr_list, node)
		sr_late_init(sr_info);

	return 0;
}

/**
 * omap_sr_enable() -  API to enable SR clocks and to call into the
 *			registered smartreflex class enable API.
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 * @volt_data:	Voltage data to go to
 *
 * This API is to be called from the kernel in order to enable
 * a particular smartreflex module. This API will do the initial
 * configurations to turn on the smartreflex module and in turn call
 * into the registered smartreflex class enable API.
 */
void omap_sr_enable(struct voltagedomain *voltdm,
			struct omap_volt_data *volt_data)
{
	struct omap_sr *sr = _sr_lookup(voltdm);

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return;
	}

	if (!sr->autocomp_active)
		return;

	if (!sr_class || !(sr_class->enable) || !(sr_class->configure)) {
		dev_warn(&sr->pdev->dev, "%s: smartreflex class driver not"
			"registered\n", __func__);
		return;
	}

	sr_class->enable(voltdm, sr->voltdm_cdata,
			omap_voltage_get_curr_vdata(voltdm));
}

/**
 * omap_sr_disable() - API to disable SR without resetting the voltage
 *			processor voltage
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 *
 * This API is to be called from the kernel in order to disable
 * a particular smartreflex module. This API will in turn call
 * into the registered smartreflex class disable API. This API will tell
 * the smartreflex class disable not to reset the VP voltage after
 * disabling smartreflex.
 */
void omap_sr_disable(struct voltagedomain *voltdm)
{
	struct omap_sr *sr = _sr_lookup(voltdm);

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return;
	}

	if (!sr->autocomp_active)
		return;

	if (!sr_class || !(sr_class->disable)) {
		dev_warn(&sr->pdev->dev, "%s: smartreflex class driver not"
			"registered\n", __func__);
		return;
	}

	sr_class->disable(voltdm, sr->voltdm_cdata,
			  omap_voltage_get_curr_vdata(voltdm), 0);
}

/**
 * omap_sr_disable_reset_volt() - API to disable SR and reset the
 *				voltage processor voltage
 * @voltdm:	VDD pointer to which the SR module to be configured belongs to.
 *
 * This API is to be called from the kernel in order to disable
 * a particular smartreflex module. This API will in turn call
 * into the registered smartreflex class disable API. This API will tell
 * the smartreflex class disable to reset the VP voltage after
 * disabling smartreflex.
 *
 * Returns result of transition request.
 */
int omap_sr_disable_reset_volt(struct voltagedomain *voltdm)
{
	struct omap_sr *sr = _sr_lookup(voltdm);

	if (IS_ERR(sr)) {
		pr_warning("%s: omap_sr struct for sr_%s not found\n",
			__func__, voltdm->name);
		return -ENODEV;
	}

	if (!sr->autocomp_active)
		return 0;

	if (!sr_class || !(sr_class->disable)) {
		dev_warn(&sr->pdev->dev, "%s: smartreflex class driver not"
			"registered\n", __func__);
		return -ENODEV;
	}

	return sr_class->disable(voltdm, sr->voltdm_cdata,
			  omap_voltage_get_curr_vdata(voltdm), 1);
}

/**
 * omap_sr_register_pmic() - API to register pmic specific info.
 * @pmic_data:	The structure containing pmic specific data.
 *
 * This API is to be called from the PMIC specific code to register with
 * smartreflex driver pmic specific info. Currently the only info required
 * is the smartreflex init on the PMIC side.
 */
void omap_sr_register_pmic(struct omap_sr_pmic_data *pmic_data)
{
	if (!pmic_data) {
		pr_warning("%s: Trying to register NULL PMIC data structure"
			"with smartreflex\n", __func__);
		return;
	}

	sr_pmic_data = pmic_data;
}

/**
 * Debug Fs enteries to tune smartreflex. 
 * author: imoseyon@gmail.com
 * 
 * code needs to be cleaned up still.
 */
static int omap_sr_highvolt_show(void *data, u64 *val)
{
	struct omap_sr *sr_info = (struct omap_sr *) data;

	if (!sr_info) {
		pr_warning("%s: omap_sr struct not found\n", __func__);
		return -EINVAL;
	}

	*val = enable_highvolt_sr;

	return 0;
}
static int omap_sr_vmin_show(void *data, u64 *val)
{
        struct omap_sr *sr = (struct omap_sr *) data;

	if (!sr) {
		pr_warning("%s: omap_sr struct not found\n", __func__);
		return -EINVAL;
	}

        if (!(strcmp(sr->voltdm->name, "mpu"))) *val = mpumin;
        else if (!(strcmp(sr->voltdm->name, "iva"))) *val = ivamin;
        else if (!(strcmp(sr->voltdm->name, "core"))) *val = coremin;
	return 0;
}
static int omap_sr_highvolt_store(void *data, u64 val)
{
	struct omap_sr *sr_info = (struct omap_sr *) data;

	if (!sr_info) {
		pr_warning("%s: omap_sr struct not found\n", __func__);
		return -EINVAL;
	}
	/* Sanity check */
	if (val && (val != 1)) {
		pr_warning("%s: Invalid argument %lld\n", __func__, val);
		return -EINVAL;
	}

	/* control enable/disable only if there is a delta in value */
	if (enable_highvolt_sr != val) {
		enable_highvolt_sr = val;
		sr_stop_vddautocomp(sr_info);
		sr_start_vddautocomp(sr_info);
	}
	return 0;
}

static int omap_sr_vmin_store(void *data, u64 val)
{
        struct omap_sr *sr_info = (struct omap_sr *) data;
        struct omap_vp_instance *vp;
	u32 val2, sys_clk_rate, timeout, vddmax, lowfloor, lowceiling, vddmin, srtype;
	lowfloor=LOWFLOOR;
	lowceiling=LOWCEILING;

        if (!sr_info) {
                pr_warning("%s: omap_sr struct not found\n", __func__);
                return -EINVAL;
        }

        if (!(strcmp(sr_info->voltdm->name, "mpu"))) srtype=1;
        else if (!(strcmp(sr_info->voltdm->name, "iva"))) srtype=2;
        else if (!(strcmp(sr_info->voltdm->name, "core"))) srtype=3;
	else return -EINVAL;

        if (srtype==1) vddmax=sr_info->voltdm->pmic->uv_to_vsel(OMAP4_VP_MPU_VLIMITTO_VDDMAX);
        else if (srtype==2) vddmax=sr_info->voltdm->pmic->uv_to_vsel(OMAP4_VP_IVA_VLIMITTO_VDDMAX);
        else vddmax=sr_info->voltdm->pmic->uv_to_vsel(OMAP4_VP_CORE_VLIMITTO_VDDMAX);

	vp = sr_info->voltdm->vp;

	if (val == 0) {
	  if (srtype==1) {
		vddmin = sr_info->voltdm->pmic->uv_to_vsel(OMAP4_VP_MPU_VLIMITTO_VDDMIN);
		mpumin = OMAP4_VP_MPU_VLIMITTO_VDDMIN;
	  } else if (srtype==2) {
		vddmin = sr_info->voltdm->pmic->uv_to_vsel(OMAP4_VP_IVA_VLIMITTO_VDDMIN);
		ivamin = OMAP4_VP_IVA_VLIMITTO_VDDMIN;
	  } else {
		vddmin = sr_info->voltdm->pmic->uv_to_vsel(OMAP4_VP_CORE_VLIMITTO_VDDMIN);
		coremin = OMAP4_VP_CORE_VLIMITTO_VDDMIN;
	  }
	} else if (val < lowfloor) {
		vddmin = sr_info->voltdm->pmic->uv_to_vsel(lowfloor);
		if (srtype==1) mpumin = lowfloor;
		else if (srtype==2) ivamin = lowfloor;
		else coremin = lowfloor;
	} else if (val > lowceiling) {
		vddmin = sr_info->voltdm->pmic->uv_to_vsel(lowceiling);
		if (srtype==1) mpumin = lowceiling;
		else if (srtype==2) ivamin = lowceiling;
		else coremin = lowceiling;
	} else {
		vddmin = sr_info->voltdm->pmic->uv_to_vsel(val);
		if (srtype==1) mpumin = val;
		else if (srtype==2) ivamin = val;
		else coremin = val;
	}

        sys_clk_rate = sr_info->voltdm->sys_clk.rate / 1000;
        timeout = (sys_clk_rate * sr_info->voltdm->pmic->vp_timeout_us) / 1000;

        val2 = (vddmax << vp->common->vlimitto_vddmax_shift) |
                (vddmin << vp->common->vlimitto_vddmin_shift) |
                (timeout <<  vp->common->vlimitto_timeout_shift);
        sr_info->voltdm->write(val2, vp->vlimitto);

        sr_stop_vddautocomp(sr_info);
        sr_start_vddautocomp(sr_info);
	return 0;
}

/* PM Debug Fs enteries to enable disable smartreflex. */
static int omap_sr_autocomp_show(void *data, u64 *val)
{
	struct omap_sr *sr_info = (struct omap_sr *) data;

	if (!sr_info) {
		pr_warning("%s: omap_sr struct not found\n", __func__);
		return -EINVAL;
	}

	*val = sr_info->autocomp_active;

	return 0;
}

static int omap_sr_autocomp_store(void *data, u64 val)
{
	struct omap_sr *sr_info = (struct omap_sr *) data;

	if (!sr_info) {
		pr_warning("%s: omap_sr struct not found\n", __func__);
		return -EINVAL;
	}

	/* Sanity check */
	if (val && (val != 1)) {
		pr_warning("%s: Invalid argument %lld\n", __func__, val);
		return -EINVAL;
	}

	/* control enable/disable only if there is a delta in value */
	if (sr_info->autocomp_active != val) {
		if (!val)
			sr_stop_vddautocomp(sr_info);
		else
			sr_start_vddautocomp(sr_info);
	}

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(pm_sr_fops, omap_sr_autocomp_show,
		omap_sr_autocomp_store, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(pm_sr_fops2, omap_sr_vmin_show,
		omap_sr_vmin_store, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(pm_sr_fops3, omap_sr_highvolt_show,
		omap_sr_highvolt_store, "%llu\n");

static int __init omap_sr_probe(struct platform_device *pdev)
{
	struct omap_sr *sr_info = kzalloc(sizeof(struct omap_sr), GFP_KERNEL);
	struct omap_sr_data *pdata = pdev->dev.platform_data;
	struct resource *mem, *irq;
	struct dentry *nvalue_dir;
	struct omap_volt_data *volt_data;
	int i, ret = 0;
	char *name;

	if (!sr_info) {
		dev_err(&pdev->dev, "%s: unable to allocate sr_info\n",
			__func__);
		return -ENOMEM;
	}

	if (!pdata) {
		dev_err(&pdev->dev, "%s: platform data missing\n", __func__);
		ret = -EINVAL;
		goto err_free_devinfo;
	}

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "%s: no mem resource\n", __func__);
		ret = -ENODEV;
		goto err_free_devinfo;
	}

	mem = request_mem_region(mem->start, resource_size(mem),
					dev_name(&pdev->dev));
	if (!mem) {
		dev_err(&pdev->dev, "%s: no mem region\n", __func__);
		ret = -EBUSY;
		goto err_free_devinfo;
	}

	irq = platform_get_resource(pdev, IORESOURCE_IRQ, 0);

	pm_runtime_enable(&pdev->dev);
	pm_runtime_irq_safe(&pdev->dev);

	sr_info->pdev = pdev;
	sr_info->srid = pdev->id;
	sr_info->voltdm = pdata->voltdm;
	sr_info->nvalue_table = pdata->nvalue_table;
	sr_info->nvalue_count = pdata->nvalue_count;
	sr_info->senn_mod = pdata->senn_mod;
	sr_info->senp_mod = pdata->senp_mod;
	sr_info->autocomp_active = false;
	sr_info->ip_type = pdata->ip_type;
	sr_info->base = ioremap(mem->start, resource_size(mem));
	if (!sr_info->base) {
		dev_err(&pdev->dev, "%s: ioremap fail\n", __func__);
		ret = -ENOMEM;
		goto err_release_region;
	}

	if (irq)
		sr_info->irq = irq->start;

	sr_set_clk_length(sr_info);
	sr_set_regfields(sr_info);

	list_add(&sr_info->node, &sr_list);

	/*
	 * Call into late init to do intializations that require
	 * both sr driver and sr class driver to be initiallized.
	 */
	if (sr_class) {
		ret = sr_late_init(sr_info);
		if (ret) {
			pr_warning("%s: Error in SR late init\n", __func__);
			goto err_iounmap;
		}
	}

	dev_info(&pdev->dev, "%s: SmartReflex driver initialized\n", __func__);
	if (!sr_dbg_dir) {
		sr_dbg_dir = debugfs_create_dir("smartreflex", NULL);
		if (!sr_dbg_dir) {
			ret = PTR_ERR(sr_dbg_dir);
			pr_err("%s:sr debugfs dir creation failed(%d)\n",
				__func__, ret);
			goto err_iounmap;
		}
	}

	name = kasprintf(GFP_KERNEL, "sr_%s", sr_info->voltdm->name);
	if (!name) {
		dev_err(&pdev->dev, "%s: Unable to alloc debugfs name\n",
			__func__);
		ret = -ENOMEM;
		goto err_iounmap;
	}
	sr_info->dbg_dir = debugfs_create_dir(name, sr_dbg_dir);
	kfree(name);
	if (IS_ERR(sr_info->dbg_dir)) {
		dev_err(&pdev->dev, "%s: Unable to create debugfs directory\n",
			__func__);
		ret = PTR_ERR(sr_info->dbg_dir);
		goto err_iounmap;
	}

	(void) debugfs_create_file("autocomp", S_IRUGO | S_IWUSR,
			sr_info->dbg_dir, (void *)sr_info, &pm_sr_fops);
	(void) debugfs_create_file("vmin", S_IRUGO | S_IWUSR,
			sr_info->dbg_dir, (void *)sr_info, &pm_sr_fops2);
	(void) debugfs_create_file("enable_highvolt", S_IRUGO | S_IWUSR,
			sr_info->dbg_dir, (void *)sr_info, &pm_sr_fops3);
	(void) debugfs_create_x32("errweight", S_IRUGO, sr_info->dbg_dir,
			&sr_info->err_weight);
	(void) debugfs_create_x32("errmaxlimit", S_IRUGO, sr_info->dbg_dir,
			&sr_info->err_maxlimit);
	(void) debugfs_create_x32("errminlimit", S_IRUGO, sr_info->dbg_dir,
			&sr_info->err_minlimit);

	nvalue_dir = debugfs_create_dir("nvalue", sr_info->dbg_dir);
	if (IS_ERR(nvalue_dir)) {
		dev_err(&pdev->dev, "%s: Unable to create debugfs directory"
			"for n-values\n", __func__);
		ret = PTR_ERR(nvalue_dir);
		goto err_debugfs;
	}

	omap_voltage_get_volttable(sr_info->voltdm, &volt_data);
	if (!volt_data) {
		dev_warn(&pdev->dev, "%s: No Voltage table for the"
			" corresponding vdd vdd_%s. Cannot create debugfs"
			"entries for n-values\n",
			__func__, sr_info->voltdm->name);
		ret = -ENODATA;
		goto err_debugfs;
	}

	for (i = 0; i < sr_info->nvalue_count; i++) {
		char name[NVALUE_NAME_LEN + 1];

		snprintf(name, sizeof(name), "volt_%d",
			 volt_data[i].volt_nominal);
		(void) debugfs_create_x32(name, S_IRUGO | S_IWUSR, nvalue_dir,
				&(sr_info->nvalue_table[i].nvalue));
	}

	return ret;

err_debugfs:
	debugfs_remove_recursive(sr_info->dbg_dir);
err_iounmap:
	list_del(&sr_info->node);
	iounmap(sr_info->base);
err_release_region:
	release_mem_region(mem->start, resource_size(mem));
err_free_devinfo:
	kfree(sr_info);

	return ret;
}

static int __devexit omap_sr_remove(struct platform_device *pdev)
{
	struct omap_sr_data *pdata = pdev->dev.platform_data;
	struct omap_sr *sr_info;
	struct resource *mem;

	if (!pdata) {
		dev_err(&pdev->dev, "%s: platform data missing\n", __func__);
		return -EINVAL;
	}

	sr_info = _sr_lookup(pdata->voltdm);
	if (IS_ERR(sr_info)) {
		dev_warn(&pdev->dev, "%s: omap_sr struct not found\n",
			__func__);
		return -EINVAL;
	}

	if (sr_info->autocomp_active)
		sr_stop_vddautocomp(sr_info);
	if (sr_info->dbg_dir)
		debugfs_remove_recursive(sr_info->dbg_dir);

	list_del(&sr_info->node);
	iounmap(sr_info->base);
	kfree(sr_info);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(mem->start, resource_size(mem));

	return 0;
}

static void __devexit omap_sr_shutdown(struct platform_device *pdev)
{
	struct omap_sr_data *pdata = pdev->dev.platform_data;
	struct omap_sr *sr_info;

	if (!pdata) {
		dev_err(&pdev->dev, "%s: platform data missing\n", __func__);
		return;
	}

	sr_info = _sr_lookup(pdata->voltdm);
	if (IS_ERR(sr_info)) {
		dev_warn(&pdev->dev, "%s: omap_sr struct not found\n",
			__func__);
		return;
	}

	if (sr_info->autocomp_active)
		sr_stop_vddautocomp(sr_info);

	return;
}

static struct platform_driver smartreflex_driver = {
	.remove         = omap_sr_remove,
	.shutdown	= omap_sr_shutdown,
	.driver		= {
		.name	= "smartreflex",
	},
};

static int __init sr_init(void)
{
	int ret = 0;

	/*
	 * sr_init is a late init. If by then a pmic specific API is not
	 * registered either there is no need for anything to be done on
	 * the PMIC side or somebody has forgotten to register a PMIC
	 * handler. Warn for the second condition.
	 */
	if (sr_pmic_data && sr_pmic_data->sr_pmic_init)
		sr_pmic_data->sr_pmic_init();
	else
		pr_warning("%s: No PMIC hook to init smartreflex\n", __func__);

	ret = platform_driver_probe(&smartreflex_driver, omap_sr_probe);
	if (ret) {
		pr_err("%s: platform driver register failed for SR\n",
			__func__);
		return ret;
	}

	return 0;
}

static void __exit sr_exit(void)
{
	platform_driver_unregister(&smartreflex_driver);
}
late_initcall(sr_init);
module_exit(sr_exit);

MODULE_DESCRIPTION("OMAP Smartreflex Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_AUTHOR("Texas Instruments Inc");