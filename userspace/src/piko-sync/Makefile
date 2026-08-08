# piko-sync -- resilient file transfer between the host and the Zaurus.
#
# Two binaries share this Makefile:
#   piko-sync-server   cross-compiled against the staged FLTK/X11 tree,
#                     runs ON the Zaurus (see docs/HOWTO-FLTK.md in piko).
#   piko-sync-client   built against the HOST's own FLTK, runs on the
#                     build machine.
#
# Normally built by tools/build-piko-sync.sh in the piko repo, which knows
# where the cross toolchain and the staged FLTK/X11 tree live. This
# Makefile exists so either binary can also be built directly:
#
#   make server STAGE=/path/to/userspace/stage-target \
#               CXX=arm-unknown-linux-uclibcgnueabi-g++
#   make client HOST_FLTK_CXXFLAGS="$(fltk-config --cxxflags)" \
#               HOST_FLTK_LDFLAGS="$(fltk-config --ldflags)"
#
# FLTK_LDLIBS should be the LDLIBS line out of the FLTK build's
# makeinclude for the server -- see pikostore/Makefile for why
# (re-deriving it by hand would drift from build-fltk.sh's --disable-*
# choices).

STAGE       ?= ../../stage-target
CXX         ?= g++
CXXFLAGS    ?= -O2 -Wall -Wextra
FLTK_LDLIBS ?= -lXft -lXrender -lfontconfig -lXext -lX11 -lm -lpthread

HOSTCXX            ?= g++
HOSTCXXFLAGS       ?= -O2 -Wall -Wextra
HOST_FLTK_CXXFLAGS ?=
HOST_FLTK_LDFLAGS  ?= -lfltk

COMMON_HDRS := protocol.h transfer_state.h transfer_queue.h transfer_table.h net_io.h icon_xpm.h settings.h

.PHONY: all server client clean

all: server client

server: piko-sync-server

# -isystem, not -I: FLTK 1.3's own headers emit hundreds of
# -Wunused-parameter warnings that would bury any real warning in this
# app's own code -- same reasoning as pikostore/Makefile.
piko-sync-server: piko-sync-server.cxx transfer_table.cxx $(COMMON_HDRS)
	$(CXX) $(CXXFLAGS) \
	    -isystem $(STAGE)/usr/include \
	    -o $@ piko-sync-server.cxx transfer_table.cxx \
	    -L$(STAGE)/usr/lib -Wl,-rpath-link=$(STAGE)/usr/lib \
	    -lfltk $(FLTK_LDLIBS)

client: piko-sync-client

piko-sync-client: piko-sync-client.cxx transfer_table.cxx $(COMMON_HDRS)
	$(HOSTCXX) $(HOSTCXXFLAGS) $(HOST_FLTK_CXXFLAGS) \
	    -o $@ piko-sync-client.cxx transfer_table.cxx \
	    $(HOST_FLTK_LDFLAGS)

clean:
	rm -f piko-sync-server piko-sync-client

.PHONY: test
test:
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-protocol-test tests/protocol-test.cxx
	/tmp/piko-sync-protocol-test
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-settings-test tests/settings-test.cxx
	/tmp/piko-sync-settings-test
