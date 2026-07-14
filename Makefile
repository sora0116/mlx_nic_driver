CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS ?= -D_DEFAULT_SOURCE -Iinclude
LDFLAGS ?= -pthread
AR ?= ar

TARGET := mlxnicd
LIB := libmlxnicd.a
EXAMPLE_API := examples/api_loop_minimal
LIB_SRC := src/pci.c src/vfio.c src/mlx5.c src/mlx5_debug.c
LIB_OBJ := $(LIB_SRC:.c=.o)
APP_SRC := src/main.c src/raw.c src/sample.c
APP_OBJ := $(APP_SRC:.c=.o)
EXAMPLE_SRC := examples/api_loop_minimal.c
EXAMPLE_OBJ := $(EXAMPLE_SRC:.c=.o)

REMOTE_HOST ?= sdn-svr6
REMOTE_DIR ?= ~/work/takagi/nicd

.PHONY: all clean sync remote-run remote-probe remote-vfio-check remote-vfio-probe remote-raw-loop-preflight examples

all: $(TARGET)

examples: $(EXAMPLE_API)

$(TARGET): $(APP_OBJ) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(APP_OBJ) -L. -lmlxnicd

$(EXAMPLE_API): $(EXAMPLE_OBJ) $(LIB)
	$(CC) $(LDFLAGS) -o $@ $(EXAMPLE_OBJ) -L. -lmlxnicd

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

%.o: %.c src/pci.h src/vfio.h src/vfio_compat.h src/raw.h src/mlx5.h src/mlx5_priv.h include/mlxnicd.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(LIB) $(APP_OBJ) $(LIB_OBJ) $(EXAMPLE_OBJ) $(EXAMPLE_API)

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
