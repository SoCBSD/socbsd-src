.PATH:	${SRCTOP}/sys/dev/etherswitch/mtkswitch

KMOD=	mtkswitch
SRCS=	mtkswitch.c
SRCS+=	mtkswitch_mt7531.c
SRCS+=	mtkswitch_mt7620.c
SRCS+=	mtkswitch_rt3050.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h
SRCS+=	etherswitch_if.h mdio_if.h miibus_if.h
SRCS+=	opt_platform.h

CFLAGS+=	-I${SRCTOP}/sys/dev/etherswitch

.include <bsd.kmod.mk>
