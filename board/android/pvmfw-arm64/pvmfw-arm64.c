// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2017 Tuomas Tynkkynen
 * Copyright (C) 2021 Google LLC
 */

#include <asm/sections.h>
#include <common.h>
#include <cpu_func.h>
#include <dm.h>
#include <dm/root.h>
#include <env.h>
#include <fdtdec.h>
#include <init.h>
#include <log.h>
#include <virtio_types.h>
#include <virtio.h>

#include <asm/armv8/mmu.h>

int pvmfw_boot_flow(void *fdt, size_t fdt_max_size, void *image, size_t size,
		    void *bcc, size_t bcc_size);

/* Assigned in lowlevel_init.S
 * Push the variable into the .data section so that it
 * does not get cleared later.
 */
void * __section(".data") fw_dtb_pointer;
void * __section(".data") fw_kernel_image_pointer;
size_t __section(".data") fw_kernel_image_size;

enum pvmfw_mem_map_idx {
	PVMFW_MEM_MAP_MMIO,
	PVMFW_MEM_MAP_DICE,
	PVMFW_MEM_MAP_SDRAM,
};

static struct mm_region pvmfw_mem_map[] = {
	[PVMFW_MEM_MAP_MMIO] = {
		/*
		 * Emulated I/O: 0x0000_0000-0x0001_0000
		 * PCI (virtio): 0x0001_0000-0x1110_0000
		 * GIC region  : 0x????_????-0x4000_0000
		 */
		.virt = 0x00000000UL,
		.phys = 0x00000000UL,
		.size = SZ_1G,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	},
	/* 0x4000_0000-0x7000_0000: RESERVED */
	[PVMFW_MEM_MAP_DICE] = {
		/* DICE region */
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	},
	[PVMFW_MEM_MAP_SDRAM] = {
		 /* RAM region */
		.virt = CONFIG_SYS_SDRAM_BASE,
		.phys = CONFIG_SYS_SDRAM_BASE,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = pvmfw_mem_map;

static void *locate_bcc(size_t *size)
{
	if (size)
		*size = PAGE_SIZE;

	return (void *)ALIGN((uintptr_t)_end - gd->reloc_off, PAGE_SIZE);
}

void board_cleanup_before_linux(void)
{
	/*
	 * The DM needs instantiated device drivers to be removed before the
	 * payload is executed for features such as DM_FLAG_ACTIVE_DMA and
	 * DM_FLAG_OS_PREPARE to work as expected.
	 *
	 * This is typically handled by the 'bootm' command (and friends) but
	 * those are made unavailable by pvmfw as it provides a fixed boot path
	 * and removes support for CLI. Therefore, it is necessary to manually
	 * handle it in cleanup_before_linux().
	 */
	dm_remove_devices_flags(DM_REMOVE_ACTIVE_ALL);
}

int board_run_command(const char *cmdline)
{
	int err;
	size_t bcc_size;
	void *bcc = locate_bcc(&bcc_size);
	void (*entry)(void *fdt_addr, void *res0, void *res1, void *res2) =
		fw_kernel_image_pointer;

	err = pvmfw_boot_flow(fw_dtb_pointer, CROSVM_FDT_MAX_SIZE,
			      fw_kernel_image_pointer, fw_kernel_image_size,
			      bcc, bcc_size);
	if (err) {
		panic("pvmfw boot failed: %d", err);
		__builtin_unreachable();
	}

	cleanup_before_linux();
	entry(fw_dtb_pointer, 0, 0, 0);
	__builtin_unreachable();
}

int board_init(void)
{
	return 0;
}

int board_late_init(void)
{
	/*
	 * Make sure virtio bus is enumerated so that peripherals
	 * on the virtio bus can be discovered by their drivers
	 */
	virtio_init();

	return 0;
}

int dram_init(void)
{
	size_t bcc_size;
	uintptr_t bcc = (uintptr_t)locate_bcc(&bcc_size);

	pvmfw_mem_map[PVMFW_MEM_MAP_DICE].phys = bcc;
	pvmfw_mem_map[PVMFW_MEM_MAP_DICE].virt = bcc;
	pvmfw_mem_map[PVMFW_MEM_MAP_DICE].size = bcc_size;

	if (fdtdec_setup_mem_size_base() != 0)
		return -EINVAL;

	pvmfw_mem_map[PVMFW_MEM_MAP_SDRAM].size = gd->ram_size;

	return 0;
}

int dram_init_banksize(void)
{
	fdtdec_setup_memory_banksize();

	return 0;
}

void *board_fdt_blob_setup(int *err)
{
	*err = 0;
	return fw_dtb_pointer;
}

int misc_init_r(void)
{
	env_set_hex("fdt_addr", (u64)fw_dtb_pointer);
	env_set_hex("kernel_image_addr", (u64)fw_kernel_image_pointer);
	env_set_hex("kernel_image_size", (u64)fw_kernel_image_size);

	return 0;
}

void enable_caches(void)
{
	 icache_enable();
	 dcache_enable();
}
