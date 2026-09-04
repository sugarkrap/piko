STAGE       ?= ../../../build/target
HOSTBIN     ?= ../../../build/host/bin
TARGETBIN   ?= ../../../build/target/bin
CXX         ?= g++
CXXFLAGS    ?= -O2 -Wall -Wextra
FLTK_LDLIBS ?= -lXft -lXrender -lfontconfig -lXext -lX11 -lm -lpthread

HOSTCXX            ?= g++
HOSTCXXFLAGS       ?= -O2 -Wall -Wextra
HOST_FLTK_CXXFLAGS ?=
HOST_FLTK_LDFLAGS  ?= -lfltk

PIKOROM_DIR := ../libpikorom
HOSTCXXFLAGS += -I$(PIKOROM_DIR)
COMMON_HDRS := protocol.h transfer_state.h transfer_queue.h transfer_table.h net_io.h icon_xpm.h settings.h sha1.h base64.h websocket.h \
	$(PIKOROM_DIR)/emulation_db.h $(PIKOROM_DIR)/jar_meta.h $(PIKOROM_DIR)/rom_detect.h \
	$(PIKOROM_DIR)/crc32.h $(PIKOROM_DIR)/piko_asset_format.h

SERVER := $(TARGETBIN)/piko-sync-server
CLIENT := $(HOSTBIN)/piko-sync-client

.PHONY: all server client clean

all: server client

server: $(SERVER)

$(SERVER): piko-sync-server.cxx transfer_table.cxx $(COMMON_HDRS) $(PIKOROM_DIR)/pikorom.h
	@mkdir -p $(TARGETBIN)
	$(CXX) $(CXXFLAGS) \
	    -isystem $(STAGE)/usr/include -I$(PIKOROM_DIR) \
	    -o $@ piko-sync-server.cxx transfer_table.cxx \
	    -L$(STAGE)/usr/lib -Wl,-rpath-link=$(STAGE)/usr/lib \
	    -lpikorom -lfltk $(FLTK_LDLIBS)

client: $(CLIENT)

$(CLIENT): piko-sync-client.cxx transfer_table.cxx $(COMMON_HDRS) png_write.h
	@mkdir -p $(HOSTBIN)
	$(HOSTCXX) $(HOSTCXXFLAGS) $(HOST_FLTK_CXXFLAGS) \
	    -o $@ piko-sync-client.cxx transfer_table.cxx \
	    $(HOST_FLTK_LDFLAGS) -lz

clean:
	rm -f $(SERVER) $(CLIENT)

.PHONY: test
test:
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-protocol-test tests/protocol-test.cxx
	/tmp/piko-sync-protocol-test
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-settings-test tests/settings-test.cxx
	/tmp/piko-sync-settings-test
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-websocket-test tests/websocket-test.cxx
	/tmp/piko-sync-websocket-test
	$(HOSTCXX) $(HOSTCXXFLAGS) -o /tmp/piko-sync-device-info-test tests/device-info-test.cxx
	/tmp/piko-sync-device-info-test
