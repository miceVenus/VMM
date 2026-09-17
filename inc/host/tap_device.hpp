#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>

/* Small RAII wrapper around one Linux TAP interface. */
class tap_device {
public:
    tap_device() = default;
    ~tap_device();

    tap_device(const tap_device&) = delete;
    tap_device& operator=(const tap_device&) = delete;

    bool open(int vm_id);
    void close() noexcept;

    ssize_t read_frame(void* buffer, size_t capacity) const;
    bool write_frame(const void* buffer, size_t length) const;
    void wait_readable(int timeout_ms) const;

    const std::string& name() const noexcept;

private:
    int fd_ = -1;
    std::string name_;
};
