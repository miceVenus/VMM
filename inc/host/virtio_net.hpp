#pragma once

#include "host/kvm.hpp"
#include "host/tap_device.hpp"
#include "host/virtqueue.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*
 * Host-side model of one Virtio-net device.
 *
 * The Guest sees this object through the Virtio-MMIO registers. The object
 * owns the Host TAP backend and the state needed to interpret the Guest's
 * Virtqueues. Queue-layout structures are private implementation details.
 */
class virtio_net {
public:
    virtio_net(vm& owner, int vm_id);
    ~virtio_net();

    virtio_net(const virtio_net&) = delete;
    virtio_net& operator=(const virtio_net&) = delete;

    bool initialize();

    /* Handles one KVM_EXIT_MMIO. */
    bool handle_mmio(struct kvm_run& run);

    const std::string& tap_name() const noexcept;

private:
    static constexpr uint16_t queue_count = 2;
    static constexpr uint16_t queue_size = 8;
    static constexpr uint64_t version_1_feature =
        UINT64_C(1) << VIRTIO_F_VERSION_1;
    static constexpr uint64_t mac_feature =
        UINT64_C(1) << VIRTIO_NET_F_MAC;
    static constexpr uint64_t offered_features =
        version_1_feature | mac_feature;
    static constexpr size_t virtio_net_header_size = 10;
    static constexpr size_t max_frame_size = 65535;
    static constexpr size_t max_packet_size =
        max_frame_size + virtio_net_header_size;
    static constexpr size_t max_pending_rx_frames = 256;

    static uint64_t load_little_endian(const uint8_t* bytes, uint32_t length);
    static void store_little_endian(uint8_t* bytes,
                                    uint32_t length,
                                    uint64_t value);

    void wake_vcpu();
    void inject_guest_interrupt();

    void tap_reader();

    bool take_pending_frame(std::vector<uint8_t>& frame);
    void mark_device_failed();

    void process_receive_queue();
    bool write_tap_frame(const std::vector<uint8_t>& bytes);
    void process_transmit_queue();
    void process_queue(uint16_t queue_index);

    virtqueue* selected_queue();
    const virtqueue* selected_queue() const;
    uint64_t read_register(uint32_t offset) const;
    uint64_t read_mmio_value(uint32_t offset, uint32_t length) const;
    void reset_device();
    void write_register(uint32_t offset, uint64_t value);

    vm& owner_;
    int vm_id_;
    tap_device tap_;
    virtqueue rx_queue_;
    virtqueue tx_queue_;
    std::array<uint8_t, 6> mac_{};

    uint8_t status_ = 0;
    uint32_t device_features_select_ = 0;
    uint32_t driver_features_select_ = 0;
    uint64_t driver_features_ = 0;
    /* Bit 0 is raised for used buffers and acknowledged by the Guest. */
    uint32_t interrupt_status_ = 0;
    uint32_t selected_queue_ = 0;

    std::mutex device_mutex_;
    std::mutex rx_mutex_;
    std::deque<std::vector<uint8_t>> rx_frames_;
    std::atomic<bool> stop_requested_{false};
    std::thread tap_thread_;
};
