CXX := g++
CC  := gcc
LD  := ld

INC_DIR   := inc
SRC_DIR   := src
TEST_DIR  := test
BUILD_DIR := build
BIN_DIR   := bin

HV_BIN := $(BIN_DIR)/mini_hypervisor.a
GUEST_NAME := testN1
GUEST_IMAGE := $(BIN_DIR)/$(GUEST_NAME).img
GUEST_OBJECTS := $(BUILD_DIR)/$(GUEST_NAME)/guest.o \
		$(BUILD_DIR)/guest_console.o \
		$(BUILD_DIR)/virtio_net_guest.o
HOST_SOURCES := $(wildcard $(SRC_DIR)/*.cpp)

CXXFLAGS    := -std=c++17 -O2 -g -Wall -Wextra -I$(INC_DIR)
LDFLAGS_HV  := -lpthread

GUEST_CFLAGS := -m64 -ffreestanding -fno-pic -fno-builtin -Wall -Wextra \
		-I$(INC_DIR) -DGUEST_BUILD

DEFAULT_GUEST_LD := guest.ld

.PRECIOUS: $(BUILD_DIR)/$(GUEST_NAME)/guest.o
.PHONY: all hypervisor tests clean list $(GUEST_NAME)

all: hypervisor tests

list:
	@echo "Hypervisor: $(HV_BIN)"
	@echo "Guest: $(GUEST_IMAGE)"

$(BIN_DIR) $(BUILD_DIR):
	mkdir -p $@

hypervisor: $(HV_BIN)
	@echo "\033[32m[Hypervisor]\033[0m Compilation finished successfully!"

$(BUILD_DIR)/guest_console.o: $(SRC_DIR)/guest_console.c | $(BUILD_DIR)
	$(CC) $(GUEST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/virtio_net_guest.o: $(SRC_DIR)/virtio_net_guest.c | $(BUILD_DIR)
	$(CC) $(GUEST_CFLAGS) -c $< -o $@


$(HV_BIN): $(HOST_SOURCES) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS_HV)

tests: $(GUEST_NAME)

$(GUEST_NAME): $(GUEST_IMAGE)

$(BUILD_DIR)/$(GUEST_NAME)/guest.o: $(TEST_DIR)/$(GUEST_NAME)/guest.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(GUEST_CFLAGS) -c $< -o $@


$(GUEST_IMAGE): $(GUEST_OBJECTS) | $(BIN_DIR)
	echo "LD  $@"
	$(LD) -T "$(DEFAULT_GUEST_LD)" $^ -o "$@"
	@echo "\033[32m[$(GUEST_NAME)]\033[0m Compilation finished successfully!"

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
