# SPDX-License-Identifier: GPL-2.0+

obj-y += fb_common.o
obj-y += fb_getvar.o
obj-y += fb_command.o
obj-$(CONFIG_FASTBOOT_FLASH_BLOCK) += fb_block.o
# MMC reuses block implementation
obj-$(CONFIG_FASTBOOT_FLASH_MMC) += fb_block.o fb_mmc.o
obj-$(CONFIG_FASTBOOT_FLASH_NAND) += fb_nand.o
