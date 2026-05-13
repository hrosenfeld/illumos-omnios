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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2015, Joyent Inc.
 * Copyright 2020 OmniOS Community Edition (OmniOSce) Association.
 * Copyright 2026 Hans Rosenfeld
 */

#include <stdio.h>
#include <sys/debug.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stropts.h>
#include <stdlib.h>
#include <errno.h>
#include <strings.h>
#include <libintl.h>
#include <net/if_types.h>
#include <net/if_dl.h>
#include <sys/dld.h>
#include <sys/vlan.h>
#include <libdladm_impl.h>
#include <libvrrpadm.h>
#include <libdllink.h>
#include <libdlbridge.h>
#include <libdlvnic.h>

/*
 * VNIC administration library.
 */

/*
 * Default random MAC address prefix (locally administered).
 */
static char dladm_vnic_def_prefix[] = {0x02, 0x08, 0x20};

static const char	*dladm_vnic_macaddr2str(const uchar_t *, char *);
static dladm_status_t	dladm_vnic_str2macaddr(const char *, uchar_t *);

/*
 * Convert a diagnostic returned by the kernel into a dladm_status_t.
 */
static dladm_status_t
dladm_vnic_diag2status(vnic_ioc_diag_t ioc_diag)
{
	switch (ioc_diag) {
	case VNIC_IOC_DIAG_NONE:
		return (DLADM_STATUS_OK);
	case VNIC_IOC_DIAG_MACADDRLEN_INVALID:
		return (DLADM_STATUS_INVALIDMACADDRLEN);
	case VNIC_IOC_DIAG_MACADDR_NIC:
		return (DLADM_STATUS_INVALIDMACADDRNIC);
	case VNIC_IOC_DIAG_MACADDR_INUSE:
		return (DLADM_STATUS_INVALIDMACADDRINUSE);
	case VNIC_IOC_DIAG_MACFACTORYSLOTINVALID:
		return (DLADM_STATUS_MACFACTORYSLOTINVALID);
	case VNIC_IOC_DIAG_MACFACTORYSLOTUSED:
		return (DLADM_STATUS_MACFACTORYSLOTUSED);
	case VNIC_IOC_DIAG_MACFACTORYSLOTALLUSED:
		return (DLADM_STATUS_MACFACTORYSLOTALLUSED);
	case VNIC_IOC_DIAG_MACFACTORYNOTSUP:
		return (DLADM_STATUS_MACFACTORYNOTSUP);
	case VNIC_IOC_DIAG_MACPREFIX_INVALID:
		return (DLADM_STATUS_INVALIDMACPREFIX);
	case VNIC_IOC_DIAG_MACPREFIXLEN_INVALID:
		return (DLADM_STATUS_INVALIDMACPREFIXLEN);
	case VNIC_IOC_DIAG_MACMARGIN_INVALID:
		return (DLADM_STATUS_INVALID_MACMARGIN);
	case VNIC_IOC_DIAG_NO_HWRINGS:
		return (DLADM_STATUS_NO_HWRINGS);
	case VNIC_IOC_DIAG_MACADDR_INVALID:
		return (DLADM_STATUS_INVALIDMACADDR);
	case VNIC_IOC_DIAG_MACMTU_INVALID:
		return (DLADM_STATUS_INVALID_MTU);
	default:
		return (DLADM_STATUS_FAILED);
	}
}

/*
 * Send an ioctl command to the VNIC driver.
 */
static dladm_status_t
i_dladm_vnic_ioctl(dladm_handle_t handle, int cmd, dladm_vnic_attr_t *attrp,
    boolean_t is_etherstub)
{
	int rc;
	vnic_ioc_t ioc;
	dladm_status_t status = DLADM_STATUS_OK;

	bzero(&ioc, sizeof (ioc));
	ioc.vi_vnic_id = attrp->va_vnic_id;
	ioc.vi_link_id = attrp->va_link_id;
	ioc.vi_mac_addr_type = attrp->va_mac_addr_type;
	ioc.vi_mac_len = attrp->va_mac_len;
	ioc.vi_mac_slot = attrp->va_mac_slot;
	ioc.vi_mac_prefix_len = attrp->va_mac_prefix_len;
	ioc.vi_vid = attrp->va_vid;
	ioc.vi_vrid = attrp->va_vrid;
	ioc.vi_af = attrp->va_af;
	ioc.vi_force = attrp->va_force;

	if (attrp->va_mac_len > 0 || ioc.vi_mac_prefix_len > 0) {
		CTASSERT(sizeof (attrp->va_mac_addr) == MAXMACADDRLEN);
		CTASSERT(sizeof (ioc.vi_mac_addr) == MAXMACADDRLEN);
		bcopy(attrp->va_mac_addr, ioc.vi_mac_addr, MAXMACADDRLEN);
	}

	bcopy(&attrp->va_resource_props, &ioc.vi_resource_props,
	    sizeof (mac_resource_props_t));

	if (is_etherstub)
		ioc.vi_flags |= VNIC_IOC_FLAGS_ANCHOR;

	rc = ioctl(dladm_dld_fd(handle), cmd, &ioc);
	if (rc < 0)
		status = dladm_errno2status(errno);

	if (status != DLADM_STATUS_OK) {
		if (ioc.vi_diag != VNIC_IOC_DIAG_NONE)
			status = dladm_vnic_diag2status(ioc.vi_diag);
		return (status);
	}

	attrp->va_vnic_id = ioc.vi_vnic_id;
	attrp->va_link_id = ioc.vi_link_id;
	attrp->va_mac_addr_type = ioc.vi_mac_addr_type;
	bcopy(ioc.vi_mac_addr, attrp->va_mac_addr, MAXMACADDRLEN);
	attrp->va_mac_len = ioc.vi_mac_len;
	attrp->va_mac_slot = ioc.vi_mac_slot;
	attrp->va_mac_prefix_len = ioc.vi_mac_prefix_len;
	attrp->va_vid = ioc.vi_vid;
	attrp->va_vrid = ioc.vi_vrid;
	attrp->va_af = ioc.vi_af;
	attrp->va_force = ioc.vi_force;

	bcopy(&ioc.vi_resource_props, &attrp->va_resource_props,
	    sizeof (mac_resource_props_t));

	return (status);
}

/*
 * Get the configuration information of the given VNIC.
 */
static dladm_status_t
i_dladm_vnic_info_active(dladm_handle_t handle, datalink_id_t linkid,
    dladm_vnic_attr_t *attrp)
{
	attrp->va_vnic_id = linkid;

	return (i_dladm_vnic_ioctl(handle, VNIC_IOC_INFO, attrp, B_FALSE));
}

static dladm_status_t
i_dladm_vnic_info_persist(dladm_handle_t handle, datalink_id_t linkid,
    dladm_vnic_attr_t *attrp)
{
	dladm_conf_t conf;
	dladm_status_t status;
	char macstr[ETHERADDRL * 3];
	char linkover[MAXLINKNAMELEN];
	uint64_t u64;
	datalink_class_t class;

	attrp->va_vnic_id = linkid;
	if ((status = dladm_getsnap_conf(handle, linkid, &conf)) !=
	    DLADM_STATUS_OK)
		return (status);

	status = dladm_get_conf_field(handle, conf, FLINKOVER, linkover,
	    sizeof (linkover));
	if (status != DLADM_STATUS_OK) {
		/*
		 * This isn't an error, etherstubs don't have a FLINKOVER
		 * property.
		 */
		attrp->va_link_id = DATALINK_INVALID_LINKID;
	} else {
		if ((status = dladm_name2info(handle, linkover,
		    &attrp->va_link_id, NULL, NULL, NULL)) != DLADM_STATUS_OK)
			goto done;
	}

	if ((status = dladm_datalink_id2info(handle, linkid, NULL, &class,
	    NULL, NULL, 0)) != DLADM_STATUS_OK)
		goto done;

	if (class == DATALINK_CLASS_VLAN) {
		if (attrp->va_link_id == DATALINK_INVALID_LINKID) {
			status = DLADM_STATUS_BADARG;
			goto done;
		}
		attrp->va_mac_addr_type = VNIC_MAC_ADDR_TYPE_PRIMARY;
		attrp->va_mac_len = 0;
	} else {
		status = dladm_get_conf_field(handle, conf, FMADDRTYPE, &u64,
		    sizeof (u64));
		if (status != DLADM_STATUS_OK)
			goto done;

		attrp->va_mac_addr_type = (vnic_mac_addr_type_t)u64;

		if ((status = dladm_get_conf_field(handle, conf, FVRID,
		    &u64, sizeof (u64))) != DLADM_STATUS_OK) {
			attrp->va_vrid = VRRP_VRID_NONE;
		} else {
			attrp->va_vrid = (vrid_t)u64;
		}

		if ((status = dladm_get_conf_field(handle, conf, FVRAF,
		    &u64, sizeof (u64))) != DLADM_STATUS_OK) {
			attrp->va_af = AF_UNSPEC;
		} else {
			attrp->va_af = (int)u64;
		}

		status = dladm_get_conf_field(handle, conf, FMADDRLEN, &u64,
		    sizeof (u64));
		attrp->va_mac_len = ((status == DLADM_STATUS_OK) ?
		    (uint_t)u64 : ETHERADDRL);

		status = dladm_get_conf_field(handle, conf, FMADDRSLOT, &u64,
		    sizeof (u64));
		attrp->va_mac_slot = ((status == DLADM_STATUS_OK) ?
		    (int)u64 : -1);

		status = dladm_get_conf_field(handle, conf, FMADDRPREFIXLEN,
		    &u64, sizeof (u64));
		attrp->va_mac_prefix_len = ((status == DLADM_STATUS_OK) ?
		    (uint_t)u64 : sizeof (dladm_vnic_def_prefix));

		status = dladm_get_conf_field(handle, conf, FMACADDR, macstr,
		    sizeof (macstr));
		if (status != DLADM_STATUS_OK)
			goto done;

		status = dladm_vnic_str2macaddr(macstr, attrp->va_mac_addr);
		if (status != DLADM_STATUS_OK)
			goto done;
	}

	status = dladm_get_conf_field(handle, conf, FVLANID, &u64,
	    sizeof (u64));
	attrp->va_vid = ((status == DLADM_STATUS_OK) ?
	    (uint16_t)u64 : VLAN_ID_UNTAGGED);

	status = DLADM_STATUS_OK;
done:
	dladm_destroy_conf(handle, conf);
	return (status);
}

dladm_status_t
dladm_vnic_info(dladm_handle_t handle, datalink_id_t linkid,
    dladm_vnic_attr_t *attrp, uint32_t flags)
{
	if (flags == DLADM_OPT_ACTIVE)
		return (i_dladm_vnic_info_active(handle, linkid, attrp));
	else if (flags == DLADM_OPT_PERSIST)
		return (i_dladm_vnic_info_persist(handle, linkid, attrp));
	else
		return (DLADM_STATUS_BADARG);
}

/*
 * Remove a VNIC from the kernel.
 */
static dladm_status_t
i_dladm_vnic_delete_sys(dladm_handle_t handle, datalink_id_t linkid)
{
	vnic_ioc_t ioc;

	bzero(&ioc, sizeof (ioc));
	ioc.vi_vnic_id = linkid;

	return (ioctl(dladm_dld_fd(handle), VNIC_IOC_DELETE, &ioc));
}

/*
 * Convert between MAC address types and their string representations.
 */

typedef struct dladm_vnic_addr_type_s {
	const char		*va_str;
	vnic_mac_addr_type_t	va_type;
} dladm_vnic_addr_type_t;

static dladm_vnic_addr_type_t addr_types[] = {
	{"fixed", VNIC_MAC_ADDR_TYPE_FIXED},
	{"random", VNIC_MAC_ADDR_TYPE_RANDOM},
	{"factory", VNIC_MAC_ADDR_TYPE_FACTORY},
	{"auto", VNIC_MAC_ADDR_TYPE_AUTO},
	{"fixed", VNIC_MAC_ADDR_TYPE_PRIMARY},
	{"vrrp", VNIC_MAC_ADDR_TYPE_VRID}
};

#define	NADDR_TYPES (sizeof (addr_types) / sizeof (dladm_vnic_addr_type_t))

static const char *
dladm_vnic_macaddrtype2str(vnic_mac_addr_type_t type)
{
	uint_t i;

	for (i = 0; i < NADDR_TYPES; i++) {
		if (type == addr_types[i].va_type)
			return (addr_types[i].va_str);
	}
	return (NULL);
}

dladm_status_t
dladm_vnic_str2macaddrtype(const char *str, vnic_mac_addr_type_t *val)
{
	uint_t i;
	dladm_vnic_addr_type_t *type;

	for (i = 0; i < NADDR_TYPES; i++) {
		type = &addr_types[i];
		if (strncmp(str, type->va_str, strlen(type->va_str)) == 0) {
			*val = type->va_type;
			return (DLADM_STATUS_OK);
		}
	}
	return (DLADM_STATUS_BADARG);
}

static dladm_status_t
i_dladm_vnic_attr2conf(dladm_handle_t handle, dladm_vnic_attr_t *attrp,
    dladm_conf_t conf, datalink_class_t class)
{
	dladm_status_t status;
	uint64_t u64;

	if (attrp->va_link_id != DATALINK_INVALID_LINKID) {
		char linkover[MAXLINKNAMELEN];

		status = dladm_datalink_id2info(handle, attrp->va_link_id, NULL,
		    NULL, NULL, linkover, sizeof (linkover));
		if (status != DLADM_STATUS_OK)
			return (status);

		status = dladm_set_conf_field(handle, conf, FLINKOVER,
		    DLADM_TYPE_STR, linkover);
		if (status != DLADM_STATUS_OK)
			return (status);
	}

	if (class != DATALINK_CLASS_VLAN &&
	    attrp->va_mac_addr_type != VNIC_MAC_ADDR_TYPE_UNKNOWN) {
		char macstr[ETHERADDRL * 3];

		u64 = attrp->va_mac_addr_type;
		status = dladm_set_conf_field(handle, conf, FMADDRTYPE,
		    DLADM_TYPE_UINT64, &u64);
		if (status != DLADM_STATUS_OK)
			return (status);

		u64 = attrp->va_vrid;
		status = dladm_set_conf_field(handle, conf, FVRID,
		    DLADM_TYPE_UINT64, &u64);
		if (status != DLADM_STATUS_OK)
			return (status);

		u64 = attrp->va_af;
		status = dladm_set_conf_field(handle, conf, FVRAF,
		    DLADM_TYPE_UINT64, &u64);
		if (status != DLADM_STATUS_OK)
			return (status);

		if (attrp->va_mac_len != ETHERADDRL) {
			u64 = attrp->va_mac_len;
			status = dladm_set_conf_field(handle, conf, FMADDRLEN,
			    DLADM_TYPE_UINT64, &u64);
			if (status != DLADM_STATUS_OK)
				return (status);
		}

		if (attrp->va_mac_slot != -1) {
			u64 = attrp->va_mac_slot;
			status = dladm_set_conf_field(handle, conf,
			    FMADDRSLOT, DLADM_TYPE_UINT64, &u64);
			if (status != DLADM_STATUS_OK)
				return (status);
		}

		if (attrp->va_mac_prefix_len !=
		    sizeof (dladm_vnic_def_prefix)) {
			u64 = attrp->va_mac_prefix_len;
			status = dladm_set_conf_field(handle, conf,
			    FMADDRPREFIXLEN, DLADM_TYPE_UINT64, &u64);
			if (status != DLADM_STATUS_OK)
				return (status);
		}

		(void) dladm_vnic_macaddr2str(attrp->va_mac_addr, macstr);
		status = dladm_set_conf_field(handle, conf, FMACADDR,
		    DLADM_TYPE_STR, macstr);
		if (status != DLADM_STATUS_OK)
			return (status);
	}

	if (attrp->va_vid != VLAN_ID_NONE) {
		u64 = attrp->va_vid;
		status = dladm_set_conf_field(handle, conf, FVLANID,
		    DLADM_TYPE_UINT64, &u64);
		if (status != DLADM_STATUS_OK)
			return (status);
	}

	return (DLADM_STATUS_OK);
}

/*
 * Based on the VRRP specification, the virtual router MAC address associated
 * with a virtual router is an IEEE 802 MAC address in the following format:
 *
 * IPv4 case: 00-00-5E-00-01-{VRID} (in hex in internet standard bit-order)
 *
 * IPv6 case: 00-00-5E-00-02-{VRID} (in hex in internet standard bit-order)
 */
static dladm_status_t
i_dladm_vnic_vrrp_mac(vrid_t vrid, int af, uint8_t *mac, uint_t maclen)
{
	if (maclen < ETHERADDRL || vrid < VRRP_VRID_MIN ||
	    vrid > VRRP_VRID_MAX || (af != AF_INET && af != AF_INET6)) {
		return (DLADM_STATUS_BADARG);
	}

	mac[0] = mac[1] = mac[3] = 0x0;
	mac[2] = 0x5e;
	mac[4] = (af == AF_INET) ? 0x01 : 0x02;
	mac[5] = vrid;
	return (DLADM_STATUS_OK);
}

/*
 * Perform all the heavy lifting involved with creating a new VNIC / VLAN.
 * Update the configuration file and bring it up.
 *
 * The "vrid" and "af" arguments are only required if the mac_addr_type is
 * VNIC_MAC_ADDR_TYPE_VRID. In that case, the MAC address will be caculated
 * by i_dladm_vnic_vrrp_mac() above.
 */
static dladm_status_t
i_dladm_vnic_common(dladm_handle_t handle, int cmd, dladm_vnic_attr_t *attrp,
    uint32_t class, dladm_arg_list_t *proplist, dladm_errlist_t *errs,
    uint32_t flags, uint32_t *pclassp, dladm_conf_t conf)
{
	datalink_class_t pclass;
	uint32_t media = DL_ETHER;
	uint32_t link_flags;
	dladm_status_t status;
	boolean_t is_etherstub;

	/*
	 * Sanity test arguments.
	 */
	if ((flags & DLADM_OPT_ACTIVE) == 0)
		return (DLADM_STATUS_NOTSUP);

	is_etherstub = (flags & DLADM_OPT_ANCHOR) != 0;

	/*
	 * Make sure the link_id is sensible if it was given.
	 */
	if (attrp->va_link_id != DATALINK_INVALID_LINKID) {
		if ((status = dladm_datalink_id2info(handle, attrp->va_link_id,
		    &link_flags, &pclass, &media, NULL, 0)) != DLADM_STATUS_OK)
			return (status);

		/* Disallow persistent objects on top of temporary ones */
		if ((flags & DLADM_OPT_PERSIST) != 0 &&
		    (link_flags & DLMGMT_PERSIST) == 0)
			return (DLADM_STATUS_PERSIST_ON_TEMP);

		/* Links cannot be created on top of these object types */
		if (pclass == DATALINK_CLASS_VNIC ||
		    pclass == DATALINK_CLASS_VLAN)
			return (DLADM_STATUS_BADARG);

		if (pclassp != NULL)
			*pclassp = pclass;
	}

	/*
	 * Only VRRP VNIC need VRID and address family specified.
	 */
	if (attrp->va_mac_addr_type != VNIC_MAC_ADDR_TYPE_VRID &&
	    (attrp->va_af != AF_UNSPEC || attrp->va_vrid != VRRP_VRID_NONE)) {
		return (DLADM_STATUS_BADARG);
	}

	/*
	 * If a random address might be generated, but no prefix
	 * was specified by the caller, use the default MAC address
	 * prefix.
	 */
	if ((attrp->va_mac_addr_type == VNIC_MAC_ADDR_TYPE_RANDOM ||
	    attrp->va_mac_addr_type == VNIC_MAC_ADDR_TYPE_AUTO) &&
	    attrp->va_mac_prefix_len == 0) {
		attrp->va_mac_prefix_len = sizeof (dladm_vnic_def_prefix);
		bcopy(dladm_vnic_def_prefix, attrp->va_mac_addr,
		    attrp->va_mac_prefix_len);
	}

	/*
	 * If this is a VRRP VNIC, generate its MAC address using the given
	 * VRID and address family.
	 */
	if (attrp->va_mac_addr_type == VNIC_MAC_ADDR_TYPE_VRID) {
		/*
		 * VRRP VNICs must be created over ethernet data-links.
		 */
		if (attrp->va_vrid < VRRP_VRID_MIN ||
		    attrp->va_vrid > VRRP_VRID_MAX ||
		    (attrp->va_af != AF_INET && attrp->va_af != AF_INET6) ||
		    attrp->va_mac_len != 0 || attrp->va_mac_prefix_len != 0 ||
		    attrp->va_mac_slot != -1 ||
		    is_etherstub || media != DL_ETHER) {
			return (DLADM_STATUS_BADARG);
		}
		attrp->va_mac_len = ETHERADDRL;
		status = i_dladm_vnic_vrrp_mac(attrp->va_vrid, attrp->va_af,
		    attrp->va_mac_addr, attrp->va_mac_len);
		if (status != DLADM_STATUS_OK)
			return (status);
	}

	if (attrp->va_mac_len > MAXMACADDRLEN)
		return (DLADM_STATUS_INVALIDMACADDRLEN);

	/* Extract resource_ctl and cpu_list from proplist */
	if (proplist != NULL) {
		status = dladm_link_proplist_extract(handle, proplist,
		    &attrp->va_resource_props, 0);
		if (status != DLADM_STATUS_OK)
			return (status);
	}

	attrp->va_force = (flags & DLADM_OPT_FORCE) != 0;

	status = i_dladm_vnic_ioctl(handle, cmd, attrp, is_etherstub);
	if (status != DLADM_STATUS_OK)
		return (status);

	/* Save vnic configuration and its properties */
	if (!(flags & DLADM_OPT_PERSIST))
		return (status);

	/*
	 * If anything fails here, we must cleanup the vnic we've created,
	 */

	status = i_dladm_vnic_attr2conf(handle, attrp, conf, class);
	if (status != DLADM_STATUS_OK)
		goto out;

	status = dladm_write_conf(handle, conf);
	if (status != DLADM_STATUS_OK)
		goto out;

	if (proplist == NULL)
		return (DLADM_STATUS_OK);

	for (int i = 0; i < proplist->al_count; i++) {
		dladm_arg_info_t *aip = &proplist->al_info[i];

		status = dladm_set_linkprop(handle, attrp->va_vnic_id,
		    aip->ai_name, aip->ai_val, aip->ai_count,
		    DLADM_OPT_PERSIST);
		if (status != DLADM_STATUS_OK) {
			char	errmsg[DLADM_STRSIZE];

			(void) dladm_errlist_append(errs,
			    "failed to set property %s: %s",
			    aip->ai_name, dladm_status2str(status, errmsg));

			goto out;
		}
	}

out:
	if (status != DLADM_STATUS_OK) {
		(void) i_dladm_vnic_delete_sys(handle, attrp->va_vnic_id);
		(void) dladm_destroy_datalink_id(handle, attrp->va_vnic_id,
		    flags);
	}

	return (status);
}

/*
 * Create a new VNIC / VLAN.
 */
dladm_status_t
dladm_vnic_create(dladm_handle_t handle, const char *vnic,
    dladm_vnic_attr_t *attrp, dladm_arg_list_t *proplist,
    dladm_errlist_t *errs, uint32_t flags)
{
	dladm_conf_t conf = { 0 };
	char name[MAXLINKNAMELEN];
	datalink_class_t class, pclass;
	dladm_status_t status;
	boolean_t is_vlan;
	boolean_t is_etherstub;
	boolean_t vnic_created = B_FALSE;
	boolean_t conf_created = B_FALSE;

	/*
	 * If it is an anchor VNIC, va_link_id must be set to
	 * DATALINK_INVALID_LINKID and the VLAN id must be 0.
	 */
	if ((flags & DLADM_OPT_ANCHOR) != 0 &&
	    (attrp->va_link_id != DATALINK_INVALID_LINKID ||
	    attrp->va_vid != VLAN_ID_UNTAGGED)) {
		return (DLADM_STATUS_BADARG);
	}

	is_vlan = ((flags & DLADM_OPT_VLAN) != 0);
	if ((attrp->va_vid < VLAN_ID_MIN || attrp->va_vid > VLAN_ID_MAX) &&
	    (is_vlan || attrp->va_vid != VLAN_ID_UNTAGGED))
		return (DLADM_STATUS_VIDINVAL);

	is_etherstub = (flags & DLADM_OPT_ANCHOR) != 0;

	if (dladm_vnic_macaddrtype2str(attrp->va_mac_addr_type) == NULL)
		return (DLADM_STATUS_INVALIDMACADDRTYPE);

	if (vnic == NULL) {
		flags |= DLADM_OPT_PREFIX;
		(void) strlcpy(name, "vnic", sizeof (name));
	} else {
		(void) strlcpy(name, vnic, sizeof (name));
	}

	class = is_vlan ? DATALINK_CLASS_VLAN :
	    (is_etherstub ? DATALINK_CLASS_ETHERSTUB : DATALINK_CLASS_VNIC);
	status = dladm_create_datalink_id(handle, name, class, DL_ETHER, flags,
	    &attrp->va_vnic_id);
	if (status != DLADM_STATUS_OK)
		return (status);

	if ((flags & DLADM_OPT_PREFIX) != 0) {
		(void) snprintf(name + 4, sizeof (name), "%llu",
		    attrp->va_vnic_id);
		flags &= ~DLADM_OPT_PREFIX;
	}

	/* Save vnic configuration and its properties */
	if ((flags & DLADM_OPT_PERSIST) != 0) {
		status = dladm_create_conf(handle, name, attrp->va_vnic_id,
		    class, DL_ETHER, &conf);
		if (status != DLADM_STATUS_OK)
			goto done;
		conf_created = B_TRUE;
	}

	status = i_dladm_vnic_common(handle, VNIC_IOC_CREATE, attrp, class,
	    proplist, errs, flags, &pclass, conf);
	if (status != DLADM_STATUS_OK) {
		if (!is_etherstub && pclass == DATALINK_CLASS_OVERLAY &&
		    status == DLADM_STATUS_ADDRNOTAVAIL) {
			char errmsg[DLADM_STRSIZE];

			(void) dladm_errlist_append(errs,
			    "failed to start overlay device; "
			    "could not open underlay socket: %s",
			    dladm_status2str(status, errmsg));
		}
		goto done;
	}
	vnic_created = B_TRUE;


done:
	if (conf_created)
		dladm_destroy_conf(handle, conf);

	if (status != DLADM_STATUS_OK) {
		if (conf_created)
			(void) dladm_remove_conf(handle, attrp->va_vnic_id);

		if (vnic_created)
			(void) i_dladm_vnic_delete_sys(handle,
			    attrp->va_vnic_id);
		(void) dladm_destroy_datalink_id(handle, attrp->va_vnic_id,
		    flags);
	}

	if (is_vlan) {
		dladm_status_t stat2;

		stat2 = dladm_bridge_refresh(handle, attrp->va_link_id);
		if (status == DLADM_STATUS_OK && stat2 != DLADM_STATUS_OK)
			status = stat2;
	}
	return (status);
}

/*
 * Delete a VNIC / VLAN.
 */
dladm_status_t
dladm_vnic_delete(dladm_handle_t handle, datalink_id_t linkid, uint32_t flags)
{
	dladm_status_t status;
	datalink_class_t class;

	if (flags == 0)
		return (DLADM_STATUS_BADARG);

	if ((dladm_datalink_id2info(handle, linkid, NULL, &class, NULL, NULL, 0)
	    != DLADM_STATUS_OK))
		return (DLADM_STATUS_BADARG);

	if ((flags & DLADM_OPT_VLAN) != 0) {
		if (class != DATALINK_CLASS_VLAN)
			return (DLADM_STATUS_BADARG);
	} else {
		if (class != DATALINK_CLASS_VNIC &&
		    class != DATALINK_CLASS_ETHERSTUB)
			return (DLADM_STATUS_BADARG);
	}

	if ((flags & DLADM_OPT_ACTIVE) != 0) {
		status = i_dladm_vnic_delete_sys(handle, linkid);
		if (status == DLADM_STATUS_OK) {
			(void) dladm_set_linkprop(handle, linkid, NULL, NULL, 0,
			    DLADM_OPT_ACTIVE);
			(void) dladm_destroy_datalink_id(handle, linkid,
			    DLADM_OPT_ACTIVE);
		} else if (status != DLADM_STATUS_NOTFOUND ||
		    !(flags & DLADM_OPT_PERSIST)) {
			return (status);
		}
	}
	if ((flags & DLADM_OPT_PERSIST) != 0) {
		(void) dladm_remove_conf(handle, linkid);
		(void) dladm_destroy_datalink_id(handle, linkid,
		    DLADM_OPT_PERSIST);
	}
	return (dladm_bridge_refresh(handle, linkid));
}

static const char *
dladm_vnic_macaddr2str(const uchar_t *mac, char *buf)
{
	static char unknown_mac[] = {0, 0, 0, 0, 0, 0};

	if (buf == NULL)
		return (NULL);

	if (bcmp(unknown_mac, mac, ETHERADDRL) == 0)
		(void) strlcpy(buf, "unknown", DLADM_STRSIZE);
	else
		return (_link_ntoa(mac, buf, ETHERADDRL, IFT_OTHER));

	return (buf);
}

static dladm_status_t
dladm_vnic_str2macaddr(const char *str, uchar_t *buf)
{
	int len = 0;
	uchar_t *b = _link_aton(str, &len);

	if (b == NULL || len >= MAXMACADDRLEN)
		return (DLADM_STATUS_BADARG);

	bcopy(b, buf, len);
	free(b);
	return (DLADM_STATUS_OK);
}

typedef struct dladm_vnic_up_arg_s {
	datalink_class_t vua_class;
	dladm_status_t vua_status;
} dladm_vnic_up_arg_t;

static int
i_dladm_vnic_up(dladm_handle_t handle, datalink_id_t linkid, void *arg)
{
	dladm_vnic_up_arg_t *vua = arg;
	dladm_vnic_attr_t attr;
	dladm_status_t status;
	dladm_arg_list_t *proplist;
	boolean_t is_etherstub;

	is_etherstub = (vua->vua_class == DATALINK_CLASS_ETHERSTUB);

	bzero(&attr, sizeof (attr));

	status = dladm_vnic_info(handle, linkid, &attr, DLADM_OPT_PERSIST);
	if (status != DLADM_STATUS_OK)
		goto done;

	/* Get all properties for this vnic */
	status = dladm_link_get_proplist(handle, linkid, &proplist);
	if (status != DLADM_STATUS_OK)
		goto done;

	if (proplist != NULL) {
		status = dladm_link_proplist_extract(handle, proplist,
		    &attr.va_resource_props, DLADM_OPT_BOOT);
	}

	status = i_dladm_vnic_ioctl(handle, VNIC_IOC_CREATE, &attr,
	    is_etherstub);
	if (status == DLADM_STATUS_OK) {
		status = dladm_up_datalink_id(handle, linkid);
		if (status != DLADM_STATUS_OK)
			(void) i_dladm_vnic_delete_sys(handle, linkid);
	}

done:
	vua->vua_status = status;
	return (DLADM_WALK_CONTINUE);
}

dladm_status_t
dladm_vnic_up(dladm_handle_t handle, datalink_id_t linkid, uint32_t flags)
{
	dladm_vnic_up_arg_t vua;

	vua.vua_class = ((flags & DLADM_OPT_VLAN) != 0) ? DATALINK_CLASS_VLAN :
	    (DATALINK_CLASS_VNIC | DATALINK_CLASS_ETHERSTUB);
	vua.vua_status = DLADM_STATUS_OK;

	if (linkid == DATALINK_ALL_LINKID) {
		(void) dladm_walk_datalink_id(i_dladm_vnic_up, handle, &vua,
		    vua.vua_class, DATALINK_ANY_MEDIATYPE, DLADM_OPT_PERSIST);
		return (DLADM_STATUS_OK);
	} else {
		(void) i_dladm_vnic_up(handle, linkid, &vua);
		return (vua.vua_status);
	}
}
