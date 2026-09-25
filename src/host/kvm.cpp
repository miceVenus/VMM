#include "host/kvm.hpp"

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/kvm.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

int vm_init(struct vm &v, size_t mem_size) {
	v.vcpu_fd = -1;
	v.run = nullptr;
	v.kvm_fd = v.vm_fd = -1;
	v.mem_start = (uint8_t*)MAP_FAILED;
	v.run_mmap_size = 0;
	v.net = nullptr;
	v.mem_size = mem_size;

	v.kvm_fd = open("/dev/kvm", O_RDWR);
	if (v.kvm_fd < 0) return 0x10;

    int api = ioctl(v.kvm_fd, KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) return 0x11;

	v.vm_fd = ioctl(v.kvm_fd, KVM_CREATE_VM, 0);
	if (v.vm_fd < 0) return 0x12;

	/* Create the in-kernel PIC/IOAPIC before the vCPU. This small VMM only
	 * wires one legacy PIC input; replacing the default routes avoids also
	 * presenting the same Virtio interrupt to the unconfigured IOAPIC. */
	if (ioctl(v.vm_fd, KVM_CREATE_IRQCHIP, 0) < 0) return 0x18;

	struct {
		struct kvm_irq_routing header;
		struct kvm_irq_routing_entry entry;
	} routing{};
	routing.header.nr = 1;
	routing.entry.gsi = VIRTIO_NET_GSI;
	routing.entry.type = KVM_IRQ_ROUTING_IRQCHIP;
	routing.entry.u.irqchip.irqchip = KVM_IRQCHIP_PIC_MASTER;
	routing.entry.u.irqchip.pin = VIRTIO_NET_PIC_IRQ;
	if (ioctl(v.vm_fd, KVM_SET_GSI_ROUTING, &routing) < 0) return 0x19;

	v.mem_start = (uint8_t*)mmap(0, mem_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (v.mem_start == MAP_FAILED) return 0x13;

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .flags = 0,
        .guest_phys_addr = 0,
        .memory_size = v.mem_size, /* bytes */
        .userspace_addr = (uintptr_t)v.mem_start,
    };

    if (ioctl(v.vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) return 0x14;

	v.run_mmap_size = ioctl(v.kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (v.run_mmap_size <= 0) return 0x16;


	v.vcpu_fd = ioctl(v.vm_fd, KVM_CREATE_VCPU, 0);
	if (v.vcpu_fd < 0) return 0x15;

	v.run = (struct kvm_run*)mmap(NULL, v.run_mmap_size, PROT_READ | PROT_WRITE,
					 MAP_SHARED, v.vcpu_fd, 0);
	if (v.run == MAP_FAILED) {
		v.run = nullptr;
		return 0x17;
	}

	return 0;
}

void vm_destroy(struct vm &v) {
	if(v.mem_start && v.mem_start != MAP_FAILED) {
		munmap(v.mem_start, v.mem_size);
		v.mem_start = (uint8_t*)MAP_FAILED;
	}

	if (v.run && v.run != MAP_FAILED) {
		munmap(v.run, (size_t)v.run_mmap_size);
		v.run = nullptr;
	}
	if (v.vcpu_fd >= 0) {
		close(v.vcpu_fd);
		v.vcpu_fd = -1;
	}

	if (v.vm_fd >= 0) {
		close(v.vm_fd);
		v.vm_fd = -1;
	}
	if (v.kvm_fd >= 0) {
		close(v.kvm_fd);
		v.kvm_fd = -1;
	}
}

static void setup_segments_64(struct kvm_sregs &sregs) {

	struct kvm_segment code = {
		.base = 0,
		.limit = ~0U,
        .selector = 0x8,
        .type = 11,     // Code: execute, read, accessed
		.present = 1,
		.dpl = 0,       // Ring 0
		.db = 0,        // Required for 64-bit code
		.s = 1,         // Code/data segment
		.l = 1,         // 64-bit code segment
		.g = 1,         // 4 KiB granularity
        .avl = 0,
        .unusable = 0,
        .padding = 0,
	};
	struct kvm_segment data = code;
	data.type = 3; // Data: read, write, accessed
	data.l = 0;
	data.db = 1;
	data.selector = 0x10; // Data segment selector

	sregs.cs = code;
	sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;
}

int setup_long_mode(struct vm &v) {

	const uint64_t MEM_END = v.mem_size;

	uint64_t pml4_addr = MEM_END - PML4_OFF;
	uint64_t *pml4 = (uint64_t *)(v.mem_start + pml4_addr);

	uint64_t pdpt_addr = MEM_END - PDP_OFF;
	uint64_t *pdpt = (uint64_t *)(v.mem_start + pdpt_addr);

	uint64_t pd_addr = MEM_END - PD_OFF;
	uint64_t *pd = (uint64_t *)(v.mem_start + pd_addr);
	uint64_t gdt_addr = MEM_END - GDT_OFF;
	uint64_t *gdt = (uint64_t *)(v.mem_start + gdt_addr);
	gdt[0] = 0;
	gdt[1] = UINT64_C(0x00af9b000000ffff); // 0x08: ring-0 64-bit code
	gdt[2] = UINT64_C(0x00cf93000000ffff); // 0x10: ring-0 data

	pml4[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pdpt_addr;
	pdpt[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pd_addr;

	const size_t n_pages = v.mem_size / GUEST_PAGE_SIZE;
	for (size_t ix = 0; ix < n_pages; ++ix) {
		const uint64_t page = ix * GUEST_PAGE_SIZE;
		pd[ix] = page | PDE64_PRESENT | PDE64_RW | PDE64_USER | PDE64_PS;
	}

	/* Map the Virtio-MMIO window without adding it to the RAM memslot. */
	const uint64_t mmio_pd_index = VIRTIO_MMIO_BASE_GPA >> 21;
	pd[mmio_pd_index] = VIRTIO_MMIO_BASE_GPA |
						PDE64_PRESENT | PDE64_RW | PDE64_USER | PDE64_PS;

	struct kvm_sregs sregs{};
	if (ioctl(v.vcpu_fd, KVM_GET_SREGS, &sregs) != 0)
		return 0x20;

	sregs.cr3 = pml4_addr;
	sregs.cr4 = CR4_PAE;
	sregs.cr0 = CR0_PE | CR0_PG;
	sregs.efer = EFER_LME | EFER_LMA;
	sregs.gdt.base = gdt_addr;
	sregs.gdt.limit = 3 * sizeof(uint64_t) - 1;

	setup_segments_64(sregs);

	if (ioctl(v.vcpu_fd, KVM_SET_SREGS, &sregs) != 0)
		return 0x22;

    return 0;
	
}

int load_guest_image(struct vm &v, const char* path) {
    FILE *f = fopen(path, "rb");
	if (!f)
		return 0x30;

	if (fseek(f, 0, SEEK_END) < 0) {
		fclose(f);
		return 0x31;
    }

	long fsz = ftell(f);
	if (fsz < 0) {
		fclose(f);
		return 0x32;
	}
	rewind(f);

	if((uint64_t)fsz > v.mem_size - GUEST_START_ADDR) {
		fclose(f);
		return 0x33;
	}

	if (fread(v.mem_start + GUEST_START_ADDR, 1, (size_t)fsz, f) != (size_t)fsz) {
		fclose(f);
		return 0x34;
	}
	fclose(f);

	return 0;
}

int set_context(struct vm &v) {
    struct kvm_regs regs;
    memset(&regs, 0, sizeof(regs));

    regs.rip = GUEST_START_ADDR; 
	regs.rflags = 0x2;

	regs.rsp = v.mem_size - STACK_START_OFF; // Stack grows down.
	if (ioctl(v.vcpu_fd, KVM_SET_REGS, &regs) < 0)
		return 0x40;

    return 0;
}
