
#pragma once

#include <cstddef>
#include <stdint.h>
#include <linux/kvm.h>

#include "virtio_mmio.h"


/* Project-defined offsets, measured from the end of Guest RAM, reserved for
 * the four-level x86-64 page-table structures. */
/* Offset of the PML4 (top-level page-map level-4 table). */
#define PML4_OFF 0x1000
/* Offset of the PDP/PML3 (page-directory-pointer table). */
#define PDP_OFF  0x2000
/* Offset of the PD (page-directory table). */
#define PD_OFF   0x3000
/* Three descriptors: null, ring-0 64-bit code, and ring-0 data. */
#define GDT_OFF  0x4000

/* Project-defined Guest physical address where the Guest image is loaded. */
#define GUEST_START_ADDR    0x0000
/* Project-defined distance below the end of Guest RAM used as the initial SP. */
#define STACK_START_OFF     0x10000
/* x86-64 large-page size used for Guest identity mapping: 2 MiB. */
#define GUEST_PAGE_SIZE     0x200000

class virtio_net;

/*
 * Intel/AMD x86-64 page-table entry flags. They are bit positions in a
 * 64-bit PDE/PDPTE/PML4E value, used to describe the Guest address space.
 */
/* Present (P): entry is valid and the page/table is available. */
#define PDE64_PRESENT (1ULL << 0)
/* Read/write (R/W): 1 permits writes through this entry. */
#define PDE64_RW (1ULL << 1)
/* User/supervisor (U/S): 1 permits user-mode access. */
#define PDE64_USER (1ULL << 2)
/* Page size (PS): 1 makes a PDE map a large page instead of a next-level table. */
#define PDE64_PS (1ULL << 7)

/*
 * x86 control-register flags from the Intel/AMD architecture manuals.
 */
/* CR0.PE: enables protected mode. */
#define CR0_PE (1ULL << 0)
/* CR0.PG: enables paging. */
#define CR0_PG (1ULL << 31)
/* CR4.PAE: enables Physical Address Extension, required before long mode. */
#define CR4_PAE (1ULL << 5)

/* EFER.LME: enables long-mode operation when paging is subsequently enabled. */
#define EFER_LME (1ULL << 8)
/* EFER.LMA: indicates that the processor is currently active in long mode. */
#define EFER_LMA (1ULL << 10)


struct vm {
    int kvm_fd;
    int vm_fd;
    int vcpu_fd;
    uint8_t *mem_start;
    size_t mem_size;
    struct kvm_run *run;
    int run_mmap_size;
    virtio_net* net; /* Non-owning; child_main owns the device object. */

};


int vm_init(struct vm &v, size_t mem_size);

void vm_destroy(struct vm &v);

int setup_long_mode(struct vm &v);

int load_guest_image(struct vm &v, const char* path);

int set_context(struct vm &v);
