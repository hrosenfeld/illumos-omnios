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
#include <sys/dld.h>
#include <sys/dlpi.h>
#include <sys/dls.h>
#include <sys/modctl.h>
#include <sys/sockio.h>
#include <sys/strsun.h>
#include <sys/vnic.h>
#include <sys/vnic_impl.h>
#include <sys/policy.h>

/* module description */
#define	VNIC_LINKINFO		"Virtual NIC"
#define	VNIC_DEV_NAME		"vnic"

/* device info ptr, only one for instance 0 */
static dev_info_t *vnic_dip = NULL;
static kmem_cache_t *vnic_upper_cachep;

static int vnic_upper_constructor(void *, void *, int);
static void vnic_upper_destructor(void *, void *);

static int vnic_hold(dev_t, vnic_t **);
static void vnic_rele(vnic_t *);

static void vnic_replumb_done(vnic_upper_t *);
static void vnic_bind_req(vnic_upper_t *, mblk_t *);
static void vnic_wput_single_nondata(vnic_upper_t *, mblk_t *);
static void vnic_wput_nondata_task(void *);
static int vnic_wput_nondata(vnic_upper_t *, mblk_t *);

static int vnic_str_open(queue_t *, dev_t *, int, int, cred_t *);
static int vnic_str_close(queue_t *, int, cred_t *);
static int vnic_str_wput(queue_t *, mblk_t *);
static int vnic_str_wsrv(queue_t *);

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
	{VNIC_IOC_MODIFY, DLDCOPYINOUT, sizeof (vnic_ioc_t),
	    vnic_ioc_modify, secpolicy_dl_config}
};

/*
 * mi_hiwat is 1 because of the flow control mechanism implemented in dld.
 * Refer to the comments in dld_str.c for details.
 */
static struct module_info vnic_modinfo = {
	.mi_idnum = 0,
	.mi_idname = VNIC_DEV_NAME,
	.mi_minpsz = 0,
	.mi_maxpsz = INFPSZ,
	.mi_hiwat = 1,
	.mi_lowat = 0
};

static struct qinit vnic_rd_qinit = {
	.qi_putp = NULL,
	.qi_srvp = NULL,
	.qi_qopen = vnic_str_open,
	.qi_qclose = vnic_str_close,
	.qi_qadmin = NULL,
	.qi_minfo = &vnic_modinfo,
};

static struct qinit vnic_wr_qinit = {
	.qi_putp = vnic_str_wput,
	.qi_srvp = vnic_str_wsrv,
	.qi_qopen = NULL,
	.qi_qclose = NULL,
	.qi_qadmin = NULL,
	.qi_minfo = &vnic_modinfo
};

static struct streamtab vnic_strtab = {
	.st_rdinit = &vnic_rd_qinit,
	.st_wrinit = &vnic_wr_qinit
};

DDI_DEFINE_STREAM_OPS(vnic_dev_ops, nulldev, nulldev, vnic_attach, vnic_detach,
    nodev, vnic_getinfo, D_MP, &vnic_strtab, ddi_quiesce_not_supported);

static struct fmodsw vnic_fmodsw = {
	.f_name = VNIC_DEV_NAME,
	.f_str = &vnic_strtab,
	.f_flag = D_MP
};

static struct modldrv vnic_modldrv = {
	.drv_modops = &mod_driverops,
	.drv_linkinfo = VNIC_DEV_NAME " driver",
	.drv_dev_ops = &vnic_dev_ops
};

static struct modlstrmod vnic_modlstrmod = {
	.strmod_modops = &mod_strmodops,
	.strmod_linkinfo = VNIC_DEV_NAME " module",
	.strmod_fmodsw = &vnic_fmodsw
};

static struct modlinkage vnic_modlinkage = {
	.ml_rev = MODREV_1,
	.ml_linkage = { &vnic_modlstrmod, &vnic_modldrv, NULL }
};

int
_init(void)
{
	int err;

	vnic_upper_cachep = kmem_cache_create("vnic_upper_cache",
	    sizeof (vnic_upper_t), 0, vnic_upper_constructor,
	    vnic_upper_destructor, NULL, NULL, NULL, 0);
	if (vnic_upper_cachep == NULL)
		return (ENOMEM);

	mac_init_ops(NULL, VNIC_DEV_NAME);

	err = mod_install(&vnic_modlinkage);
	if (err != 0) {
		kmem_cache_destroy(vnic_upper_cachep);
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

	kmem_cache_destroy(vnic_upper_cachep);
	return (0);
}

int
_info(struct modinfo *modinfop)
{
	return (mod_info(&vnic_modlinkage, modinfop));
}


static int
vnic_upper_constructor(void *buf, void *arg, int kmflag)
{
	vnic_upper_t *vu = buf;

	bzero(vu, sizeof (vnic_upper_t));

	mutex_init(&vu->vu_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&vu->vu_cv, NULL, CV_DEFAULT, NULL);

	return (0);
}

static void
vnic_upper_destructor(void *buf, void *arg)
{
	vnic_upper_t *vu = buf;

	ASSERT(vu->vu_pending_head == NULL);
	ASSERT(vu->vu_pending_tail == NULL);
	ASSERT(!vu->vu_dlpi_pending);
	ASSERT(!vu->vu_closing);

	mutex_destroy(&vu->vu_lock);
	cv_destroy(&vu->vu_cv);
}

static int
vnic_hold(dev_t dev, vnic_t **vnicp)
{
	char *drv = ddi_major_to_name(getmajor(dev));
	char name[MAXNAMELEN];
	mac_handle_t mh;
	vnic_t *vnic;
	int err;

	if (drv == NULL)
		return (EINVAL);

	(void) snprintf(name, MAXNAMELEN, "%s%d", drv, getminor(dev) - 1);
	err = mac_open(name, &mh);
	if (err != 0)
		return (err);

	vnic = mac_driver(mh);
	mutex_enter(&vnic->vn_lock);
	vnic->vn_hold_cnt++;
	mutex_exit(&vnic->vn_lock);
	mac_close(mh);

	*vnicp = vnic;
	return (0);
}

static void
vnic_rele(vnic_t *vnic)
{
	mutex_enter(&vnic->vn_lock);
	vnic->vn_hold_cnt--;
	mutex_exit(&vnic->vn_lock);
}

static void
vnic_replumb_done(vnic_upper_t *vu)
{
	vnic_t *vnic = vu->vu_vnic;

	/* No need to do anything if we're not modifying anything. */
	mutex_enter(&vnic->vn_lock);
	if (!vnic->vn_modifying || vnic->vn_modify_cnt == 0) {
		mutex_exit(&vnic->vn_lock);
		return;
	}

	vnic->vn_modify_cnt--;

	/*
	 * If we're the last one, wake up vnic_dev_modify() and wait
	 * until it is done.
	 */
	if (vnic->vn_modify_cnt == 0) {
		cv_signal(&vnic->vn_modify_cv);

		while (vnic->vn_modifying)
			cv_wait(&vnic->vn_switch_cv, &vnic->vn_lock);
	}

	mutex_exit(&vnic->vn_lock);
}

static void
vnic_bind_req(vnic_upper_t *vu, mblk_t *mp)
{
	vnic_t *vnic = vu->vu_vnic;

	/*
	 * If we're in the middle of a vnic modification, but haven't actually
	 * modified the vnic yet, do call vnic_do_modify() to do it now before
	 * the bind request is processed.
	 */
	mutex_enter(&vnic->vn_lock);
	if (vnic->vn_modifying && !vnic->vn_modify_done)
		vnic_lower_modify(vnic);
	mutex_exit(&vnic->vn_lock);

	(void) dld_wput(vu->vu_wq, mp);
}

static void
vnic_wput_single_nondata(vnic_upper_t *vu, mblk_t *mp)
{
	t_uscalar_t prim;

	switch (DB_TYPE(mp)) {
	case M_PROTO:
	case M_PCPROTO:
		if (MBLKL(mp) < sizeof (t_uscalar_t))
			goto bail;

		prim = ((union DL_primitives *)mp->b_rptr)->dl_primitive;
		switch (prim) {
		case DL_BIND_REQ:
			vnic_bind_req(vu, mp);
			return;

		case DL_NOTIFY_IND:
			if (MBLKL(mp) < sizeof (dl_notify_ind_t))
				goto bail;
			if (((dl_notify_ind_t *)mp->b_rptr)->dl_notification !=
			    DL_NOTE_REPLUMB)
				goto bail;

			/*
			 * This DL_NOTE_REPLUMB message is initiated
			 * and queued by the vnic itself, when the
			 * vnic is trying to switch its lower MAC client
			 * handle as a result of vnic_dev_modify().
			 * Send this message upstream.
			 */
			qreply(vu->vu_wq, mp);
			return;

		case DL_NOTIFY_CONF:
			if (MBLKL(mp) < sizeof (dl_notify_conf_t))
				goto bail;
			if (((dl_notify_conf_t *)mp->b_rptr)->dl_notification !=
			    DL_NOTE_REPLUMB_DONE)
				goto bail;
			/*
			 * This is an indication from IP/ARP that the
			 * MAC client switch is done.
			 */
			freemsg(mp);
			vnic_replumb_done(vu);
			return;
		}

		break;
	}

	(void) dld_wput(vu->vu_wq, mp);
	return;

bail:
	freemsg(mp);
}

static void
vnic_wput_nondata_task(void *arg)
{
	vnic_upper_t *vu = arg;
	mblk_t *mp;

	mutex_enter(&vu->vu_lock);

	while (vu->vu_pending_head != NULL) {
		if (vu->vu_closing)
			break;

		vnic_dq_pending(vu, &mp);
		mutex_exit(&vu->vu_lock);
		vnic_wput_single_nondata(vu, mp);
		mutex_enter(&vu->vu_lock);
	}

	freemsgchain(vu->vu_pending_head);
	vu->vu_pending_head = vu->vu_pending_tail = NULL;
	vu->vu_dlpi_pending = B_FALSE;
	cv_signal(&vu->vu_cv);
	mutex_exit(&vu->vu_lock);
}

static int
vnic_wput_nondata(vnic_upper_t *vu, mblk_t *mp)
{
	taskqid_t tid;

	mutex_enter(&vu->vu_lock);

	if (vu->vu_closing) {
		mutex_exit(&vu->vu_lock);
		freemsg(mp);
		return (0);
	}

	vnic_eq_pending(vu, mp);

	if (vu->vu_dlpi_pending) {
		mutex_exit(&vu->vu_lock);
		return (0);
	}

	vu->vu_dlpi_pending = B_TRUE;
	mutex_exit(&vu->vu_lock);

	tid = taskq_dispatch(vu->vu_vnic->vn_taskq, vnic_wput_nondata_task, vu,
	    TQ_NOSLEEP);
	VERIFY(tid != TASKQID_INVALID);

	return (0);
}

static int
vnic_str_open(queue_t *rq, dev_t *devp, int flag, int sflag, cred_t *credp)
{
	vnic_upper_t *vu;
	vnic_t *vnic;
	int err;

	/*
	 * This is a self-cloning driver so that each queue should only
	 * get opened once.
	 */
	if (rq->q_ptr != NULL)
		return (EBUSY);

	if (sflag == MODOPEN)
		return (ENOTSUP);

	err = vnic_hold(*devp, &vnic);
	if (err != 0)
		return (err);

	vu = kmem_cache_alloc(vnic_upper_cachep, KM_NOSLEEP);
	if (vu == NULL) {
		vnic_rele(vnic);
		return (ENOMEM);
	}

	vu->vu_rq = rq;
	vu->vu_wq = WR(rq);
	vu->vu_vnic = vnic;

	mutex_enter(&vnic->vn_lock);
	list_insert_head(&vnic->vn_upper_list, vu);
	mutex_exit(&vnic->vn_lock);

	err = dld_str_open(rq, devp, vu);
	if (err != 0) {
		mutex_enter(&vnic->vn_lock);
		list_remove(&vnic->vn_upper_list, vu);
		mutex_exit(&vnic->vn_lock);
		kmem_cache_free(vnic_upper_cachep, vu);
		vnic_rele(vnic);
		return (err);
	}

	return (0);
}

static int
vnic_str_close(queue_t *rq, int flags, cred_t *credp)
{
	vnic_upper_t *vu = dld_str_private(rq);
	vnic_t *vnic = vu->vu_vnic;

	ASSERT(WR(rq)->q_next == NULL);
	qprocsoff(rq);

	/*
	 * Wait until pending and waiting requests are processed.
	 */
	mutex_enter(&vu->vu_lock);
	vu->vu_closing = B_TRUE;
	while (vu->vu_dlpi_pending)
		cv_wait(&vu->vu_cv, &vu->vu_lock);
	mutex_exit(&vu->vu_lock);

	mutex_enter(&vnic->vn_lock);
	list_remove(&vnic->vn_upper_list, vu);
	mutex_exit(&vnic->vn_lock);

	vu->vu_closing = B_FALSE;

	kmem_cache_free(vnic_upper_cachep, vu);
	vnic_rele(vnic);

	return (dld_str_close(rq));
}


static int
vnic_str_wput(queue_t *wq, mblk_t *mp)
{
	vnic_upper_t *vu = dld_str_private(wq);
	t_uscalar_t prim;
	int err;

	ASSERT(wq->q_next == NULL);

	switch (DB_TYPE(mp)) {
	case M_DATA:
		err = dld_wput(wq, mp);
		break;
	case M_PROTO:
	case M_PCPROTO:
		if (MBLKL(mp) < sizeof (t_uscalar_t)) {
			freemsg(mp);
			return (0);
		}

		prim = ((union DL_primitives *)mp->b_rptr)->dl_primitive;
		if (prim == DL_UNITDATA_REQ) {
			err = dld_wput(wq, mp);
		} else {
			err = vnic_wput_nondata(vu, mp);
		}
		break;
	default:
		err = vnic_wput_nondata(vu, mp);
		break;
	}

	return (err);
}

static int
vnic_str_wsrv(queue_t *wq)
{
	ASSERT(wq->q_next == NULL);

	return (dld_wsrv(wq));
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
