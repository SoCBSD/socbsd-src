/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016 Stanislav Galabov.
 * Copyright (c) 2023 Priit Trees.
 * Copyright (c) 2025 Martin Filla <freebsd@sysctl.cz>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef	__MTKSWITCH_MT7531_H__
#define	__MTKSWITCH_MT7531_H__

#define	MTKSWITCH_ATC	0x0080
#define		ATC_BUSY		(1u<<15)
#define		ATC_AC_MAT_NON_STATIC_MACS	(4u<<8)
#define		ATC_AC_CMD_CLEAN	(2u<<0)

#define	MTKSWITCH_VTCR	0x0090
#define		VTCR_BUSY		(1u<<31)
#define		VTCR_FUNC_VID_READ	(0u<<12)
#define		VTCR_FUNC_VID_WRITE	(1u<<12)
#define		VTCR_FUNC_VID_INVALID	(2u<<12)
#define		VTCR_FUNC_VID_VALID	(3u<<12)
#define		VTCR_IDX_INVALID	(1u<<16)
#define		VTCR_VID_MASK		0xfff

#define	MTKSWITCH_VAWD1	0x0094
#define		VAWD1_IVL_MAC		(1u<<30)
#define		VAWD1_VTAG_EN		(1u<<28)
#define		VAWD1_PORT_MEMBER(p)	((1u<<16)<<(p))
#define		VAWD1_MEMBER_OFF	16
#define		VAWD1_MEMBER_MASK	0xff
#define		VAWD1_FID_OFFSET	1
#define		VAWD1_VALID		(1u<<0)

#define	MTKSWITCH_VAWD2	0x0098
#define		VAWD2_PORT_UNTAGGED(p)	(0u<<((p)*2))
#define		VAWD2_PORT_TAGGED(p)	(2u<<((p)*2))
#define		VAWD2_PORT_MASK(p)	(3u<<((p)*2))

/*
 * Switch system control, module SYS at base 0x7000.  [DS 8.4, p.732]
 */
#define	MTKSWITCH_SYS_CTRL	0x7000
#define		SYS_CTRL_ACL_TAB_INIT	(1u<<22)
#define		SYS_CTRL_MAC_TAB_INIT	(1u<<21)
#define		SYS_CTRL_VLAN_TAB_INIT	(1u<<20)
#define		SYS_CTRL_BMU_MEM_INIT	(1u<<16)
#define		SYS_CTRL_TAB_INIT_DONE	(SYS_CTRL_ACL_TAB_INIT |	\
			    SYS_CTRL_MAC_TAB_INIT | SYS_CTRL_VLAN_TAB_INIT | \
			    SYS_CTRL_BMU_MEM_INIT)
#define		SYS_CTRL_SW_SYS_RST	(1u<<1)
#define		SYS_CTRL_SW_REG_RST	(1u<<0)

/*
 * Interrupt enable and status.  Only the per-PHY link change bits are of
 * interest here: the switch has no interrupt line wired up on the boards
 * we support, but polling the single status register is a great deal
 * cheaper than reading the status of every port.  [DS 8.4, p.736-739]
 */
#define	MTKSWITCH_SYS_INT_EN	0x7008
#define	MTKSWITCH_SYS_INT_STS	0x700c
#define		SYS_INT_PHY_LC(p)	(1u<<(p))
#define		SYS_INT_PHY_LC_ALL	0x7f

/*
 * PHY polling and SMI master control.  PHY_AP_EN decides, per port,
 * whether the status registers below are fed by the switch's own MDIO
 * master polling the PHY or by side band signals; it is clear out of
 * reset, and the status registers stay empty until it is set.
 * [DS 8.4, p.741-743]
 */
#define	MTKSWITCH_PHY_POLL	0x7018
#define		PHY_POLL_AP_EN(p)	(1u<<(24 + (p)))
#define		PHY_POLL_AP_EN_MASK	(0x7fu<<24)

/*
 * Unlike MT7620/MT7621, the PHY indirect access control register lives
 * in the switch's own register space.  [DS 8.4, p.743-744]
 */
#define	MTKSWITCH_PIAC	0x701c
#define		PIAC_PHY_ACS_ST		(1u<<31)
#define		PIAC_MDIO_REG_ADDR_OFF	25
#define		PIAC_MDIO_PHY_ADDR_OFF	20
#define		PIAC_MDIO_CMD_MASK	(3u<<18)
#define		PIAC_MDIO_CMD_ADDR	(0u<<18)	/* Clause 45 only. */
#define		PIAC_MDIO_CMD_WRITE	(1u<<18)
#define		PIAC_MDIO_CMD_READ	(2u<<18)
#define		PIAC_MDIO_CMD_READ_C45	(3u<<18)
#define		PIAC_MDIO_ST_MASK	(3u<<16)
#define		PIAC_MDIO_ST_C45	(0u<<16)
#define		PIAC_MDIO_ST		(1u<<16)	/* Clause 22. */
#define		PIAC_MDIO_RW_DATA_MASK	0xffff

/*
 * Per-port PHY status, as maintained by the switch's own SMI master or by
 * the side band signals of the embedded PHYs.  One register covers four
 * ports, so the state of the whole switch is two reads rather than seven.
 * [DS 8.4, p.744-748]
 */
#define	MTKSWITCH_PSR_P3_P0	0x7020
#define	MTKSWITCH_PSR_P6_P4	0x7024
#define		PSR_REG(p)		(((p) < 4) ? MTKSWITCH_PSR_P3_P0 : \
			    MTKSWITCH_PSR_P6_P4)
#define		PSR_SHIFT(p)		(((p) & 3) * 8)
#define		PSR_PORT(x, p)		(((x) >> PSR_SHIFT(p)) & 0xff)
#define		PSR_LINKUP		(1u<<0)
#define		PSR_SPEED(x)		(((x) >> 1) & 0x3)
#define		PSR_SPEED_10		0
#define		PSR_SPEED_100		1
#define		PSR_SPEED_1000		2
#define		PSR_DUPLEX		(1u<<3)
#define		PSR_XFC			(1u<<4)

#define	MTKSWITCH_PORTREG(r, p)	((r) + ((p) * 0x100))

#define	MTKSWITCH_PCR(x)	MTKSWITCH_PORTREG(0x2004, (x))
#define		PCR_PORT_VLAN_SECURE	(3u<<0)

#define	MTKSWITCH_PVC(x)	MTKSWITCH_PORTREG(0x2010, (x))
#define		PVC_VLAN_ATTR_MASK	(3u<<6)

#define	MTKSWITCH_PPBV1(x)	MTKSWITCH_PORTREG(0x2014, (x))
#define		PPBV_VID(v)		(((v)<<16) | (v))
#define		PPBV_VID_FROM_REG(x)	((x) & 0xfff)
#define		PPBV_VID_MASK		0xfff

#define	MTKSWITCH_PMCR(x)	MTKSWITCH_PORTREG(0x3000, (x))
#define		PMCR_FORCE_LINK		(1u<<0)
#define		PMCR_FORCE_DPX		(1u<<1)
#define		PMCR_FORCE_SPD_10	(0u<<2)
#define		PMCR_FORCE_SPD_100	(1u<<2)
#define		PMCR_FORCE_SPD_1000	(2u<<2)
#define		PMCR_FORCE_TX_FC	(1u<<4)
#define		PMCR_FORCE_RX_FC	(1u<<5)
#define		PMCR_BACKPR_EN		(1u<<8)
#define		PMCR_BKOFF_EN		(1u<<9)
#define		PMCR_MAC_RX_EN		(1u<<13)
#define		PMCR_MAC_TX_EN		(1u<<14)
#define		PMCR_IPG_CFG_RND	(1u<<18)
/* MT7531 moved the per-parameter force enables to the top of PMCR. */
#define		MT7531_PMCR_FORCE_TX_FC	(1u<<27)
#define		MT7531_PMCR_FORCE_RX_FC	(1u<<28)
#define		MT7531_PMCR_FORCE_DPX	(1u<<29)
#define		MT7531_PMCR_FORCE_SPD	(1u<<30)
#define		MT7531_PMCR_FORCE_LINK	(1u<<31)
#define		MT7531_PMCR_FORCE_MODE	(MT7531_PMCR_FORCE_TX_FC | \
		    MT7531_PMCR_FORCE_RX_FC | MT7531_PMCR_FORCE_DPX | \
		    MT7531_PMCR_FORCE_SPD | MT7531_PMCR_FORCE_LINK)
#define		PMCR_CFG_DEFAULT	(PMCR_BACKPR_EN | PMCR_BKOFF_EN | \
		    PMCR_MAC_RX_EN | PMCR_MAC_TX_EN | PMCR_IPG_CFG_RND |  \
		    PMCR_FORCE_RX_FC | PMCR_FORCE_TX_FC)

/*
 * Module TOP at base 0x7800, holding the strap status and the chip
 * revision.  Note that MT7531 keeps the revision here rather than at
 * 0x7ffc, where MT7530 has it.  [DS 9.2, p.756-759]
 */
#define	MTKSWITCH_STRAP		0x7800
#define	MTKSWITCH_SWSTRAP	0x7804
#define		STRAP_CHG_STRAP		(1u<<8)	/* SWSTRAP only. */
#define		STRAP_XTAL25		(1u<<7)	/* 0: 40MHz, 1: 25MHz */
#define		STRAP_PHY_EN		(1u<<6)
#define		STRAP_EEP_DIS		(1u<<5)
#define		STRAP_EEE_DIS		(1u<<4)
#define		STRAP_PLL_SW		(1u<<3)
#define		STRAP_PON_LT		(1u<<2)
#define		STRAP_EEP_MODE		(1u<<1)
#define		STRAP_TM_DIS		(1u<<0)

#define	MTKSWITCH_CREV		0x781c
#define		CREV_CHIP_ID(x)		(((x) >> 16) & 0xffff)
#define		CREV_CHIP_REV(x)	((x) & 0xf)

#define	MTKSWITCH_PMSR(x)	MTKSWITCH_PORTREG(0x3008, (x))
#define		PMSR_MAC_LINK_STS	(1u<<0)
#define		PMSR_MAC_DPX_STS	(1u<<1)
#define		PMSR_MAC_SPD_STS	(3u<<2)
#define		PMSR_MAC_SPD(x)		(((x)>>2) & 0x3)
#define		PMSR_MAC_SPD_10		0
#define		PMSR_MAC_SPD_100	1
#define		PMSR_MAC_SPD_1000	2
#define		PMSR_TX_FC_STS		(1u<<4)
#define		PMSR_RX_FC_STS		(1u<<5)

/* Indirect register access through the switch's MDIO slave interface. */
#define	MTKSWITCH_REG_ADDR(r)	(((r) >> 6) & 0x3ff)
#define	MTKSWITCH_REG_LO(r)	(((r) >> 2) & 0xf)
#define	MTKSWITCH_REG_HI	(1 << 4)
#define	MTKSWITCH_VAL_LO(v)	((v) & 0xffff)
#define	MTKSWITCH_VAL_HI(v)	(((v) >> 16) & 0xffff)
#define	MTKSWITCH_GLOBAL_REG	31

#endif	/* __MTKSWITCH_MT7531_H__ */
