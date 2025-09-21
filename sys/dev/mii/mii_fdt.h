/*-
 * Copyright (c) 2017 Ian Lepore <ian@freebsd.org>
 * All rights reserved.
 *
 * Development sponsored by Microsemi, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _DEV_MII_FDT_H_
#define	_DEV_MII_FDT_H_

/*
 * Common FDT config for a PHY, as documented in the devicetree bindings
 * documents ethernet.txt and phy.txt.  Boolean properties are represented as
 * bits in the flags member.
 */
struct mii_fdt_phy_config {
	phandle_t	macnode;	/* Node (not xref) of parent MAC */
	phandle_t	phynode;	/* Node (not xref) of PHY */
	mii_contype_t	con_type;	/* MAC<->PHY connection type */
	u_int		max_speed;	/* Mbits/sec, 0 = not specified */
	uint32_t	flags;		/* MIIF_FDT_xxx boolean properties */
};
typedef struct mii_fdt_phy_config mii_fdt_phy_config_t;

/* PHY config flags. */
#define	MIIF_FDT_COMPAT_CLAUSE45	0x0001
#define	MIIF_FDT_BROKEN_TURNAROUND	0x0002
#define	MIIF_FDT_LANE_SWAP		0x0004
#define	MIIF_FDT_NO_LANE_SWAP		0x0008
#define	MIIF_FDT_EEE_BROKEN_100TX	0x0010
#define	MIIF_FDT_EEE_BROKEN_1000T	0x0020
#define	MIIF_FDT_EEE_BROKEN_10GT	0x0040
#define	MIIF_FDT_EEE_BROKEN_1000KX	0x0080
#define	MIIF_FDT_EEE_BROKEN_10GKX4	0x0100
#define	MIIF_FDT_EEE_BROKEN_10GKR	0x0200

/*
 * Convert between mii_contype enums and devicetree property strings.
 */
const char *mii_fdt_contype_to_name(mii_contype_t contype);
mii_contype_t mii_fdt_contype_from_name(const char *name);

/* Get the connection type from the given MAC node. */
mii_contype_t mii_fdt_get_contype(phandle_t macnode);

/*
 * Get/free the config for the given PHY device.
 */
void mii_fdt_free_config(struct mii_fdt_phy_config *cfg);
mii_fdt_phy_config_t *mii_fdt_get_config(device_t phydev);

/*
 * A parsed "fixed-link" binding.  Both the subnode form and the deprecated
 * five cell property form are represented here.
 */
struct mii_fixed_link {
	u_int	fl_speed;	/* Mbit/s: 10, 100, 1000, 2500, 5000, 10000 */
	bool	fl_fdx;		/* "full-duplex" present */
	bool	fl_pause;	/* "pause" present */
	bool	fl_asym_pause;	/* "asym-pause" present */
	bool	fl_legacy;	/* came from the property form */
};

/*
 * Parse the "fixed-link" binding of a MAC node.  Returns 0 on success,
 * ENOENT if the node has no fixed link at all, EINVAL if it has a malformed
 * one.
 */
int mii_fdt_get_fixed_link(phandle_t macnode, struct mii_fixed_link *fl);

/* Convert a parsed fixed link to an IFM_ETHER media word. */
int mii_fdt_fixed_link_media(const struct mii_fixed_link *fl, u_int *mediap);

/*
 * Attach a fixed PHY described by the "fixed-link" binding of the given
 * device.  Returns ENOENT if there is no fixed link, in which case the
 * caller should fall back to mii_attach().
 */
int mii_fdt_attach_fixed(device_t dev, device_t *miibus, if_t ifp,
    ifm_change_cb_t ifmedia_upd, ifm_stat_cb_t ifmedia_sts, int flags);

#endif
