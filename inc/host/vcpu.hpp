#pragma once

#include "host/kvm.hpp"

#include <stdint.h>

/* The console is kept as a small diagnostic interface for Guest programs. */
static constexpr uint16_t CIO_PORT = 0xE9;

int run_vcpu(struct vm &v, int vm_id);
