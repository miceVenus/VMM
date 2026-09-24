CXX := g++
CC  := gcc
LD  := ld

INC_DIR   := inc
SRC_DIR   := src
TEST_DIR  := test
BUILD_DIR := build
BIN_DIR   := bin
GUEST_BUILD_DIR := $(BUILD_DIR)/guest

HV_BIN := $(BIN_DIR)/mini_hypervisor.a
GUEST_NAME := testN1
GUEST_IMAGE := $(BIN_DIR)/$(GUEST_NAME).img
GUEST_OBJECTS := $(BUILD_DIR)/$(GUEST_NAME)/guest.o \
		$(GUEST_BUILD_DIR)/guest_console.o \
		$(GUEST_BUILD_DIR)/guest_dma.o \
		$(GUEST_BUILD_DIR)/virtio_net.o \
		$(GUEST_BUILD_DIR)/network_stack.o
HOST_SOURCES := $(wildcard $(SRC_DIR)/host/*.cpp)

# Set UDP_ECHO_IP to a numeric IPv4 value; zero uses the TAP gateway.
UDP_ECHO_IP ?= 0
UDP_ECHO_PORT ?= 9999
GUEST_TEST_CFLAGS = $(GUEST_CFLAGS) \
		-DTEST_UDP_ECHO_IP=$(UDP_ECHO_IP) \
		-DTEST_UDP_ECHO_PORT=$(UDP_ECHO_PORT)

CXXFLAGS    := -std=c++17 -O2 -g -Wall -Wextra -I$(INC_DIR)
LDFLAGS_HV  := -lpthread

GUEST_CFLAGS := -m64 -mno-red-zone -ffreestanding -fno-pic -fno-builtin -Wall -Wextra \
		-I$(INC_DIR) -DGUEST_BUILD

DEFAULT_GUEST_LD := guest.ld

.PRECIOUS: $(BUILD_DIR)/$(GUEST_NAME)/guest.o
.PHONY: all hypervisor tests clean list $(GUEST_NAME)

all: hypervisor tests

list:
	@echo "Hypervisor: $(HV_BIN)"
	@echo "Guest: $(GUEST_IMAGE)"

$(BIN_DIR) $(BUILD_DIR) $(GUEST_BUILD_DIR):
	mkdir -p $@

hypervisor: $(HV_BIN)
	@echo "\033[32m[Hypervisor]\033[0m Compilation finished successfully!"

$(GUEST_BUILD_DIR)/guest_console.o: $(SRC_DIR)/guest/guest_console.c | $(GUEST_BUILD_DIR)
	$(CC) $(GUEST_CFLAGS) -c $< -o $@

$(GUEST_BUILD_DIR)/guest_dma.o: $(SRC_DIR)/guest/guest_dma.c | $(GUEST_BUILD_DIR)
	$(CC) $(GUEST_CFLAGS) -c $< -o $@

$(GUEST_BUILD_DIR)/virtio_net.o: $(SRC_DIR)/guest/virtio_net.c | $(GUEST_BUILD_DIR)
	$(CC) $(GUEST_CFLAGS) -c $< -o $@

$(GUEST_BUILD_DIR)/network_stack.o: $(SRC_DIR)/guest/network_stack.c | $(GUEST_BUILD_DIR)
	$(CC) $(GUEST_CFLAGS) -c $< -o $@

$(HV_BIN): $(HOST_SOURCES) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS_HV)

tests: $(GUEST_NAME) $(BIN_DIR)/network_stack_unit
	$(BIN_DIR)/network_stack_unit

$(BIN_DIR)/network_stack_unit: $(TEST_DIR)/network_stack_unit.c $(SRC_DIR)/guest/network_stack.c | $(BIN_DIR)
	$(CC) -std=c11 -Wall -Wextra -I$(INC_DIR) -DGUEST_BUILD $^ -o $@

$(GUEST_NAME): $(GUEST_IMAGE)

$(BUILD_DIR)/$(GUEST_NAME)/guest.o: $(TEST_DIR)/$(GUEST_NAME)/guest.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(GUEST_TEST_CFLAGS) -c $< -o $@

$(GUEST_IMAGE): $(GUEST_OBJECTS) | $(BIN_DIR)
	echo "LD  $@"
	$(LD) -T "$(DEFAULT_GUEST_LD)" $^ -o "$@"
	@echo "\033[32m[$(GUEST_NAME)]\033[0m Compilation finished successfully!"

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
