# mb-irda
#
# Plain make rather than autotools, same rationale as mb-volume and
# mb-applet-card: one source file, one pkg-config module, and the
# surrounding project cross-compiles everything by hand anyway (see
# docs/HOWTO-MATCHBOX-DESKTOP.md).
#
# Native build:
#   make
#
# Cross build for the Zaurus, matching the rest of the X/Matchbox stack:
#   HOST=arm-unknown-linux-uclibcgnueabi
#   STAGE=<repo>/userspace/stage-target
#   export PKG_CONFIG_SYSROOT_DIR="$STAGE"
#   export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig"
#   make CC=$HOST-gcc \
#        CPPFLAGS="-I$STAGE/usr/include" \
#        LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"
#   make DESTDIR=/tmp/irda-stage prefix=/usr install
#
# No ALSA here, unlike mb-volume -- this applet talks to ird over a unix
# socket and touches no device of its own.

PACKAGE     = mb-irda
prefix     ?= /usr
bindir      = $(prefix)/bin
datadir     = $(prefix)/share
pixmapsdir  = $(datadir)/pixmaps
desktopdir  = $(datadir)/applications

PKG_CONFIG ?= pkg-config
CC         ?= cc
INSTALL    ?= install

LIBMB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libmb)
LIBMB_LIBS   := $(shell $(PKG_CONFIG) --libs libmb)

WARN     = -Wall -W -Wstrict-prototypes -Wmissing-prototypes
CFLAGS  ?= -Os
ALL_CFLAGS = $(CFLAGS) $(WARN) $(LIBMB_CFLAGS) $(CPPFLAGS)

OBJS = mb-irda.o

all: $(PACKAGE)

$(PACKAGE): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBMB_LIBS)

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

# The panel finds the tray icon by name through
# mb_dot_desktop_icon_get_full_path(), which searches the theme directories
# and then $(datadir)/pixmaps -- so that is where the PNG has to land.
install: $(PACKAGE)
	$(INSTALL) -d $(DESTDIR)$(bindir)
	$(INSTALL) -m 755 $(PACKAGE) $(DESTDIR)$(bindir)/$(PACKAGE)
	$(INSTALL) -d $(DESTDIR)$(pixmapsdir)
	$(INSTALL) -m 644 icons/$(PACKAGE).png $(DESTDIR)$(pixmapsdir)/$(PACKAGE).png
	$(INSTALL) -d $(DESTDIR)$(desktopdir)
	$(INSTALL) -m 644 $(PACKAGE).desktop $(DESTDIR)$(desktopdir)/$(PACKAGE).desktop

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(PACKAGE)
	rm -f $(DESTDIR)$(pixmapsdir)/$(PACKAGE).png
	rm -f $(DESTDIR)$(desktopdir)/$(PACKAGE).desktop

clean:
	rm -f $(PACKAGE) $(OBJS)

.PHONY: all install uninstall clean
