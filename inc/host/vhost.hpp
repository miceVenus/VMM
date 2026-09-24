#pragma once

#include <cstddef>
#include <cstdint>

struct vhost_memory_region_config {
    uint64_t guest_phys_addr;
    uint64_t memory_size;
    uintptr_t userspace_addr;
};

struct vhost_vring_config {
    uint32_t index;
    uint32_t size;
    uint32_t base;
    uint64_t descriptor_userspace_address;
    uint64_t available_userspace_address;
    uint64_t used_userspace_address;
    int kick_fd;
    int call_fd;
};

/* Owns a Linux vhost endpoint and its device-independent setup operations. */
class vhost_device {
public:
    vhost_device() = default;
    ~vhost_device();

    vhost_device(const vhost_device&) = delete;
    vhost_device& operator=(const vhost_device&) = delete;

    bool open(const char* path, const char* description);
    bool initialize(const vhost_memory_region_config* regions,
                    size_t region_count);
    bool set_features(uint64_t features);
    bool configure_vring(const vhost_vring_config& queue);
    bool reset() noexcept;
    void close() noexcept;

    int fd() const noexcept;
    uint64_t features() const noexcept;

private:
    int fd_ = -1;
    uint64_t features_ = 0;
    bool owner_set_ = false;
};
