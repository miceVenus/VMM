#include "host/vhost_net.hpp"

#include <cstdio>
#include <fcntl.h>
/* vhost_types.h includes virtio_ring.h; this VMM only needs the UAPI layouts. */
#define VIRTIO_RING_NO_LEGACY
#include <linux/vhost.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr uint64_t version_1_feature = UINT64_C(1) << VIRTIO_F_VERSION_1;

/* VHOST_SET_MEM_TABLE expects a header followed by its regions in one block. */
struct single_region_memory {
    uint32_t nregions;
    uint32_t padding;
    struct vhost_memory_region regions[1];
};

static_assert(offsetof(single_region_memory, regions) ==
              offsetof(struct vhost_memory, regions));

bool vhost_ioctl(int fd, unsigned long request, void* argument,
                 const char* operation) {
    if (::ioctl(fd, request, argument) == 0) return true;
    std::perror(operation);
    return false;
}

} // namespace

vhost_net::~vhost_net() {
    reset();
    if (fd_ >= 0) ::close(fd_);
}

bool vhost_net::initialize(void* guest_memory, size_t guest_memory_size) {
    if (initialized_) return true;
    if (guest_memory == nullptr || guest_memory_size == 0) return false;

    if (fd_ < 0) {
        fd_ = ::open("/dev/vhost-net", O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            std::perror("open /dev/vhost-net");
            return false;
        }
    }

    if (::ioctl(fd_, VHOST_SET_OWNER, 0) < 0) {
        std::perror("VHOST_SET_OWNER");
        return false;
    }
    owner_set_ = true;

    if (!vhost_ioctl(fd_, VHOST_GET_FEATURES, &features_,
                     "VHOST_GET_FEATURES")) {
        reset();
        return false;
    }

    single_region_memory memory{};
    memory.nregions = 1;
    memory.regions[0].guest_phys_addr = 0;
    memory.regions[0].memory_size = guest_memory_size;
    memory.regions[0].userspace_addr =
        reinterpret_cast<uintptr_t>(guest_memory);
    if (!vhost_ioctl(fd_, VHOST_SET_MEM_TABLE, &memory,
                     "VHOST_SET_MEM_TABLE")) {
        reset();
        return false;
    }

    if ((features_ & version_1_feature) == 0) {
        std::fprintf(stderr,
                     "vhost-net does not support the required VERSION_1 feature.\n");
        reset();
        return false;
    }

    initialized_ = true;
    return true;
}

bool vhost_net::set_features(uint64_t negotiated_features) {
    if (!initialized_) return false;
    uint64_t features = negotiated_features & features_;
    return vhost_ioctl(fd_, VHOST_SET_FEATURES, &features,
                       "VHOST_SET_FEATURES");
}

bool vhost_net::configure_queue(const vhost_vring_config& queue, int tap_fd) {
    if (!initialized_ || queue.index >= backend_attached_.size() ||
        tap_fd < 0 || queue.kick_fd < 0 || queue.call_fd < 0) {
        return false;
    }

    struct vhost_vring_state state{};
    state.index = queue.index;
    state.num = queue.size;
    if (!vhost_ioctl(fd_, VHOST_SET_VRING_NUM, &state,
                     "VHOST_SET_VRING_NUM")) {
        return false;
    }

    struct vhost_vring_addr address{};
    address.index = queue.index;
    address.desc_user_addr = queue.descriptor_userspace_address;
    address.avail_user_addr = queue.available_userspace_address;
    address.used_user_addr = queue.used_userspace_address;
    if (!vhost_ioctl(fd_, VHOST_SET_VRING_ADDR, &address,
                     "VHOST_SET_VRING_ADDR")) {
        return false;
    }

    state.num = 0;
    if (!vhost_ioctl(fd_, VHOST_SET_VRING_BASE, &state,
                     "VHOST_SET_VRING_BASE")) {
        return false;
    }

    struct vhost_vring_file event{};
    event.index = queue.index;
    event.fd = queue.call_fd;
    if (!vhost_ioctl(fd_, VHOST_SET_VRING_CALL, &event,
                     "VHOST_SET_VRING_CALL")) {
        return false;
    }

    event.fd = queue.kick_fd;
    if (!vhost_ioctl(fd_, VHOST_SET_VRING_KICK, &event,
                     "VHOST_SET_VRING_KICK")) {
        return false;
    }

    struct vhost_vring_file backend{};
    backend.index = queue.index;
    backend.fd = tap_fd;
    if (!vhost_ioctl(fd_, VHOST_NET_SET_BACKEND, &backend,
                     "VHOST_NET_SET_BACKEND")) {
        return false;
    }
    backend_attached_[queue.index] = true;
    return true;
}

void vhost_net::reset() noexcept {
    for (size_t index = 0; index < backend_attached_.size(); ++index) {
        if (!backend_attached_[index]) continue;

        struct vhost_vring_file backend{};
        backend.index = static_cast<uint32_t>(index);
        backend.fd = -1;
        if (::ioctl(fd_, VHOST_NET_SET_BACKEND, &backend) < 0) {
            std::perror("VHOST_NET_SET_BACKEND detach");
        }
    }

    if (owner_set_ && ::ioctl(fd_, VHOST_RESET_OWNER, 0) < 0) {
        std::perror("VHOST_RESET_OWNER");
        return;
    }
    owner_set_ = false;
    initialized_ = false;
    features_ = 0;
    backend_attached_.fill(false);
}
