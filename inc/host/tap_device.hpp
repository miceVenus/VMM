#pragma once

#include <cstddef>
#include <string>

/* Small RAII wrapper around one Linux TAP interface. */
class tap_device {
public:
    tap_device() = default;
    ~tap_device();

    tap_device(const tap_device&) = delete;
    tap_device& operator=(const tap_device&) = delete;

    bool open(int vm_id);
    void close() noexcept;

    int fd() const noexcept;
    const std::string& name() const noexcept;

private:
    int fd_ = -1;
    std::string name_;
};
