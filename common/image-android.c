// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2011 Sebastian Andrzej Siewior <bigeasy@linutronix.de>
 */

#include <common.h>
#include <env.h>
#include <image.h>
#include <image-android-dt.h>
#include <android_image.h>
#include <malloc.h>
#include <mapmem.h>
#include <errno.h>
#include <asm/unaligned.h>
#include <mapmem.h>

#define BLK_CNT(_num_bytes, _block_size) ((_num_bytes + _block_size - 1) / \
    _block_size)

/**
 * android_image_get_kernel() - processes kernel part of Android boot images
 * @hdr:	Pointer to image header, which is at the start
 *			of the image.
 * @verify:	Checksum verification flag. Currently unimplemented.
 * @os_data:	Pointer to a ulong variable, will hold os data start
 *			address.
 * @os_len:	Pointer to a ulong variable, will hold os data length.
 *
 * This function returns the os image's start address and length. Also,
 * it appends the kernel command line to the bootargs env variable.
 *
 * Return: Zero, os start address and length on success,
 *		otherwise on failure.
 */
int android_image_get_kernel(const andr_boot_info *boot_info, int verify,
			     ulong *os_data, ulong *os_len)
{
	return 0;
}

int android_image_check_header(const andr_boot_info *boot_info)
{
	return 0;
}

ulong android_image_get_end(const andr_boot_info *boot_info)
{
	return 0;
}

ulong android_image_get_kload(const andr_boot_info *boot_info)
{
	return (ulong)(boot_info->kernel_addr);
}

ulong android_image_get_kcomp(const andr_boot_info *boot_info)
{
	return 0;
}

int android_image_get_ramdisk(const andr_boot_info *boot_info,
			      ulong *rd_data, ulong *rd_len)
{
	*rd_data = (ulong)(boot_info->vendor_ramdisk_addr);
	*rd_len = boot_info->vendor_ramdisk_size + boot_info->boot_ramdisk_size;
	return 0;
}

static struct boot_img_hdr_v3* _extract_boot_image_header(
		struct blk_desc *dev_desc,
		const disk_partition_t *boot_img) {
	long blk_cnt, blks_read;
	blk_cnt = BLK_CNT(sizeof(struct boot_img_hdr_v3), boot_img->blksz);

	struct boot_img_hdr_v3 *boot_hdr = (struct boot_img_hdr_v3*)
		(malloc(blk_cnt * boot_img->blksz));

	if(!blk_cnt || !boot_hdr) {
		return NULL;
	}

	blks_read  = blk_dread(dev_desc, boot_img->start, blk_cnt, boot_hdr);
	if(blks_read != blk_cnt) {
		debug("boot img header blk cnt is %ld and blks read is %ld\n",
			blk_cnt, blks_read);
		return NULL;
	}

	if(strncmp(ANDR_BOOT_MAGIC, (const char *)boot_hdr->magic,
		   ANDR_BOOT_MAGIC_SIZE)) {
		debug("boot header magic is invalid.\n");
		return NULL;
	}

	if(boot_hdr->header_version != 3) {
		debug("boot header is not v3.\n");
		return NULL;
	}

	// TODO Add support for boot headers v1 and v2.
	return boot_hdr;
}

static struct vendor_boot_img_hdr_v3* _extract_vendor_boot_image_header(
		struct blk_desc *dev_desc,
		const disk_partition_t *vendor_boot_img) {
	long blk_cnt, blks_read;
	blk_cnt = BLK_CNT(sizeof(struct vendor_boot_img_hdr_v3),
			vendor_boot_img->blksz);

	struct vendor_boot_img_hdr_v3 *vboot_hdr =
		(struct vendor_boot_img_hdr_v3*)
		(malloc(blk_cnt * vendor_boot_img->blksz));

	if(!blk_cnt || !vboot_hdr) {
		return NULL;
	}

	blks_read = blk_dread(dev_desc, vendor_boot_img->start, blk_cnt, vboot_hdr);
	if(blks_read != blk_cnt) {
		debug("vboot img header blk cnt is %ld and blks read is %ld\n",
			blk_cnt, blks_read);
		return NULL;
	}

	if(strncmp(VENDOR_BOOT_MAGIC, (const char *)vboot_hdr->magic,
		   VENDOR_BOOT_MAGIC_SIZE)) {
		debug("vendor boot header magic is invalid.\n");
		return NULL;
	}

	if(vboot_hdr->header_version != 3) {
		debug("vendor boot header is not v3.\n");
		return NULL;
	}

	return vboot_hdr;
}

static void _populate_boot_info(const struct boot_img_hdr_v3* boot_hdr,
		const struct vendor_boot_img_hdr_v3* vboot_hdr,
		const void* load_addr,
		andr_boot_info *boot_info) {
	boot_info->kernel_size = boot_hdr->kernel_size;
	boot_info->boot_ramdisk_size = boot_hdr->ramdisk_size;
	boot_info->vendor_ramdisk_size = vboot_hdr->vendor_ramdisk_size;
	boot_info->tags_addr = vboot_hdr->tags_addr;
	boot_info->os_version = boot_hdr->os_version;
	boot_info->page_size = vboot_hdr->page_size;
	boot_info->dtb_size = vboot_hdr->dtb_size;
	boot_info->dtb_addr = vboot_hdr->dtb_addr;

	memset(boot_info->name, 0, ANDR_BOOT_NAME_SIZE);
	strncpy(boot_info->name, (const char *)vboot_hdr->name,
		ANDR_BOOT_NAME_SIZE);

	memset(boot_info->cmdline, 0, TOTAL_BOOT_ARGS_SIZE);

	strncpy(boot_info->cmdline, (const char *)boot_hdr->cmdline,
		sizeof(boot_hdr->cmdline));
	strncat(boot_info->cmdline, " ", 1);
	strncat(boot_info->cmdline, (const char *)vboot_hdr->cmdline,
		sizeof(vboot_hdr->cmdline));

	boot_info->kernel_addr = (ulong)load_addr;
	boot_info->vendor_ramdisk_addr = boot_info->kernel_addr
		+ ALIGN(boot_info->kernel_size, vboot_hdr->page_size);
	boot_info->boot_ramdisk_addr = boot_info->vendor_ramdisk_addr
		+ boot_info->vendor_ramdisk_size;
}

static bool _read_in_kernel(struct blk_desc *dev_desc,
		const disk_partition_t *boot_img,
		const andr_boot_info *boot_info) {
	lbaint_t kernel_lba = boot_img->start
		+ BLK_CNT(ANDR_BOOT_IMG_HDR_SIZE, boot_img->blksz);
	u32 kernel_size_page_aligned =
		ALIGN(boot_info->kernel_size, ANDR_BOOT_IMG_HDR_SIZE);

	long blk_cnt, blks_read;
	blk_cnt = BLK_CNT(kernel_size_page_aligned, boot_img->blksz);
	blks_read = blk_dread(dev_desc, kernel_lba, blk_cnt,
			(void*)boot_info->kernel_addr);

	if(blk_cnt != blks_read) {
		debug("Reading out %lu blocks containing the kernel."
				"Expect to read out %lu blks.\n",
				blks_read, blk_cnt);
		return false;
	}

	return true;
}

static bool _read_in_vendor_ramdisk(struct blk_desc *dev_desc,
		const disk_partition_t *vendor_boot_img,
		const andr_boot_info *boot_info) {
	u32 vendor_hdr_size_page_aligned = 
		ALIGN(sizeof(struct vendor_boot_img_hdr_v3),
			boot_info->page_size);
	u32 vendor_ramdisk_size_page_aligned = 
		ALIGN(boot_info->vendor_ramdisk_size, boot_info->page_size);
	lbaint_t ramdisk_lba = vendor_boot_img->start
		+ BLK_CNT(vendor_hdr_size_page_aligned, vendor_boot_img->blksz);

	long blk_cnt, blks_read;
	blk_cnt = BLK_CNT(vendor_ramdisk_size_page_aligned,
			vendor_boot_img->blksz);
	blks_read = blk_dread(dev_desc, ramdisk_lba, blk_cnt,
			(void*)boot_info->vendor_ramdisk_addr);

	if(blk_cnt != blks_read) {
		debug("Reading out %lu blocks containing the vendor ramdisk."
				"Expect to read out %lu blks.\n",
				blks_read, blk_cnt);
		return false;
	}

	return true;
}

static bool _read_in_boot_ramdisk(struct blk_desc *dev_desc,
		const disk_partition_t *boot_img,
		const andr_boot_info *boot_info) {
	u32 kernel_size_page_aligned =
		ALIGN(boot_info->kernel_size, ANDR_BOOT_IMG_HDR_SIZE);
	lbaint_t ramdisk_lba = boot_img->start
		+ BLK_CNT(ANDR_BOOT_IMG_HDR_SIZE, boot_img->blksz)
		+ BLK_CNT(kernel_size_page_aligned, boot_img->blksz);
	u32 ramdisk_size_page_aligned =
		ALIGN(boot_info->boot_ramdisk_size, ANDR_BOOT_IMG_PAGE_SIZE);

	long blk_cnt, blks_read;
	blk_cnt = BLK_CNT(ramdisk_size_page_aligned, boot_img->blksz);
	blks_read = blk_dread(dev_desc, ramdisk_lba, blk_cnt,
			(void*)boot_info->boot_ramdisk_addr);

	if(blk_cnt != blks_read) {
		debug("Reading out %lu blocks containing the boot ramdisk."
				"Expect to read out %lu blks.\n",
				blks_read, blk_cnt);
		return false;
	}

	return true;
}

andr_boot_info* android_image_load(struct blk_desc *dev_desc,
			const disk_partition_t *boot_img,
			const disk_partition_t *vendor_boot_img,
			unsigned long load_address) {
	struct boot_img_hdr_v3 *boot_hdr = NULL;
	struct vendor_boot_img_hdr_v3 *vboot_hdr = NULL;
	andr_boot_info *boot_info = NULL;
	void *kernel_rd_addr = NULL;

	if(!dev_desc || !boot_img || !vendor_boot_img || !load_address) {
		debug("Android Image load inputs are invalid.\n");
		goto image_load_exit;
	}

	boot_hdr = _extract_boot_image_header(dev_desc, boot_img);
	vboot_hdr = _extract_vendor_boot_image_header(dev_desc,
						      vendor_boot_img);
	if(!boot_hdr || !vboot_hdr) {
		goto image_load_exit;
	}

	boot_info = (andr_boot_info*)malloc(sizeof(andr_boot_info));
	if(!boot_info) {
		debug("Couldn't allocate memory for boot info.\n");
		goto image_load_exit;
	}

	// Read in kernel and ramdisk.
	// TODO cap this memory eventually by only mapping exactly as much
	// memory as needed
	kernel_rd_addr = map_sysmem(load_address, 0 /* size */);
	if(!kernel_rd_addr) {
		debug("Can't map the input load address.\n");
		goto image_load_exit;
	}

	_populate_boot_info(boot_hdr, vboot_hdr, kernel_rd_addr, boot_info);
	if(!_read_in_kernel(dev_desc, boot_img, boot_info)
		|| !_read_in_vendor_ramdisk(dev_desc, vendor_boot_img, boot_info)
		|| !_read_in_boot_ramdisk(dev_desc, boot_img, boot_info)) {
		goto image_load_exit;
	}

	free(boot_hdr);
	free(vboot_hdr);
	return boot_info;

image_load_exit:
	free(boot_hdr);
	free(vboot_hdr);
	free(boot_info);
	unmap_sysmem(kernel_rd_addr);
	return NULL;
}

int android_image_get_second(const andr_boot_info *boot_info,
			      ulong *second_data, ulong *second_len)
{
	return -1;
}

/**
 * android_image_get_dtbo() - Get address and size of recovery DTBO image.
 * @hdr_addr: Boot image header address
 * @addr: If not NULL, will contain address of recovery DTBO image
 * @size: If not NULL, will contain size of recovery DTBO image
 *
 * Get the address and size of DTBO image in "Recovery DTBO" area of Android
 * Boot Image in RAM. The format of this image is Android DTBO (see
 * corresponding "DTB/DTBO Partitions" AOSP documentation for details). Once
 * the address is obtained from this function, one can use 'adtimg' U-Boot
 * command or android_dt_*() functions to extract desired DTBO blob.
 *
 * This DTBO (included in boot image) is only needed for non-A/B devices, and it
 * only can be found in recovery image. On A/B devices we can always rely on
 * "dtbo" partition. See "Including DTBO in Recovery for Non-A/B Devices" in
 * AOSP documentation for details.
 *
 * Return: true on success or false on error.
 */
bool android_image_get_dtbo(ulong hdr_addr, ulong *addr, u32 *size)
{
	return false;
}

/**
 * android_image_get_dtb_by_index() - Get address and size of blob in DTB area.
 * @hdr_addr: Boot image header address
 * @index: Index of desired DTB in DTB area (starting from 0)
 * @addr: If not NULL, will contain address to specified DTB
 * @size: If not NULL, will contain size of specified DTB
 *
 * Get the address and size of DTB blob by its index in DTB area of Android
 * Boot Image in RAM.
 *
 * Return: true on success or false on error.
 */
bool android_image_get_dtb_by_index(ulong hdr_addr, u32 index, ulong *addr,
				    u32 *size)
{
	return false;
}

#if !defined(CONFIG_SPL_BUILD)
/**
 * android_print_contents - prints out the contents of the Android format image
 * @hdr: pointer to the Android format image header
 *
 * android_print_contents() formats a multi line Android image contents
 * description.
 * The routine prints out Android image properties
 *
 * returns:
 *     no returned results
 */
void android_print_contents(const andr_boot_info *boot_info)
{
	const char * const p = IMAGE_INDENT_STRING;
	/* os_version = ver << 11 | lvl */
	u32 os_ver = boot_info->os_version >> 11;
	u32 os_lvl = boot_info->os_version & ((1U << 11) - 1);

	printf("%skernel size:          %x\n", p, boot_info->kernel_size);
	printf("%skernel address:       %x\n", p, boot_info->kernel_addr);
	printf("%sramdisk size:         %x\n", p,
		boot_info->vendor_ramdisk_size + boot_info->boot_ramdisk_size);
	printf("%sramdisk address:      %x\n", p,
		boot_info->vendor_ramdisk_addr);
	printf("%stags address:         %x\n", p, boot_info->tags_addr);
	printf("%spage size:            %x\n", p, boot_info->page_size);
	/* ver = A << 14 | B << 7 | C         (7 bits for each of A, B, C)
	 * lvl = ((Y - 2000) & 127) << 4 | M  (7 bits for Y, 4 bits for M) */
	printf("%sos_version:           %x (ver: %u.%u.%u, level: %u.%u)\n",
	       p, boot_info->os_version,
	       (os_ver >> 7) & 0x7F, (os_ver >> 14) & 0x7F, os_ver & 0x7F,
	       (os_lvl >> 4) + 2000, os_lvl & 0x0F);
	printf("%sname:                 %s\n", p, boot_info->name);
	printf("%scmdline:              %s\n", p, boot_info->cmdline);
}

/**
 * android_image_print_dtb_contents() - Print info for DTB blobs in DTB area.
 * @hdr_addr: Boot image header address
 *
 * DTB payload in Android Boot Image v2+ can be in one of following formats:
 *   1. Concatenated DTB blobs
 *   2. Android DTBO format (see CONFIG_CMD_ADTIMG for details)
 *
 * This function does next:
 *   1. Prints out the format used in DTB area
 *   2. Iterates over all DTB blobs in DTB area and prints out the info for
 *      each blob.
 *
 * Return: true on success or false on error.
 */
bool android_image_print_dtb_contents(ulong hdr_addr)
{
	return true;
}
#endif
