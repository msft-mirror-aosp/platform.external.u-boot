/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2017 The Android Open Source Project
 */

#ifndef __ANDROID_AB_H
#define __ANDROID_AB_H

#include <common.h>

/* Android standard boot slot names are 'a', 'b', 'c', ... */
#define BOOT_SLOT_NAME(slot_num) ('a' + (slot_num))

/* Number of slots */
#define NUM_SLOTS 2

/**
 * Select the slot where to boot from.
 *
 * On Android devices with more than one boot slot (multiple copies of the
 * kernel and system images) selects which slot should be used to boot from and
 * registers the boot attempt. This is used in by the new A/B update model where
 * one slot is updated in the background while running from the other slot. If
 * the selected slot did not successfully boot in the past, a boot attempt is
 * registered before returning from this function so it isn't selected
 * indefinitely.
 *
 * @param[in] dev_desc Device where we should read/write the boot_control struct.
 * @param[in] part_info Partition on the 'dev_desc' to read/write.
 * @param[in] normal_boot True if a normal boot, false if booting to recovery.
 * @return The slot number (>= 0) on success, or a negative on error
 */
int ab_select_slot(struct blk_desc *dev_desc, const disk_partition_t *part_info,
				   bool normal_boot);

#endif /* __ANDROID_AB_H */
