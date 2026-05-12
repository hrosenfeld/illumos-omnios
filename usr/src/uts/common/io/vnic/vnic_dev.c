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
 * Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2018 Joyent, Inc.
 * Copyright 2016 OmniTI Computer Consulting, Inc. All rights reserved.
 * Copyright 2020 OmniOS Community Edition (OmniOSce) Association.
 * Copyright 2022 RackTop Systems, Inc.
 * Copyright 2026 Hans Rosenfeld
 */

#include <sys/types.h>
#include <sys/cred.h>
#include <sys/sysmacros.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/list.h>
#include <sys/ksynch.h>
#include <sys/kmem.h>
#include <sys/stream.h>
#include <sys/modctl.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/atomic.h>
#include <sys/stat.h>
#include <sys/modhash.h>
#include <sys/strsubr.h>
#include <sys/strsun.h>
#include <sys/dlpi.h>
#include <sys/mac.h>
#include <sys/mac_provider.h>
#include <sys/mac_client.h>
#include <sys/mac_client_priv.h>
#include <sys/mac_ether.h>
#include <sys/dls.h>
#include <sys/pattr.h>
#include <sys/time.h>
#include <sys/vlan.h>
#include <sys/vnic.h>
#include <sys/vnic_impl.h>
#include <sys/mac_impl.h>
#include <sys/mac_flow_impl.h>
#include <inet/ip_impl.h>
#include <netinet/vrrp.h>

/*
 * Note that for best performance, the VNIC is a passthrough design.
 * For each VNIC corresponds a MAC client of the underlying MAC (lower MAC).
 * This MAC client is opened by the VNIC driver at VNIC creation,
 * and closed when the VNIC is deleted.
 * When a MAC client of the VNIC itself opens a VNIC, the MAC layer
 * (upper MAC) detects that the MAC being opened is a VNIC. Instead
 * of allocating a new MAC client, it asks the VNIC driver to return
 * the lower MAC client handle associated with the VNIC, and that handle
 * is returned to the upper MAC client directly. This allows access
 * by upper MAC clients of the VNIC to have direct access to the lower
 * MAC client for the control path and data path.
 *
 * Due to this passthrough, some of the entry points exported by the
 * VNIC driver are never directly invoked. These entry points include
 * vnic_m_start, vnic_m_stop, vnic_m_promisc, vnic_m_multicst, etc.
 *
 * VNICs support multiple upper mac clients to enable support for
 * multiple MAC addresses on the VNIC. When the VNIC is created the
 * initial mac client is the primary upper mac. Any additional mac
 * clients are secondary macs.
 */

static vnic_ioc_diag_t vnic_mac2vnic_diag(mac_diag_t);
static int vnic_unicast_add(vnic_ioc_t *, mac_handle_t, mac_client_handle_t,
    mac_unicast_handle_t *, boolean_t);
static int vnic_lower_open(vnic_t *, vnic_ioc_t *, boolean_t);
static int vnic_lower_setup(vnic_t *, vnic_ioc_t *, uint32_t *);
static void vnic_lower_teardown(vnic_t *, boolean_t);
static void vnic_lower_close(vnic_t *);

static int vnic_m_start(void *);
static void vnic_m_stop(void *);
static int vnic_m_promisc(void *, boolean_t);
static int vnic_m_multicst(void *, boolean_t, const uint8_t *);
static int vnic_m_unicst(void *, const uint8_t *);
static int vnic_m_stat(void *, uint_t, uint64_t *);
static void vnic_m_ioctl(void *, queue_t *, mblk_t *);
static int vnic_m_setprop(void *, const char *, mac_prop_id_t, uint_t,
    const void *);
static int vnic_m_getprop(void *, const char *, mac_prop_id_t, uint_t, void *);
static void vnic_m_propinfo(void *, const char *, mac_prop_id_t,
    mac_prop_info_handle_t);
static mblk_t *vnic_m_tx(void *, mblk_t *);
static boolean_t vnic_m_capab_get(void *, mac_capab_t, void *);

static void *vnic_mac_client_handle(void *);
static void vnic_mac_secondary_update(void *);

static void vnic_cleanup_secondary_macs(vnic_t *, int);
static int vnic_set_secondary_macs(vnic_t *, mac_secondary_addr_t *);
static int vnic_get_secondary_macs(vnic_t *, uint_t, void *);

static void vnic_notify_cb(void *, mac_notify_type_t);
static void vnic_notify_lower_cb(void *, mac_notify_type_t);

static kmem_cache_t	*vnic_cache;
static krwlock_t	vnic_lock;
static uint_t		vnic_count;

#define	ANCHOR_VNIC_MIN_MTU	576
#define	ANCHOR_VNIC_MAX_MTU	9000

/* hash of VNICs (vnic_t's), keyed by VNIC id */
static mod_hash_t	*vnic_hash;
#define	VNIC_HASHSZ	64
#define	VNIC_HASH_KEY(vnic_id)	((mod_hash_key_t)(uintptr_t)vnic_id)

#define	VNIC_M_CALLBACK_FLAGS	\
	(MC_IOCTL | MC_GETCAPAB | MC_SETPROP | MC_GETPROP | MC_PROPINFO)

static mac_callbacks_t vnic_m_callbacks = {
	VNIC_M_CALLBACK_FLAGS,
	vnic_m_stat,
	vnic_m_start,
	vnic_m_stop,
	vnic_m_promisc,
	vnic_m_multicst,
	vnic_m_unicst,
	vnic_m_tx,
	NULL,
	vnic_m_ioctl,
	vnic_m_capab_get,
	NULL,
	NULL,
	vnic_m_setprop,
	vnic_m_getprop,
	vnic_m_propinfo
};

void
vnic_dev_init(void)
{
	vnic_cache = kmem_cache_create("vnic_cache",
	    sizeof (vnic_t), 0, NULL, NULL, NULL, NULL, NULL, 0);

	vnic_hash = mod_hash_create_idhash("vnic_hash",
	    VNIC_HASHSZ, mod_hash_null_valdtor);

	rw_init(&vnic_lock, NULL, RW_DEFAULT, NULL);

	vnic_count = 0;
}

void
vnic_dev_fini(void)
{
	ASSERT(vnic_count == 0);

	rw_destroy(&vnic_lock);
	mod_hash_destroy_idhash(vnic_hash);
	kmem_cache_destroy(vnic_cache);
}

uint_t
vnic_dev_count(void)
{
	return (vnic_count);
}

static vnic_ioc_diag_t
vnic_mac2vnic_diag(mac_diag_t diag)
{
	switch (diag) {
	case MAC_DIAG_MACADDR_NIC:
		return (VNIC_IOC_DIAG_MACADDR_NIC);
	case MAC_DIAG_MACADDR_INUSE:
		return (VNIC_IOC_DIAG_MACADDR_INUSE);
	case MAC_DIAG_MACADDR_INVALID:
		return (VNIC_IOC_DIAG_MACADDR_INVALID);
	case MAC_DIAG_MACADDRLEN_INVALID:
		return (VNIC_IOC_DIAG_MACADDRLEN_INVALID);
	case MAC_DIAG_MACFACTORYSLOTINVALID:
		return (VNIC_IOC_DIAG_MACFACTORYSLOTINVALID);
	case MAC_DIAG_MACFACTORYSLOTUSED:
		return (VNIC_IOC_DIAG_MACFACTORYSLOTUSED);
	case MAC_DIAG_MACFACTORYSLOTALLUSED:
		return (VNIC_IOC_DIAG_MACFACTORYSLOTALLUSED);
	case MAC_DIAG_MACFACTORYNOTSUP:
		return (VNIC_IOC_DIAG_MACFACTORYNOTSUP);
	case MAC_DIAG_MACPREFIX_INVALID:
		return (VNIC_IOC_DIAG_MACPREFIX_INVALID);
	case MAC_DIAG_MACPREFIXLEN_INVALID:
		return (VNIC_IOC_DIAG_MACPREFIXLEN_INVALID);
	case MAC_DIAG_MACNO_HWRINGS:
		return (VNIC_IOC_DIAG_NO_HWRINGS);
	default:
		return (VNIC_IOC_DIAG_NONE);
	}
}

static int
vnic_unicast_add(vnic_ioc_t *ioc, mac_handle_t mh, mac_client_handle_t mch,
    mac_unicast_handle_t *muhp, boolean_t reuse_mac_slot)
{
	mac_diag_t mac_diag = MAC_DIAG_NONE;
	mac_unicast_handle_t muh = NULL;
	uint16_t mac_flags = 0;
	int err;

	/*
	 * Sanity check VRID and AF for non-VRRP MACs.
	 */
	if ((ioc->vi_vrid != VRRP_VRID_NONE || ioc->vi_af != AF_UNSPEC) &&
	    (ioc->vi_mac_addr_type != VNIC_MAC_ADDR_TYPE_VRID)) {
		ioc->vi_diag = VNIC_IOC_DIAG_MACADDR_INVALID;
		return (EINVAL);
	}

	switch (ioc->vi_mac_addr_type) {
	case VNIC_MAC_ADDR_TYPE_VRID:
		if (ioc->vi_vrid < VRRP_VRID_MIN ||
		    ioc->vi_vrid > VRRP_VRID_MAX ||
		    (ioc->vi_af != AF_INET && ioc->vi_af != AF_INET6)) {
			ioc->vi_diag = VNIC_IOC_DIAG_MACADDR_INVALID;
			return (EINVAL);
		}
		/* FALLTHROUGH */

	case VNIC_MAC_ADDR_TYPE_FIXED:
		/*
		 * The MAC address value to assign to the VNIC is already
		 * provided in mac_addr. mac_len already contains the MAC
		 * address length.
		 */
		break;

	case VNIC_MAC_ADDR_TYPE_AUTO:
		ioc->vi_mac_slot = -1;
		/* first try to allocate a factory MAC address */
		/* FALLTHROUGH */

	case VNIC_MAC_ADDR_TYPE_FACTORY:
		/* sanity check the specified slot number */
		if (ioc->vi_mac_slot < 0 && ioc->vi_mac_slot != -1) {
			ioc->vi_diag = VNIC_IOC_DIAG_MACFACTORYSLOTINVALID;
			return (EINVAL);
		}

		if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_FACTORY &&
		    reuse_mac_slot) {
			if (ioc->vi_mac_slot == -1) {
				ioc->vi_diag =
				    VNIC_IOC_DIAG_MACFACTORYSLOTINVALID;
				return (EINVAL);
			}
			break;
		}

		err = mac_addr_factory_reserve(mch, &ioc->vi_mac_slot);
		if (err == 0) {
			mac_addr_factory_value(mh, ioc->vi_mac_slot,
			    ioc->vi_mac_addr, &ioc->vi_mac_len, NULL, NULL);
			ioc->vi_mac_addr_type = VNIC_MAC_ADDR_TYPE_FACTORY;
			break;
		}

		if (err == EINVAL)
			ioc->vi_diag = VNIC_IOC_DIAG_MACFACTORYSLOTINVALID;
		else if (err == EBUSY)
			ioc->vi_diag = VNIC_IOC_DIAG_MACFACTORYSLOTUSED;
		else if (err == ENOSPC)
			ioc->vi_diag = VNIC_IOC_DIAG_MACFACTORYSLOTALLUSED;

		if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_FACTORY)
			return (err);

		/*
		 * Allocating a factory MAC address failed, generate a
		 * random MAC address instead.
		 */
		/* FALLTHROUGH */

	case VNIC_MAC_ADDR_TYPE_RANDOM:
		/*
		 * Random MAC address. There are two sub-cases:
		 *
		 * 1 - If mac_len == 0, a new MAC address is generated.
		 *	The length of the MAC address to generated depends
		 *	on the type of MAC used. The prefix to use for the MAC
		 *	address is stored in the most significant bytes
		 *	of the mac_addr argument, and its length is specified
		 *	by the mac_prefix_len argument. This prefix can
		 *	correspond to a IEEE OUI in the case of Ethernet,
		 *	for example.
		 *
		 * 2 - If mac_len > 0, the address was already picked
		 *	randomly, and is now passed back during VNIC
		 *	re-creation. The mac_addr argument contains the MAC
		 *	address that was generated. We distinguish this
		 *	case from the fixed MAC address case, since we
		 *	want the user consumers to know, when they query
		 *	the list of VNICs, that a VNIC was assigned a
		 *	random MAC address vs assigned a fixed address
		 *	specified by the user.
		 */

		/*
		 * If it's a pre-generated address, we're done. mac_addr and
		 * mac_len already contain the MAC address value and length.
		 */
		if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_RANDOM &&
		    ioc->vi_mac_len > 0)
			break;

		if (ioc->vi_mac_len > 0 &&
		    ioc->vi_mac_len != mac_addr_len(mh)) {
			ioc->vi_diag = VNIC_IOC_DIAG_MACADDRLEN_INVALID;
			return (EINVAL);
		}

		/* sanity check the speficied prefix length */
		if (ioc->vi_mac_prefix_len > MAXMACADDRLEN) {
			ioc->vi_diag = VNIC_IOC_DIAG_MACPREFIXLEN_INVALID;
			return (EINVAL);
		}

		/* generate a new random MAC address */
		err = mac_addr_random(mch, ioc->vi_mac_prefix_len,
		    ioc->vi_mac_addr, &mac_diag);
		if (err != 0) {
			ioc->vi_diag = vnic_mac2vnic_diag(mac_diag);
			return (err);
		}
		ioc->vi_mac_len = mac_addr_len(mh);
		ioc->vi_mac_addr_type = VNIC_MAC_ADDR_TYPE_RANDOM;
		break;

	case VNIC_MAC_ADDR_TYPE_PRIMARY:
		/*
		 * We get the address here since we copy it in the
		 * vnic's vn_addr.
		 */
		mac_unicast_primary_get(mh, ioc->vi_mac_addr);
		ioc->vi_mac_len = mac_addr_len(mh);
		mac_flags |= MAC_UNICAST_VNIC_PRIMARY;
		break;
	default:
		return (EINVAL);
	}

	/*
	 * Sanity check the MAC address length.
	 */
	if (ioc->vi_mac_len == 0 || ioc->vi_mac_len > MAXMACADDRLEN) {
		ioc->vi_diag = VNIC_IOC_DIAG_MACADDRLEN_INVALID;
		return (EINVAL);
	}

	err = mac_unicast_add(mch, ioc->vi_mac_addr, mac_flags,
	    &muh, ioc->vi_vid, &mac_diag);
	if (err != 0) {
		if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_FACTORY) {
			/* release factory MAC address */
			mac_addr_factory_release(mch, ioc->vi_mac_slot);
		}
		ioc->vi_diag = vnic_mac2vnic_diag(mac_diag);
		return (err);
	}

	*muhp = muh;

	return (0);
}

/* Open the lower MAC and get a MAC client handle for the VNIC. */
static int
vnic_lower_open(vnic_t *vnic, vnic_ioc_t *ioc, boolean_t reuse_mac_slot)
{
	mac_handle_t new_lower_mh = NULL;
	mac_client_handle_t new_mch = NULL;
	mac_unicast_handle_t new_muh = NULL;
	const mac_info_t *minfop;
	char vnic_name[MAXNAMELEN];
	int err;

	if (ioc->vi_link_id == DATALINK_INVALID_LINKID)
		return (EINVAL);

	err = mac_open_by_linkid(ioc->vi_link_id, &new_lower_mh);
	if (err != 0)
		return (err);

	/*
	 * VNIC(vlan) over VNICs(vlans) is not supported.
	 */
	if (mac_is_vnic(new_lower_mh)) {
		err = EINVAL;
		goto bail;
	}

	/* only ethernet support for now */
	minfop = mac_info(new_lower_mh);
	if (minfop->mi_nativemedia != DL_ETHER) {
		err = ENOTSUP;
		goto bail;
	}

	(void) dls_mgmt_get_linkinfo(ioc->vi_vnic_id, vnic_name, NULL,
	    NULL, NULL);
	err = mac_client_open(new_lower_mh, &new_mch,
	    vnic_name, MAC_OPEN_FLAGS_IS_VNIC);
	if (err != 0)
		goto bail;


	/* Assign the MAC address to the VNIC. */
	err = vnic_unicast_add(ioc, new_lower_mh, new_mch, &new_muh,
	    reuse_mac_slot);
	if (err != 0)
		goto bail;

	vnic->vn_lower_mh = new_lower_mh;
	vnic->vn_mch = new_mch;
	vnic->vn_muh = new_muh;

	vnic->vn_addr_type = ioc->vi_mac_addr_type;
	vnic->vn_addr_len = ioc->vi_mac_len;
	bcopy(ioc->vi_mac_addr, vnic->vn_addr, vnic->vn_addr_len);
	vnic->vn_slot_id = ioc->vi_mac_slot;
	vnic->vn_vrid = ioc->vi_vrid;
	vnic->vn_af = ioc->vi_af;
	vnic->vn_vid = ioc->vi_vid;

	/* register to receive notification from underlying MAC */
	vnic->vn_lower_mnh = mac_notify_add(new_lower_mh, vnic_notify_lower_cb,
	    vnic);

	return (0);

bail:
	if (new_mch != NULL)
		mac_client_close(new_mch, MAC_CLOSE_FLAGS_IS_VNIC);

	if (new_lower_mh != NULL)
		mac_close(new_lower_mh);

	return (err);
}

static int
vnic_lower_setup(vnic_t *vnic, vnic_ioc_t *ioc, uint32_t *mtu)
{
	int err;

	/*
	 * Set the initial VNIC capabilities. If the VNIC is created
	 * over MACs which do not support native vlan, disable the
	 * VNIC's hardware checksum capability if its VID is not 0,
	 * since the underlying MAC would get the hardware checksum
	 * offset wrong in case of VLAN packets.
	 */
	vnic->vn_hcksum_txflags = 0;
	if (ioc->vi_vid != VLAN_ID_UNTAGGED &&
	    !mac_capab_get(vnic->vn_lower_mh, MAC_CAPAB_NO_NATIVEVLAN, NULL)) {
		(void) mac_capab_get(vnic->vn_lower_mh, MAC_CAPAB_HCKSUM,
		    &vnic->vn_hcksum_txflags);
	}

	/*
	 * Check for LSO capabilities. LSO implementations depend on
	 * hardware checksumming, so the same requirement is enforced here.
	 */
	vnic->vn_cap_lso.lso_flags = 0;
	if (vnic->vn_hcksum_txflags != 0) {
		(void) mac_capab_get(vnic->vn_lower_mh, MAC_CAPAB_LSO,
		    &vnic->vn_cap_lso);
	}

	/*
	 * If this is a VNIC-based VLAN, then we check for the margin unless
	 * it has been created with the force flag. If we are configuring a
	 * VLAN over an etherstub, we don't check the margin even if force
	 * is not set.
	 */
	if (ioc->vi_vid == VLAN_ID_UNTAGGED || ioc->vi_force != 0) {
		if (ioc->vi_vid != VLAN_ID_UNTAGGED)
			vnic->vn_force = B_TRUE;
		/*
		 * As the current margin size of the underlying mac is
		 * used to determine the margin size of the VNIC itself,
		 * request the underlying mac not to change to a smaller
		 * margin size.
		 */
		err = mac_margin_add(vnic->vn_lower_mh, &vnic->vn_margin,
		    B_TRUE);
		ASSERT(err == 0);
	} else {
		vnic->vn_margin = VLAN_TAGSZ;
		err = mac_margin_add(vnic->vn_lower_mh, &vnic->vn_margin,
		    B_FALSE);
		if (err != 0) {
			ioc->vi_diag = VNIC_IOC_DIAG_MACMARGIN_INVALID;
			return (err);
		}
	}

	err = mac_mtu_add(vnic->vn_lower_mh, mtu, B_FALSE);
	if (err != 0) {
		VERIFY0(mac_margin_remove(vnic->vn_lower_mh, vnic->vn_margin));
		ioc->vi_diag = VNIC_IOC_DIAG_MACMTU_INVALID;
		return (err);
	}

	return (0);
}

static void
vnic_lower_teardown(vnic_t *vnic, boolean_t reuse_mac_slot)
{
	if (vnic->vn_lower_mh != NULL && vnic->vn_mtu != 0) {
		(void) mac_margin_remove(vnic->vn_lower_mh, vnic->vn_margin);
		(void) mac_mtu_remove(vnic->vn_lower_mh, vnic->vn_mtu);
	}

	/*
	 * Check if the old MAC address for the vnic was obtained from the
	 * factory MAC addresses. If yes, release it.
	 */
	if (vnic->vn_mch != NULL &&
	    vnic->vn_addr_type == VNIC_MAC_ADDR_TYPE_FACTORY &&
	    !reuse_mac_slot)
		(void) mac_addr_factory_release(vnic->vn_mch, vnic->vn_slot_id);
}

static void
vnic_lower_close(vnic_t *vnic)
{
	if (vnic->vn_lower_mnh != NULL)
		(void) mac_notify_remove(vnic->vn_lower_mnh, B_TRUE);

	if (vnic->vn_muh != NULL)
		(void) mac_unicast_remove(vnic->vn_mch, vnic->vn_muh);

	if (vnic->vn_mch != NULL)
		mac_client_close(vnic->vn_mch, MAC_CLOSE_FLAGS_IS_VNIC);

	if (vnic->vn_lower_mh != NULL)
		mac_close(vnic->vn_lower_mh);
}

void
vnic_lower_modify(vnic_t *vnic)
{
	vnic_ioc_t *ioc = vnic->vn_modify_ioc;
	mac_resource_props_t *mrp;
	boolean_t reuse_mac_slot = B_FALSE;
	int err;

	if (vnic->vn_modify_done)
		return;

	vnic->vn_modify_done = B_TRUE;

	/* Check whether we're reusing the same factory MAC address. */
	if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_FACTORY &&
	    vnic->vn_addr_type == VNIC_MAC_ADDR_TYPE_FACTORY &&
	    ioc->vi_mac_slot == vnic->vn_slot_id)
		reuse_mac_slot = B_TRUE;

	/*
	 * Remove the previous unicast address and the associated data
	 * paths and flows, else we'll fail due to collisions.
	 */
	vnic_cleanup_secondary_macs(vnic, vnic->vn_nhandles);
	VERIFY0(mac_unicast_remove(vnic->vn_mch, vnic->vn_muh));
	vnic->vn_muh = NULL;

	/* Make a copy of the vnic_t in case we need to restore it. */
	bcopy(vnic, vnic->vn_orig_vnic, sizeof (vnic_t));

	/* Setup new lower MAC as vnic_dev_create() would do. */
	err = vnic_lower_open(vnic, ioc, reuse_mac_slot);
	if (err != 0)
		goto fail_open;

	err = vnic_lower_setup(vnic, ioc, &vnic->vn_mtu);
	if (err != 0) {
		goto fail_setup;
	}

	/*
	 * Copy our original resources from the lower MAC, then apply
	 * any new resources we have been given in the modify request.
	 */
	mrp = kmem_zalloc(sizeof (*mrp), KM_SLEEP);
	mac_client_get_resources(vnic->vn_orig_vnic->vn_mch, mrp);
	mac_set_upper_mac(vnic->vn_mch, vnic->vn_mh, NULL);
	err = mac_client_set_resources(vnic->vn_mch, mrp);
	kmem_free(mrp, sizeof (*mrp));
	if (err != 0)
		goto fail;

	err = mac_client_set_resources(vnic->vn_mch, &ioc->vi_resource_props);
	if (err != 0)
		goto fail;

	/*
	 * The modification was successful. Tell clients to reopen their
	 * client handles, and update link state and capabilities based on
	 * new lower MAC.
	 */
	vnic->vn_modify_error = 0;

	mac_notify_reopen(vnic->vn_mh);

	vnic->vn_ls = mac_client_stat_get(vnic->vn_mch, MAC_STAT_LINK_STATE);
	mac_link_update(vnic->vn_mh, vnic->vn_ls);

	mac_capab_update(vnic->vn_mh);

	return;

fail:
	vnic_lower_teardown(vnic, reuse_mac_slot);

fail_setup:
	vnic_lower_close(vnic);

fail_open:
	/* Restore the original vnic_t */
	bcopy(vnic->vn_orig_vnic, vnic, sizeof (vnic_t));

	/* Restore the original MAC address. */
	ioc->vi_mac_addr_type = vnic->vn_addr_type;
	ioc->vi_mac_len = vnic->vn_addr_len;
	bcopy(vnic->vn_addr, ioc->vi_mac_addr, ioc->vi_mac_len);
	ioc->vi_mac_slot = vnic->vn_slot_id;
	ioc->vi_vrid = vnic->vn_vrid;
	ioc->vi_af = vnic->vn_af;
	ioc->vi_vid = vnic->vn_vid;

	VERIFY0(vnic_unicast_add(ioc, vnic->vn_lower_mh, vnic->vn_mch,
	    &vnic->vn_muh, reuse_mac_slot));

	vnic->vn_modify_error = err;
}

/*
 * Create a new VNIC upon request from administrator.
 * Returns 0 on success, an errno on failure.
 */
/* ARGSUSED */
int
vnic_dev_create(vnic_ioc_t *ioc, cred_t *credp)
{
	boolean_t is_anchor = ((ioc->vi_flags & VNIC_IOC_FLAGS_ANCHOR) != 0);
	mac_register_t *mac = NULL;
	vnic_t *vnic;
	int err;

	ioc->vi_diag = VNIC_IOC_DIAG_NONE;

	rw_enter(&vnic_lock, RW_WRITER);

	/* Does a VNIC with the same id already exist? */
	err = mod_hash_find(vnic_hash, VNIC_HASH_KEY(ioc->vi_vnic_id),
	    (mod_hash_val_t *)&vnic);
	if (err == 0) {
		rw_exit(&vnic_lock);
		return (EEXIST);
	}

	vnic = kmem_cache_alloc(vnic_cache, KM_NOSLEEP);
	if (vnic == NULL) {
		rw_exit(&vnic_lock);
		return (ENOMEM);
	}

	bzero(vnic, sizeof (*vnic));

	mutex_init(&vnic->vn_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&vnic->vn_modify_cv, NULL, CV_DRIVER, NULL);
	cv_init(&vnic->vn_switch_cv, NULL, CV_DRIVER, NULL);
	list_create(&vnic->vn_upper_list, sizeof (vnic_upper_t),
	    offsetof(vnic_upper_t, vu_list_node));

	vnic->vn_ls = LINK_STATE_UNKNOWN;
	vnic->vn_id = ioc->vi_vnic_id;

	if (ioc->vi_vid == VLAN_ID_UNTAGGED)
		ioc->vi_vid = VLAN_ID_NONE;

	/* Check whether a VLAN ID was given. */
	if (ioc->vi_vid > VLAN_ID_MAX) {
		err = EINVAL;
		/* ioc->vi_diag = VNIC_IOC_DIAG_VID_INVALID; */
		goto bail;
	}

	mac = mac_alloc(MAC_VERSION);
	VERIFY(mac != NULL);

	mac->m_type_ident = MAC_PLUGIN_IDENT_ETHER;
	mac->m_driver = vnic;
	mac->m_dip = vnic_get_dip();
	mac->m_instance = (uint_t)-1;
	mac->m_src_addr = vnic->vn_addr;
	mac->m_callbacks = &vnic_m_callbacks;

	if (!is_anchor) {
		/*
		 * Open the lower MAC and assign its initial bandwidth and
		 * MAC address. We do this here during VNIC creation and
		 * do not wait until the upper MAC client open so that we
		 * can validate the VNIC creation parameters (bandwidth,
		 * MAC address, etc) and reserve a factory MAC address if
		 * one was requested.
		 */
		err = vnic_lower_open(vnic, ioc, B_FALSE);
		if (err != 0)
			goto bail;

		mac_sdu_get(vnic->vn_lower_mh, &mac->m_min_sdu,
		    &mac->m_max_sdu);

		err = vnic_lower_setup(vnic, ioc, &mac->m_max_sdu);
		if (err != 0)
			goto bail;
	} else {
		vnic->vn_margin = VLAN_TAGSZ;
		mac->m_min_sdu = 1;
		mac->m_max_sdu = ANCHOR_VNIC_MAX_MTU;
	}
	vnic->vn_mtu = mac->m_max_sdu;

	mac->m_margin = vnic->vn_margin;

	vnic->vn_link_id = ioc->vi_link_id;

	/* register with the MAC module */
	err = mac_register(mac, &vnic->vn_mh);
	mac_free(mac);
	mac = NULL;
	if (err != 0)
		goto bail;

	if (!is_anchor) {
		/* register to receive our own notifications */
		vnic->vn_mnh = mac_notify_add(vnic->vn_mh, vnic_notify_cb,
		    vnic);

		vnic->vn_taskq = taskq_create(mac_name(vnic->vn_mh), 1,
		    minclsyspri, 3, 512, TASKQ_DYNAMIC);
		if (vnic->vn_taskq == NULL)
			goto bail;

		/* Set the VNIC's MAC in the client */
		mac_set_upper_mac(vnic->vn_mch, vnic->vn_mh,
		    &ioc->vi_resource_props);

		if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_PRIMARY &&
		    ((ioc->vi_resource_props.mrp_mask & MRP_RX_RINGS) != 0 ||
		    (ioc->vi_resource_props.mrp_mask & MRP_TX_RINGS) != 0)) {
			err = ENOTSUP;
			ioc->vi_diag = VNIC_IOC_DIAG_NO_HWRINGS;
			goto bail;
		}

		err = mac_client_set_resources(vnic->vn_mch,
		    &ioc->vi_resource_props);
		if (err != 0)
			goto bail;
	}

	err = dls_devnet_create(vnic->vn_mh, vnic->vn_id, crgetzoneid(credp));
	if (err != 0)
		goto bail;

	/* add new VNIC to hash table */
	err = mod_hash_insert(vnic_hash, VNIC_HASH_KEY(ioc->vi_vnic_id),
	    (mod_hash_val_t)vnic);
	ASSERT(err == 0);
	vnic_count++;

	/*
	 * Now that we've enabled this VNIC, we should go through and update the
	 * link state by setting it to our parents.
	 */
	vnic->vn_enabled = B_TRUE;

	if (is_anchor) {
		vnic->vn_ls = LINK_STATE_UP;
	} else {
		vnic->vn_ls = mac_client_stat_get(vnic->vn_mch,
		    MAC_STAT_LINK_STATE);
	}

	ioc->vi_vid = MAC_VLAN_UNTAGGED_VID(vnic->vn_vid);

	mac_link_update(vnic->vn_mh, vnic->vn_ls);

	rw_exit(&vnic_lock);

	return (0);

bail:
	rw_exit(&vnic_lock);

	if (mac != NULL)
		mac_free(mac);

	if (vnic->vn_mh != NULL)
		(void) mac_unregister(vnic->vn_mh);

	if (!is_anchor) {
		vnic_lower_teardown(vnic, B_FALSE);
		vnic_lower_close(vnic);
	}

	if (vnic->vn_taskq != NULL)
		taskq_destroy(vnic->vn_taskq);

	VERIFY(list_is_empty(&vnic->vn_upper_list));
	VERIFY0(vnic->vn_hold_cnt);
	list_destroy(&vnic->vn_upper_list);
	mutex_destroy(&vnic->vn_lock);

	kmem_cache_free(vnic_cache, vnic);
	return (err);
}

/*
 * Modify the properties of an existing VNIC.
 */
/* ARGSUSED */
int
vnic_dev_modify(vnic_ioc_t *ioc, cred_t *credp)
{
	vnic_t *vnic;
	int err;

	/* We don't work on etherstub vnics. */
	if ((ioc->vi_flags & VNIC_IOC_FLAGS_ANCHOR) != 0)
		return (ENOTSUP);

	/* We also don't work on VLAN vnics. */
	if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_PRIMARY)
		return (ENOTSUP);

	if ((ioc->vi_resource_props.mrp_mask & MRP_RX_RINGS) != 0 ||
	    (ioc->vi_resource_props.mrp_mask & MRP_TX_RINGS) != 0) {
		ioc->vi_diag = VNIC_IOC_DIAG_NO_HWRINGS;
		return (ENOTSUP);
	}

	ioc->vi_diag = VNIC_IOC_DIAG_NONE;

	rw_enter(&vnic_lock, RW_WRITER);

	if (mod_hash_find(vnic_hash, VNIC_HASH_KEY(ioc->vi_vnic_id),
	    (mod_hash_val_t *)&vnic) != 0) {
		rw_exit(&vnic_lock);
		return (ENOENT);
	}

	mutex_enter(&vnic->vn_lock);
	if (vnic->vn_modifying) {
		mutex_exit(&vnic->vn_lock);
		rw_exit(&vnic_lock);
		return (EBUSY);
	}

	/* Allocate a 2nd vnic_t for error recovery. */
	vnic->vn_orig_vnic = kmem_cache_alloc(vnic_cache, KM_NOSLEEP);
	if (vnic->vn_orig_vnic == NULL) {
		mutex_exit(&vnic->vn_lock);
		rw_exit(&vnic_lock);
		return (ENOMEM);
	}

	/* No new link ID given, use existing. */
	if (ioc->vi_link_id == DATALINK_INVALID_LINKID) {
		ioc->vi_link_id = vnic->vn_link_id;
	}

	/* No new VLAN ID given, use existing. */
	if (ioc->vi_vid == VLAN_ID_NONE)
		ioc->vi_vid = MAC_VLAN_UNTAGGED_VID(vnic->vn_vid);

	/* No new MAC address given, use existing. */
	if (ioc->vi_mac_addr_type == VNIC_MAC_ADDR_TYPE_UNKNOWN) {
		ioc->vi_mac_addr_type = vnic->vn_addr_type;
		ioc->vi_mac_len = vnic->vn_addr_len;
		bcopy(vnic->vn_addr, ioc->vi_mac_addr, ioc->vi_mac_len);
		ioc->vi_mac_slot = vnic->vn_slot_id;
		ioc->vi_vrid = vnic->vn_vrid;
		ioc->vi_af = vnic->vn_af;
	}

	if (ioc->vi_vid == VLAN_ID_UNTAGGED)
		ioc->vi_vid = VLAN_ID_NONE;

	VERIFY0(vnic->vn_modify_cnt);
	VERIFY0(vnic->vn_modifying);
	vnic->vn_modifying = B_TRUE;
	vnic->vn_modify_done = B_FALSE;
	vnic->vn_modify_error = ENOTSUP;
	vnic->vn_modify_ioc = ioc;

	if (mac_has_vnic_primary_client(vnic->vn_mh)) {
		/*
		 * Notify clients that they'll need to replumb their streams.
		 */
		vnic->vn_modify_cnt = vnic->vn_hold_cnt;
		vnic->vn_replumb_done = B_FALSE;
		mac_notify_replumb(vnic->vn_mh);

		while (!vnic->vn_replumb_done || vnic->vn_modify_cnt > 0)
			cv_wait(&vnic->vn_modify_cv, &vnic->vn_lock);
	}

	/*
	 * If the modification wasn't performed already as part of the REPLUMB
	 * notification processing, do it now.
	 */
	if (!vnic->vn_modify_done)
		vnic_lower_modify(vnic);

	err = vnic->vn_modify_error;

	if (err != 0)
		goto out;

	vnic_lower_teardown(vnic->vn_orig_vnic, B_FALSE);
	vnic_lower_close(vnic->vn_orig_vnic);

out:
	kmem_cache_free(vnic_cache, vnic->vn_orig_vnic);
	vnic->vn_orig_vnic = NULL;
	vnic->vn_modify_ioc = NULL;
	vnic->vn_modify_cnt = 0;
	vnic->vn_modify_error = 0;
	vnic->vn_modify_done = B_FALSE;
	vnic->vn_modifying = B_FALSE;
	vnic->vn_replumb_done = B_FALSE;
	cv_signal(&vnic->vn_switch_cv);
	mutex_exit(&vnic->vn_lock);
	rw_exit(&vnic_lock);

	ioc->vi_vid = MAC_VLAN_UNTAGGED_VID(vnic->vn_vid);

	return (err);
}

/* ARGSUSED */
int
vnic_dev_delete(vnic_ioc_t *ioc, cred_t *credp)
{
	vnic_t *vnic = NULL;
	mod_hash_val_t val;
	datalink_id_t tmpid;
	int rc;

	rw_enter(&vnic_lock, RW_WRITER);

	if (mod_hash_find(vnic_hash, VNIC_HASH_KEY(ioc->vi_vnic_id),
	    (mod_hash_val_t *)&vnic) != 0) {
		rw_exit(&vnic_lock);
		return (ENOENT);
	}

	if ((rc = dls_devnet_destroy(vnic->vn_mh, &tmpid, B_TRUE)) != 0) {
		rw_exit(&vnic_lock);
		return (rc);
	}

	ASSERT(ioc->vi_vnic_id == tmpid);

	/*
	 * We cannot unregister the MAC yet. Unregistering would
	 * free up mac_impl_t which should not happen at this time.
	 * So disable mac_impl_t by calling mac_disable(). This will prevent
	 * any new claims on mac_impl_t.
	 */
	if ((rc = mac_disable(vnic->vn_mh)) != 0) {
		(void) dls_devnet_create(vnic->vn_mh, ioc->vi_vnic_id,
		    crgetzoneid(credp));
		rw_exit(&vnic_lock);
		return (rc);
	}

	vnic_cleanup_secondary_macs(vnic, vnic->vn_nhandles);

	vnic->vn_enabled = B_FALSE;
	(void) mod_hash_remove(vnic_hash, VNIC_HASH_KEY(ioc->vi_vnic_id), &val);
	ASSERT(vnic == (vnic_t *)val);
	vnic_count--;
	rw_exit(&vnic_lock);

	if (vnic->vn_mnh != NULL)
		(void) mac_notify_remove(vnic->vn_mnh, B_TRUE);

	/*
	 * XXX-nicolas shouldn't have a void cast here, if it's
	 * expected that the function will never fail, then we should
	 * have an ASSERT().
	 */
	(void) mac_unregister(vnic->vn_mh);

	VERIFY(list_is_empty(&vnic->vn_upper_list));
	VERIFY0(vnic->vn_hold_cnt);
	list_destroy(&vnic->vn_upper_list);
	mutex_destroy(&vnic->vn_lock);

	if (vnic->vn_lower_mh != NULL) {
		vnic_lower_teardown(vnic, B_FALSE);
		vnic_lower_close(vnic);
	}

	kmem_cache_free(vnic_cache, vnic);
	return (0);
}

int
vnic_dev_info(vnic_ioc_t *ioc, cred_t *credp)
{
	vnic_t		*vnic;
	int		err;

	/* Make sure that the VNIC link is visible from the caller's zone. */
	if (!dls_devnet_islinkvisible(ioc->vi_vnic_id, crgetzoneid(credp)))
		return (ENOENT);

	rw_enter(&vnic_lock, RW_WRITER);

	err = mod_hash_find(vnic_hash, VNIC_HASH_KEY(ioc->vi_vnic_id),
	    (mod_hash_val_t *)&vnic);
	if (err != 0) {
		rw_exit(&vnic_lock);
		return (ENOENT);
	}

	bzero(ioc, sizeof (vnic_ioc_t));
	ioc->vi_vnic_id = vnic->vn_id;
	ioc->vi_link_id = vnic->vn_link_id;
	ioc->vi_mac_addr_type = vnic->vn_addr_type;
	ioc->vi_mac_len = vnic->vn_addr_len;
	bcopy(vnic->vn_addr, ioc->vi_mac_addr, MAXMACADDRLEN);
	ioc->vi_mac_prefix_len = 0;
	ioc->vi_mac_slot = vnic->vn_slot_id;
	ioc->vi_vid = MAC_VLAN_UNTAGGED_VID(vnic->vn_vid);
	ioc->vi_force = vnic->vn_force;
	ioc->vi_flags = vnic->vn_link_id == DATALINK_INVALID_LINKID ?
	    VNIC_IOC_FLAGS_ANCHOR : 0;
	ioc->vi_vrid = vnic->vn_vrid;
	ioc->vi_af = vnic->vn_af;
	ioc->vi_diag = VNIC_IOC_DIAG_NONE;

	if (vnic->vn_mch != NULL)
		mac_client_get_resources(vnic->vn_mch,
		    &ioc->vi_resource_props);

	rw_exit(&vnic_lock);
	return (0);
}

/* ARGSUSED */
mblk_t *
vnic_m_tx(void *arg, mblk_t *mp_chain)
{
	/*
	 * This function could be invoked for an anchor VNIC when sending
	 * broadcast and multicast packets, and unicast packets which did
	 * not match any local known destination.
	 */
	freemsgchain(mp_chain);
	return (NULL);
}

/*ARGSUSED*/
static void
vnic_m_ioctl(void *arg, queue_t *q, mblk_t *mp)
{
	miocnak(q, mp, 0, ENOTSUP);
}

/*
 * This entry point cannot be passed-through, since it is invoked
 * for the per-VNIC kstats which must be exported independently
 * of the existence of VNIC MAC clients.
 */
static int
vnic_m_stat(void *arg, uint_t stat, uint64_t *val)
{
	vnic_t *vnic = arg;
	int rval = 0;

	if (vnic->vn_lower_mh == NULL) {
		/*
		 * It's an anchor VNIC, which does not have any
		 * statistics in itself.
		 */
		return (ENOTSUP);
	}

	/*
	 * ENOTSUP must be reported for unsupported stats, the VNIC
	 * driver reports a subset of the stats that would
	 * be returned by a real piece of hardware.
	 */

	switch (stat) {
	case MAC_STAT_LINK_STATE:
	case MAC_STAT_LINK_UP:
	case MAC_STAT_PROMISC:
	case MAC_STAT_IFSPEED:
	case MAC_STAT_MULTIRCV:
	case MAC_STAT_MULTIXMT:
	case MAC_STAT_BRDCSTRCV:
	case MAC_STAT_BRDCSTXMT:
	case MAC_STAT_OPACKETS:
	case MAC_STAT_OBYTES:
	case MAC_STAT_IERRORS:
	case MAC_STAT_OERRORS:
	case MAC_STAT_RBYTES:
	case MAC_STAT_IPACKETS:
		*val = mac_client_stat_get(vnic->vn_mch, stat);
		break;
	default:
		rval = ENOTSUP;
	}

	return (rval);
}

/*
 * Invoked by the upper MAC to retrieve the lower MAC client handle
 * corresponding to a VNIC. A pointer to this function is obtained
 * by the upper MAC via capability query.
 *
 * XXX-nicolas Note: this currently causes all VNIC MAC clients to
 * receive the same MAC client handle for the same VNIC. This is ok
 * as long as we have only one VNIC MAC client which sends and
 * receives data, but we don't currently enforce this at the MAC layer.
 */
static void *
vnic_mac_client_handle(void *vnic_arg)
{
	vnic_t *vnic = vnic_arg;

	return (vnic->vn_mch);
}

/*
 * Invoked when updating the primary MAC so that the secondary MACs are
 * kept in sync.
 */
static void
vnic_mac_secondary_update(void *vnic_arg)
{
	vnic_t *vn = vnic_arg;
	int i;

	for (i = 1; i <= vn->vn_nhandles; i++) {
		mac_secondary_dup(vn->vn_mc_handles[0], vn->vn_mc_handles[i]);
	}
}

/*
 * Return information about the specified capability.
 */
/* ARGSUSED */
static boolean_t
vnic_m_capab_get(void *arg, mac_capab_t cap, void *cap_data)
{
	vnic_t *vnic = arg;

	switch (cap) {
	case MAC_CAPAB_HCKSUM: {
		uint32_t *hcksum_txflags = cap_data;

		*hcksum_txflags = vnic->vn_hcksum_txflags &
		    (HCKSUM_INET_FULL_V4 | HCKSUM_INET_FULL_V6 |
		    HCKSUM_IPHDRCKSUM | HCKSUM_INET_PARTIAL);
		break;
	}
	case MAC_CAPAB_LSO: {
		mac_capab_lso_t *cap_lso = cap_data;

		if (vnic->vn_cap_lso.lso_flags == 0) {
			return (B_FALSE);
		}
		*cap_lso = vnic->vn_cap_lso;
		break;
	}
	case MAC_CAPAB_VNIC: {
		mac_capab_vnic_t *vnic_capab = cap_data;

		if (vnic->vn_lower_mh == NULL) {
			/*
			 * It's an anchor VNIC, we don't have an underlying
			 * NIC and MAC client handle.
			 */
			return (B_FALSE);
		}

		if (vnic_capab != NULL) {
			vnic_capab->mcv_arg = vnic;
			vnic_capab->mcv_mac_client_handle =
			    vnic_mac_client_handle;
			vnic_capab->mcv_mac_secondary_update =
			    vnic_mac_secondary_update;
		}
		break;
	}
	case MAC_CAPAB_ANCHOR_VNIC: {
		/* since it's an anchor VNIC we don't have lower mac handle */
		if (vnic->vn_lower_mh == NULL) {
			ASSERT(vnic->vn_link_id == 0);
			return (B_TRUE);
		}
		return (B_FALSE);
	}
	case MAC_CAPAB_NO_NATIVEVLAN:
		return (B_FALSE);
	case MAC_CAPAB_NO_ZCOPY:
		return (B_TRUE);
	case MAC_CAPAB_VRRP: {
		mac_capab_vrrp_t *vrrp_capab = cap_data;

		if (vnic->vn_vrid != 0) {
			if (vrrp_capab != NULL)
				vrrp_capab->mcv_af = vnic->vn_af;
			return (B_TRUE);
		}
		return (B_FALSE);
	}
	default:
		return (B_FALSE);
	}
	return (B_TRUE);
}

/* ARGSUSED */
static int
vnic_m_start(void *arg)
{
	return (0);
}

/* ARGSUSED */
static void
vnic_m_stop(void *arg)
{
}

/* ARGSUSED */
static int
vnic_m_promisc(void *arg, boolean_t on)
{
	return (0);
}

/* ARGSUSED */
static int
vnic_m_multicst(void *arg, boolean_t add, const uint8_t *addrp)
{
	return (0);
}

static int
vnic_m_unicst(void *arg, const uint8_t *macaddr)
{
	vnic_t *vnic = arg;

	return (mac_vnic_unicast_set(vnic->vn_mch, macaddr));
}

static void
vnic_cleanup_secondary_macs(vnic_t *vn, int cnt)
{
	int i;

	/* Remove existing secondaries (primary is at 0) */
	for (i = 1; i <= cnt; i++) {
		mac_rx_clear(vn->vn_mc_handles[i]);

		/* unicast handle might not have been set yet */
		if (vn->vn_mu_handles[i] != NULL)
			(void) mac_unicast_remove(vn->vn_mc_handles[i],
			    vn->vn_mu_handles[i]);

		mac_secondary_cleanup(vn->vn_mc_handles[i]);

		mac_client_close(vn->vn_mc_handles[i], MAC_CLOSE_FLAGS_IS_VNIC);

		vn->vn_mu_handles[i] = NULL;
		vn->vn_mc_handles[i] = NULL;
	}

	vn->vn_nhandles = 0;
}

/*
 * Setup secondary MAC addresses on the vnic. Due to limitations in the mac
 * code, each mac address must be associated with a mac_client (and the
 * flow that goes along with the client) so we need to create those clients
 * here.
 */
static int
vnic_set_secondary_macs(vnic_t *vn, mac_secondary_addr_t *msa)
{
	int i, err;
	char primary_name[MAXNAMELEN];

	/* First, remove pre-existing secondaries */
	ASSERT(vn->vn_nhandles < MPT_MAXMACADDR);
	vnic_cleanup_secondary_macs(vn, vn->vn_nhandles);

	if (msa->ms_addrcnt == (uint32_t)-1)
		msa->ms_addrcnt = 0;

	vn->vn_nhandles = msa->ms_addrcnt;

	(void) dls_mgmt_get_linkinfo(vn->vn_id, primary_name, NULL, NULL, NULL);

	/*
	 * Now add the new secondary MACs
	 * Recall that the primary MAC address is the first element.
	 * The secondary clients are named after the primary with their
	 * index to distinguish them.
	 */
	for (i = 1; i <= vn->vn_nhandles; i++) {
		uint8_t *addr;
		mac_diag_t mac_diag;
		char secondary_name[MAXNAMELEN];

		(void) snprintf(secondary_name, sizeof (secondary_name),
		    "%s%02d", primary_name, i);

		err = mac_client_open(vn->vn_lower_mh, &vn->vn_mc_handles[i],
		    secondary_name, MAC_OPEN_FLAGS_IS_VNIC);
		if (err != 0) {
			/* Remove any that we successfully added */
			vnic_cleanup_secondary_macs(vn, --i);
			return (err);
		}

		/*
		 * Assign a MAC address to the VNIC
		 *
		 * Normally this would be done with vnic_unicast_add but since
		 * we know these are fixed adddresses, and since we need to
		 * save this in the proper array slot, we bypass that function
		 * and go direct.
		 */
		addr = msa->ms_addrs[i - 1];
		err = mac_unicast_add(vn->vn_mc_handles[i], addr, 0,
		    &vn->vn_mu_handles[i], vn->vn_vid, &mac_diag);
		if (err != 0) {
			/* Remove any that we successfully added */
			vnic_cleanup_secondary_macs(vn, i);
			return (err);
		}

		/*
		 * Setup the secondary the same way as the primary (i.e.
		 * receiver function/argument (e.g. i_dls_link_rx, mac_pkt_drop,
		 * etc.), the promisc list, and the resource controls).
		 */
		mac_secondary_dup(vn->vn_mc_handles[0], vn->vn_mc_handles[i]);
	}

	return (0);
}

static int
vnic_get_secondary_macs(vnic_t *vn, uint_t pr_valsize, void *pr_val)
{
	int i;
	mac_secondary_addr_t msa;

	if (pr_valsize < sizeof (msa))
		return (EINVAL);

	/* Get existing addresses (primary is at 0) */
	ASSERT(vn->vn_nhandles < MPT_MAXMACADDR);
	for (i = 1; i <= vn->vn_nhandles; i++) {
		ASSERT(vn->vn_mc_handles[i] != NULL);
		mac_unicast_secondary_get(vn->vn_mc_handles[i],
		    msa.ms_addrs[i - 1]);
	}
	msa.ms_addrcnt = vn->vn_nhandles;

	bcopy(&msa, pr_val, sizeof (msa));
	return (0);
}

/*
 * Callback functions for set/get of properties
 */
/*ARGSUSED*/
static int
vnic_m_setprop(void *m_driver, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, const void *pr_val)
{
	int		err = 0;
	vnic_t		*vn = m_driver;

	switch (pr_num) {
	case MAC_PROP_MTU: {
		uint32_t	mtu;

		if (pr_valsize < sizeof (mtu)) {
			err = EINVAL;
			break;
		}
		bcopy(pr_val, &mtu, sizeof (mtu));

		if (vn->vn_link_id == DATALINK_INVALID_LINKID) {
			if (mtu < ANCHOR_VNIC_MIN_MTU ||
			    mtu > ANCHOR_VNIC_MAX_MTU) {
				err = EINVAL;
				break;
			}
		} else {
			err = mac_mtu_add(vn->vn_lower_mh, &mtu, B_FALSE);
			/*
			 * If it's not supported to set a value here, translate
			 * that to EINVAL, so user land gets a better idea of
			 * what went wrong. This realistically means that they
			 * violated the output of prop info.
			 */
			if (err == ENOTSUP)
				err = EINVAL;
			if (err != 0)
				break;
			VERIFY(mac_mtu_remove(vn->vn_lower_mh,
			    vn->vn_mtu) == 0);
		}
		vn->vn_mtu = mtu;
		err = mac_maxsdu_update(vn->vn_mh, mtu);
		break;
	}
	case MAC_PROP_VN_PROMISC_FILTERED: {
		boolean_t filtered;

		if (pr_valsize < sizeof (filtered)) {
			err = EINVAL;
			break;
		}

		bcopy(pr_val, &filtered, sizeof (filtered));
		mac_set_promisc_filtered(vn->vn_mch, filtered);
		break;
	}
	case MAC_PROP_SECONDARY_ADDRS: {
		mac_secondary_addr_t msa;

		bcopy(pr_val, &msa, sizeof (msa));
		err = vnic_set_secondary_macs(vn, &msa);
		break;
	}
	case MAC_PROP_PRIVATE: {
		if (vn->vn_link_id != DATALINK_INVALID_LINKID ||
		    strcmp(pr_name, "_linkstate") != 0) {
			err = ENOTSUP;
			break;
		}

		if (strcmp(pr_val, "up") == 0) {
			vn->vn_ls = LINK_STATE_UP;
		} else if (strcmp(pr_val, "down") == 0) {
			vn->vn_ls = LINK_STATE_DOWN;
		} else if (strcmp(pr_val, "unknown") == 0) {
			vn->vn_ls = LINK_STATE_UNKNOWN;
		} else {
			return (EINVAL);
		}
		mac_link_update(vn->vn_mh, vn->vn_ls);
		break;
	}
	default:
		err = ENOTSUP;
		break;
	}
	return (err);
}

/* ARGSUSED */
static int
vnic_m_getprop(void *arg, const char *pr_name, mac_prop_id_t pr_num,
    uint_t pr_valsize, void *pr_val)
{
	vnic_t		*vn = arg;
	int		ret = 0;
	boolean_t	out;

	switch (pr_num) {
	case MAC_PROP_VN_PROMISC_FILTERED:
		out = mac_get_promisc_filtered(vn->vn_mch);
		ASSERT(pr_valsize >= sizeof (boolean_t));
		bcopy(&out, pr_val, sizeof (boolean_t));
		break;
	case MAC_PROP_SECONDARY_ADDRS:
		ret = vnic_get_secondary_macs(vn, pr_valsize, pr_val);
		break;
	case MAC_PROP_PRIVATE:
		if (vn->vn_link_id != DATALINK_INVALID_LINKID) {
			ret = EINVAL;
			break;
		}

		if (strcmp(pr_name, "_linkstate") != 0) {
			ret = EINVAL;
			break;
		}
		if (vn->vn_ls == LINK_STATE_UP) {
			(void) sprintf(pr_val, "up");
		} else if (vn->vn_ls == LINK_STATE_DOWN) {
			(void) sprintf(pr_val, "down");
		} else {
			(void) sprintf(pr_val, "unknown");
		}
		break;
	default:
		ret = ENOTSUP;
		break;
	}

	return (ret);
}

/* ARGSUSED */
static void
vnic_m_propinfo(void *m_driver, const char *pr_name,
    mac_prop_id_t pr_num, mac_prop_info_handle_t prh)
{
	vnic_t		*vn = m_driver;

	switch (pr_num) {
	case MAC_PROP_MTU:
		if (vn->vn_link_id == DATALINK_INVALID_LINKID) {
			mac_prop_info_set_range_uint32(prh,
			    ANCHOR_VNIC_MIN_MTU, ANCHOR_VNIC_MAX_MTU);
		} else {
			uint32_t		max;
			mac_perim_handle_t	mph;
			mac_propval_range_t	range;

			/*
			 * The valid range for a VNIC's MTU is the minimum that
			 * the device supports and the current value of the
			 * device. A VNIC cannot increase the current MTU of the
			 * device. Therefore we need to get the range from the
			 * propinfo endpoint and current mtu from the
			 * traditional property endpoint.
			 */
			mac_perim_enter_by_mh(vn->vn_lower_mh, &mph);
			if (mac_get_prop(vn->vn_lower_mh, MAC_PROP_MTU, "mtu",
			    &max, sizeof (uint32_t)) != 0) {
				mac_perim_exit(mph);
				return;
			}

			range.mpr_count = 1;
			if (mac_prop_info(vn->vn_lower_mh, MAC_PROP_MTU, "mtu",
			    NULL, 0, &range, NULL) != 0) {
				mac_perim_exit(mph);
				return;
			}

			mac_prop_info_set_default_uint32(prh, max);
			mac_prop_info_set_range_uint32(prh,
			    range.mpr_range_uint32[0].mpur_min, max);
			mac_perim_exit(mph);
		}
		break;
	case MAC_PROP_PRIVATE:
		if (vn->vn_link_id != DATALINK_INVALID_LINKID)
			break;

		if (strcmp(pr_name, "_linkstate") == 0) {
			char buf[16];

			mac_prop_info_set_perm(prh, MAC_PROP_PERM_RW);
			(void) sprintf(buf, "unknown");
			mac_prop_info_set_default_str(prh, buf);
		}
		break;
	default:
		break;
	}
}

/*
 * Callback to receive our own notifications. As this callback is requested
 * first when the vnic is created, it'll be called last by MAC to deliver
 * the notification. Hence we know all our clients have been notified already
 * when we get here.
 */
static void
vnic_notify_cb(void *arg, mac_notify_type_t type)
{
	vnic_t *vnic = arg;

	/*
	 * Ignore notifications if the vnic is not fully initialized or is in
	 * process of being torn down.
	 */
	if (!vnic->vn_enabled)
		return;

	/* Also, these notifications only make sense when we're modifying. */
	if (!vnic->vn_modifying)
		return;

	if (type == MAC_NOTE_REPLUMB) {
		vnic->vn_replumb_done = B_TRUE;
		cv_signal(&vnic->vn_modify_cv);
	}
}

static void
vnic_notify_lower_cb(void *arg, mac_notify_type_t type)
{
	vnic_t *vnic = arg;

	/*
	 * Do not deliver notifications if the vnic is not fully initialized
	 * or is in process of being torn down.
	 */
	if (!vnic->vn_enabled)
		return;

	switch (type) {
	case MAC_NOTE_UNICST:
		/*
		 * Only the VLAN VNIC needs to be notified with primary MAC
		 * address change.
		 */
		if (vnic->vn_addr_type != VNIC_MAC_ADDR_TYPE_PRIMARY)
			return;

		/*  the unicast MAC address value */
		mac_unicast_primary_get(vnic->vn_lower_mh, vnic->vn_addr);

		/* notify its upper layer MAC about MAC address change */
		mac_unicst_update(vnic->vn_mh, (const uint8_t *)vnic->vn_addr);
		break;

	case MAC_NOTE_LINK:
		vnic->vn_ls = mac_client_stat_get(vnic->vn_mch,
		    MAC_STAT_LINK_STATE);
		mac_link_update(vnic->vn_mh, vnic->vn_ls);
		break;

	default:
		break;
	}
}
