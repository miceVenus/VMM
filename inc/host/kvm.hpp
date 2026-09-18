
#pragma once

#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <stdint.h>
#include <linux/kvm.h>

#include "virtio_mmio.h"

/*
 * The following address macros define this VMM's Guest memory layout; they
 * are not fixed addresses mandated by the KVM API. Page tables are allocated
 * downward from the end of Guest RAM, the Guest image is loaded at address 0,
 * and the stack grows downward from the high end of memory.
 */
/* Project convention: the PML4 table is located 0x1000 bytes below Guest RAM's end. */
#define PML4_OFF 0x1000
/* Project convention: the PDP/PDPT table is located 0x2000 bytes below Guest RAM's end. */
#define PDP_OFF  0x2000
/* Project convention: the Page Directory is located 0x3000 bytes below Guest RAM's end. */
#define PD_OFF   0x3000
/* Project convention: Guest image load address and initial Guest RIP. */
#define GUEST_START_ADDR    0x0000
/* Project convention: initial stack-pointer offset counted backward from Guest RAM's end. */
#define STACK_START_OFF     0x10000
/* Architecture: x86-64 supports 2 MiB large pages; this project uses that mapping granularity. */
#define GUEST_PAGE_SIZE     0x200000

class virtio_net;

/*
 * Architecture specification: Intel 64 and IA-32 Architectures Software
 * Developer's Manual Vol. 3A §4.5 (4-level paging and PDE format), together
 * with the compatible AMD64 APM. The following values are standard bit masks
 * for x86-64 page-directory entries (PDEs).
 */
/* Architecture specification: PDE bit 0 (P/Present), marking the entry valid for translation. */
#define PDE64_PRESENT (1ULL << 0)
/* Architecture specification: PDE bit 1 (R/W), allowing writes to the mapped page. */
#define PDE64_RW (1ULL << 1)
/* Architecture specification: PDE bit 2 (U/S), allowing user-mode access to the mapped region. */
#define PDE64_USER (1ULL << 2)
/* Architecture specification: PDE bit 7 (PS/Page Size), making the PDE map a 2 MiB page directly. */
#define PDE64_PS (1ULL << 7)

/*
 * Architecture specification: Intel SDM Vol. 3A §§4.1.1 and 2.5; these are
 * the control-register and IA32_EFER masks used to enable 4-level long mode
 * from protected mode.
 */
/* Architecture specification: CR0 bit 0 (PE/Protection Enable), enabling protected mode. */
#define CR0_PE (1ULL << 0)
/* Architecture specification: CR0 bit 31 (PG/Paging), enabling paging. */
#define CR0_PG (1ULL << 31)
/* Architecture specification: CR4 bit 5 (PAE), enabling Physical Address Extension; required for 4-level paging. */
#define CR4_PAE (1ULL << 5)

/* Architecture specification: IA32_EFER bit 8 (LME), enabling IA-32e/long-mode paging. */
#define EFER_LME (1ULL << 8)
/* Architecture specification: IA32_EFER bit 10 (LMA), indicating active IA-32e mode; it is normally derived from PG and LME, but this project sets it in the initial KVM state as well. */
#define EFER_LMA (1ULL << 10)


struct vm {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    uint8_t *mem_start;
    size_t mem_size;
    struct kvm_run *run;
    int run_mmap_size;
    struct kvm_sregs sregs;
    virtio_net* net; /* Non-owning; child_main owns the device object. */

    /* TAP can inject an interrupt while the vCPU is outside KVM_RUN. */
    std::mutex interrupt_mutex;
    std::condition_variable interrupt_cv;
    bool interrupt_wakeup = false;
};


int vm_init(struct vm &v, size_t mem_size);

void vm_destroy(struct vm &v);

int setup_long_mode(struct vm &v);

int load_guest_image(struct vm &v, const char* path);

int set_context(struct vm &v);
