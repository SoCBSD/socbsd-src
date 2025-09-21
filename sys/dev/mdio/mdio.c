/*-
 * Copyright (c) 2011-2012 Stefan Bethke.
 * All rights reserved.
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

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/module.h>
#include <sys/sbuf.h>

#ifdef FDT
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/openfirm.h>
#endif

#include <dev/mdio/mdio.h>
#include <dev/mii/mii.h>

#include "mdio_if.h"
#ifdef FDT
#include "ofw_bus_if.h"
#endif

struct mdio_softc {
#ifdef FDT
	phandle_t	sc_node;	/* Node to enumerate, or -1. */
#else
	int		sc_unused;
#endif
};

#ifdef FDT
struct mdio_devinfo {
	uint32_t		mdi_phy;	/* "reg", the MDIO address. */
	struct ofw_bus_devinfo	mdi_obd;
	struct resource_list	mdi_rl;
};

/*
 * True for a node describing an ordinary PHY.  Those are attached by
 * miibus(4) hanging off the MAC, never by a driver on this bus, and
 * creating devices for them would only offer them to the ethernet switch
 * drivers in the mdio devclass, some of which claim anything they are
 * offered or poke registers from their probe method.
 */
static bool
mdio_fdt_is_phy(phandle_t node)
{
	char *compat, *s;
	ssize_t len, l;
	bool phy;

	if (ofw_bus_node_is_compatible(node, "ethernet-phy-ieee802.3-c22") ||
	    ofw_bus_node_is_compatible(node, "ethernet-phy-ieee802.3-c45"))
		return (true);

	/*
	 * A PHY may also be described by its ID alone, as in
	 * "ethernet-phy-id001c.c916", with no generic fallback.
	 */
	len = OF_getprop_alloc(node, "compatible", (void **)&compat);
	if (len <= 0)
		return (false);
	phy = false;
	for (s = compat; s < compat + len; s += l) {
		l = strlen(s) + 1;
		if (strncmp(s, "ethernet-phy-id", 15) == 0) {
			phy = true;
			break;
		}
	}
	OF_prop_free(compat);

	return (phy);
}

static void
mdio_fdt_enumerate(device_t dev)
{
	struct mdio_softc *sc;
	struct mdio_devinfo *di;
	device_t child;
	phandle_t node;
	uint32_t addr;

	sc = device_get_softc(dev);

	for (node = OF_child(sc->sc_node); node != 0; node = OF_peer(node)) {
		if (!ofw_bus_node_status_okay(node))
			continue;
		if (!OF_hasprop(node, "compatible") || mdio_fdt_is_phy(node))
			continue;
		/*
		 * On an MDIO bus #address-cells is 1 and #size-cells is 0,
		 * so "reg" is the bare MDIO address.
		 */
		if (OF_getencprop(node, "reg", &addr, sizeof(addr)) <= 0)
			continue;
		if (addr >= MII_NPHY) {
			device_printf(dev,
			    "ignoring MDIO address %u, out of range\n", addr);
			continue;
		}
		if (ofw_bus_find_child_device_by_phandle(dev, node) != NULL)
			continue;

		di = malloc(sizeof(*di), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (di == NULL)
			continue;
		if (ofw_bus_gen_setup_devinfo(&di->mdi_obd, node) != 0) {
			free(di, M_DEVBUF);
			continue;
		}
		di->mdi_phy = addr;

		/*
		 * device_add_child(), the function rather than
		 * BUS_ADD_CHILD(), so that mdio_add_child() does not
		 * allocate a devinfo that the one installed below leaks.
		 */
		child = device_add_child(dev, NULL, DEVICE_UNIT_ANY);
		if (child == NULL) {
			ofw_bus_gen_destroy_devinfo(&di->mdi_obd);
			free(di, M_DEVBUF);
			continue;
		}
		resource_list_init(&di->mdi_rl);
		(void)ofw_bus_intr_to_rl(child, node, &di->mdi_rl, NULL);
		device_set_ivars(child, di);
	}
}
#endif /* FDT */

static void
mdio_identify(driver_t *driver, device_t parent)
{

	if (device_find_child(parent, mdio_driver.name, DEVICE_UNIT_ANY) == NULL)
		BUS_ADD_CHILD(parent, 0, mdio_driver.name, DEVICE_UNIT_ANY);
}

static int
mdio_probe(device_t dev)
{

	device_set_desc(dev, "MDIO");

	return (BUS_PROBE_SPECIFIC);
}

/*
 * Report what answers on the bus.  An idle MDIO bus is pulled up and
 * reads as all ones, so a bus that reads as all zeroes is one whose
 * MDIO line is not driven at all -- wrong pin mux, or a device with no
 * power.  That is worth telling apart from a device that is merely
 * silent, and neither is visible from the drivers above.
 */
static void
mdio_scan(device_t dev)
{
	int addr, found, id1, id2, idle;

	idle = -1;
	for (addr = 0, found = 0; addr < MII_NPHY; addr++) {
		id1 = MDIO_READREG(dev, addr, MII_PHYIDR1);
		id2 = MDIO_READREG(dev, addr, MII_PHYIDR2);
		if (id1 < 0 || id2 < 0)
			continue;
		id1 &= 0xffff;
		id2 &= 0xffff;
		if ((id1 == 0 && id2 == 0) || (id1 == 0xffff && id2 == 0xffff)) {
			if (idle < 0)
				idle = id1;
			continue;
		}
		device_printf(dev, "address %d: 0x%04x 0x%04x\n", addr, id1,
		    id2);
		found++;
	}
	if (idle >= 0)
		device_printf(dev, "%d device(s) answer, idle bus reads "
		    "0x%04x\n", found, idle);
	else
		device_printf(dev, "%d device(s) answer\n", found);
}

static int
mdio_attach(device_t dev)
{
#ifdef FDT
	struct mdio_softc *sc;
#endif

	if (bootverbose)
		mdio_scan(dev);

#ifdef FDT
	sc = device_get_softc(dev);
	sc->sc_node = ofw_bus_get_node(dev);
	if (sc->sc_node != 0 && sc->sc_node != (phandle_t)-1) {
		/*
		 * The device tree describes this bus and is authoritative.
		 * Do not run the identify routines: on mdio(4) they exist
		 * only to work around the absence of an FDT node, and they
		 * would add a second, nodeless device for a chip that is
		 * about to be enumerated from the tree.  Hints are still
		 * honoured, being explicit user configuration.
		 */
		bus_enumerate_hinted_children(dev);
		mdio_fdt_enumerate(dev);
		bus_attach_children(dev);
		return (0);
	}
#endif
	bus_identify_children(dev);
	bus_enumerate_hinted_children(dev);
	bus_attach_children(dev);
	return (0);
}

#ifdef FDT
/*
 * Hinted children get the same ivars with the OFW half blanked, so that
 * ofw_bus_get_node() on them returns -1 rather than dereferencing garbage.
 */
static device_t
mdio_add_child(device_t dev, u_int order, const char *name, int unit)
{
	struct mdio_devinfo *di;
	device_t child;

	child = device_add_child_ordered(dev, order, name, unit);
	if (child == NULL)
		return (NULL);
	di = malloc(sizeof(*di), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (di == NULL) {
		device_delete_child(dev, child);
		return (NULL);
	}
	di->mdi_obd.obd_node = -1;
	resource_list_init(&di->mdi_rl);
	device_set_ivars(child, di);

	return (child);
}

static void
mdio_child_deleted(device_t dev __unused, device_t child)
{
	struct mdio_devinfo *di;

	di = device_get_ivars(child);
	if (di == NULL)
		return;
	resource_list_free(&di->mdi_rl);
	ofw_bus_gen_destroy_devinfo(&di->mdi_obd);
	free(di, M_DEVBUF);
}

static const struct ofw_bus_devinfo *
mdio_get_devinfo(device_t dev __unused, device_t child)
{
	struct mdio_devinfo *di;

	di = device_get_ivars(child);
	if (di == NULL)
		return (NULL);
	return (&di->mdi_obd);
}

static struct resource_list *
mdio_get_resource_list(device_t dev __unused, device_t child)
{
	struct mdio_devinfo *di;

	di = device_get_ivars(child);
	if (di == NULL)
		return (NULL);
	return (&di->mdi_rl);
}

static int
mdio_read_ivar(device_t dev __unused, device_t child, int which,
    uintptr_t *result)
{
	struct mdio_devinfo *di;

	di = device_get_ivars(child);
	if (di == NULL)
		return (ENOENT);

	switch (which) {
	case MDIO_IVAR_PHY:
		*result = di->mdi_phy;
		break;
	default:
		return (ENOENT);
	}
	return (0);
}

static int
mdio_child_location(device_t dev __unused, device_t child, struct sbuf *sb)
{
	struct mdio_devinfo *di;

	di = device_get_ivars(child);
	if (di != NULL && di->mdi_obd.obd_node != (phandle_t)-1)
		sbuf_printf(sb, "phy=%u", di->mdi_phy);

	return (0);
}
#endif /* FDT */

static int
mdio_readreg(device_t dev, int phy, int reg)
{

	return (MDIO_READREG(device_get_parent(dev), phy, reg));
}

static int
mdio_writereg(device_t dev, int phy, int reg, int val)
{

	return (MDIO_WRITEREG(device_get_parent(dev), phy, reg, val));
}

static int
mdio_readextreg(device_t dev, int phy, int devad, int reg)
{

	return (MDIO_READEXTREG(device_get_parent(dev), phy, devad, reg));
}

static int
mdio_writeextreg(device_t dev, int phy, int devad, int reg,
    int val)
{

	return (MDIO_WRITEEXTREG(device_get_parent(dev), phy, devad, reg, val));
}

static void
mdio_hinted_child(device_t dev, const char *name, int unit)
{

	BUS_ADD_CHILD(dev, 0, name, unit);
}

static device_method_t mdio_methods[] = {
	/* device interface */
	DEVMETHOD(device_identify,	mdio_identify),
	DEVMETHOD(device_probe,		mdio_probe),
	DEVMETHOD(device_attach,	mdio_attach),
	DEVMETHOD(device_detach,	bus_generic_detach),
	DEVMETHOD(device_shutdown,	bus_generic_shutdown),

	/* bus interface */
#ifdef FDT
	DEVMETHOD(bus_add_child,	mdio_add_child),
	DEVMETHOD(bus_child_deleted,	mdio_child_deleted),
	DEVMETHOD(bus_child_location,	mdio_child_location),
	DEVMETHOD(bus_child_pnpinfo,	ofw_bus_gen_child_pnpinfo),
	DEVMETHOD(bus_get_device_path,	ofw_bus_gen_get_device_path),
	DEVMETHOD(bus_read_ivar,	mdio_read_ivar),
	DEVMETHOD(bus_get_resource_list, mdio_get_resource_list),
	DEVMETHOD(bus_alloc_resource,	bus_generic_rl_alloc_resource),
	DEVMETHOD(bus_release_resource,	bus_generic_release_resource),
	DEVMETHOD(bus_get_resource,	bus_generic_rl_get_resource),
	DEVMETHOD(bus_set_resource,	bus_generic_rl_set_resource),
	DEVMETHOD(bus_setup_intr,	bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),

	/* ofw_bus interface */
	DEVMETHOD(ofw_bus_get_devinfo,	mdio_get_devinfo),
	DEVMETHOD(ofw_bus_get_compat,	ofw_bus_gen_get_compat),
	DEVMETHOD(ofw_bus_get_model,	ofw_bus_gen_get_model),
	DEVMETHOD(ofw_bus_get_name,	ofw_bus_gen_get_name),
	DEVMETHOD(ofw_bus_get_node,	ofw_bus_gen_get_node),
	DEVMETHOD(ofw_bus_get_type,	ofw_bus_gen_get_type),
#else
	DEVMETHOD(bus_add_child,	device_add_child_ordered),
#endif
	DEVMETHOD(bus_hinted_child,	mdio_hinted_child),

	/* MDIO access */
	DEVMETHOD(mdio_readreg,		mdio_readreg),
	DEVMETHOD(mdio_writereg,	mdio_writereg),
	DEVMETHOD(mdio_readextreg,	mdio_readextreg),
	DEVMETHOD(mdio_writeextreg,	mdio_writeextreg),

	DEVMETHOD_END
};

driver_t mdio_driver = {
	"mdio",
	mdio_methods,
	sizeof(struct mdio_softc)
};

MODULE_VERSION(mdio, 1);
