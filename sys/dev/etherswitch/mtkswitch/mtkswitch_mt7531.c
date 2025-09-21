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

/*
 * Support for the MediaTek MT7531 ethernet switch, as found e.g. on the
 * Banana Pi R2 Pro.  Unlike the older MediaTek/Ralink switches handled
 * by this driver, MT7531 is an external part hanging off an MDIO bus;
 * all register accesses go through the parent bus using the indirect
 * access scheme of the MT7530 family.  The switch is expected to have
 * been brought up (PLLs, port modes) by the bootloader.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_media.h>
#include <net/if_types.h>

#include <machine/bus.h>
#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>
#include <dev/mdio/mdio.h>

#include <dev/etherswitch/etherswitch.h>
#include <dev/etherswitch/mtkswitch/mtkswitchvar.h>
#include <dev/etherswitch/mtkswitch/mtkswitch_mt7531.h>

#include "mdio_if.h"

#define	MTKSWITCH_MDIO_READ(sc, reg)					\
	MDIO_READREG(device_get_parent((sc)->sc_dev),			\
	    (sc)->sc_mdio_addr, (reg))
#define	MTKSWITCH_MDIO_WRITE(sc, reg, val)				\
	MDIO_WRITEREG(device_get_parent((sc)->sc_dev),			\
	    (sc)->sc_mdio_addr, (reg), (val))

/*
 * Every register access is three MDIO frames on the parent bus, each of
 * which busy-waits for hundreds of microseconds, so the retry budget has
 * to be far smaller than it would be for a memory mapped part.  The
 * operations we wait on - PIAC, ATC and VTCR - complete within a couple
 * of MDC periods of the switch's internal SMI master, so this is
 * generous for working hardware and bounded at roughly 65 ms for a part
 * that has stopped answering.
 */
#define	MTKSWITCH_BUSY_RETRIES	200
#define	MTKSWITCH_BUSY_DELAY	25

/*
 * Wait for the bits in mask to clear in the given register.  Returns 0
 * with the last register contents in *valp on success, ETIMEDOUT if the
 * bits never cleared or the bus stopped answering.
 */
static int
mtkswitch_reg_wait(struct mtkswitch_softc *sc, int reg, uint32_t mask,
    uint32_t *valp)
{
	uint32_t val;
	int retries;

	for (retries = MTKSWITCH_BUSY_RETRIES; retries > 0; retries--) {
		sc->sc_mdio_error = false;
		val = sc->hal.mtkswitch_read(sc, reg);
		if (sc->sc_mdio_error)
			return (ENXIO);
		if ((val & mask) == 0) {
			if (valp != NULL)
				*valp = val;
			return (0);
		}
		DELAY(MTKSWITCH_BUSY_DELAY);
	}
	device_printf(sc->sc_dev, "timeout waiting on register 0x%08x\n", reg);
	return (ETIMEDOUT);
}

/*
 * The MDIO interface reports a failed transfer as a negative value.  That
 * has to be caught here: the switch register is assembled from two reads,
 * so an error would otherwise turn into a plausible looking register
 * value - and, worse, one whose busy bit reads as clear.
 */
static uint32_t
mtkswitch_reg_read32(struct mtkswitch_softc *sc, int reg)
{
	int low, hi;

	MTKSWITCH_MDIO_WRITE(sc, MTKSWITCH_GLOBAL_REG,
	    MTKSWITCH_REG_ADDR(reg));
	low = MTKSWITCH_MDIO_READ(sc, MTKSWITCH_REG_LO(reg));
	hi = MTKSWITCH_MDIO_READ(sc, MTKSWITCH_REG_HI);
	if (low < 0 || hi < 0) {
		sc->sc_mdio_error = true;
		return (0xffffffff);
	}
	return ((low & 0xffff) | ((hi & 0xffff) << 16));
}

static uint32_t
mtkswitch_reg_write32(struct mtkswitch_softc *sc, int reg, uint32_t val)
{
	int err;

	err = MTKSWITCH_MDIO_WRITE(sc, MTKSWITCH_GLOBAL_REG,
	    MTKSWITCH_REG_ADDR(reg));
	if (err == 0)
		err = MTKSWITCH_MDIO_WRITE(sc, MTKSWITCH_REG_LO(reg),
		    MTKSWITCH_VAL_LO(val));
	if (err == 0)
		err = MTKSWITCH_MDIO_WRITE(sc, MTKSWITCH_REG_HI,
		    MTKSWITCH_VAL_HI(val));
	if (err != 0)
		sc->sc_mdio_error = true;

	return (0);
}

/*
 * The PHYs integrated in MT7531 are accessed through the switch's own
 * PHY indirect access control register.  Failures are reported as
 * 0xffff, the value an idle MDIO bus reads back as: mii_attach() takes
 * that as "no PHY at this address" and moves on, whereas -1 would have
 * it attach ukphy(4) to an address that never answered.
 */
#define	MTKSWITCH_PHY_ERR	0xffff

static int
mtkswitch_piac(struct mtkswitch_softc *sc, uint32_t cmd, uint32_t *datap)
{
	uint32_t data;

	if (mtkswitch_reg_wait(sc, MTKSWITCH_PIAC, PIAC_PHY_ACS_ST,
	    NULL) != 0)
		return (ETIMEDOUT);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_PIAC, PIAC_PHY_ACS_ST | cmd);
	if (mtkswitch_reg_wait(sc, MTKSWITCH_PIAC, PIAC_PHY_ACS_ST,
	    &data) != 0)
		return (ETIMEDOUT);
	if (datap != NULL)
		*datap = data & PIAC_MDIO_RW_DATA_MASK;

	return (0);
}

static int
mtkswitch_phy_read_locked(struct mtkswitch_softc *sc, int phy, int reg)
{
	uint32_t data;

	if (mtkswitch_piac(sc, PIAC_MDIO_ST | PIAC_MDIO_CMD_READ |
	    (reg << PIAC_MDIO_REG_ADDR_OFF) |
	    (phy << PIAC_MDIO_PHY_ADDR_OFF), &data) != 0)
		return (MTKSWITCH_PHY_ERR);

	return ((int)data);
}

/*
 * Clause 45 access, needed for the vendor MMDs of the embedded PHYs.
 * The address phase and the data phase are two separate PIAC commands,
 * both carrying the device address in the field clause 22 uses for the
 * register number.
 *
 * Unused for now: the GPHY core PLL power-up that needs them lives in a
 * part of the register map we do not have documentation for yet.
 */
static int __unused
mtkswitch_phy_read_c45_locked(struct mtkswitch_softc *sc, int phy, int devad,
    int reg)
{
	uint32_t sel, data;

	sel = PIAC_MDIO_ST_C45 | (devad << PIAC_MDIO_REG_ADDR_OFF) |
	    (phy << PIAC_MDIO_PHY_ADDR_OFF);
	if (mtkswitch_piac(sc, sel | PIAC_MDIO_CMD_ADDR |
	    (reg & PIAC_MDIO_RW_DATA_MASK), NULL) != 0)
		return (MTKSWITCH_PHY_ERR);
	if (mtkswitch_piac(sc, sel | PIAC_MDIO_CMD_READ_C45, &data) != 0)
		return (MTKSWITCH_PHY_ERR);

	return ((int)data);
}

static int __unused
mtkswitch_phy_write_c45_locked(struct mtkswitch_softc *sc, int phy, int devad,
    int reg, int val)
{
	uint32_t sel;

	sel = PIAC_MDIO_ST_C45 | (devad << PIAC_MDIO_REG_ADDR_OFF) |
	    (phy << PIAC_MDIO_PHY_ADDR_OFF);
	if (mtkswitch_piac(sc, sel | PIAC_MDIO_CMD_ADDR |
	    (reg & PIAC_MDIO_RW_DATA_MASK), NULL) != 0)
		return (ETIMEDOUT);

	return (mtkswitch_piac(sc, sel | PIAC_MDIO_CMD_WRITE |
	    (val & PIAC_MDIO_RW_DATA_MASK), NULL));
}

static int
mtkswitch_phy_read(device_t dev, int phy, int reg)
{
	struct mtkswitch_softc *sc = device_get_softc(dev);
	int data;

	if ((phy < 0 || phy >= 32) || (reg < 0 || reg >= 32))
		return (MTKSWITCH_PHY_ERR);

	MTKSWITCH_LOCK_ASSERT(sc, MA_NOTOWNED);
	MTKSWITCH_LOCK(sc);
	data = mtkswitch_phy_read_locked(sc, phy, reg);
	MTKSWITCH_UNLOCK(sc);

	return (data);
}

static int
mtkswitch_phy_write_locked(struct mtkswitch_softc *sc, int phy, int reg,
    int val)
{

	return (mtkswitch_piac(sc, PIAC_MDIO_ST | PIAC_MDIO_CMD_WRITE |
	    (reg << PIAC_MDIO_REG_ADDR_OFF) |
	    (phy << PIAC_MDIO_PHY_ADDR_OFF) |
	    (val & PIAC_MDIO_RW_DATA_MASK), NULL));
}

static int
mtkswitch_phy_write(device_t dev, int phy, int reg, int val)
{
	struct mtkswitch_softc *sc = device_get_softc(dev);
	int res;

	if ((phy < 0 || phy >= 32) || (reg < 0 || reg >= 32))
		return (ENXIO);

	MTKSWITCH_LOCK_ASSERT(sc, MA_NOTOWNED);
	MTKSWITCH_LOCK(sc);
	res = mtkswitch_phy_write_locked(sc, phy, reg, val);
	MTKSWITCH_UNLOCK(sc);

	return (res);
}

static int
mtkswitch_reg_read(device_t dev, int reg)
{
	struct mtkswitch_softc *sc = device_get_softc(dev);
	uint32_t val;

	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_REG32(reg));
	if (MTKSWITCH_IS_HI16(reg))
		return (MTKSWITCH_HI16(val));
	return (MTKSWITCH_LO16(val));
}

static int
mtkswitch_reg_write(device_t dev, int reg, int val)
{
	struct mtkswitch_softc *sc = device_get_softc(dev);
	uint32_t tmp;

	tmp = sc->hal.mtkswitch_read(sc, MTKSWITCH_REG32(reg));
	if (MTKSWITCH_IS_HI16(reg)) {
		tmp &= MTKSWITCH_LO16_MSK;
		tmp |= MTKSWITCH_TO_HI16(val);
	} else {
		tmp &= MTKSWITCH_HI16_MSK;
		tmp |= MTKSWITCH_TO_LO16(val);
	}
	sc->hal.mtkswitch_write(sc, MTKSWITCH_REG32(reg), tmp);

	return (0);
}

/*
 * Read the strap status.  The bootloader may have overridden the pin
 * strapping through SWSTRAP, in which case that is what the hardware is
 * actually running on, so prefer it.  [DS 9.2, p.756-757]
 */
static void
mtkswitch_read_strap(struct mtkswitch_softc *sc)
{
	uint32_t strap, swstrap;

	strap = sc->hal.mtkswitch_read(sc, MTKSWITCH_STRAP);
	swstrap = sc->hal.mtkswitch_read(sc, MTKSWITCH_SWSTRAP);
	if ((swstrap & STRAP_CHG_STRAP) != 0)
		strap = swstrap;
	sc->sc_strap = strap;

	if (bootverbose)
		device_printf(sc->sc_dev,
		    "strap 0x%04x: %s MHz XTAL, embedded PHYs %s, EEE %s%s\n",
		    strap & 0xffff,
		    (strap & STRAP_XTAL25) != 0 ? "25" : "40",
		    (strap & STRAP_PHY_EN) != 0 ? "enabled" : "disabled",
		    (strap & STRAP_EEE_DIS) != 0 ? "disabled" : "enabled",
		    (swstrap & STRAP_CHG_STRAP) != 0 ? " (software strap)" : "");
}

/*
 * Bring the switch to a known state.
 *
 * The comment this replaced claimed the bootloader had already done the
 * bring-up.  That is not true on the boards we care about: the reset
 * line of the switch is described as the MAC's "snps,reset-gpio", so the
 * ethernet driver pulses it during its own attach, moments before this
 * driver ever sees the part.  Whatever the bootloader configured is gone
 * by then.
 *
 * The datasheet requires all MACs to be forced link-down before either
 * reset bit is set, and exposes the completion of the internal table
 * initialisation in the same register, so there is no need to guess at a
 * settling time.  [DS 8.4, p.734]
 */
static int
mtkswitch_reset(struct mtkswitch_softc *sc)
{
	uint32_t val;
	int err, port, retries;

	val = 0;
	/* Assume the VLAN table needs clearing until the hardware says so. */
	sc->sc_vlans_dirty = true;
	mtkswitch_read_strap(sc);

	/* Force every MAC link-down, as the reset bits require. */
	for (port = 0; port < sc->numports; port++)
		sc->hal.mtkswitch_write(sc, MTKSWITCH_PMCR(port),
		    MT7531_PMCR_FORCE_MODE);

	sc->hal.mtkswitch_write(sc, MTKSWITCH_SYS_CTRL,
	    SYS_CTRL_SW_SYS_RST | SYS_CTRL_SW_REG_RST);

	/*
	 * Both reset bits are self clearing, and the MDIO registers are
	 * part of what SW_REG_RST puts back to defaults, so give the bus
	 * a moment before talking to it again.
	 */
	DELAY(1000);

	for (retries = 100; retries > 0; retries--) {
		sc->sc_mdio_error = false;
		val = sc->hal.mtkswitch_read(sc, MTKSWITCH_SYS_CTRL);
		if (!sc->sc_mdio_error &&
		    (val & SYS_CTRL_TAB_INIT_DONE) == SYS_CTRL_TAB_INIT_DONE)
			break;
		DELAY(1000);
	}
	/*
	 * Not fatal: the read of the chip revision below is the authoritative
	 * test of whether the part survived the reset, and treating a status
	 * bit that never came up as fatal would keep the switch from
	 * attaching at all on a part whose table initialisation we have
	 * misread.  Say so and carry on.
	 */
	if (retries == 0)
		device_printf(sc->sc_dev,
		    "table initialisation did not complete (SYS_CTRL 0x%08x)\n",
		    val);
	else
		/*
		 * The hardware has just initialised the VLAN table for us,
		 * so the software pass over it can be skipped until
		 * something changes it.
		 */
		sc->sc_vlans_dirty = false;

	/* Make sure the part still answers after the reset. */
	sc->sc_mdio_error = false;
	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_CREV);
	err = sc->sc_mdio_error ? ENXIO : 0;
	if (err == 0 && CREV_CHIP_ID(val) != MTKSWITCH_MT7531_ID) {
		device_printf(sc->sc_dev,
		    "switch stopped responding after reset (CREV 0x%08x)\n",
		    val);
		err = ENXIO;
	}

	return (err);
}

static int
mtkswitch_hw_setup(struct mtkswitch_softc *sc)
{

	/* Called early and hence unlocked */
	return (0);
}

/*
 * Have the switch's own MDIO master poll the embedded PHYs, which is what
 * fills in the per-port status registers.  Without this they read back as
 * zero and the only view of a port's state is its MAC, which on this part
 * does not follow the PHY by itself.
 *
 * Not every configuration can do this, so check that the enables stick and
 * remember the answer; mtkswitch_get_port_status() falls back to the MAC
 * status registers when they do not.  [DS 8.4, p.741]
 */
static int
mtkswitch_hw_global_setup(struct mtkswitch_softc *sc)
{
	uint32_t val, want;
	int phy;

	/* Called early and hence unlocked */

	want = 0;
	for (phy = 0; phy < sc->numphys; phy++)
		if ((sc->phymap & (1u << phy)) != 0)
			want |= PHY_POLL_AP_EN(phy);

	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_PHY_POLL);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_PHY_POLL, val | want);

	sc->sc_mdio_error = false;
	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_PHY_POLL);
	sc->sc_use_psr = !sc->sc_mdio_error && (val & want) == want;
	if (!sc->sc_use_psr)
		device_printf(sc->sc_dev, "PHY auto-polling did not enable "
		    "(PHY_POLL 0x%08x), port link state may be unreliable\n",
		    val);
	else if (bootverbose)
		device_printf(sc->sc_dev, "PHY auto-polling enabled for %#x\n",
		    sc->phymap);

	return (0);
}

static void
mtkswitch_port_init(struct mtkswitch_softc *sc, int port)
{
	uint32_t val;

	/* Called early and hence unlocked */

	/* Set the port to secure mode */
	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_PCR(port));
	val |= PCR_PORT_VLAN_SECURE;
	sc->hal.mtkswitch_write(sc, MTKSWITCH_PCR(port), val);

	/* Set port's vlan_attr to user port */
	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_PVC(port));
	val &= ~PVC_VLAN_ATTR_MASK;
	sc->hal.mtkswitch_write(sc, MTKSWITCH_PVC(port), val);

	/*
	 * The MAC of this part does not pick up the negotiated parameters
	 * from its PHY, so every port runs in forced mode and gets told
	 * what they are: the CPU port once, from the fixed link in the
	 * device tree, and the user ports on each link change.  Bring them
	 * up forced-down; mtkswitch_port_link_update() takes it from there.
	 */
	val = PMCR_CFG_DEFAULT | MT7531_PMCR_FORCE_MODE;
	if (port == sc->cpuport)
		val |= PMCR_FORCE_LINK | PMCR_FORCE_DPX | PMCR_FORCE_SPD_1000;
	/* Set port's MAC to default settings */
	sc->hal.mtkswitch_write(sc, MTKSWITCH_PMCR(port), val);
}

/*
 * Tell a port's MAC what its PHY negotiated.  Called from the link state
 * poll with the softc lock held.
 */
static void
mtkswitch_port_link_update(struct mtkswitch_softc *sc, int port,
    uint32_t portstatus)
{
	uint32_t val;

	MTKSWITCH_LOCK_ASSERT(sc, MA_OWNED);

	val = PMCR_CFG_DEFAULT | MT7531_PMCR_FORCE_MODE;
	if ((portstatus & MTKSWITCH_LINK_UP) != 0) {
		val |= PMCR_FORCE_LINK;
		switch (portstatus & MTKSWITCH_SPEED_MASK) {
		case MTKSWITCH_SPEED_1000:
			val |= PMCR_FORCE_SPD_1000;
			break;
		case MTKSWITCH_SPEED_100:
			val |= PMCR_FORCE_SPD_100;
			break;
		default:
			val |= PMCR_FORCE_SPD_10;
			break;
		}
		if ((portstatus & MTKSWITCH_DUPLEX) != 0)
			val |= PMCR_FORCE_DPX;
	}
	sc->hal.mtkswitch_write(sc, MTKSWITCH_PMCR(port), val);
}

/*
 * Report the state of a port.
 *
 * For a port with a PHY this has to come from the PHY, not from the MAC:
 * the MAC is in forced mode and only knows what we last told it, so using
 * it here would be circular - and it is also what the ukphy(4) attached to
 * the same port reports, so the two would take turns overwriting each
 * other's answer.  The CPU port has no PHY and its MAC status is the only
 * thing there is.
 */
static uint32_t
mtkswitch_get_port_status(struct mtkswitch_softc *sc, int port)
{
	uint32_t val, res, tmp;

	MTKSWITCH_LOCK_ASSERT(sc, MA_OWNED);
	res = 0;

	if (sc->sc_use_psr && (sc->phymap & (1u << port)) != 0) {
		val = sc->hal.mtkswitch_read(sc, PSR_REG(port));
		val = PSR_PORT(val, port);

		if (val & PSR_LINKUP)
			res |= MTKSWITCH_LINK_UP;
		if (val & PSR_DUPLEX)
			res |= MTKSWITCH_DUPLEX;
		tmp = PSR_SPEED(val);
		if (tmp == PSR_SPEED_100)
			res |= MTKSWITCH_SPEED_100;
		else if (tmp == PSR_SPEED_1000)
			res |= MTKSWITCH_SPEED_1000;
		else
			res |= MTKSWITCH_SPEED_10;
		if (val & PSR_XFC)
			res |= MTKSWITCH_TXFLOW | MTKSWITCH_RXFLOW;

		return (res);
	}

	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_PMSR(port));

	if (val & PMSR_MAC_LINK_STS)
		res |= MTKSWITCH_LINK_UP;
	if (val & PMSR_MAC_DPX_STS)
		res |= MTKSWITCH_DUPLEX;
	tmp = PMSR_MAC_SPD(val);
	if (tmp == PMSR_MAC_SPD_10)
		res |= MTKSWITCH_SPEED_10;
	else if (tmp == PMSR_MAC_SPD_100)
		res |= MTKSWITCH_SPEED_100;
	else if (tmp == PMSR_MAC_SPD_1000)
		res |= MTKSWITCH_SPEED_1000;
	if (val & PMSR_TX_FC_STS)
		res |= MTKSWITCH_TXFLOW;
	if (val & PMSR_RX_FC_STS)
		res |= MTKSWITCH_RXFLOW;

	return (res);
}

static int
mtkswitch_atu_flush(struct mtkswitch_softc *sc)
{

	MTKSWITCH_LOCK_ASSERT(sc, MA_OWNED);

	/* Flush all non-static MAC addresses */
	if (mtkswitch_reg_wait(sc, MTKSWITCH_ATC, ATC_BUSY, NULL) != 0)
		return (ETIMEDOUT);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_ATC, ATC_BUSY |
	    ATC_AC_MAT_NON_STATIC_MACS | ATC_AC_CMD_CLEAN);
	return (mtkswitch_reg_wait(sc, MTKSWITCH_ATC, ATC_BUSY, NULL));
}

static int
mtkswitch_port_vlan_setup(struct mtkswitch_softc *sc, etherswitch_port_t *p)
{
	int err;

	/*
	 * Port behaviour wrt tag/untag/stack is currently defined per-VLAN.
	 * So we say we don't support it here.
	 */
	if ((p->es_flags & (ETHERSWITCH_PORT_DOUBLE_TAG |
	    ETHERSWITCH_PORT_ADDTAG | ETHERSWITCH_PORT_STRIPTAG)) != 0)
		return (ENOTSUP);

	MTKSWITCH_LOCK_ASSERT(sc, MA_NOTOWNED);
	MTKSWITCH_LOCK(sc);

	/* Set the PVID */
	if (p->es_pvid != 0) {
		err = sc->hal.mtkswitch_vlan_set_pvid(sc, p->es_port,
		    p->es_pvid);
		if (err != 0) {
			MTKSWITCH_UNLOCK(sc);
			return (err);
		}
	}

	MTKSWITCH_UNLOCK(sc);

	return (0);
}

static int
mtkswitch_port_vlan_get(struct mtkswitch_softc *sc, etherswitch_port_t *p)
{

	MTKSWITCH_LOCK_ASSERT(sc, MA_NOTOWNED);
	MTKSWITCH_LOCK(sc);

	/* Retrieve the PVID */
	sc->hal.mtkswitch_vlan_get_pvid(sc, p->es_port, &p->es_pvid);

	/*
	 * Port flags are not supported at the moment.
	 * Port's tag/untag/stack behaviour is defined per-VLAN.
	 */
	p->es_flags = 0;

	MTKSWITCH_UNLOCK(sc);

	return (0);
}

static void
mtkswitch_invalidate_vlan(struct mtkswitch_softc *sc, uint32_t vid)
{

	if (mtkswitch_reg_wait(sc, MTKSWITCH_VTCR, VTCR_BUSY, NULL) != 0)
		return;
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VTCR, VTCR_BUSY |
	    VTCR_FUNC_VID_INVALID | (vid & VTCR_VID_MASK));
	(void)mtkswitch_reg_wait(sc, MTKSWITCH_VTCR, VTCR_BUSY, NULL);
}

static void
mtkswitch_vlan_init_hw(struct mtkswitch_softc *sc)
{
	uint32_t val, i;

	MTKSWITCH_LOCK_ASSERT(sc, MA_NOTOWNED);
	MTKSWITCH_LOCK(sc);

	/*
	 * Reset all VLANs to defaults first.  Each entry costs two waits
	 * on VTCR and a handful of MDIO frames, so walking the whole 4096
	 * entry VID space takes seconds; skip it entirely when the switch
	 * reset has just done the same thing in hardware.
	 */
	if (sc->sc_vlans_dirty) {
		for (i = 0; i < sc->info.es_nvlangroups; i++)
			mtkswitch_invalidate_vlan(sc, i);
	}

	/* Now, add all ports as untagged members of VLAN 1 */
	val = VAWD1_IVL_MAC | VAWD1_VTAG_EN | VAWD1_VALID;
	for (i = 0; i < sc->info.es_nports; i++)
		val |= VAWD1_PORT_MEMBER(i);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VAWD1, val);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VAWD2, 0);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VTCR, VTCR_BUSY |
	    VTCR_FUNC_VID_WRITE | 1);
	(void)mtkswitch_reg_wait(sc, MTKSWITCH_VTCR, VTCR_BUSY, NULL);

	/* Set all port PVIDs to 1 */
	for (i = 0; i < sc->info.es_nports; i++)
		sc->hal.mtkswitch_vlan_set_pvid(sc, i, 1);

	sc->sc_vlans_dirty = true;

	MTKSWITCH_UNLOCK(sc);
}

static int
mtkswitch_vlan_getvgroup(struct mtkswitch_softc *sc, etherswitch_vlangroup_t *v)
{
	uint32_t val, i;

	MTKSWITCH_LOCK_ASSERT(sc, MA_NOTOWNED);

	if ((sc->vlan_mode != ETHERSWITCH_VLAN_DOT1Q) ||
	    (v->es_vlangroup < 0) ||
	    (v->es_vlangroup >= sc->info.es_nvlangroups))
		return (EINVAL);

	/* Reset the member ports. */
	v->es_untagged_ports = 0;
	v->es_member_ports = 0;

	/* Not supported for now */
	v->es_fid = 0;

	/* Vlangroup number maps directly to the vid */
	v->es_vid = v->es_vlangroup;

	MTKSWITCH_LOCK(sc);
	if (mtkswitch_reg_wait(sc, MTKSWITCH_VTCR, VTCR_BUSY, NULL) != 0) {
		MTKSWITCH_UNLOCK(sc);
		return (ETIMEDOUT);
	}
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VTCR, VTCR_BUSY |
	    VTCR_FUNC_VID_READ | (v->es_vid & VTCR_VID_MASK));
	if (mtkswitch_reg_wait(sc, MTKSWITCH_VTCR, VTCR_BUSY, &val) != 0) {
		MTKSWITCH_UNLOCK(sc);
		return (ETIMEDOUT);
	}
	if (val & VTCR_IDX_INVALID) {
		MTKSWITCH_UNLOCK(sc);
		return (0);
	}

	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_VAWD1);
	if (val & VAWD1_VALID)
		v->es_vid |= ETHERSWITCH_VID_VALID;
	else {
		MTKSWITCH_UNLOCK(sc);
		return (0);
	}
	v->es_member_ports = (val >> VAWD1_MEMBER_OFF) & VAWD1_MEMBER_MASK;

	val = sc->hal.mtkswitch_read(sc, MTKSWITCH_VAWD2);
	for (i = 0; i < sc->info.es_nports; i++) {
		if ((val & VAWD2_PORT_MASK(i)) == VAWD2_PORT_UNTAGGED(i))
			v->es_untagged_ports |= (1<<i);
	}

	MTKSWITCH_UNLOCK(sc);
	return (0);
}

static int
mtkswitch_vlan_setvgroup(struct mtkswitch_softc *sc, etherswitch_vlangroup_t *v)
{
	uint32_t val, i;

	MTKSWITCH_LOCK_ASSERT(sc, MA_NOTOWNED);

	if ((sc->vlan_mode != ETHERSWITCH_VLAN_DOT1Q) ||
	    (v->es_vlangroup < 0) ||
	    (v->es_vlangroup >= sc->info.es_nvlangroups))
		return (EINVAL);

	/* We currently don't support FID */
	if (v->es_fid != 0)
		return (EINVAL);

	MTKSWITCH_LOCK(sc);
	if (mtkswitch_reg_wait(sc, MTKSWITCH_VTCR, VTCR_BUSY, NULL) != 0) {
		MTKSWITCH_UNLOCK(sc);
		return (ETIMEDOUT);
	}

	/* We use FID 0 */
	val = VAWD1_IVL_MAC | VAWD1_VTAG_EN | VAWD1_VALID;
	val |= ((v->es_member_ports & VAWD1_MEMBER_MASK) << VAWD1_MEMBER_OFF);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VAWD1, val);

	/* Set tagged ports */
	val = 0;
	for (i = 0; i < sc->info.es_nports; i++)
		if (((1<<i) & v->es_untagged_ports) == 0)
			val |= VAWD2_PORT_TAGGED(i);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VAWD2, val);

	/* Write the VLAN entry */
	sc->sc_vlans_dirty = true;
	sc->hal.mtkswitch_write(sc, MTKSWITCH_VTCR, VTCR_BUSY |
	    VTCR_FUNC_VID_WRITE | (v->es_vid & VTCR_VID_MASK));
	if (mtkswitch_reg_wait(sc, MTKSWITCH_VTCR, VTCR_BUSY, &val) != 0) {
		MTKSWITCH_UNLOCK(sc);
		return (ETIMEDOUT);
	}

	MTKSWITCH_UNLOCK(sc);

	if (val & VTCR_IDX_INVALID)
		return (EINVAL);

	return (0);
}

static int
mtkswitch_vlan_get_pvid(struct mtkswitch_softc *sc, int port, int *pvid)
{

	MTKSWITCH_LOCK_ASSERT(sc, MA_OWNED);

	*pvid = sc->hal.mtkswitch_read(sc, MTKSWITCH_PPBV1(port));
	*pvid = PPBV_VID_FROM_REG(*pvid);

	return (0);
}

static int
mtkswitch_vlan_set_pvid(struct mtkswitch_softc *sc, int port, int pvid)
{
	uint32_t val;

	MTKSWITCH_LOCK_ASSERT(sc, MA_OWNED);
	val = PPBV_VID(pvid & PPBV_VID_MASK);
	sc->hal.mtkswitch_write(sc, MTKSWITCH_PPBV1(port), val);

	return (0);
}

extern void
mtk_attach_switch_mt7531(struct mtkswitch_softc *sc)
{

	sc->portmap = 0x7f;
	sc->phymap = 0x1f;

	sc->info.es_nports = 7;
	sc->info.es_vlan_caps = ETHERSWITCH_VLAN_DOT1Q;
	sc->info.es_nvlangroups = 4096;
	sprintf(sc->info.es_name, "MediaTek MT7531");

	sc->hal.mtkswitch_read = mtkswitch_reg_read32;
	sc->hal.mtkswitch_write = mtkswitch_reg_write32;
	sc->hal.mtkswitch_reset = mtkswitch_reset;
	sc->hal.mtkswitch_hw_setup = mtkswitch_hw_setup;
	sc->hal.mtkswitch_hw_global_setup = mtkswitch_hw_global_setup;
	sc->hal.mtkswitch_port_init = mtkswitch_port_init;
	sc->hal.mtkswitch_get_port_status = mtkswitch_get_port_status;
	sc->hal.mtkswitch_port_link_update = mtkswitch_port_link_update;
	sc->hal.mtkswitch_atu_flush = mtkswitch_atu_flush;
	sc->hal.mtkswitch_port_vlan_setup = mtkswitch_port_vlan_setup;
	sc->hal.mtkswitch_port_vlan_get = mtkswitch_port_vlan_get;
	sc->hal.mtkswitch_vlan_init_hw = mtkswitch_vlan_init_hw;
	sc->hal.mtkswitch_vlan_getvgroup = mtkswitch_vlan_getvgroup;
	sc->hal.mtkswitch_vlan_setvgroup = mtkswitch_vlan_setvgroup;
	sc->hal.mtkswitch_vlan_get_pvid = mtkswitch_vlan_get_pvid;
	sc->hal.mtkswitch_vlan_set_pvid = mtkswitch_vlan_set_pvid;
	sc->hal.mtkswitch_phy_read = mtkswitch_phy_read;
	sc->hal.mtkswitch_phy_write = mtkswitch_phy_write;
	sc->hal.mtkswitch_reg_read = mtkswitch_reg_read;
	sc->hal.mtkswitch_reg_write = mtkswitch_reg_write;
}
