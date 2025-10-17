APP_NAME := mini-dpdk
SRC := app/mini/main.c
BUILD_DIR := build
BIN := $(BUILD_DIR)/$(APP_NAME)
MY_REPO := paramoshka

CC ?= cc
CSTD ?= gnu11
CFLAGS ?= -O3 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

PKGCONF ?= pkg-config
DPDK_PC ?= libdpdk

DPDK_CFLAGS := $(shell $(PKGCONF) --cflags $(DPDK_PC) 2>/dev/null)
DPDK_LDFLAGS := $(shell $(PKGCONF) --libs $(DPDK_PC) 2>/dev/null)

.PHONY: all clean run docker-build

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p $(BUILD_DIR)
	# Ensure dpdk bus/driver libs are linked in despite --as-needed
	$(CC) -std=$(CSTD) $(CFLAGS) $(DPDK_CFLAGS) -Wl,--no-as-needed -o $@ $< \
		$(DPDK_LDFLAGS) -lrte_bus_pci -lrte_bus_vdev -lrte_mempool_ring -Wl,--as-needed
	@echo "Built $@"

clean:
	rm -rf $(BUILD_DIR)

# Example run: make run ARGS="-l 0-1 -a 0000:af:00.1 -a 0000:af:00.2 --"
run: $(BIN)
	sudo $< $(ARGS)

docker-build:
	docker build -t $(MY_REPO)/$(APP_NAME):latest .
