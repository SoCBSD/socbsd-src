/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Martin Filla <freebsd@sysctl.cz>
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
 * Driver for synthetic "fixed link" PHYs, created by mii_attach_fixed().
 * The device has no presence on an MDIO bus at all: the media is whatever
 * the caller asked for, the link is always up and not a single MII register
 * is ever touched.  This lets a MAC whose link partner is another MAC - an
 * ethernet switch CPU port, a media converter, a backplane connection - use
 * the ordinary miibus(4) path instead of open coding its own ifmedia.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_media.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include "miibus_if.h"

struct fixedphy_softc {
	struct mii_softc	mii_sc;		/* Must be first. */
	u_int			fixed_media;
};

static int	fixedphy_probe(device_t);
static int	fixedphy_attach(device_t);

static int	fixedphy_service(struct mii_softc *, struct mii_data *, int);
static void	fixedphy_status(struct mii_softc *);
static void	fixedphy_reset(struct mii_softc *);

static const struct mii_phy_funcs fixedphy_funcs = {
	fixedphy_service,
	fixedphy_status,
	fixedphy_reset
};

static device_method_t fixedphy_methods[] = {
	/* device interface */
	DEVMETHOD(device_probe,		fixedphy_probe),
	DEVMETHOD(device_attach,	fixedphy_attach),
	DEVMETHOD(device_detach,	mii_phy_detach),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),

	DEVMETHOD_END
};

static driver_t fixedphy_driver = {
	"fixedphy",
	fixedphy_methods,
	sizeof(struct fixedphy_softc)
};

DRIVER_MODULE(fixedphy, miibus, fixedphy_driver, 0, 0);

static int
fixedphy_probe(device_t dev)
{
	struct mii_attach_args *ma;

	/*
	 * Only claim devices created by mii_attach_fixed().  A PHY found by
	 * the bus scan in mii_attach() has to be left to the real drivers
	 * and to ukphy(4).
	 */
	ma = device_get_ivars(dev);
	if (IFM_TYPE(ma->mii_fixed_media) != IFM_ETHER)
		return (ENXIO);

	device_set_desc(dev, "Fixed-link media interface");

	return (BUS_PROBE_DEFAULT);
}

static int
fixedphy_attach(device_t dev)
{
	struct fixedphy_softc *fsc;
	struct mii_attach_args *ma;
	struct mii_softc *sc;
	struct mii_data *mii;

	fsc = device_get_softc(dev);
	sc = &fsc->mii_sc;
	ma = device_get_ivars(dev);
	fsc->fixed_media = ma->mii_fixed_media;

	/*
	 * Passing add_media as 0 makes mii_phy_dev_attach() return before
	 * PHY_RESET() and before it reads BMSR and EXTSR, so no MDIO cycle
	 * is issued.  MIIF_NOISOLATE keeps mii_mediachg() from doing a
	 * read-modify-write of BMCR.  MIIF_DOPAUSE must not be set, as
	 * mii_phy_flowstatus() would then read ANAR and ANLPAR.
	 */
	mii_phy_dev_attach(dev, MIIF_NOISOLATE | MIIF_NOMANPAUSE,
	    &fixedphy_funcs, 0);
	mii = sc->mii_pdata;

	/*
	 * Publish the single media this link runs at.  The ifmedia entry
	 * carries our instance, as mii_mediachg(), mii_tick() and
	 * mii_pollstat() compare it against sc->mii_inst; the status
	 * reported by fixedphy_status() must not carry it.  At least one
	 * entry has to exist before MIIBUS_MEDIAINIT(), whose ifmedia_set()
	 * asserts on an empty list.
	 */
	ifmedia_add(&mii->mii_media,
	    fsc->fixed_media | IFM_MAKEWORD(0, 0, 0, sc->mii_inst), 0, NULL);

	device_printf(dev, "fixed link\n");

	MIIBUS_MEDIAINIT(sc->mii_dev);

	return (0);
}

static void
fixedphy_reset(struct mii_softc *sc __unused)
{

	/* Nothing to reset; mii_phy_reset() would write and poll BMCR. */
}

static int
fixedphy_service(struct mii_softc *sc, struct mii_data *mii __unused, int cmd)
{

	switch (cmd) {
	case MII_POLLSTAT:
	case MII_MEDIACHG:
	case MII_TICK:
		/*
		 * There is nothing to program and nothing that can change.
		 * In particular do not call mii_phy_setmedia(), mii_phy_tick()
		 * or mii_phy_reset(): all of them touch MII registers.  Any
		 * media other than the one we published has already been
		 * rejected by ifmedia_ioctl().
		 */
		break;
	default:
		return (0);
	}

	/* Update the media status. */
	PHY_STATUS(sc);

	/* Callback if something changed. */
	mii_phy_update(sc, cmd);

	return (0);
}

static void
fixedphy_status(struct mii_softc *sc)
{
	struct fixedphy_softc *fsc = (struct fixedphy_softc *)sc;
	struct mii_data *mii = sc->mii_pdata;

	/* A fixed link is unconditionally up. */
	mii->mii_media_status = IFM_AVALID | IFM_ACTIVE;
	mii->mii_media_active = fsc->fixed_media;
}
