CC ?= cc
CFLAGS ?= -O3 -g -Wall -Wextra -Wpedantic
PKG_CONFIG ?= pkg-config

TARGET := dpdk-macswap-peer
DPDK_CFLAGS := $(shell $(PKG_CONFIG) --cflags libdpdk)
DPDK_LIBS := $(shell $(PKG_CONFIG) --libs libdpdk)

.PHONY: all clean

all: $(TARGET)

$(TARGET): dpdk_macswap.c
	$(CC) $(CFLAGS) $(DPDK_CFLAGS) -o $@ $< $(DPDK_LIBS)

clean:
	rm -f $(TARGET)
