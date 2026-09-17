#include "host/virtqueue.hpp"
#include "virtio_mmio.h"

#include <algorithm>
#include <cstring>

virtqueue::virtqueue(uint8_t* guest_memory,
                     size_t guest_memory_size,
                     uint16_t max_size)
    : guest_memory_(guest_memory),
      guest_memory_size_(guest_memory_size),
      max_size_(max_size) {}

uint16_t virtqueue::max_size() const noexcept {
    return max_size_;
}

uint16_t virtqueue::size() const noexcept {
    return size_;
}

bool virtqueue::ready() const noexcept {
    return ready_;
}

bool virtqueue::valid() const {
    return queue_memory_is_valid();
}

void virtqueue::set_size(uint16_t size) noexcept {
    if (size > 0 && size <= max_size_) size_ = size;
}

void virtqueue::set_ready(bool ready) noexcept {
    ready_ = ready;
}

void virtqueue::set_address_part(uint64_t& address,
                                 bool high,
                                 uint32_t value) noexcept {
    if (high) {
        address = (address & UINT64_C(0x00000000ffffffff)) |
                  (static_cast<uint64_t>(value) << 32);
    } else {
        address = (address & UINT64_C(0xffffffff00000000)) | value;
    }
}

void virtqueue::set_descriptor_address_part(bool high,
                                            uint32_t value) noexcept {
    set_address_part(descriptor_address_, high, value);
}

void virtqueue::set_available_ring_address_part(bool high,
                                                uint32_t value) noexcept {
    set_address_part(available_ring_address_, high, value);
}

void virtqueue::set_used_ring_address_part(bool high,
                                           uint32_t value) noexcept {
    set_address_part(used_ring_address_, high, value);
}

bool virtqueue::guest_range_is_valid(uint64_t address, size_t length) const {
    if (address > guest_memory_size_) return false;
    return length <= guest_memory_size_ - address;
}

template <typename T>
bool virtqueue::read_guest(uint64_t address, T& value) const {
    if (!guest_range_is_valid(address, sizeof(T))) return false;
    std::memcpy(&value, guest_memory_ + address, sizeof(T));
    return true;
}

template <typename T>
bool virtqueue::write_guest(uint64_t address, const T& value) const {
    if (!guest_range_is_valid(address, sizeof(T))) return false;
    std::memcpy(guest_memory_ + address, &value, sizeof(T));
    return true;
}

bool virtqueue::queue_memory_is_valid() const {
    if (!ready_ ||
        size_ == 0 ||
        size_ > max_size_ ||
        (size_ & (size_ - 1)) != 0) {
        return false;
    }

    const size_t descriptor_bytes =
        static_cast<size_t>(size_) * sizeof(descriptor);
    const size_t available_ring_bytes =
        4 + static_cast<size_t>(size_) * sizeof(uint16_t);
    const size_t used_ring_bytes =
        4 + static_cast<size_t>(size_) * sizeof(used_element);

    return guest_range_is_valid(descriptor_address_, descriptor_bytes) &&
           guest_range_is_valid(available_ring_address_,
                                available_ring_bytes) &&
           guest_range_is_valid(used_ring_address_, used_ring_bytes);
}

bool virtqueue::read_descriptor_chain(uint16_t head,
                                       descriptor_chain& chain) const {
    if (head >= size_) return false;

    uint16_t index = head;
    for (uint16_t count = 0; count < size_; ++count) {
        if (index >= size_) return false;

        descriptor entry{};
        const uint64_t address = descriptor_address_ +
            static_cast<uint64_t>(index) * sizeof(descriptor);
        if (!read_guest(address, entry)) return false;
        chain.push_back(entry);

        if ((entry.flags & VIRTQ_DESC_F_NEXT) == 0) return true;
        index = entry.next;
    }

    /* A valid chain cannot contain more descriptors than the queue itself. */
    return false;
}

virtqueue::next_result virtqueue::next_available(
    uint16_t& descriptor_index,
    descriptor_chain& chain) const {
    if (!queue_memory_is_valid()) return next_result::invalid;

    uint16_t available_index = 0;
    if (!read_guest(available_ring_address_ + 2, available_index)) {
        return next_result::invalid;
    }
    if (available_index == last_available_) return next_result::empty;
    if (static_cast<uint16_t>(available_index - last_available_) > size_) {
        return next_result::invalid;
    }

    const uint64_t ring_address = available_ring_address_ +
        4 + static_cast<uint64_t>(last_available_ % size_) * sizeof(uint16_t);
    if (!read_guest(ring_address, descriptor_index)) {
        return next_result::invalid;
    }

    return read_descriptor_chain(descriptor_index, chain)
        ? next_result::available
        : next_result::invalid;
}

bool virtqueue::read_device_readable(const descriptor_chain& chain,
                                     std::vector<uint8_t>& bytes,
                                     size_t max_length) const {
    for (const descriptor& entry : chain) {
        if (entry.flags & VIRTQ_DESC_F_WRITE) return false;
        if (!guest_range_is_valid(entry.address, entry.length)) return false;
        if (bytes.size() > max_length ||
            entry.length > max_length - bytes.size()) {
            return false;
        }

        const uint8_t* source = guest_memory_ + entry.address;
        bytes.insert(bytes.end(), source, source + entry.length);
    }
    return true;
}

bool virtqueue::write_device_writable(const descriptor_chain& chain,
                                      const uint8_t* bytes,
                                      size_t length,
                                      size_t& copied) const {
    copied = 0;
    if (bytes == nullptr && length != 0) return false;

    for (const descriptor& entry : chain) {
        if ((entry.flags & VIRTQ_DESC_F_WRITE) == 0) return false;
        if (!guest_range_is_valid(entry.address, entry.length)) return false;

        const size_t remaining = length - copied;
        const size_t chunk = std::min<size_t>(remaining, entry.length);
        if (chunk != 0) {
            std::memcpy(guest_memory_ + entry.address,
                        bytes + copied,
                        chunk);
            copied += chunk;
        }
        if (copied == length) break;
    }

    return copied == length;
}

bool virtqueue::complete(uint16_t descriptor_index, uint32_t length) {
    if (!queue_memory_is_valid()) return false;

    uint16_t used_index = 0;
    if (!read_guest(used_ring_address_ + 2, used_index)) return false;

    const uint64_t element_address = used_ring_address_ +
        4 + static_cast<uint64_t>(used_index % size_) * sizeof(used_element);
    const used_element element{
        static_cast<uint32_t>(descriptor_index),
        length,
    };

    if (!write_guest(element_address, element)) return false;

    __sync_synchronize();
    ++used_index;
    if (!write_guest(used_ring_address_ + 2, used_index)) return false;

    ++last_available_;
    return true;
}

void virtqueue::reset() noexcept {
    size_ = 0;
    ready_ = false;
    descriptor_address_ = 0;
    available_ring_address_ = 0;
    used_ring_address_ = 0;
    last_available_ = 0;
}
