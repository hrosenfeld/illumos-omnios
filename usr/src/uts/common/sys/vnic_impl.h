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
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright 2018 Joyent, Inc.
 * Copyright 2026 Hans Rosenfeld
 */

#ifndef	_SYS_VNIC_IMPL_H
#define	_SYS_VNIC_IMPL_H

#include <sys/cred.h>
#include <sys/mac_provider.h>
#include <sys/mac_client.h>
#include <sys/mac_client_priv.h>
#include <sys/vnic.h>
#include <sys/mac_flow.h>
#include <sys/ksynch.h>

#ifdef	__cplusplus
extern "C" {
#endif

typedef struct vnic_upper_s vnic_upper_t;
typedef struct vnic_s vnic_t;

struct vnic_s {
	datalink_id_t		vn_id;
	uint32_t
				vn_enabled : 1,
				vn_pad_to_bit_31 : 31;

	mac_handle_t		vn_mh;
	mac_notify_handle_t	vn_mnh;
	mac_handle_t		vn_lower_mh;
	mac_notify_handle_t	vn_lower_mnh;
	uint_t			vn_nhandles; /* # of secondary mac handles */
	/* The primary handle is always the first element in the array */
	mac_client_handle_t	vn_mc_handles[MPT_MAXMACADDR];
	mac_unicast_handle_t	vn_mu_handles[MPT_MAXMACADDR];
	uint32_t		vn_margin;
	int			vn_slot_id;
	vnic_mac_addr_type_t	vn_addr_type;
	uint8_t			vn_addr[MAXMACADDRLEN];
	size_t			vn_addr_len;
	uint16_t		vn_vid;
	vrid_t			vn_vrid;
	int			vn_af;
	boolean_t		vn_force;
	datalink_id_t		vn_link_id;

	uint32_t		vn_hcksum_txflags;
	mac_capab_lso_t		vn_cap_lso;
	uint32_t		vn_mtu;
	link_state_t		vn_ls;

	taskq_t			*vn_taskq;
	volatile uint_t		vn_hold_cnt;
	volatile uint_t		vn_modify_cnt;
	volatile boolean_t	vn_modifying;
	volatile boolean_t	vn_modify_done;
	volatile int		vn_modify_error;
	volatile boolean_t	vn_replumb_done;

	kmutex_t		vn_lock;
	kcondvar_t		vn_modify_cv;
	kcondvar_t		vn_switch_cv;
	list_t			vn_upper_list;
	vnic_t			*vn_orig_vnic;
	vnic_ioc_t		*vn_modify_ioc;
};

#define	vn_mch	vn_mc_handles[0]
#define	vn_muh	vn_mu_handles[0]

struct vnic_upper_s {
	vnic_t			*vu_vnic;
	queue_t			*vu_rq;
	queue_t			*vu_wq;
	list_node_t		vu_list_node;
	kmutex_t		vu_lock;
	kcondvar_t		vu_cv;
	mblk_t			*vu_pending_head;
	mblk_t			*vu_pending_tail;
	boolean_t		vu_dlpi_pending;
	boolean_t		vu_closing;
};

static inline void
vnic_eq_pending(vnic_upper_t *vu, mblk_t *mp)
{
	if (vu->vu_pending_head == NULL) {
		vu->vu_pending_head = vu->vu_pending_tail = mp;
	} else {
		vu->vu_pending_tail->b_next = mp;
		vu->vu_pending_tail = mp;
	}
}

static inline void
vnic_dq_pending(vnic_upper_t *vu, mblk_t **mpp)
{
	if (vu->vu_pending_head == NULL) {
		*mpp = NULL;
	} else {
		*mpp = vu->vu_pending_head;
		vu->vu_pending_head = (*mpp)->b_next;
		if (vu->vu_pending_head == NULL)
			vu->vu_pending_tail = NULL;
		(*mpp)->b_next = NULL;
	}
}

extern void vnic_lower_modify(vnic_t *);

extern int vnic_dev_create(vnic_ioc_t *, cred_t *);
extern int vnic_dev_modify(vnic_ioc_t *, cred_t *);
extern int vnic_dev_delete(vnic_ioc_t *, cred_t *);
extern int vnic_dev_info(vnic_ioc_t *, cred_t *);

extern void vnic_dev_init(void);
extern void vnic_dev_fini(void);
extern uint_t vnic_dev_count(void);
extern dev_info_t *vnic_get_dip(void);


#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_VNIC_IMPL_H */
