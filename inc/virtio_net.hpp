#pragma once

#include "kvm.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct virtqueue_state {
    uint16_t size = 0;
    bool ready = false;
    uint64_t descriptor_address = 0;
    uint64_t driver_address = 0;
    uint64_t device_address = 0;
    uint16_t last_available = 0;
};

/* One network device owns an RX queue (0) and a TX queue (1). */
struct virtio_net_device {
    static constexpr uint16_t queue_count = 2;
    static constexpr uint16_t queue_size = 8;

    int tap_fd = -1;
    std::string tap_name;
    std::array<uint8_t, 6> mac{};

    uint8_t status = 0;
    uint32_t device_features_select = 0;
    uint32_t driver_features_select = 0;
    uint64_t driver_features = 0;
    /* Bit 0 is raised for used buffers; the guest polls it in this MVP. */
    uint32_t interrupt_status = 0;
    uint32_t selected_queue = 0;
    std::array<virtqueue_state, queue_count> queues{};

    std::mutex device_mutex;
    std::mutex rx_mutex;
    std::deque<std::vector<uint8_t>> rx_frames;
    std::atomic<bool> stop_requested{false};
    std::thread tap_thread;
};

int virtio_net_init(vm& v, int vm_id);

void virtio_net_destroy(vm& v);

/* Handles one KVM_EXIT_MMIO.  Returns false when the address is not ours. */
bool virtio_net_handle_mmio(vm& v, struct kvm_run& run);
