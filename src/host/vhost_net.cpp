#include "host/vhost_net.hpp"

#include <cstdio>
/* vhost_types.h includes virtio_ring.h; this VMM only needs the UAPI layouts. */
#define VIRTIO_RING_NO_LEGACY
#include <linux/vhost.h>
#include <sys/ioctl.h>

namespace {

constexpr const char* vhost_net_path = "/dev/vhost-net";
constexpr uint64_t version_1_feature = UINT64_C(1) << VIRTIO_F_VERSION_1;

} // namespace

vhost_net::~vhost_net() {
    reset();
}

bool vhost_net::initialize(void* guest_memory, size_t guest_memory_size) {
    if (initialized_) return true;
    if (guest_memory == nullptr || guest_memory_size == 0 ||
        !device_.open(vhost_net_path, "open /dev/vhost-net")) {
        return false;
    }

    const vhost_memory_region_config region{
        0,
        static_cast<uint64_t>(guest_memory_size),
        reinterpret_cast<uintptr_t>(guest_memory),
    };
    if (!device_.initialize(&region, 1)) return false;

    if ((device_.features() & version_1_feature) == 0) {
        std::fprintf(stderr,
                     "vhost-net does not support the required VERSION_1 feature.\n");
        device_.reset();
        return false;
    }

    initialized_ = true;
    return true;
}

bool vhost_net::set_features(uint64_t negotiated_features) {
    if (!initialized_) return false;
    return device_.set_features(negotiated_features & device_.features());
}

bool vhost_net::configure_queue(const vhost_vring_config& queue,
                                int tap_fd) {
    if (!initialized_ || tap_fd < 0) return false;
    if (!device_.configure_vring(queue)) return false;

    if (backend_attached_.size() <= queue.index) {
        backend_attached_.resize(static_cast<size_t>(queue.index) + 1, false);
    }

    struct vhost_vring_file backend{};
    backend.index = queue.index;
    backend.fd = tap_fd;
    if (::ioctl(device_.fd(), VHOST_NET_SET_BACKEND, &backend) < 0) {
        std::perror("VHOST_NET_SET_BACKEND");
        return false;
    }
    backend_attached_[queue.index] = true;
    return true;
}

bool vhost_net::reset() noexcept {
    bool success = true;
    for (size_t index = 0; index < backend_attached_.size(); ++index) {
        if (!backend_attached_[index]) continue;

        struct vhost_vring_file backend{};
        backend.index = static_cast<uint32_t>(index);
        backend.fd = -1;
        if (::ioctl(device_.fd(), VHOST_NET_SET_BACKEND, &backend) < 0) {
            std::perror("VHOST_NET_SET_BACKEND detach");
            success = false;
        } else {
            backend_attached_[index] = false;
        }
    }

    if (!device_.reset()) return false;
    initialized_ = false;
    backend_attached_.clear();
    return success;
}

uint64_t vhost_net::features() const noexcept {
    return device_.features();
}
