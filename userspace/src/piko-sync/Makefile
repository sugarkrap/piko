
STAGE       ?= ../../stage-target
CXX         ?= g++
CXXFLAGS    ?= -O2 -Wall -Wextra
FLTK_LDLIBS ?= -lXft -lXrender -lfontconfig -lXext -lX11 -lm -lpthread

HOSTCXX            ?= g++
HOSTCXXFLAGS       ?= -O2 -Wall -Wextra
HOST_FLTK_CXXFLAGS ?=
HOST_FLTK_LDFLAGS  ?= -lfltk

COMMON_HDRS := protocol.h emulation_db.h transfer_state.h transfer_queue.h transfer_table.h net_io.h icon_xpm.h settings.h jar_meta.h rom_detect.h

.PHONY: all server client clean

all: server client

server: piko-sync-server

piko-sync-server: piko-sync-server.cxx transfer_table.cxx $(COMMON_HDRS)
	$(CXX) $(CXXFLAGS) \
	    -isystem $(STAGE)/usr/include \
	    -o $@ piko-sync-server.cxx transfer_table.cxx \
	    -L$(STAGE)/usr/lib -Wl,-rpath-link=$(STAGE)/usr/lib \
	    -lfltk_images -lpng -lz -lfltk $(FLTK_LDLIBS)

client: piko-sync-client

piko-sync-client: piko-sync-client.cxx transfer_table.cxx $(COMMON_HDRS) png_write.h
	$(HOSTCXX) $(HOSTCXXFLAGS) $(HOST_FLTK_CXXFLAGS) \
	    -o $@ piko-sync-client.cxx transfer_table.cxx \
	    $(HOST_FLTK_LDFLAGS) -lz

clean:
	rm -f piko-sync-server piko-sync-client

.PHONY: test
test:
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-protocol-test tests/protocol-test.cxx
	/tmp/piko-sync-protocol-test
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-settings-test tests/settings-test.cxx
	/tmp/piko-sync-settings-test
