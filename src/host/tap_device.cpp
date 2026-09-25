#include "host/tap_device.hpp"

#include <cstdio>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "virtio_defs.h"

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
    interface_request.ifr_flags = IFF_TAP | IFF_NO_PI | IFF_VNET_HDR;
    std::snprintf(interface_request.ifr_name,
                  IFNAMSIZ,
                  "vmtap%d",
                  vm_id);

    if (::ioctl(fd_, TUNSETIFF, &interface_request) < 0) {
        perror("TUNSETIFF (run the VMM with CAP_NET_ADMIN or pre-create the TAP)");
        close();
        return false;
    }

    int header_size = VIRTIO_NET_HEADER_SIZE;
    if (::ioctl(fd_, TUNSETVNETHDRSZ, &header_size) < 0) {
        perror("TUNSETVNETHDRSZ");
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

int tap_device::fd() const noexcept {
    return fd_;
}

const std::string& tap_device::name() const noexcept {
    return name_;
}
