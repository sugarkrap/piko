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
