#include "host/vhost.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
/* vhost_types.h includes virtio_ring.h; this VMM only needs the UAPI layouts. */
#define VIRTIO_RING_NO_LEGACY
#include <linux/vhost.h>
#include <limits>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace {

bool vhost_ioctl(int fd, unsigned long request, void* argument,
                 const char* operation) {
    if (::ioctl(fd, request, argument) == 0) return true;
    std::perror(operation);
    return false;
}

bool vhost_ioctl_no_arg(int fd, unsigned long request,
                        const char* operation) {
    if (::ioctl(fd, request, 0) == 0) return true;
    std::perror(operation);
    return false;
}

} // namespace

vhost_device::~vhost_device() {
    close();
}

bool vhost_device::open(const char* path, const char* description) {
    if (fd_ >= 0) return true;
    if (path == nullptr || description == nullptr) return false;

    fd_ = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        std::perror(description);
        return false;
    }
    return true;
}

bool vhost_device::initialize(const vhost_memory_region_config* regions,
                              size_t region_count) {
    if (fd_ < 0 || regions == nullptr || region_count == 0 || owner_set_ ||
        region_count > std::numeric_limits<uint32_t>::max()) {
        return false;
    }

    if (!vhost_ioctl_no_arg(fd_, VHOST_SET_OWNER, "VHOST_SET_OWNER")) {
        return false;
    }
    owner_set_ = true;

    if (!vhost_ioctl(fd_, VHOST_GET_FEATURES, &features_,
                     "VHOST_GET_FEATURES")) {
        reset();
        return false;
    }

    const size_t header_size = offsetof(struct vhost_memory, regions);
    if (region_count > (std::numeric_limits<size_t>::max() - header_size) /
                           sizeof(struct vhost_memory_region)) {
        reset();
        return false;
    }
    const size_t memory_size = header_size +
        region_count * sizeof(struct vhost_memory_region);
    std::vector<uint64_t> storage(
        (memory_size + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    auto* memory = reinterpret_cast<struct vhost_memory*>(storage.data());
    memory->nregions = static_cast<uint32_t>(region_count);

    for (size_t i = 0; i < region_count; ++i) {
        memory->regions[i].guest_phys_addr = regions[i].guest_phys_addr;
        memory->regions[i].memory_size = regions[i].memory_size;
        memory->regions[i].userspace_addr = regions[i].userspace_addr;
        memory->regions[i].flags_padding = 0;
    }

    if (!vhost_ioctl(fd_, VHOST_SET_MEM_TABLE, memory,
                     "VHOST_SET_MEM_TABLE")) {
        reset();
        return false;
    }
    return true;
}

bool vhost_device::set_features(uint64_t features) {
    if (fd_ < 0 || !owner_set_) return false;
    return vhost_ioctl(fd_, VHOST_SET_FEATURES, &features,
                       "VHOST_SET_FEATURES");
}

bool vhost_device::configure_vring(const vhost_vring_config& queue) {
    if (fd_ < 0 || !owner_set_ || queue.kick_fd < 0 || queue.call_fd < 0) {
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

    state.num = queue.base;
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
    return vhost_ioctl(fd_, VHOST_SET_VRING_KICK, &event,
                       "VHOST_SET_VRING_KICK");
}

bool vhost_device::reset() noexcept {
    if (fd_ < 0 || !owner_set_) return true;
    if (!vhost_ioctl_no_arg(fd_, VHOST_RESET_OWNER, "VHOST_RESET_OWNER")) {
        return false;
    }
    owner_set_ = false;
    features_ = 0;
    return true;
}

void vhost_device::close() noexcept {
    reset();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    owner_set_ = false;
    features_ = 0;
}

int vhost_device::fd() const noexcept {
    return fd_;
}

uint64_t vhost_device::features() const noexcept {
    return features_;
}
