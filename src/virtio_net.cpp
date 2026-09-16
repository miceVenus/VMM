#include "virtio_net.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <new>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

constexpr uint64_t version_1_feature = UINT64_C(1) << VIRTIO_F_VERSION_1;
constexpr uint64_t mac_feature = UINT64_C(1) << VIRTIO_NET_F_MAC;
constexpr uint64_t offered_features = version_1_feature | mac_feature;
constexpr size_t virtio_net_header_size = 10;
constexpr size_t max_frame_size = 65535;
constexpr size_t max_packet_size = max_frame_size + virtio_net_header_size;
constexpr size_t max_pending_rx_frames = 256;

struct virtq_descriptor {
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

static_assert(sizeof(virtq_descriptor) == 16, "Virtio descriptor layout changed");

struct virtq_used_element {
    uint32_t id;
    uint32_t length;
};

static_assert(sizeof(virtq_used_element) == 8, "Virtio used element layout changed");

bool guest_range_is_valid(const vm& v, uint64_t address, size_t length) {
    if (address > v.mem_size) return false;
    return length <= v.mem_size - address;
}

template <typename T>
bool read_guest(const vm& v, uint64_t address, T& value) {
    if (!guest_range_is_valid(v, address, sizeof(T))) return false;
    std::memcpy(&value, v.mem_start + address, sizeof(T));
    return true;
}

template <typename T>
bool write_guest(const vm& v, uint64_t address, const T& value) {
    if (!guest_range_is_valid(v, address, sizeof(T))) return false;
    std::memcpy(v.mem_start + address, &value, sizeof(T));
    return true;
}

uint64_t load_little_endian(const uint8_t* bytes, uint32_t length) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < length; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

void store_little_endian(uint8_t* bytes, uint32_t length, uint64_t value) {
    for (uint32_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

/* ------------------------------ TAP backend ----------------------------- */

int open_tap(std::string& actual_name, int vm_id) {
    const int fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/net/tun");
        return -1;
    }

    struct ifreq interface_request{};
    interface_request.ifr_flags = IFF_TAP | IFF_NO_PI;
    std::snprintf(interface_request.ifr_name,
                  IFNAMSIZ,
                  "vmtap%d",
                  vm_id);

    if (ioctl(fd, TUNSETIFF, &interface_request) < 0) {
        perror("TUNSETIFF (run the VMM with CAP_NET_ADMIN or pre-create the TAP)");
        close(fd);
        return -1;
    }

    actual_name = interface_request.ifr_name;
    return fd;
}

void tap_reader(virtio_net_device* device) {
    std::array<uint8_t, max_frame_size> frame{};

    while (!device->stop_requested.load()) {
        const ssize_t length = read(device->tap_fd, frame.data(), frame.size());
        if (length > 0) {
            std::vector<uint8_t> received(frame.begin(), frame.begin() + length);
            std::lock_guard<std::mutex> lock(device->rx_mutex);
            if (device->rx_frames.size() < max_pending_rx_frames) {
                device->rx_frames.push_back(std::move(received));
            }
            continue;
        }

        if (length < 0 && (errno == EINTR)) continue;
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)) {
            struct pollfd descriptor{};
            descriptor.fd = device->tap_fd;
            descriptor.events = POLLIN;
            poll(&descriptor, 1, 50);
            continue;
        }

        if (length == 0 || (length < 0 && errno != EINTR)) break;
    }
}

/* ---------------------------- Virtqueue access -------------------------- */

virtqueue_state* selected_queue(virtio_net_device& device) {
    if (device.selected_queue >= virtio_net_device::queue_count) return nullptr;
    return &device.queues[device.selected_queue];
}

bool queue_memory_is_valid(const vm& v, const virtqueue_state& queue) {
    if (!queue.ready ||
        queue.size == 0 ||
        queue.size > virtio_net_device::queue_size ||
        (queue.size & (queue.size - 1)) != 0) {
        return false;
    }

    const size_t descriptor_bytes = static_cast<size_t>(queue.size) * sizeof(virtq_descriptor);
    const size_t driver_bytes = 4 + static_cast<size_t>(queue.size) * sizeof(uint16_t);
    const size_t device_bytes = 4 + static_cast<size_t>(queue.size) * sizeof(virtq_used_element);

    return guest_range_is_valid(v, queue.descriptor_address, descriptor_bytes) &&
           guest_range_is_valid(v, queue.driver_address, driver_bytes) &&
           guest_range_is_valid(v, queue.device_address, device_bytes);
}

bool read_available_head(const vm& v,
                         const virtqueue_state& queue,
                         uint16_t& available_index,
                         uint16_t& descriptor_index) {
    if (!queue_memory_is_valid(v, queue)) return false;
    if (!read_guest(v, queue.driver_address + 2, available_index)) return false;
    if (available_index == queue.last_available) return false;
    if (static_cast<uint16_t>(available_index - queue.last_available) > queue.size) {
        return false;
    }

    const uint64_t ring_address = queue.driver_address +
        4 + static_cast<uint64_t>(queue.last_available % queue.size) * sizeof(uint16_t);
    return read_guest(v, ring_address, descriptor_index);
}

bool read_descriptor_chain(const vm& v,
                           const virtqueue_state& queue,
                           uint16_t head,
                           std::vector<virtq_descriptor>& chain) {
    if (head >= queue.size) return false;

    uint16_t index = head;
    for (uint16_t count = 0; count < queue.size; ++count) {
        if (index >= queue.size) return false;

        virtq_descriptor descriptor{};
        const uint64_t address = queue.descriptor_address +
            static_cast<uint64_t>(index) * sizeof(virtq_descriptor);
        if (!read_guest(v, address, descriptor)) return false;
        chain.push_back(descriptor);

        if ((descriptor.flags & VIRTQ_DESC_F_NEXT) == 0) return true;
        index = descriptor.next;
    }

    /* A valid chain cannot contain more descriptors than the queue itself. */
    return false;
}

bool read_guest_readable_bytes(const vm& v,
                               const std::vector<virtq_descriptor>& chain,
                               std::vector<uint8_t>& bytes) {
    for (const virtq_descriptor& descriptor : chain) {
        if (descriptor.flags & VIRTQ_DESC_F_WRITE) return false;
        if (!guest_range_is_valid(v, descriptor.address, descriptor.length)) return false;
        if (bytes.size() > max_packet_size ||
            descriptor.length > max_packet_size - bytes.size()) {
            return false;
        }

        const uint8_t* source = v.mem_start + descriptor.address;
        bytes.insert(bytes.end(), source, source + descriptor.length);
    }
    return true;
}

bool write_guest_writable_bytes(const vm& v,
                                const std::vector<virtq_descriptor>& chain,
                                const uint8_t* bytes,
                                size_t length,
                                size_t& copied) {
    copied = 0;

    for (const virtq_descriptor& descriptor : chain) {
        if ((descriptor.flags & VIRTQ_DESC_F_WRITE) == 0) return false;
        if (!guest_range_is_valid(v, descriptor.address, descriptor.length)) return false;

        const size_t remaining = length - copied;
        const size_t chunk = std::min<size_t>(remaining, descriptor.length);
        if (chunk != 0) {
            std::memcpy(v.mem_start + descriptor.address, bytes + copied, chunk);
            copied += chunk;
        }
        if (copied == length) break;
    }

    return copied == length;
}

bool add_used_buffer(const vm& v,
                     virtqueue_state& queue,
                     uint16_t descriptor_index,
                     uint32_t length) {
    uint16_t used_index = 0;
    if (!read_guest(v, queue.device_address + 2, used_index)) return false;

    const uint64_t element_address = queue.device_address +
        4 + static_cast<uint64_t>(used_index % queue.size) * sizeof(virtq_used_element);
    const virtq_used_element element{
        static_cast<uint32_t>(descriptor_index),
        length,
    };

    if (!write_guest(v, element_address, element)) return false;

    __sync_synchronize();
    ++used_index;
    return write_guest(v, queue.device_address + 2, used_index);
}

bool take_pending_frame(virtio_net_device& device, std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(device.rx_mutex);
    if (device.rx_frames.empty()) return false;
    frame = std::move(device.rx_frames.front());
    device.rx_frames.pop_front();
    return true;
}

void mark_device_failed(virtio_net_device& device) {
    device.status |= VIRTIO_STATUS_FAILED;
}

void process_receive_queue(vm& v, virtio_net_device& device) {
    virtqueue_state& queue = device.queues[0];
    if (!queue_memory_is_valid(v, queue)) return;

    while (true) {
        uint16_t available_index = 0;
        uint16_t descriptor_index = 0;
        if (!read_available_head(v, queue, available_index, descriptor_index)) return;

        std::vector<uint8_t> frame;
        if (!take_pending_frame(device, frame)) return;

        std::vector<virtq_descriptor> chain;
        if (!read_descriptor_chain(v, queue, descriptor_index, chain)) {
            mark_device_failed(device);
            return;
        }

        std::vector<uint8_t> packet(virtio_net_header_size + frame.size(), 0);
        std::copy(frame.begin(), frame.end(), packet.begin() + virtio_net_header_size);
        size_t written = 0;
        if (!write_guest_writable_bytes(v,
                                        chain,
                                        packet.data(),
                                        packet.size(),
                                        written)) {
            mark_device_failed(device);
            return;
        }

        if (!add_used_buffer(v, queue, descriptor_index, static_cast<uint32_t>(written))) {
            mark_device_failed(device);
            return;
        }
        ++queue.last_available;
        device.interrupt_status |= 1U;
    }
}

bool write_tap_frame(virtio_net_device& device, const std::vector<uint8_t>& bytes) {
    if (bytes.size() > max_frame_size) return false;

    const ssize_t written = write(device.tap_fd, bytes.data(), bytes.size());
    return written == static_cast<ssize_t>(bytes.size());
}

void process_transmit_queue(vm& v, virtio_net_device& device) {
    virtqueue_state& queue = device.queues[1];
    if (!queue_memory_is_valid(v, queue)) return;

    while (true) {
        uint16_t available_index = 0;
        uint16_t descriptor_index = 0;
        if (!read_available_head(v, queue, available_index, descriptor_index)) return;

        std::vector<virtq_descriptor> chain;
        std::vector<uint8_t> packet;
        if (!read_descriptor_chain(v, queue, descriptor_index, chain) ||
            !read_guest_readable_bytes(v, chain, packet)) {
            mark_device_failed(device);
            return;
        }

        uint32_t transmitted_length = 0;
        if (packet.size() >= virtio_net_header_size) {
            packet.erase(packet.begin(), packet.begin() + virtio_net_header_size);
            if (write_tap_frame(device, packet)) {
                transmitted_length = static_cast<uint32_t>(packet.size());
            }
        }

        if (!add_used_buffer(v, queue, descriptor_index, transmitted_length)) {
            mark_device_failed(device);
            return;
        }
        ++queue.last_available;
        device.interrupt_status |= 1U;
    }
}

void process_queue(vm& v, virtio_net_device& device, uint16_t queue_index) {
    if (queue_index == 0) {
        process_receive_queue(v, device);
    } else if (queue_index == 1) {
        process_transmit_queue(v, device);
    }
}

/* --------------------------- Virtio-MMIO model --------------------------- */

uint64_t read_register(const virtio_net_device& device, uint32_t offset) {
    switch (offset) {
        case VIRTIO_MMIO_REG_MAGIC_VALUE:
            return VIRTIO_MMIO_MAGIC_VALUE;
        case VIRTIO_MMIO_REG_VERSION:
            return VIRTIO_MMIO_VERSION;
        case VIRTIO_MMIO_REG_DEVICE_ID:
            return VIRTIO_MMIO_DEVICE_NET;
        case VIRTIO_MMIO_REG_VENDOR_ID:
            return VIRTIO_MMIO_VENDOR_ID;
        case VIRTIO_MMIO_REG_DEVICE_FEATURES:
            if (device.device_features_select == 0) {
                return static_cast<uint32_t>(offered_features);
            }
            if (device.device_features_select == 1) {
                return static_cast<uint32_t>(offered_features >> 32);
            }
            return 0;
        case VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL:
            return device.device_features_select;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES:
            if (device.driver_features_select == 0) {
                return static_cast<uint32_t>(device.driver_features);
            }
            if (device.driver_features_select == 1) {
                return static_cast<uint32_t>(device.driver_features >> 32);
            }
            return 0;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL:
            return device.driver_features_select;
        case VIRTIO_MMIO_REG_QUEUE_SEL:
            return device.selected_queue;
        case VIRTIO_MMIO_REG_QUEUE_NUM_MAX:
            return device.selected_queue < virtio_net_device::queue_count
                ? virtio_net_device::queue_size : 0;
        case VIRTIO_MMIO_REG_QUEUE_NUM:
            return device.selected_queue < virtio_net_device::queue_count
                ? device.queues[device.selected_queue].size : 0;
        case VIRTIO_MMIO_REG_QUEUE_READY:
            return device.selected_queue < virtio_net_device::queue_count &&
                           device.queues[device.selected_queue].ready ? 1 : 0;
        case VIRTIO_MMIO_REG_INTERRUPT_STATUS:
            return device.interrupt_status;
        case VIRTIO_MMIO_REG_STATUS:
            return device.status;
        case VIRTIO_MMIO_REG_CONFIG_GENERATION:
            return 0;
        default:
            return 0;
    }
}

uint64_t read_mmio_value(const virtio_net_device& device,
                         uint32_t offset,
                         uint32_t length) {
    if (offset >= VIRTIO_MMIO_REG_CONFIG_SPACE &&
        offset < VIRTIO_MMIO_REG_CONFIG_SPACE + device.mac.size()) {
        uint64_t value = 0;
        for (uint32_t i = 0; i < length; ++i) {
            const uint32_t byte_offset = offset + i - VIRTIO_MMIO_REG_CONFIG_SPACE;
            if (byte_offset < device.mac.size()) {
                value |= static_cast<uint64_t>(device.mac[byte_offset]) << (8 * i);
            }
        }
        return value;
    }
    return read_register(device, offset);
}

void reset_device(virtio_net_device& device) {
    device.status = 0;
    device.device_features_select = 0;
    device.driver_features_select = 0;
    device.driver_features = 0;
    device.interrupt_status = 0;
    device.selected_queue = 0;
    for (virtqueue_state& queue : device.queues) queue = virtqueue_state{};
}

void write_queue_address(uint64_t& address, uint32_t offset, uint32_t value) {
    if (offset == VIRTIO_MMIO_REG_QUEUE_DESC_LOW ||
        offset == VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW ||
        offset == VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW) {
        address = (address & UINT64_C(0xffffffff00000000)) | value;
    } else {
        address = (address & UINT64_C(0x00000000ffffffff)) |
                  (static_cast<uint64_t>(value) << 32);
    }
}

void write_register(vm& v,
                    virtio_net_device& device,
                    uint32_t offset,
                    uint64_t value) {
    switch (offset) {
        case VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL:
            device.device_features_select = static_cast<uint32_t>(value);
            break;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL:
            device.driver_features_select = static_cast<uint32_t>(value);
            break;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES:
            if (device.driver_features_select == 0) {
                device.driver_features = (device.driver_features & UINT64_C(0xffffffff00000000)) |
                                         static_cast<uint32_t>(value);
            } else if (device.driver_features_select == 1) {
                device.driver_features = (device.driver_features & UINT64_C(0x00000000ffffffff)) |
                    (static_cast<uint64_t>(static_cast<uint32_t>(value)) << 32);
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_SEL:
            device.selected_queue = static_cast<uint32_t>(value);
            break;
        case VIRTIO_MMIO_REG_QUEUE_NUM:
            if (device.selected_queue < virtio_net_device::queue_count &&
                value > 0 && value <= virtio_net_device::queue_size) {
                device.queues[device.selected_queue].size = static_cast<uint16_t>(value);
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_READY:
            if (device.selected_queue < virtio_net_device::queue_count) {
                device.queues[device.selected_queue].ready = value != 0;
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DESC_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DESC_HIGH:
            if (virtqueue_state* queue = selected_queue(device)) {
                write_queue_address(queue->descriptor_address,
                                    offset,
                                    static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH:
            if (virtqueue_state* queue = selected_queue(device)) {
                write_queue_address(queue->driver_address, offset, static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH:
            if (virtqueue_state* queue = selected_queue(device)) {
                write_queue_address(queue->device_address, offset, static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_NOTIFY:
            if (value < virtio_net_device::queue_count &&
                (device.status & VIRTIO_STATUS_DRIVER_OK)) {
                process_queue(v, device, static_cast<uint16_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_INTERRUPT_ACK:
            device.interrupt_status &= ~static_cast<uint32_t>(value);
            break;
        case VIRTIO_MMIO_REG_STATUS: {
            const uint8_t new_status = static_cast<uint8_t>(value);
            if (new_status == 0) {
                reset_device(device);
                break;
            }

            device.status = new_status;
            if ((device.status & VIRTIO_STATUS_FEATURES_OK) &&
                ((device.driver_features & ~offered_features) != 0 ||
                 (device.driver_features & version_1_feature) == 0)) {
                device.status |= VIRTIO_STATUS_FAILED;
            }
            break;
        }
        default:
            /* The network configuration is read-only in this implementation. */
            break;
    }
}

} // namespace

int virtio_net_init(vm& v, int vm_id) {
    if (v.net != nullptr || vm_id < 0 || vm_id > 253) return -1;

    virtio_net_device* device = new (std::nothrow) virtio_net_device;
    if (device == nullptr) return -1;

    device->mac = {0x52, 0x54, 0x00, 0x12, 0x00,
                   static_cast<uint8_t>(vm_id + 1)};
    device->tap_fd = open_tap(device->tap_name, vm_id);
    if (device->tap_fd < 0) {
        delete device;
        return -1;
    }

    try {
        device->tap_thread = std::thread(tap_reader, device);
    } catch (...) {
        close(device->tap_fd);
        delete device;
        return -1;
    }
    v.net = device;
    return 0;
}

void virtio_net_destroy(vm& v) {
    virtio_net_device* device = v.net;
    if (device == nullptr) return;

    device->stop_requested.store(true);
    if (device->tap_thread.joinable()) device->tap_thread.join();
    if (device->tap_fd >= 0) close(device->tap_fd);

    v.net = nullptr;
    delete device;
}

bool virtio_net_handle_mmio(vm& v, struct kvm_run& run) {
    virtio_net_device* device = v.net;
    if (device == nullptr) return false;

    const uint64_t address = run.mmio.phys_addr;
    const uint32_t length = run.mmio.len;
    if (address < VIRTIO_MMIO_BASE_GPA ||
        address >= VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_SIZE ||
        length == 0 ||
        address + length > VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_SIZE ||
        length > sizeof(run.mmio.data)) {
        return false;
    }

    const uint32_t offset = static_cast<uint32_t>(address - VIRTIO_MMIO_BASE_GPA);
    std::lock_guard<std::mutex> lock(device->device_mutex);

    if (run.mmio.is_write) {
        write_register(v,
                       *device,
                       offset,
                       load_little_endian(run.mmio.data, length));
    } else {
        std::memset(run.mmio.data, 0, sizeof(run.mmio.data));
        store_little_endian(run.mmio.data,
                            length,
                            read_mmio_value(*device, offset, length));
    }

    return true;
}
