#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/*
 * Host-side view of one split Virtqueue in Guest RAM.
 *
 * This class knows how to operate the generic Descriptor/Available/Used ring
 * layout. It deliberately knows nothing about Ethernet, TAP, or interrupts.
 */
class virtqueue {
public:
    struct descriptor {
        uint64_t address;
        uint32_t length;
        uint16_t flags;
        uint16_t next;
    };

    using descriptor_chain = std::vector<descriptor>;

    enum class next_result {
        empty,
        available,
        invalid,
    };

    virtqueue(uint8_t* guest_memory,
              size_t guest_memory_size,
              uint16_t max_size);

    virtqueue(const virtqueue&) = delete;
    virtqueue& operator=(const virtqueue&) = delete;

    uint16_t max_size() const noexcept;
    uint16_t size() const noexcept;
    bool ready() const noexcept;
    bool valid() const;

    void set_size(uint16_t size) noexcept;
    void set_ready(bool ready) noexcept;

    void set_descriptor_address_part(bool high, uint32_t value) noexcept;
    void set_available_ring_address_part(bool high,
                                         uint32_t value) noexcept;
    void set_used_ring_address_part(bool high, uint32_t value) noexcept;

    /*
     * Reads the next Guest-published Descriptor chain without consuming it.
     * The chain is consumed only after complete() succeeds.
     */
    next_result next_available(uint16_t& descriptor_index,
                               descriptor_chain& chain) const;

    /* Reads buffers owned by the device. */
    bool read_device_readable(const descriptor_chain& chain,
                              std::vector<uint8_t>& bytes,
                              size_t max_length) const;

    /* Writes buffers owned by the device. */
    bool write_device_writable(const descriptor_chain& chain,
                               const uint8_t* bytes,
                               size_t length,
                               size_t& copied) const;

    /* Publishes a completed buffer in Used Ring and advances the queue. */
    bool complete(uint16_t descriptor_index, uint32_t length);

    void reset() noexcept;

private:
    struct used_element {
        uint32_t id;
        uint32_t length;
    };

    static_assert(sizeof(descriptor) == 16,
                  "Virtio descriptor layout changed");
    static_assert(sizeof(used_element) == 8,
                  "Virtio used element layout changed");

    bool guest_range_is_valid(uint64_t address, size_t length) const;

    template <typename T>
    bool read_guest(uint64_t address, T& value) const;

    template <typename T>
    bool write_guest(uint64_t address, const T& value) const;

    bool read_descriptor_chain(uint16_t head,
                               descriptor_chain& chain) const;
    bool queue_memory_is_valid() const;

    static void set_address_part(uint64_t& address,
                                 bool high,
                                 uint32_t value) noexcept;

    uint8_t* guest_memory_;
    size_t guest_memory_size_;
    uint16_t max_size_;
    uint16_t size_ = 0;
    bool ready_ = false;
    uint64_t descriptor_address_ = 0;
    uint64_t available_ring_address_ = 0;
    uint64_t used_ring_address_ = 0;
    uint16_t last_available_ = 0;
};
