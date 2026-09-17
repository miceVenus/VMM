#include "host/tap_device.hpp"

#include <cstdio>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

tap_device::~tap_device() {
    close();
}

bool tap_device::open(int vm_id) {
    if (fd_ >= 0 || vm_id < 0 || vm_id > 253) return false;

    fd_ = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        perror("open /dev/net/tun");
        return false;
    }

    struct ifreq interface_request{};
    interface_request.ifr_flags = IFF_TAP | IFF_NO_PI;
    std::snprintf(interface_request.ifr_name,
                  IFNAMSIZ,
                  "vmtap%d",
                  vm_id);

    if (::ioctl(fd_, TUNSETIFF, &interface_request) < 0) {
        perror("TUNSETIFF (run the VMM with CAP_NET_ADMIN or pre-create the TAP)");
        close();
        return false;
    }

    name_ = interface_request.ifr_name;
    return true;
}

void tap_device::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    name_.clear();
}

ssize_t tap_device::read_frame(void* buffer, size_t capacity) const {
    if (fd_ < 0) return -1;
    return ::read(fd_, buffer, capacity);
}

bool tap_device::write_frame(const void* buffer, size_t length) const {
    if (fd_ < 0) return false;
    return ::write(fd_, buffer, length) == static_cast<ssize_t>(length);
}

void tap_device::wait_readable(int timeout_ms) const {
    if (fd_ < 0) return;

    struct pollfd descriptor{};
    descriptor.fd = fd_;
    descriptor.events = POLLIN;
    (void)::poll(&descriptor, 1, timeout_ms);
}

const std::string& tap_device::name() const noexcept {
    return name_;
}
