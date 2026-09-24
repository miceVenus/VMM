#include "host/virtio_net.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <linux/kvm.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

bool guest_range_is_valid(size_t guest_memory_size,
                          uint64_t address,
                          size_t length) {
    return address <= guest_memory_size &&
           length <= guest_memory_size - static_cast<size_t>(address);
}

} // namespace

virtio_net::virtio_net(vm& owner, int vm_id)
    : owner_(owner), vm_id_(vm_id) {}

virtio_net::~virtio_net() {
    stop_vhost();

    for (queue_config& queue : queues_) {
        if (queue.kick_fd >= 0) {
            ::close(queue.kick_fd);
            queue.kick_fd = -1;
        }
    }
    if (call_fd_ >= 0) {
        ::close(call_fd_);
        call_fd_ = -1;
    }
    if (resample_fd_ >= 0) {
        ::close(resample_fd_);
        resample_fd_ = -1;
    }
    if (owner_.net == this) owner_.net = nullptr;
}

bool virtio_net::initialize() {
    if (owner_.net != nullptr || vm_id_ < 0 || vm_id_ > 253) return false;

    mac_ = {0x52, 0x54, 0x00, 0x12, 0x00,
            static_cast<uint8_t>(vm_id_ + 1)};

    if (!tap_.open(vm_id_)) return false;

    if (!backend_.initialize(owner_.mem_start, owner_.mem_size)) return false;

    for (queue_config& queue : queues_) {
        queue.kick_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (queue.kick_fd < 0) {
            std::perror("eventfd for vhost kick");
            return false;
        }
    }

    /* Configured vhost queues share one completion eventfd/GSI. The resample fd
     * lets the ACK path reassert the IRQ if used-ring work remains after PIC
     * EOI, without a userspace interrupt-polling thread. */
    call_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (call_fd_ < 0) {
        std::perror("eventfd for vhost completion");
        return false;
    }
    resample_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (resample_fd_ < 0) {
        std::perror("eventfd for KVM IRQ resampling");
        return false;
    }

    owner_.net = this;
    return true;
}

const std::string& virtio_net::tap_name() const noexcept {
    return tap_.name();
}

bool virtio_net::queue_memory_is_valid(const queue_config& queue) const {
    if (!queue.ready || queue.size == 0 || queue.size > queue_size_max ||
        (queue.size & (queue.size - 1)) != 0) {
        return false;
    }

    const size_t descriptor_bytes = static_cast<size_t>(queue.size) * 16;
    const size_t available_ring_bytes = 4 + static_cast<size_t>(queue.size) * 2;
    const size_t used_ring_bytes = 4 + static_cast<size_t>(queue.size) * 8;

    return (queue.descriptor_address & 0xf) == 0 &&
           (queue.available_ring_address & 0x1) == 0 &&
           (queue.used_ring_address & 0x3) == 0 &&
           guest_range_is_valid(owner_.mem_size, queue.descriptor_address,
                                descriptor_bytes) &&
           guest_range_is_valid(owner_.mem_size,
                                queue.available_ring_address,
                                available_ring_bytes) &&
           guest_range_is_valid(owner_.mem_size, queue.used_ring_address,
                                used_ring_bytes);
}

bool virtio_net::configure_vhost_queue(uint16_t index) {
    queue_config& queue = queues_[index];
    if (!queue_memory_is_valid(queue)) {
        std::fprintf(stderr, "Invalid Guest memory for Virtio queue %u.\n",
                     static_cast<unsigned>(index));
        return false;
    }

    queue.acknowledged_used_index = used_ring_index(queue);

    const vhost_vring_config config{
        index,
        queue.size,
        0,
        reinterpret_cast<uintptr_t>(owner_.mem_start +
                                    queue.descriptor_address),
        reinterpret_cast<uintptr_t>(owner_.mem_start +
                                    queue.available_ring_address),
        reinterpret_cast<uintptr_t>(owner_.mem_start +
                                    queue.used_ring_address),
        queue.kick_fd,
        call_fd_,
    };
    if (!backend_.configure_queue(config, tap_.fd())) return false;
    return true;
}

bool virtio_net::register_queue_ioeventfd(uint16_t index) {
    queue_config& queue = queues_[index];
    struct kvm_ioeventfd ioevent{};
    ioevent.datamatch = index;
    ioevent.addr = VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_REG_QUEUE_NOTIFY;
    ioevent.len = sizeof(uint32_t);
    ioevent.fd = queue.kick_fd;
    ioevent.flags = KVM_IOEVENTFD_FLAG_DATAMATCH;

    if (::ioctl(owner_.vm_fd, KVM_IOEVENTFD, &ioevent) < 0) {
        std::perror("KVM_IOEVENTFD (Virtio queue notify)");
        return false;
    }
    queue.ioeventfd_registered = true;
    return true;
}

bool virtio_net::register_irqfd() {
    if (::ioctl(owner_.kvm_fd, KVM_CHECK_EXTENSION,
                KVM_CAP_IRQFD_RESAMPLE) <= 0) {
        std::fprintf(stderr,
                     "KVM_IRQFD resampling is required for Virtio-net IRQs.\n");
        return false;
    }

    struct kvm_irqfd irqfd{};
    irqfd.fd = call_fd_;
    irqfd.gsi = VIRTIO_NET_GSI;
    irqfd.flags = KVM_IRQFD_FLAG_RESAMPLE;
    irqfd.resamplefd = resample_fd_;

    if (::ioctl(owner_.vm_fd, KVM_IRQFD, &irqfd) < 0) {
        std::perror("KVM_IRQFD (vhost completion)");
        return false;
    }
    irqfd_registered_ = true;
    return true;
}

bool virtio_net::configure_vhost() {
    if (vhost_started_) return true;
    if (!backend_.initialize(owner_.mem_start, owner_.mem_size)) return false;

    if ((driver_features_ & version_1_feature) == 0) {
        std::fprintf(stderr, "Guest did not negotiate Virtio VERSION_1.\n");
        return false;
    }

    if (!backend_.set_features(driver_features_)) {
        stop_vhost();
        return false;
    }

    bool configured_queue = false;
    for (uint16_t index = 0; index < queue_count; ++index) {
        if (!queues_[index].ready) continue;
        if (!configure_vhost_queue(index)) {
            stop_vhost();
            return false;
        }
        configured_queue = true;
    }
    if (!configured_queue) {
        std::fprintf(stderr, "Virtio-net has no Guest-configured queues.\n");
        stop_vhost();
        return false;
    }

    for (uint16_t index = 0; index < queue_count; ++index) {
        if (!queues_[index].ready) continue;
        if (!register_queue_ioeventfd(index)) {
            stop_vhost();
            return false;
        }
    }

    if (!register_irqfd()) {
        stop_vhost();
        return false;
    }

    /* The Guest may have posted buffers before DRIVER_OK was set. */
    const uint64_t kick = 1;
    for (const queue_config& queue : queues_) {
        if (!queue.ready) continue;
        if (::write(queue.kick_fd, &kick, sizeof(kick)) !=
            static_cast<ssize_t>(sizeof(kick))) {
            std::perror("write initial vhost kick eventfd");
            stop_vhost();
            return false;
        }
    }

    vhost_started_ = true;
    return true;
}

void virtio_net::stop_vhost() noexcept {
    if (irqfd_registered_ && owner_.vm_fd >= 0) {
        struct kvm_irqfd irqfd{};
        irqfd.fd = call_fd_;
        irqfd.gsi = VIRTIO_NET_GSI;
        irqfd.flags = KVM_IRQFD_FLAG_DEASSIGN;
        if (::ioctl(owner_.vm_fd, KVM_IRQFD, &irqfd) < 0) {
            std::perror("KVM_IRQFD deassign");
        }
        irqfd_registered_ = false;
    }

    for (uint16_t index = 0; index < queue_count; ++index) {
        queue_config& queue = queues_[index];
        if (queue.ioeventfd_registered && owner_.vm_fd >= 0) {
            struct kvm_ioeventfd ioevent{};
            ioevent.datamatch = index;
            ioevent.addr = VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_REG_QUEUE_NOTIFY;
            ioevent.len = sizeof(uint32_t);
            ioevent.fd = queue.kick_fd;
            ioevent.flags = KVM_IOEVENTFD_FLAG_DATAMATCH |
                            KVM_IOEVENTFD_FLAG_DEASSIGN;
            if (::ioctl(owner_.vm_fd, KVM_IOEVENTFD, &ioevent) < 0) {
                std::perror("KVM_IOEVENTFD deassign");
            }
            queue.ioeventfd_registered = false;
        }
    }

    backend_.reset();
    vhost_started_ = false;
}

uint64_t virtio_net::load_little_endian(const uint8_t* bytes,
                                        uint32_t length) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < length; ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

void virtio_net::store_little_endian(uint8_t* bytes,
                                     uint32_t length,
                                     uint64_t value) {
    for (uint32_t i = 0; i < length; ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void virtio_net::set_address_part(uint64_t& address,
                                  bool high,
                                  uint32_t value) noexcept {
    if (high) {
        address = (address & UINT64_C(0x00000000ffffffff)) |
                  (static_cast<uint64_t>(value) << 32);
    } else {
        address = (address & UINT64_C(0xffffffff00000000)) | value;
    }
}

virtio_net::queue_config* virtio_net::selected_queue() {
    if (selected_queue_ >= queue_count) return nullptr;
    return &queues_[selected_queue_];
}

const virtio_net::queue_config* virtio_net::selected_queue() const {
    if (selected_queue_ >= queue_count) return nullptr;
    return &queues_[selected_queue_];
}

/* MMIO is an address-decoded register bank; keep each read case explicit. */
uint64_t virtio_net::read_register(uint32_t offset) const {
    const queue_config* queue = selected_queue();

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
            if (device_features_select_ == 0) {
                return static_cast<uint32_t>(offered_features);
            }
            if (device_features_select_ == 1) {
                return static_cast<uint32_t>(offered_features >> 32);
            }
            return 0;
        case VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL:
            return device_features_select_;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES:
            if (driver_features_select_ == 0) {
                return static_cast<uint32_t>(driver_features_);
            }
            if (driver_features_select_ == 1) {
                return static_cast<uint32_t>(driver_features_ >> 32);
            }
            return 0;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL:
            return driver_features_select_;
        case VIRTIO_MMIO_REG_QUEUE_SEL:
            return selected_queue_;
        case VIRTIO_MMIO_REG_QUEUE_NUM_MAX:
            return queue != nullptr ? queue_size_max : 0;
        case VIRTIO_MMIO_REG_QUEUE_NUM:
            return queue != nullptr ? queue->size : 0;
        case VIRTIO_MMIO_REG_QUEUE_READY:
            return queue != nullptr && queue->ready ? 1 : 0;
        case VIRTIO_MMIO_REG_INTERRUPT_STATUS:
            return pending_interrupt_status();
        case VIRTIO_MMIO_REG_STATUS:
            return status_;
        case VIRTIO_MMIO_REG_CONFIG_GENERATION:
            return 0;
        default:
            return 0;
    }
}

uint16_t virtio_net::used_ring_index(const queue_config& queue) const {
    if (!queue.ready ||
        !guest_range_is_valid(owner_.mem_size, queue.used_ring_address, 4)) {
        return queue.acknowledged_used_index;
    }

    /* Virtio split-ring indices are little-endian 16-bit values. This VMM
     * currently runs x86 guests on x86 hosts, so an acquire atomic load reads
     * the vhost-updated index in its native representation. */
    const auto* index = reinterpret_cast<const uint16_t*>(
        owner_.mem_start + queue.used_ring_address + 2);
    return __atomic_load_n(index, __ATOMIC_ACQUIRE);
}

uint32_t virtio_net::pending_interrupt_status() const {
    uint32_t pending = interrupt_status_;
    for (const queue_config& queue : queues_) {
        if (queue.ready &&
            used_ring_index(queue) != queue.acknowledged_used_index) {
            pending |= UINT32_C(1);
            break;
        }
    }
    return pending;
}

uint64_t virtio_net::read_mmio_value(uint32_t offset,
                                     uint32_t length) const {
    if (offset >= VIRTIO_MMIO_REG_CONFIG_SPACE &&
        offset < VIRTIO_MMIO_REG_CONFIG_SPACE + mac_.size()) {
        uint64_t value = 0;
        for (uint32_t i = 0; i < length; ++i) {
            const uint32_t byte_offset =
                offset + i - VIRTIO_MMIO_REG_CONFIG_SPACE;
            if (byte_offset < mac_.size()) {
                value |= static_cast<uint64_t>(mac_[byte_offset]) << (8 * i);
            }
        }
        return value;
    }
    return read_register(offset);
}

void virtio_net::reset_device() {
    stop_vhost();
    status_ = 0;
    device_features_select_ = 0;
    driver_features_select_ = 0;
    driver_features_ = 0;
    interrupt_status_ = 0;
    selected_queue_ = 0;

    for (queue_config& queue : queues_) {
        queue.size = 0;
        queue.ready = false;
        queue.descriptor_address = 0;
        queue.available_ring_address = 0;
        queue.used_ring_address = 0;
        queue.acknowledged_used_index = 0;
    }
}

void virtio_net::write_register(uint32_t offset, uint64_t value) {
    switch (offset) {
        case VIRTIO_MMIO_REG_DEVICE_FEATURES_SEL:
            device_features_select_ = static_cast<uint32_t>(value);
            break;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES_SEL:
            driver_features_select_ = static_cast<uint32_t>(value);
            break;
        case VIRTIO_MMIO_REG_DRIVER_FEATURES:
            if (driver_features_select_ == 0) {
                driver_features_ =
                    (driver_features_ & UINT64_C(0xffffffff00000000)) |
                    static_cast<uint32_t>(value);
            } else if (driver_features_select_ == 1) {
                driver_features_ =
                    (driver_features_ & UINT64_C(0x00000000ffffffff)) |
                    (static_cast<uint64_t>(static_cast<uint32_t>(value)) << 32);
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_SEL:
            selected_queue_ = static_cast<uint32_t>(value);
            break;
        case VIRTIO_MMIO_REG_QUEUE_NUM:
            if (queue_config* queue = selected_queue()) {
                if (value == 0 || value > queue_size_max ||
                    (value & (value - 1)) != 0) {
                    status_ |= VIRTIO_STATUS_FAILED;
                } else {
                    queue->size = static_cast<uint16_t>(value);
                }
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_READY:
            if (queue_config* queue = selected_queue()) {
                if (value > 1 || vhost_started_) {
                    status_ |= VIRTIO_STATUS_FAILED;
                } else {
                    queue->ready = value != 0;
                }
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DESC_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DESC_HIGH:
            if (queue_config* queue = selected_queue()) {
                set_address_part(queue->descriptor_address,
                    offset == VIRTIO_MMIO_REG_QUEUE_DESC_HIGH,
                    static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH:
            if (queue_config* queue = selected_queue()) {
                set_address_part(queue->available_ring_address,
                    offset == VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH,
                    static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH:
            if (queue_config* queue = selected_queue()) {
                set_address_part(queue->used_ring_address,
                    offset == VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH,
                    static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_NOTIFY:
            /* Queue notifications are normally consumed by KVM_IOEVENTFD. */
            if (value >= queue_count && (status_ & VIRTIO_STATUS_DRIVER_OK)) {
                status_ |= VIRTIO_STATUS_FAILED;
            }
            break;
        case VIRTIO_MMIO_REG_INTERRUPT_ACK:
            interrupt_status_ &= ~static_cast<uint32_t>(value);
            if ((value & UINT32_C(1)) != 0) {
                for (queue_config& queue : queues_) {
                    if (queue.ready) {
                        queue.acknowledged_used_index =
                            used_ring_index(queue);
                    }
                }

                /* In resample mode KVM deasserts the PIC line on Guest EOI
                 * and notifies this eventfd. If a completion raced with the
                 * Guest's ACK snapshot, signal the shared source eventfd
                 * again so the still-pending used-ring work gets another
                 * interrupt. */
                bool irq_was_resampled = false;
                for (;;) {
                    uint64_t resampled = 0;
                    const ssize_t bytes = ::read(
                        resample_fd_, &resampled, sizeof(resampled));
                    if (bytes == static_cast<ssize_t>(sizeof(resampled))) {
                        irq_was_resampled = true;
                        continue;
                    }
                    if (bytes < 0 && errno == EINTR) continue;
                    if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        break;
                    }
                    if (bytes < 0) {
                        std::perror("read KVM IRQ resample eventfd");
                    } else {
                        std::fprintf(stderr,
                                     "Short read from KVM IRQ resample eventfd.\n");
                    }
                    break;
                }

                if (irq_was_resampled) {
                    bool work_remains = false;
                    for (const queue_config& queue : queues_) {
                        if (queue.ready && used_ring_index(queue) !=
                                queue.acknowledged_used_index) {
                            work_remains = true;
                            break;
                        }
                    }
                    if (work_remains) {
                        const uint64_t reassert = 1;
                        if (::write(call_fd_, &reassert, sizeof(reassert)) !=
                            static_cast<ssize_t>(sizeof(reassert))) {
                            std::perror("reassert pending Virtio IRQ");
                        }
                    }
                }
            }
            break;
        case VIRTIO_MMIO_REG_STATUS: {
            const uint8_t new_status = static_cast<uint8_t>(value);
            if (new_status == 0) {
                reset_device();
                break;
            }

            status_ = new_status;
            if ((status_ & VIRTIO_STATUS_FEATURES_OK) &&
                ((driver_features_ & ~offered_features) != 0 ||
                 (driver_features_ & version_1_feature) == 0)) {
                status_ |= VIRTIO_STATUS_FAILED;
            }
            if ((status_ & VIRTIO_STATUS_DRIVER_OK) &&
                (status_ & VIRTIO_STATUS_FAILED) == 0 &&
                !configure_vhost()) {
                status_ |= VIRTIO_STATUS_FAILED;
            }
            break;
        }
        default:
            /* Network configuration is read-only in this implementation. */
            break;
    }
}

bool virtio_net::handle_mmio(struct kvm_run& run) {
    const uint64_t address = run.mmio.phys_addr;
    const uint32_t length = run.mmio.len;
    if (address < VIRTIO_MMIO_BASE_GPA ||
        address >= VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_SIZE ||
        length == 0 ||
        address + length > VIRTIO_MMIO_BASE_GPA + VIRTIO_MMIO_SIZE ||
        length > sizeof(run.mmio.data)) {
        return false;
    }

    const uint32_t offset =
        static_cast<uint32_t>(address - VIRTIO_MMIO_BASE_GPA);
    if (run.mmio.is_write) {
        write_register(offset,
                       load_little_endian(run.mmio.data, length));
    } else {
        std::memset(run.mmio.data, 0, sizeof(run.mmio.data));
        store_little_endian(run.mmio.data,
                            length,
                            read_mmio_value(offset, length));
    }

    return true;
}
