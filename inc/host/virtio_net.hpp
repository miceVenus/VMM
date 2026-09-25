#pragma once

#include "host/kvm.hpp"
#include "host/tap_device.hpp"
#include "host/vhost_net.hpp"

#include <array>
#include <cstdint>
#include <string>

/*
 * Userspace Virtio-MMIO control plane backed by Linux vhost-net.
 *
 * This class keeps the MMIO registers and the device configuration visible
 * to the Guest. Linux vhost-net owns the split-ring data path and exchanges
 * packets directly with the TAP fd. KVM ioeventfds route QUEUE_NOTIFY writes
 * to vhost; resampling KVM IRQFD routes a shared vhost completion eventfd
 * through the in-kernel PIC to the Guest. The VMM derives Virtio-MMIO
 * interrupt status from the vhost-updated used rings.
 */
class virtio_net {
public:
    virtio_net(vm& owner, int vm_id);
    ~virtio_net();

    virtio_net(const virtio_net&) = delete;
    virtio_net& operator=(const virtio_net&) = delete;

    bool initialize();

    /* Handles one KVM_EXIT_MMIO from the Virtio-MMIO register window. */
    bool handle_mmio(struct kvm_run& run);

    const std::string& tap_name() const noexcept;

private:
    static constexpr uint16_t queue_count = 2;
    static constexpr uint16_t queue_size_max = 8;
    static constexpr uint64_t version_1_feature =
        UINT64_C(1) << VIRTIO_F_VERSION_1;
    static constexpr uint64_t mac_feature =
        UINT64_C(1) << VIRTIO_NET_F_MAC;
    static constexpr uint64_t offered_features =
        version_1_feature | mac_feature;

    struct queue_config {
        uint16_t size = 0;
        bool ready = false;
        uint64_t descriptor_address = 0;
        uint64_t available_ring_address = 0;
        uint64_t used_ring_address = 0;
        uint16_t acknowledged_used_index = 0;
        int kick_fd = -1;
        bool ioeventfd_registered = false;
    };

    bool configure_vhost();
    void stop_vhost() noexcept;
    bool configure_vhost_queue(uint16_t index);
    bool queue_memory_is_valid(const queue_config& queue) const;
    bool register_queue_ioeventfd(uint16_t index);
    bool register_irqfd();

    static uint64_t load_little_endian(const uint8_t* bytes,
                                       uint32_t length);
    static void store_little_endian(uint8_t* bytes,
                                    uint32_t length,
                                    uint64_t value);
    static void set_address_part(uint64_t& address,
                                 bool high,
                                 uint32_t value) noexcept;

    uint64_t read_register(uint32_t offset) const;
    uint32_t pending_interrupt_status() const;
    uint16_t used_ring_index(const queue_config& queue) const;
    uint64_t read_mmio_value(uint32_t offset, uint32_t length) const;
    void acknowledge_interrupt(uint32_t value);
    void reset_device();
    void write_status(uint8_t value);
    void write_register(uint32_t offset, uint64_t value);
    queue_config* selected_queue();
    const queue_config* selected_queue() const;

    vm& owner_;
    int vm_id_;
    tap_device tap_;
    vhost_net backend_;
    int call_fd_ = -1;
    int resample_fd_ = -1;
    std::array<queue_config, queue_count> queues_{};
    std::array<uint8_t, 6> mac_{};

    uint8_t status_ = 0;
    uint32_t device_features_select_ = 0;
    uint32_t driver_features_select_ = 0;
    uint64_t driver_features_ = 0;
    /* Bit 0 is raised for used buffers and acknowledged by the Guest. */
    uint32_t interrupt_status_ = 0;
    uint32_t selected_queue_ = 0;

    bool vhost_started_ = false;
    bool irqfd_registered_ = false;

};
