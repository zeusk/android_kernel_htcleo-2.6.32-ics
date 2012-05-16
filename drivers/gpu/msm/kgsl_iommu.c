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
 *
 */
#include <linux/types.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/genalloc.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/msm_kgsl.h>

#include "kgsl.h"
#include "kgsl_device.h"
#include "kgsl_mmu.h"
#include "kgsl_sharedmem.h"
#include "kgsl_iommu.h"

/*
 * kgsl_iommu_disable_clk - Disable iommu clocks
 * @mmu - Pointer to mmu structure
 *
 * Disables iommu clocks
 * Return - void
 */
static void kgsl_iommu_disable_clk(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	struct msm_iommu_drvdata *iommu_drvdata;
	int i, j;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (!iommu_unit->dev[j].clk_enabled)
				continue;
			iommu_drvdata = dev_get_drvdata(
					iommu_unit->dev[j].dev->parent);
			iommu_unit->dev[j].clk_enabled = false;
		}
	}
}

/*
 * kgsl_iommu_enable_clk - Enable iommu clocks
 * @mmu - Pointer to mmu structure
 * @ctx_id - The context bank whose clocks are to be turned on
 *
 * Enables iommu clocks of a given context
 * Return: 0 on success else error code
 */
static int kgsl_iommu_enable_clk(struct kgsl_mmu *mmu,
				int ctx_id)
{
	int ret = 0;
	int i, j;
	struct kgsl_iommu *iommu = mmu->priv;
	struct msm_iommu_drvdata *iommu_drvdata;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (iommu_unit->dev[j].clk_enabled ||
				ctx_id != iommu_unit->dev[j].ctx_id)
				continue;
			iommu_drvdata =
			dev_get_drvdata(iommu_unit->dev[j].dev->parent);
			goto done;
			iommu_unit->dev[j].clk_enabled = true;
		}
	}
done:
	if (ret)
		kgsl_iommu_disable_clk(mmu);
	return ret;
}

static int kgsl_iommu_pt_equal(struct kgsl_pagetable *pt,
					unsigned int pt_base)
{
	struct iommu_domain *domain = pt ? pt->priv : NULL;
	return domain && pt_base && ((unsigned int)domain == pt_base);
}

static void kgsl_iommu_destroy_pagetable(void *mmu_specific_pt)
{
	struct iommu_domain *domain = mmu_specific_pt;
	if (domain)
		iommu_domain_free(domain);
}

void *kgsl_iommu_create_pagetable(void)
{
	KGSL_CORE_ERR("Failed to create iommu domain\n");
	return NULL;
}

/*
 * kgsl_detach_pagetable_iommu_domain - Detach the IOMMU unit from a
 * pagetable
 * @mmu - Pointer to the device mmu structure
 * @priv - Flag indicating whether the private or user context is to be
 * detached
 *
 * Detach the IOMMU unit with the domain that is contained in the
 * hwpagetable of the given mmu. After detaching the IOMMU unit is not
 * in use because the PTBR will not be set after a detach
 * Return - void
 */
static void kgsl_detach_pagetable_iommu_domain(struct kgsl_mmu *mmu)
{
	struct iommu_domain *domain;
	struct kgsl_iommu *iommu = mmu->priv;
	int i, j;

	BUG_ON(mmu->hwpagetable == NULL);
	BUG_ON(mmu->hwpagetable->priv == NULL);

	domain = mmu->hwpagetable->priv;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (iommu_unit->dev[j].attached) {
				iommu_detach_device(domain,
						iommu_unit->dev[j].dev);
				iommu_unit->dev[j].attached = false;
				KGSL_MEM_INFO(mmu->device, "iommu %p detached "
					"from user dev of MMU: %p\n",
					domain, mmu);
			}
		}
	}
}

/*
 * kgsl_attach_pagetable_iommu_domain - Attach the IOMMU unit to a
 * pagetable, i.e set the IOMMU's PTBR to the pagetable address and
 * setup other IOMMU registers for the device so that it becomes
 * active
 * @mmu - Pointer to the device mmu structure
 * @priv - Flag indicating whether the private or user context is to be
 * attached
 *
 * Attach the IOMMU unit with the domain that is contained in the
 * hwpagetable of the given mmu.
 * Return - 0 on success else error code
 */
static int kgsl_attach_pagetable_iommu_domain(struct kgsl_mmu *mmu)
{
	struct iommu_domain *domain;
	struct kgsl_iommu *iommu = mmu->priv;
	int i, j, ret = 0;

	BUG_ON(mmu->hwpagetable == NULL);
	BUG_ON(mmu->hwpagetable->priv == NULL);

	domain = mmu->hwpagetable->priv;

	/*
	 * Loop through all the iommu devcies under all iommu units and
	 * attach the domain
	 */
	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (!iommu_unit->dev[j].attached) {
				ret = iommu_attach_device(domain,
							iommu_unit->dev[j].dev);
				if (ret) {
					KGSL_MEM_ERR(mmu->device,
						"Failed to attach device, err %d\n",
						ret);
					goto done;
				}
				iommu_unit->dev[j].attached = true;
				KGSL_MEM_INFO(mmu->device,
				"iommu pt %p attached to dev %p, ctx_id %d\n",
					domain, iommu_unit->dev[j].dev,
					iommu_unit->dev[j].ctx_id);
			}
		}
	}
done:
	return ret;
}

/*
 * _get_iommu_ctxs - Get device pointer to IOMMU contexts
 * @mmu - Pointer to mmu device
 * data - Pointer to the platform data containing information about
 * iommu devices for one iommu unit
 * unit_id - The IOMMU unit number. This is not a specific ID but just
 * a serial number. The serial numbers are treated as ID's of the
 * IOMMU units
 *
 * Return - 0 on success else error code
 */
static int _get_iommu_ctxs(struct kgsl_mmu *mmu,
	struct kgsl_device_iommu_data *data, unsigned int unit_id)
{
	struct kgsl_iommu *iommu = mmu->priv;
	struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[unit_id];
	int i;

	if (data->iommu_ctx_count > KGSL_IOMMU_MAX_DEVS_PER_UNIT) {
		KGSL_CORE_ERR("Too many iommu devices defined for an "
				"IOMMU unit\n");
		return -EINVAL;
	}

	for (i = 0; i < data->iommu_ctx_count; i++) {
		if (!data->iommu_ctxs[i].iommu_ctx_name)
			continue;

		iommu_unit->dev[iommu_unit->dev_count].dev =
			msm_iommu_get_ctx(data->iommu_ctxs[i].iommu_ctx_name);
		if (iommu_unit->dev[iommu_unit->dev_count].dev == NULL) {
			KGSL_CORE_ERR("Failed to get iommu dev handle for "
			"device %s\n", data->iommu_ctxs[i].iommu_ctx_name);
			return -EINVAL;
		}
		if (KGSL_IOMMU_CONTEXT_USER != data->iommu_ctxs[i].ctx_id &&
			KGSL_IOMMU_CONTEXT_PRIV != data->iommu_ctxs[i].ctx_id) {
			KGSL_CORE_ERR("Invalid context ID defined: %d\n",
					data->iommu_ctxs[i].ctx_id);
			return -EINVAL;
		}
		iommu_unit->dev[iommu_unit->dev_count].ctx_id =
						data->iommu_ctxs[i].ctx_id;
		KGSL_DRV_INFO(mmu->device,
				"Obtained dev handle %p for iommu context %s\n",
				iommu_unit->dev[iommu_unit->dev_count].dev,
				data->iommu_ctxs[i].iommu_ctx_name);

		iommu_unit->dev_count++;
	}

	return 0;
}

/*
 * kgsl_get_iommu_ctxt - Get device pointer to IOMMU contexts
 * @mmu - Pointer to mmu device
 *
 * Get the device pointers for the IOMMU user and priv contexts of the
 * kgsl device
 * Return - 0 on success else error code
 */
static int kgsl_get_iommu_ctxt(struct kgsl_mmu *mmu)
{
	struct platform_device *pdev =
		container_of(mmu->device->parentdev, struct platform_device,
				dev);
	struct kgsl_device_platform_data *pdata_dev = pdev->dev.platform_data;
	struct kgsl_iommu *iommu = mmu->device->mmu.priv;
	int i, ret = 0;

	/* Go through the IOMMU data and get all the context devices */
	if (KGSL_IOMMU_MAX_UNITS < pdata_dev->iommu_count) {
		KGSL_CORE_ERR("Too many IOMMU units defined\n");
		ret = -EINVAL;
		goto  done;
	}

	for (i = 0; i < pdata_dev->iommu_count; i++) {
		ret = _get_iommu_ctxs(mmu, &pdata_dev->iommu_data[i], i);
		if (ret)
			break;
	}
	iommu->unit_count = pdata_dev->iommu_count;
done:
	return ret;
}

/*
 * kgsl_set_register_map - Map the IOMMU regsiters in the memory descriptors
 * of the respective iommu units
 * @mmu - Pointer to mmu structure
 *
 * Return - 0 on success else error code
 */
static int kgsl_set_register_map(struct kgsl_mmu *mmu)
{
	struct platform_device *pdev =
		container_of(mmu->device->parentdev, struct platform_device,
				dev);
	struct kgsl_device_platform_data *pdata_dev = pdev->dev.platform_data;
	struct kgsl_iommu *iommu = mmu->device->mmu.priv;
	struct kgsl_iommu_unit *iommu_unit;
	int i = 0, ret = 0;

	for (; i < pdata_dev->iommu_count; i++) {
		struct kgsl_device_iommu_data data = pdata_dev->iommu_data[i];
		iommu_unit = &iommu->iommu_units[i];
		/* set up the IOMMU register map for the given IOMMU unit */
		if (!data.physstart || !data.physend) {
			KGSL_CORE_ERR("The register range for IOMMU unit not"
					" specified\n");
			ret = -EINVAL;
			goto err;
		}
		iommu_unit->reg_map.hostptr = ioremap(data.physstart,
					data.physend - data.physstart + 1);
		if (!iommu_unit->reg_map.hostptr) {
			KGSL_CORE_ERR("Failed to map SMMU register address "
				"space from %x to %x\n", data.physstart,
				data.physend - data.physstart + 1);
			ret = -ENOMEM;
			i--;
			goto err;
		}
		iommu_unit->reg_map.size = data.physend - data.physstart + 1;
		iommu_unit->reg_map.physaddr = data.physstart;
		memdesc_sg_phys(&iommu_unit->reg_map, data.physstart,
				iommu_unit->reg_map.size);
	}
	iommu->unit_count = pdata_dev->iommu_count;
	return ret;
err:
	/* Unmap any mapped IOMMU regions */
	for (; i >= 0; i--) {
		iommu_unit = &iommu->iommu_units[i];
		iounmap(iommu_unit->reg_map.hostptr);
		iommu_unit->reg_map.size = 0;
		iommu_unit->reg_map.physaddr = 0;
	}
	return ret;
}

static void kgsl_iommu_setstate(struct kgsl_mmu *mmu,
				struct kgsl_pagetable *pagetable)
{
	if (mmu->flags & KGSL_FLAGS_STARTED) {
		/* page table not current, then setup mmu to use new
		 *  specified page table
		 */
		if (mmu->hwpagetable != pagetable) {
			kgsl_idle(mmu->device, KGSL_TIMEOUT_DEFAULT);
			kgsl_detach_pagetable_iommu_domain(mmu);
			mmu->hwpagetable = pagetable;
			if (mmu->hwpagetable)
				kgsl_attach_pagetable_iommu_domain(mmu);
		}
	}
}

static int kgsl_iommu_init(struct kgsl_mmu *mmu)
{
	/*
	 * intialize device mmu
	 *
	 * call this with the global lock held
	 */
	int status = 0;
	struct kgsl_iommu *iommu;

	iommu = kzalloc(sizeof(struct kgsl_iommu), GFP_KERNEL);
	if (!iommu) {
		KGSL_CORE_ERR("kzalloc(%d) failed\n",
				sizeof(struct kgsl_iommu));
		return -ENOMEM;
	}

	mmu->priv = iommu;
	status = kgsl_get_iommu_ctxt(mmu);
	if (status)
		goto done;
	status = kgsl_set_register_map(mmu);
	if (status)
		goto done;

	dev_info(mmu->device->dev, "|%s| MMU type set for device is IOMMU\n",
			__func__);
done:
	if (status) {
		kfree(iommu);
		mmu->priv = NULL;
	}
	return status;
}

/*
 * kgsl_iommu_setup_defaultpagetable - Setup the initial defualtpagetable
 * for iommu. This function is only called once during first start, successive
 * start do not call this funciton.
 * @mmu - Pointer to mmu structure
 *
 * Create the  initial defaultpagetable and setup the iommu mappings to it
 * Return - 0 on success else error code
 */
static int kgsl_iommu_setup_defaultpagetable(struct kgsl_mmu *mmu)
{
	int status = 0;
	int i = 0;
	struct kgsl_iommu *iommu = mmu->priv;

	mmu->defaultpagetable = kgsl_mmu_getpagetable(KGSL_MMU_GLOBAL_PT);
	/* Return error if the default pagetable doesn't exist */
	if (mmu->defaultpagetable == NULL) {
		status = -ENOMEM;
		goto err;
	}
	/* Map the IOMMU regsiters to only defaultpagetable */
	for (i = 0; i < iommu->unit_count; i++) {
		iommu->iommu_units[i].reg_map.priv |= KGSL_MEMFLAGS_GLOBAL;
		status = kgsl_mmu_map(mmu->defaultpagetable,
			&(iommu->iommu_units[i].reg_map),
			GSL_PT_PAGE_RV | GSL_PT_PAGE_WV);
		if (status) {
			iommu->iommu_units[i].reg_map.priv &=
							~KGSL_MEMFLAGS_GLOBAL;
			goto err;
		}
	}
	return status;
err:
	for (i--; i >= 0; i--) {
		kgsl_mmu_unmap(mmu->defaultpagetable,
				&(iommu->iommu_units[i].reg_map));
		iommu->iommu_units[i].reg_map.priv &= ~KGSL_MEMFLAGS_GLOBAL;
	}
	if (mmu->defaultpagetable) {
		kgsl_mmu_putpagetable(mmu->defaultpagetable);
		mmu->defaultpagetable = NULL;
	}
	return status;
}

static int kgsl_iommu_start(struct kgsl_mmu *mmu)
{
	int status;

	if (mmu->flags & KGSL_FLAGS_STARTED)
		return 0;

	if (mmu->defaultpagetable == NULL) {
		status = kgsl_iommu_setup_defaultpagetable(mmu);
		if (status)
			return -ENOMEM;
	}
	kgsl_regwrite(mmu->device, MH_MMU_CONFIG, 0x00000000);

	mmu->hwpagetable = mmu->defaultpagetable;

	status = kgsl_attach_pagetable_iommu_domain(mmu);
	if (!status) {
		mmu->flags |= KGSL_FLAGS_STARTED;
	} else {
		kgsl_detach_pagetable_iommu_domain(mmu);
		mmu->hwpagetable = NULL;
	}

	return status;
}

static int
kgsl_iommu_unmap(void *mmu_specific_pt,
		struct kgsl_memdesc *memdesc)
{
	int ret;
	unsigned int range = memdesc->size;
	struct iommu_domain *domain = (struct iommu_domain *)
					mmu_specific_pt;

	/* All GPU addresses as assigned are page aligned, but some
	   functions purturb the gpuaddr with an offset, so apply the
	   mask here to make sure we have the right address */

	unsigned int gpuaddr = memdesc->gpuaddr &  KGSL_MMU_ALIGN_MASK;

	if (range == 0 || gpuaddr == 0)
		return 0;

		KGSL_CORE_ERR("iommu_unmap_range(%p, %x, %d) failed "
			"with err: %d\n", domain, gpuaddr,
			range, ret);

	return 0;
}

static int
kgsl_iommu_map(void *mmu_specific_pt,
			struct kgsl_memdesc *memdesc,
			unsigned int protflags,
			unsigned int *tlb_flags)
{
	int ret;
	unsigned int iommu_virt_addr;
	struct iommu_domain *domain = mmu_specific_pt;

	BUG_ON(NULL == domain);


	iommu_virt_addr = memdesc->gpuaddr;

	ret = iommu_map_range(domain, iommu_virt_addr, memdesc->sg,
				memdesc->size, (IOMMU_READ | IOMMU_WRITE));
	if (ret) {
		KGSL_CORE_ERR("iommu_map_range(%p, %x, %p, %d, %d) "
				"failed with err: %d\n", domain,
				iommu_virt_addr, memdesc->sg, memdesc->size,
				0, ret);
		return ret;
	}

#ifdef CONFIG_KGSL_PER_PROCESS_PAGE_TABLE
	/*
	 * Flushing only required if per process pagetables are used. With
	 * global case, flushing will happen inside iommu_map function
	 */
	if (!ret)
		*tlb_flags = UINT_MAX;
#endif
	return ret;
}

static void kgsl_iommu_stop(struct kgsl_mmu *mmu)
{
	/*
	 *  stop device mmu
	 *
	 *  call this with the global lock held
	 */

	if (mmu->flags & KGSL_FLAGS_STARTED) {
		/* detach iommu attachment */
		kgsl_detach_pagetable_iommu_domain(mmu);
		mmu->hwpagetable = NULL;

		mmu->flags &= ~KGSL_FLAGS_STARTED;
	}
}

static int kgsl_iommu_close(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	int i;
	for (i = 0; i < iommu->unit_count; i++) {
		if (iommu->iommu_units[i].reg_map.gpuaddr)
			kgsl_mmu_unmap(mmu->defaultpagetable,
			&(iommu->iommu_units[i].reg_map));
		if (iommu->iommu_units[i].reg_map.hostptr)
			iounmap(iommu->iommu_units[i].reg_map.hostptr);
		kgsl_sg_free(iommu->iommu_units[i].reg_map.sg,
				iommu->iommu_units[i].reg_map.sglen);
	}
	if (mmu->defaultpagetable)
		kgsl_mmu_putpagetable(mmu->defaultpagetable);

	return 0;
}

static unsigned int
kgsl_iommu_get_current_ptbase(struct kgsl_mmu *mmu)
{
	unsigned int pt_base;
	struct kgsl_iommu *iommu = mmu->priv;
	/* Return the current pt base by reading IOMMU pt_base register */
	kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_CONTEXT_USER);
	pt_base = readl_relaxed(iommu->iommu_units[0].reg_map.hostptr +
			(KGSL_IOMMU_CONTEXT_USER << KGSL_IOMMU_CTX_SHIFT) +
			KGSL_IOMMU_TTBR0);
	kgsl_iommu_disable_clk(mmu);
	return pt_base & (KGSL_IOMMU_TTBR0_PA_MASK <<
				KGSL_IOMMU_TTBR0_PA_SHIFT);
}

struct kgsl_mmu_ops iommu_ops = {
	.mmu_init = kgsl_iommu_init,
	.mmu_close = kgsl_iommu_close,
	.mmu_start = kgsl_iommu_start,
	.mmu_stop = kgsl_iommu_stop,
	.mmu_setstate = kgsl_iommu_setstate,
	.mmu_device_setstate = NULL,
	.mmu_pagefault = NULL,
	.mmu_get_current_ptbase = kgsl_iommu_get_current_ptbase,
	.mmu_enable_clk = kgsl_iommu_enable_clk,
	.mmu_disable_clk = kgsl_iommu_disable_clk,
};

struct kgsl_mmu_pt_ops iommu_pt_ops = {
	.mmu_map = kgsl_iommu_map,
	.mmu_unmap = kgsl_iommu_unmap,
	.mmu_create_pagetable = kgsl_iommu_create_pagetable,
	.mmu_destroy_pagetable = kgsl_iommu_destroy_pagetable,
	.mmu_pt_equal = kgsl_iommu_pt_equal,
};
