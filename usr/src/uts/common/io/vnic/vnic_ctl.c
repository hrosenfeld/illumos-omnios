/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright 2026 Hans Rosenfeld
 */

/*
 * Virtual Network Interface Card (VNIC)
 */

#include <sys/conf.h>
#include <sys/modctl.h>
#include <sys/vnic.h>
#include <sys/vnic_impl.h>
#include <sys/policy.h>

/* module description */
#define	VNIC_LINKINFO		"Virtual NIC"

/* device info ptr, only one for instance 0 */
static dev_info_t *vnic_dip = NULL;
static int vnic_getinfo(dev_info_t *, ddi_info_cmd_t, void *, void **);
static int vnic_attach(dev_info_t *, ddi_attach_cmd_t);
static int vnic_detach(dev_info_t *, ddi_detach_cmd_t);

static int vnic_ioc_create(void *, intptr_t, int, cred_t *, int *);
static int vnic_ioc_delete(void *, intptr_t, int, cred_t *, int *);
static int vnic_ioc_info(void *, intptr_t, int, cred_t *, int *);
static int vnic_ioc_modify(void *, intptr_t, int, cred_t *, int *);

static dld_ioc_info_t vnic_ioc_list[] = {
	{VNIC_IOC_CREATE, DLDCOPYINOUT, sizeof (vnic_ioc_t),
	    vnic_ioc_create, secpolicy_dl_config},
	{VNIC_IOC_DELETE, DLDCOPYIN, sizeof (vnic_ioc_t),
	    vnic_ioc_delete, secpolicy_dl_config},
	{VNIC_IOC_INFO, DLDCOPYINOUT, sizeof (vnic_ioc_t),
	    vnic_ioc_info, NULL},
	{VNIC_IOC_MODIFY, DLDCOPYIN, sizeof (vnic_ioc_t),
	    vnic_ioc_modify, secpolicy_dl_config}
};

DDI_DEFINE_STREAM_OPS(vnic_dev_ops, nulldev, nulldev, vnic_attach, vnic_detach,
    nodev, vnic_getinfo, D_MP, NULL, ddi_quiesce_not_supported);

static struct modldrv vnic_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = VNIC_LINKINFO,
	.drv_dev_ops = &vnic_dev_ops
};

static struct modlinkage vnic_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &vnic_modldrv, NULL }
};

int
_init(void)
{
	int err;

	mac_init_ops(&vnic_dev_ops, "vnic");
	err = mod_install(&vnic_modlinkage);
	if (err != 0) {
		mac_fini_ops(&vnic_dev_ops);
		return (err);
	}

	return (0);
}

int
_fini(void)
{
	int err;

	err = mod_remove(&vnic_modlinkage);
	if (err != 0)
		return (err);

	mac_fini_ops(&vnic_dev_ops);
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&vnic_modlinkage, modinfop));
}

static void
vnic_init(void)
{
	vnic_dev_init();
}

static void
vnic_fini(void)
{
	vnic_dev_fini();
}

dev_info_t *
vnic_get_dip(void)
{
	return (vnic_dip);
}

/*ARGSUSED*/
static int
vnic_getinfo(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg,
    void **result)
{
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = vnic_dip;
		return (DDI_SUCCESS);
	case DDI_INFO_DEVT2INSTANCE:
		*result = NULL;
		return (DDI_SUCCESS);
	}
	return (DDI_FAILURE);
}

static int
vnic_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_ATTACH:
		if (ddi_get_instance(dip) != 0) {
			/* we only allow instance 0 to attach */
			return (DDI_FAILURE);
		}
		if (dld_ioc_register(VNIC_IOC, vnic_ioc_list,
		    DLDIOCCNT(vnic_ioc_list)) != 0)
			return (DDI_FAILURE);

		vnic_dip = dip;
		vnic_init();
		return (DDI_SUCCESS);

	case DDI_RESUME:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}
}

/*ARGSUSED*/
static int
vnic_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	switch (cmd) {
	case DDI_DETACH:
		/*
		 * Allow the VNIC instance to be detached only if there
		 * are not VNICs configured.
		 */
		if (vnic_dev_count() > 0)
			return (DDI_FAILURE);

		vnic_dip = NULL;
		vnic_fini();
		dld_ioc_unregister(VNIC_IOC);
		return (DDI_SUCCESS);

	case DDI_SUSPEND:
		return (DDI_SUCCESS);

	default:
		return (DDI_FAILURE);
	}
}

/*
 * Process a VNICIOC_CREATE request.
 */
/* ARGSUSED */
static int
vnic_ioc_create(void *karg, intptr_t arg, int mode, cred_t *cred, int *rvalp)
{
	vnic_ioc_t *ioc = karg;

	ioc->vi_status = vnic_dev_create(ioc, cred);

	return (ioc->vi_status);
}

/* ARGSUSED */
static int
vnic_ioc_modify(void *karg, intptr_t arg, int mode, cred_t *cred, int *rvalp)
{
	vnic_ioc_t *ioc = karg;

	ioc->vi_status = vnic_dev_modify(ioc, cred);

	return (ioc->vi_status);
}

/* ARGSUSED */
static int
vnic_ioc_delete(void *karg, intptr_t arg, int mode, cred_t *cred, int *rvalp)
{
	vnic_ioc_t *ioc = karg;

	ioc->vi_status = vnic_dev_delete(ioc, cred);

	return (ioc->vi_status);
}

/* ARGSUSED */
static int
vnic_ioc_info(void *karg, intptr_t arg, int mode, cred_t *cred, int *rvalp)
{
	vnic_ioc_t *ioc = karg;

	ioc->vi_status = vnic_dev_info(ioc, cred);

	return (ioc->vi_status);
}
