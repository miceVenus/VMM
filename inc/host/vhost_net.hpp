#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

struct vhost_vring_config {
    uint32_t index;
    uint32_t size;
    uint64_t descriptor_userspace_address;
    uint64_t available_userspace_address;
    uint64_t used_userspace_address;
    int kick_fd;
    int call_fd;
};

/* Owns /dev/vhost-net and connects its two queues to a TAP fd. */
class vhost_net {
public:
    vhost_net() = default;
    ~vhost_net();

    vhost_net(const vhost_net&) = delete;
    vhost_net& operator=(const vhost_net&) = delete;

    bool initialize(void* guest_memory, size_t guest_memory_size);
    bool set_features(uint64_t negotiated_features);
    bool configure_queue(const vhost_vring_config& queue, int tap_fd);
    void reset() noexcept;

private:
    int fd_ = -1;
    uint64_t features_ = 0;
    std::array<bool, 2> backend_attached_{};
    bool owner_set_ = false;
    bool initialized_ = false;
};
