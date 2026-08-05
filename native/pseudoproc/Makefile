CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -g -pthread
LDFLAGS ?= -pthread

BUILD := build

.PHONY: all test clean

all: $(BUILD)/test_pproc $(BUILD)/test_psrv

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_pproc: tests/test_pproc.c pproc.c pproc.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_pproc.c pproc.c $(LDFLAGS)

$(BUILD)/test_psrv: tests/test_psrv.c pproc.c psrv.c pproc.h psrv.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_psrv.c pproc.c psrv.c $(LDFLAGS)

test: all
	$(BUILD)/test_pproc
	$(BUILD)/test_psrv

clean:
	rm -rf $(BUILD)
