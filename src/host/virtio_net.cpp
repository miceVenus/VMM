#include "host/virtio_net.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/ioctl.h>

virtio_net::virtio_net(vm& owner, int vm_id)
    : owner_(owner),
      vm_id_(vm_id),
      rx_queue_(owner.mem_start, owner.mem_size, queue_size),
      tx_queue_(owner.mem_start, owner.mem_size, queue_size) {}

virtio_net::~virtio_net() {
    stop_requested_.store(true);

    if (tap_thread_.joinable()) tap_thread_.join();

    if (owner_.net == this) owner_.net = nullptr;
}

bool virtio_net::initialize() {
    if (owner_.net != nullptr || vm_id_ < 0 || vm_id_ > 253) return false;

    mac_ = {0x52, 0x54, 0x00, 0x12, 0x00,
            static_cast<uint8_t>(vm_id_ + 1)};

    if (!tap_.open(vm_id_)) return false;

    try {
        tap_thread_ = std::thread(&virtio_net::tap_reader, this);
    } catch (...) {
        tap_.close();
        return false;
    }

    owner_.net = this;
    return true;
}

const std::string& virtio_net::tap_name() const noexcept {
    return tap_.name();
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

void virtio_net::wake_vcpu() {
    {
        std::lock_guard<std::mutex> lock(owner_.interrupt_mutex);
        owner_.interrupt_wakeup = true;
    }
    owner_.interrupt_cv.notify_one();
}

void virtio_net::inject_guest_interrupt() {
    struct kvm_interrupt interrupt{};
    interrupt.irq = VIRTIO_NET_INTERRUPT_VECTOR;

    if (ioctl(owner_.vcpu_fd, KVM_INTERRUPT, &interrupt) < 0) {
        /* An already queued interrupt is enough to coalesce this event. */
        if (errno != EEXIST) {
            perror("KVM_INTERRUPT");
            return;
        }
    }

    wake_vcpu();
}

void virtio_net::tap_reader() {
    std::array<uint8_t, max_frame_size> frame{};

    while (!stop_requested_.load()) {
        const ssize_t length = tap_.read_frame(frame.data(), frame.size());
        if (length > 0) {
            std::vector<uint8_t> received(frame.begin(), frame.begin() + length);
            {
                std::lock_guard<std::mutex> lock(rx_mutex_);
                if (rx_frames_.size() < max_pending_rx_frames) {
                    rx_frames_.push_back(std::move(received));
                }
            }

            /* A frame may complete an already posted RX descriptor. */
            {
                std::lock_guard<std::mutex> lock(device_mutex_);
                process_receive_queue();
            }
            continue;
        }

        if (length < 0 && errno == EINTR) continue;
        if (length < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)) {
            tap_.wait_readable(50);
            continue;
        }

        if (length == 0 || (length < 0 && errno != EINTR)) break;
    }
}

bool virtio_net::take_pending_frame(std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (rx_frames_.empty()) return false;
    frame = std::move(rx_frames_.front());
    rx_frames_.pop_front();
    return true;
}

void virtio_net::mark_device_failed() {
    status_ |= VIRTIO_STATUS_FAILED;
}

void virtio_net::process_receive_queue() {
    virtqueue& queue = rx_queue_;
    if (!queue.valid()) return;

    bool completed_buffer = false;
    while (true) {
        uint16_t descriptor_index = 0;
        virtqueue::descriptor_chain chain;
        const virtqueue::next_result result =
            queue.next_available(descriptor_index, chain);
        if (result == virtqueue::next_result::empty) break;
        if (result == virtqueue::next_result::invalid) {
            mark_device_failed();
            return;
        }

        std::vector<uint8_t> frame;
        if (!take_pending_frame(frame)) break;

        std::vector<uint8_t> packet(virtio_net_header_size + frame.size(), 0);
        std::copy(frame.begin(), frame.end(),
                  packet.begin() + virtio_net_header_size);
        size_t written = 0;
        if (!queue.write_device_writable(chain,
                                         packet.data(),
                                         packet.size(),
                                         written)) {
            mark_device_failed();
            return;
        }

        if (!queue.complete(descriptor_index, static_cast<uint32_t>(written))) {
            mark_device_failed();
            return;
        }

        interrupt_status_ |= 1U;
        completed_buffer = true;
    }

    if (completed_buffer) inject_guest_interrupt();
}

bool virtio_net::write_tap_frame(const std::vector<uint8_t>& bytes) {
    if (bytes.size() > max_frame_size) return false;

    return tap_.write_frame(bytes.data(), bytes.size());
}

void virtio_net::process_transmit_queue() {
    virtqueue& queue = tx_queue_;
    if (!queue.valid()) return;

    bool completed_buffer = false;
    while (true) {
        uint16_t descriptor_index = 0;
        virtqueue::descriptor_chain chain;
        const virtqueue::next_result result =
            queue.next_available(descriptor_index, chain);
        if (result == virtqueue::next_result::empty) break;
        if (result == virtqueue::next_result::invalid) {
            mark_device_failed();
            return;
        }

        std::vector<uint8_t> packet;
        if (!queue.read_device_readable(chain, packet, max_packet_size)) {
            mark_device_failed();
            return;
        }

        uint32_t transmitted_length = 0;
        if (packet.size() >= virtio_net_header_size) {
            packet.erase(packet.begin(),
                         packet.begin() + virtio_net_header_size);
            if (write_tap_frame(packet)) {
                transmitted_length = static_cast<uint32_t>(packet.size());
            }
        }

        if (!queue.complete(descriptor_index, transmitted_length)) {
            mark_device_failed();
            return;
        }

        interrupt_status_ |= 1U;
        completed_buffer = true;
    }

    if (completed_buffer) inject_guest_interrupt();
}

void virtio_net::process_queue(uint16_t queue_index) {
    if (queue_index == 0) {
        process_receive_queue();
    } else if (queue_index == 1) {
        process_transmit_queue();
    }
}

uint64_t virtio_net::read_register(uint32_t offset) const {
    const virtqueue* queue = selected_queue();

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
            return queue != nullptr ? queue->max_size() : 0;
        case VIRTIO_MMIO_REG_QUEUE_NUM:
            return queue != nullptr ? queue->size() : 0;
        case VIRTIO_MMIO_REG_QUEUE_READY:
            return queue != nullptr && queue->ready() ? 1 : 0;
        case VIRTIO_MMIO_REG_INTERRUPT_STATUS:
            return interrupt_status_;
        case VIRTIO_MMIO_REG_STATUS:
            return status_;
        case VIRTIO_MMIO_REG_CONFIG_GENERATION:
            return 0;
        default:
            return 0;
    }
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
    status_ = 0;
    device_features_select_ = 0;
    driver_features_select_ = 0;
    driver_features_ = 0;
    interrupt_status_ = 0;
    selected_queue_ = 0;
    rx_queue_.reset();
    tx_queue_.reset();
}

virtqueue* virtio_net::selected_queue() {
    if (selected_queue_ == 0) return &rx_queue_;
    if (selected_queue_ == 1) return &tx_queue_;
    return nullptr;
}

const virtqueue* virtio_net::selected_queue() const {
    if (selected_queue_ == 0) return &rx_queue_;
    if (selected_queue_ == 1) return &tx_queue_;
    return nullptr;
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
            if (virtqueue* queue = selected_queue()) {
                queue->set_size(static_cast<uint16_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_READY:
            if (virtqueue* queue = selected_queue()) {
                queue->set_ready(value != 0);
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DESC_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DESC_HIGH:
            if (virtqueue* queue = selected_queue()) {
                queue->set_descriptor_address_part(
                    offset == VIRTIO_MMIO_REG_QUEUE_DESC_HIGH,
                    static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DRIVER_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH:
            if (virtqueue* queue = selected_queue()) {
                queue->set_available_ring_address_part(
                    offset == VIRTIO_MMIO_REG_QUEUE_DRIVER_HIGH,
                    static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_DEVICE_LOW:
        case VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH:
            if (virtqueue* queue = selected_queue()) {
                queue->set_used_ring_address_part(
                    offset == VIRTIO_MMIO_REG_QUEUE_DEVICE_HIGH,
                    static_cast<uint32_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_QUEUE_NOTIFY:
            if (value < queue_count && (status_ & VIRTIO_STATUS_DRIVER_OK)) {
                process_queue(static_cast<uint16_t>(value));
            }
            break;
        case VIRTIO_MMIO_REG_INTERRUPT_ACK:
            interrupt_status_ &= ~static_cast<uint32_t>(value);
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
            break;
        }
        default:
            /* The network configuration is read-only in this implementation. */
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
    std::lock_guard<std::mutex> lock(device_mutex_);

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
