CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS ?= -D_DEFAULT_SOURCE
LDFLAGS ?=

TARGET := mlxnicd
SRC := src/main.c src/pci.c src/vfio.c src/raw.c src/mlx5.c
OBJ := $(SRC:.c=.o)

REMOTE_HOST ?= sdn-svr6
REMOTE_DIR ?= ~/work/takagi/nicd

.PHONY: all clean sync remote-run remote-probe remote-vfio-check remote-vfio-probe remote-raw-loop-preflight

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c src/pci.h src/vfio.h src/vfio_compat.h src/raw.h src/mlx5.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJ)

sync:
	REMOTE_HOST='$(REMOTE_HOST)' REMOTE_DIR='$(REMOTE_DIR)' scripts/sync.sh

remote-run: sync
	ssh $(REMOTE_HOST) 'cd $(REMOTE_DIR) && make && ./$(TARGET) list'

remote-probe: sync
	ssh $(REMOTE_HOST) 'cd $(REMOTE_DIR) && scripts/remote_probe.sh'

remote-vfio-check: sync
	ssh $(REMOTE_HOST) 'cd $(REMOTE_DIR) && make && ./$(TARGET) vfio-check 0000:01:00.0'

remote-vfio-probe: sync
	ssh $(REMOTE_HOST) 'cd $(REMOTE_DIR) && make && sudo ./$(TARGET) vfio-probe 0000:01:00.0'

remote-raw-loop-preflight: sync
	ssh $(REMOTE_HOST) 'cd $(REMOTE_DIR) && make && ./$(TARGET) raw-loop --bdf 0000:01:00.0 --peer-if eth2 --src-mac 02:00:00:00:00:01 --dst-mac 02:00:00:00:00:02 --ethertype 0x88b5'
