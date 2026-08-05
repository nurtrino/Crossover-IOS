# wineforge — the Wine fork workbench (host-first).
# Requires a configured+built wine tree with the fork patches applied:
#   ../wine        pinned Wine submodule (wine-11.0) + native/patches applied
#   ../wine-build  out-of-tree build with server/*.o present
# See README.md and ../patches/README.md.

WINE_BUILD ?= ../wine-build
WINE_SRC ?= ../wine
SERVER_OBJS := $(filter-out $(WINE_BUILD)/server/main.o,$(wildcard $(WINE_BUILD)/server/*.o))

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -g -pthread
BUILD := build

# wineserver locates its nls data relative to /proc/self/exe; a binary whose
# directory ends in /server/ looks in ../nls. We reproduce that layout:
# build/server/<test> + build/nls -> wine source nls. The iOS bundle must
# reproduce the same relative layout for its resources.
.PHONY: all test clean

all: $(BUILD)/server/embed_test $(BUILD)/server/embed_run_test $(BUILD)/nls

$(BUILD)/server:
	mkdir -p $(BUILD)/server

$(BUILD)/nls: | $(BUILD)/server
	ln -sfn $(abspath $(WINE_SRC))/nls $(BUILD)/nls

# main.o defines main(); neutralize it so our harness provides main().
# (Experiment 001 legacy: embed_test also uses the renamed wineserver_main.)
$(BUILD)/server/main_lib.o: $(WINE_BUILD)/server/main.o | $(BUILD)/server
	objcopy --redefine-sym main=wineserver_main $< $@

$(BUILD)/server/embed_test: embed_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ embed_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS) \
		-Wl,--wrap=exit -pthread

# Experiment 003: uses the patched, source-level wineserver_run() entry.
$(BUILD)/server/embed_run_test: embed_run_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ embed_run_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS) -pthread

test: all
	$(BUILD)/server/embed_test
	$(BUILD)/server/embed_run_test

clean:
	rm -rf $(BUILD)
