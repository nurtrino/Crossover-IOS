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

WINE_LOADER ?= $(abspath $(WINE_BUILD))/loader/wine
WINESERVER_BIN ?= $(abspath $(WINE_BUILD))/server/wineserver

all: $(BUILD)/server/embed_test $(BUILD)/server/embed_run_test \
     $(BUILD)/server/real_client_test $(BUILD)/server/real_multi_test \
     $(BUILD)/peload_test $(BUILD)/mkexe $(BUILD)/nls

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

# Experiment 002 (reframed): real Wine client against the in-thread server.
$(BUILD)/server/real_client_test: real_client_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ real_client_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS) -pthread

# Experiment 004: many concurrent real Wine clients, one in-thread server.
$(BUILD)/server/real_multi_test: real_multi_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ real_multi_test.c $(BUILD)/server/main_lib.o $(SERVER_OBJS) -pthread

# Self-contained PE mapping/relocation test (no wine objects needed).
$(BUILD)/peload_test: peload_test.c | $(BUILD)/server
	$(CC) $(CFLAGS) -o $@ peload_test.c

# Emits an import-free Win64 .exe: the program an in-process child can run
# today (experiment 0003f). No wine objects needed.
$(BUILD)/mkexe: mkexe.c | $(BUILD)/server
	$(CC) $(CFLAGS) -o $@ mkexe.c

test: all
	$(BUILD)/server/embed_test
	$(BUILD)/server/embed_run_test
	$(BUILD)/peload_test

# Require the full Wine build (loader + ntdll). Not in `test` since they are
# heavier and environment-sensitive; run explicitly.
test-real: $(BUILD)/server/real_client_test $(BUILD)/server/real_multi_test $(BUILD)/nls
	$(BUILD)/server/real_client_test $(WINESERVER_BIN) $(WINE_LOADER) wineboot.exe
	$(BUILD)/server/real_multi_test $(WINESERVER_BIN) $(WINE_LOADER) 4 wineboot.exe
	./embedded_server_test.sh
	./spawn_seam_test.sh
	./inproc_run_test.sh
	./m1_notepad_test.sh

clean:
	rm -rf $(BUILD)
