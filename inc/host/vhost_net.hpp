#pragma once

#include "host/vhost.hpp"

#include <cstdint>
#include <vector>

/* vhost-net-specific adapter: attaches a generic vhost vring to a TAP fd. */
class vhost_net {
public:
    vhost_net() = default;
    ~vhost_net();

    vhost_net(const vhost_net&) = delete;
    vhost_net& operator=(const vhost_net&) = delete;

    bool initialize(void* guest_memory, size_t guest_memory_size);
    bool set_features(uint64_t negotiated_features);
    bool configure_queue(const vhost_vring_config& queue, int tap_fd);
    bool reset() noexcept;

    uint64_t features() const noexcept;

private:
    vhost_device device_;
    std::vector<bool> backend_attached_;
    bool initialized_ = false;
};
