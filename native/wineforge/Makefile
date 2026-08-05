# wineforge — surgery on real Wine objects, host-first.
# Requires a configured+built wine tree (see README.md):
#   ../wine        pinned Wine source (submodule)
#   ../wine-build  out-of-tree build with server/*.o present

WINE_BUILD ?= ../wine-build
WINE_SRC ?= ../wine
SERVER_OBJS := $(filter-out $(WINE_BUILD)/server/main.o,$(wildcard $(WINE_BUILD)/server/*.o))

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -g -pthread
BUILD := build

# wineserver locates its nls data relative to /proc/self/exe (not argv[0]);
# a binary whose directory ends in /server/ looks in ../nls. We reproduce
# that layout: build/server/embed_test + build/nls -> wine source nls.
# The iOS bundle must reproduce the same relative layout for its resources.
.PHONY: all test clean

all: $(BUILD)/server/embed_test $(BUILD)/nls

$(BUILD)/server:
	mkdir -p $(BUILD)/server

$(BUILD)/nls: | $(BUILD)/server
	ln -sfn $(abspath $(WINE_SRC))/nls $(BUILD)/nls

# The one-symbol rename that turns wineserver into a callable library.
$(BUILD)/server/wineserver_main.o: $(WINE_BUILD)/server/main.o | $(BUILD)/server
	objcopy --redefine-sym main=wineserver_main $< $@

$(BUILD)/server/embed_test: embed_test.c $(BUILD)/server/wineserver_main.o $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ embed_test.c $(BUILD)/server/wineserver_main.o $(SERVER_OBJS) \
		-Wl,--wrap=exit -pthread

test: all
	$(BUILD)/server/embed_test

clean:
	rm -rf $(BUILD)
