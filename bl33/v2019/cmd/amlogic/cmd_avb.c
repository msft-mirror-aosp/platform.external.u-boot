// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2019 Amlogic, Inc. All rights reserved.
 */

#include <common.h>
#include <command.h>
#include <environment.h>
#include <malloc.h>
#include <asm/byteorder.h>
#include <config.h>
#include <asm/arch/io.h>
#include <partition_table.h>
#include <libavb.h>
#include <emmc_partitions.h>
#include <amlogic/storage.h>
#include <asm/arch/bl31_apis.h>
#if defined(CONFIG_AML_ANTIROLLBACK) || defined(CONFIG_AML_AVB2_ANTIROLLBACK)
#include <amlogic/anti-rollback.h>
#endif
#include <version.h>
#include <amlogic/aml_efuse.h>

#define AVB_USE_TESTKEY
#define MAX_DTB_SIZE (AML_DTB_IMG_MAX_SZ + 512)
#define DTB_PARTITION_SIZE 258048
#define AVB_NUM_SLOT (4)
#define MAX_AVBKEY_LEN (8 + 1024)

#define CONFIG_AVB2_KPUB_EMBEDDED
#define CONFIG_AVB2_KPUB_DEFAULT

#ifdef CONFIG_AVB2_KPUB_VENDOR
extern const char avb2_kpub_vendor[];
extern const int avb2_kpub_vendor_len;
#endif /* CONFIG_AVB_KPUB_VENDOR */

extern const char avb2_kpub_default[];
extern const int avb2_kpub_default_len;
extern const char avb2_kpub_production[];
extern const int avb2_kpub_production_len;

#ifndef CONFIG_AVB2_KPUB_FROM_FIP
#define CONFIG_AVB2_KPUB_FROM_FIP (0)
#endif

#if CONFIG_AVB2_KPUB_FROM_FIP
int compare_avbkey_with_fipkey(const uint8_t* public_key_data, size_t public_key_length);
#endif

AvbOps avb_ops_;

static AvbIOResult read_from_partition(AvbOps *ops, const char *partition, int64_t offset,
		size_t num_bytes, void *buffer, size_t *out_num_read)
{
	int rc = 0;
	uint64_t part_bytes = 0;
	AvbIOResult result = AVB_IO_RESULT_OK;

	if (ops->get_size_of_partition(ops, partition, &part_bytes) != AVB_IO_RESULT_OK) {
		result = AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
		goto out;
	}

	if (part_bytes < offset) {
		result = AVB_IO_RESULT_ERROR_RANGE_OUTSIDE_PARTITION;
		goto out;
	}

	*out_num_read = 0;
	if (!strcmp(partition, "dt_a") || !strcmp(partition, "dt_b") ||
			!strcmp(partition, "dt")) {
		char *dtb_buf = malloc(MAX_DTB_SIZE);

		if (!dtb_buf) {
			result = AVB_IO_RESULT_ERROR_OOM;
			goto out;
		}

		/* rc = store_dtb_rw(dtb_buf, MAX_DTB_SIZE, 2); */
		memset(dtb_buf, 0x00, MAX_DTB_SIZE);
		rc = store_rsv_read("dtb", MAX_DTB_SIZE, (void *)dtb_buf);
		if (rc) {
			printf("Failed to read dtb\n");
			free(dtb_buf);
			result = AVB_IO_RESULT_ERROR_IO;
			goto out;
		} else {
			*out_num_read = num_bytes > MAX_DTB_SIZE ? MAX_DTB_SIZE : num_bytes;
			memcpy(buffer, dtb_buf, *out_num_read);
			free(dtb_buf);
			result = AVB_IO_RESULT_OK;
			goto out;
		}
	} else {
		rc = store_read(partition, offset, num_bytes, buffer);
		if (rc) {
			printf("Failed to read %zdB from part[%s] at %lld\n",
					num_bytes, partition, offset);
			result = AVB_IO_RESULT_ERROR_IO;
			goto out;
		}
		*out_num_read = num_bytes;
	}

out:
	return result;
}

static AvbIOResult write_to_partition(AvbOps *ops, const char *partition,
		int64_t offset, size_t num_bytes, const void *buffer)
{
	int rc = 0;
	uint64_t part_bytes = 0;
	AvbIOResult result = AVB_IO_RESULT_OK;

	if (ops->get_size_of_partition(ops, partition, &part_bytes) != AVB_IO_RESULT_OK) {
		result = AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
		goto out;
	}
	if (part_bytes < offset) {
		result = AVB_IO_RESULT_ERROR_RANGE_OUTSIDE_PARTITION;
		goto out;
	}

	if (!strcmp(partition, "dt_a") || !strcmp(partition, "dt_b") ||
			!strcmp(partition, "dt")) {
		if (offset)
			return AVB_IO_RESULT_ERROR_IO;
		/* rc = store_dtb_rw((void *)buffer, num_bytes, 1); */
		rc = store_rsv_write("dtb", num_bytes, (void *)buffer);
		if (rc) {
			printf("Failed to write dtb\n");
			result = AVB_IO_RESULT_ERROR_IO;
			goto out;
		} else {
			result = AVB_IO_RESULT_OK;
			goto out;
		}
	} else {
		rc = store_write(partition, offset, num_bytes, (unsigned char *)buffer);
		if (rc) {
			printf("Failed to write %zdB from part[%s] at %lld\n",
					num_bytes, partition, offset);
			result = AVB_IO_RESULT_ERROR_IO;
			goto out;
		}
	}

out:
	return result;
}

static AvbIOResult get_unique_guid_for_partition(AvbOps *ops, const char *partition,
		char *guid_buf, size_t guid_buf_size)
{
	char *s1;
	int ret;
	char part_name[128];

	memset(guid_buf, 0, guid_buf_size);
	s1 = env_get("active_slot");
	if (!s1) {
		run_command("get_valid_slot;", 0);
		s1 = env_get("active_slot");
	}
	//printf("active_slot is %s\n", s1);
	if (!memcmp(partition, "system", strlen("system"))) {
		if (s1 && (strcmp(s1, "_a") == 0)) {
			ret = get_partition_num_by_name("system_a");
			sprintf(part_name, "/dev/mmcblk0p%d", (ret + 1));
			strncpy(guid_buf, part_name, guid_buf_size);
		} else if (s1 && (strcmp(s1, "_b") == 0)) {
			ret = get_partition_num_by_name("system_b");
			sprintf(part_name, "/dev/mmcblk0p%d", (ret + 1));
			strncpy(guid_buf, part_name, guid_buf_size);
		} else {
			ret = get_partition_num_by_name("system");
			sprintf(part_name, "/dev/mmcblk0p%d", (ret + 1));
			strncpy(guid_buf, part_name, guid_buf_size);
		}
	} else if (!memcmp(partition, "vbmeta", strlen("vbmeta"))) {
		strncpy(guid_buf, "/dev/block/vbmeta", guid_buf_size);
	}
	return AVB_IO_RESULT_OK;
}

static AvbIOResult get_size_of_partition(AvbOps *ops, const char *partition,
		uint64_t *out_size_num_bytes)
{
	int rc = 0;

	if (!strcmp(partition, "dt_a") || !strcmp(partition, "dt_b") ||
			!strcmp(partition, "dt")) {
		*out_size_num_bytes = DTB_PARTITION_SIZE;
	} else {
		rc = store_part_size(partition);
		if (rc == 1) {
			printf("Failed to get partition[%s] size\n", partition);
			return AVB_IO_RESULT_ERROR_NO_SUCH_PARTITION;
		}
		*out_size_num_bytes = rc;
	}

	return AVB_IO_RESULT_OK;
}

/**
 * normally, we should read vendor avb public key from a virtual partition with the name avb_custom_key.
 * Flashing and erasing this partition only works in the UNLOCKED state. Setting the custom key is done like this:
 * $ avbtool extract_public_key --key key.pem --output pkmd.bin
 * $ fastboot flash avb_custom_key pkmd.bin
 *
 * Erasing the key is done by erasing the virtual partition:
 * $ fastboot erase avb_custom_key
 */
static AvbIOResult validate_vbmeta_public_key(AvbOps *ops, const uint8_t *public_key_data,
		size_t public_key_length, const uint8_t *public_key_metadata,
		size_t public_key_metadata_length, bool *out_is_trusted)
{
	*out_is_trusted = false;
	AvbIOResult ret = AVB_IO_RESULT_ERROR_IO;

#if defined(CONFIG_AVB2_KPUB_VENDOR)
	printf("AVB2 verify with vendor kpub\n");
	if (avb2_kpub_vendor_len == public_key_length &&
			!avb_safe_memcmp(public_key_data,
			avb2_kpub_vendor, avb2_kpub_vendor_len)) {
		*out_is_trusted = true;
		ret = AVB_IO_RESULT_OK;
	}
#ifdef CONFIG_TESTKEY

	unsigned int isSecure = IS_FEAT_BOOT_VERIFY();

	printf("isSecure: %d\n", isSecure);

	if (isSecure == 0) {
		/**
		 * Allow re-verify with default AVB2 public key if really want to do.
		 *
		 * Use of this is *NOT* typical and you should really know what you are
		 * doing if want to enable this.
		 */
		printf("AVB2 re-verify with default kpub\n");
		if (avb2_kpub_default_len == public_key_length &&
				!avb_safe_memcmp(public_key_data,
				avb2_kpub_default, public_key_length)) {
			*out_is_trusted = true;
			ret = AVB_IO_RESULT_OK;
		}
	}
#endif
#elif defined(CONFIG_AVB2_KPUB_DEFAULT)
	printf("AVB2 verify with default kpub\n");
	if (avb2_kpub_default_len == public_key_length &&
			!avb_safe_memcmp(public_key_data,
			avb2_kpub_default, public_key_length)) {
		*out_is_trusted = true;
		ret = AVB_IO_RESULT_OK;
	}
#else
  #error "No AVB2 public key defined"
#endif /* CONFIG_AVB2_KPUB_VENDOR */

	if (ret != AVB_IO_RESULT_OK)
		printf("AVB2 key in bootloader does not match with the key in vbmeta\n");
	return ret;
}

static AvbIOResult read_rollback_index(AvbOps *ops, size_t rollback_index_location,
		uint64_t *out_rollback_index)
{
#if defined(CONFIG_AML_ANTIROLLBACK) || defined(CONFIG_AML_AVB2_ANTIROLLBACK)
	uint32_t version;

	if (get_avb_antirollback(rollback_index_location, &version)) {
		*out_rollback_index = version;
	} else {
		printf("failed to read rollback index: %zd\n", rollback_index_location);
		return AVB_IO_RESULT_ERROR_NO_SUCH_VALUE;
	}
#else
	*out_rollback_index = 0;
#endif
	return AVB_IO_RESULT_OK;
}

static AvbIOResult write_rollback_index(AvbOps* ops, size_t rollback_index_location,
		uint64_t rollback_index)
{
	AvbIOResult result = AVB_IO_RESULT_OK;
#if defined(CONFIG_AML_ANTIROLLBACK) || defined(CONFIG_AML_AVB2_ANTIROLLBACK)
	uint32_t version = rollback_index;

	if (set_avb_antirollback(rollback_index_location, version)) {
		result = AVB_IO_RESULT_OK;
		goto out;
	} else {
		printf("failed to set rollback index: %zd, version: %u\n",
			rollback_index_location, version);
		result = AVB_IO_RESULT_ERROR_NO_SUCH_VALUE;
		goto out;
	}
out:
#endif
	return result;
}

static AvbIOResult read_is_device_unlocked(AvbOps* ops, bool* out_is_unlocked)
{
	AvbIOResult result = AVB_IO_RESULT_OK;
#if defined(CONFIG_AML_ANTIROLLBACK) || defined(CONFIG_AML_AVB2_ANTIROLLBACK)
	uint32_t lock_state;
	char *lock_s;

	if (get_avb_lock_state(&lock_state)) {
		*out_is_unlocked = !lock_state;
		lock_s = env_get("lock");
		if (*out_is_unlocked)
			lock_s[4] = '0';
		else
			lock_s[4] = '1';
		lock_s = env_get("lock");
		result = AVB_IO_RESULT_OK;
		goto out;
	} else {
		printf("failed to read device lock status from rpmb\n");
		result = AVB_IO_RESULT_ERROR_IO;
		goto out;
	}
#else
	char *lock_s;
	LockData_t info;

	lock_s = env_get("lock");
	if (!lock_s) {
		result = AVB_IO_RESULT_ERROR_IO;
		goto out;
	}

	memset(&info, 0, sizeof(struct LockData));

	info.version_major = (int)(lock_s[0] - '0');
	info.version_minor = (int)(lock_s[1] - '0');
	info.lock_state = (int)(lock_s[4] - '0');
	info.lock_critical_state = (int)(lock_s[5] - '0');
	info.lock_bootloader = (int)(lock_s[6] - '0');

	if (info.lock_state == 1)
		*out_is_unlocked = false;
	else
		*out_is_unlocked = true;
	result = AVB_IO_RESULT_OK;
#endif
out:
	return result;
}

static int avb_init(void)
{

	memset(&avb_ops_, 0, sizeof(AvbOps));
	avb_ops_.read_from_partition = read_from_partition;
	avb_ops_.get_preloaded_partition = NULL;
	avb_ops_.write_to_partition = write_to_partition;
	avb_ops_.validate_vbmeta_public_key = validate_vbmeta_public_key;
	avb_ops_.read_rollback_index = read_rollback_index;
	avb_ops_.write_rollback_index = write_rollback_index;
	avb_ops_.read_is_device_unlocked = read_is_device_unlocked;
	avb_ops_.get_unique_guid_for_partition = get_unique_guid_for_partition;
	avb_ops_.get_size_of_partition = get_size_of_partition;
	avb_ops_.read_persistent_value = NULL;
	avb_ops_.write_persistent_value = NULL;

	//avb_ops_.user_data = NULL;

	return 0;
}

int is_device_unlocked(void)
{
	AvbIOResult ret;
	bool out_is_unlocked;

	ret = read_is_device_unlocked(&avb_ops_, &out_is_unlocked);
	if (ret == AVB_IO_RESULT_OK)
		return out_is_unlocked;
	else
		return 0;
}

int avb_verify(AvbSlotVerifyData** out_data)
{
#ifdef CONFIG_OF_LIBFDT_OVERLAY
	const char *requested_partitions_ab[AVB_NUM_SLOT + 1] = {"boot", "dtbo", NULL, NULL, NULL};
#else
	const char *requested_partitions_ab[AVB_NUM_SLOT + 1] = {"boot", NULL, NULL, NULL, NULL};
#endif
	const char *requested_partitions[AVB_NUM_SLOT + 1] = {"boot", "dt", NULL, NULL, NULL};
	AvbSlotVerifyResult result = AVB_SLOT_VERIFY_RESULT_OK;
	char *s1 = NULL;
	char *ab_suffix = NULL;
	const char *vendor_boot = "vendor_boot";
	char *vendor_boot_status = NULL;
	const char **partition_select = requested_partitions;
	int i = 0;

	s1 = env_get("active_slot");
	if (!s1) {
		run_command("get_valid_slot;", 0);
		s1 = env_get("active_slot");
	}
	if (s1) {
		printf("active_slot is %s\n", s1);
		if (!strcmp(s1, "normal"))
			ab_suffix = "";
		else
			ab_suffix = env_get("active_slot");
	}

	if (!ab_suffix)
		ab_suffix = "";
	printf("ab_suffix is %s\n", ab_suffix);

	if (strcmp(ab_suffix, ""))
		partition_select = requested_partitions_ab;

	AvbSlotVerifyFlags flags = AVB_SLOT_VERIFY_FLAGS_NONE;
	char *upgradestep = NULL;

	avb_init();

	upgradestep = env_get("upgrade_step");
	vendor_boot_status = env_get("vendor_boot_mode");
	if (!strcmp(vendor_boot_status, "true")) {
		for (i = 0; i < AVB_NUM_SLOT; i++) {
			if (!partition_select[i]) {
				partition_select[i] = vendor_boot;
				break;
			}
		}
	}

	if (is_device_unlocked() || (upgradestep && (!strcmp(upgradestep, "3"))))
		flags |= AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR;

	result = avb_slot_verify(&avb_ops_, partition_select, ab_suffix,
			flags,
			AVB_HASHTREE_ERROR_MODE_RESTART_AND_INVALIDATE, out_data);

	if (upgradestep && (!strcmp(upgradestep, "3"))) {
		run_command("setenv bootconfig ${bootconfig} androidboot.vbmeta.avb_version=1.1;",
			0);
		result = AVB_SLOT_VERIFY_RESULT_OK;
	}

	return result;
}

static int do_avb_verify(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	AvbSlotVerifyResult result = AVB_SLOT_VERIFY_RESULT_OK;
	AvbSlotVerifyData *out_data = NULL;
	uint32_t i = 0;

	result = (AvbSlotVerifyResult)avb_verify(&out_data);

	printf("result: %d\n", result);
	if (result == AVB_SLOT_VERIFY_RESULT_OK && out_data) {
#if defined(CONFIG_AML_ANTIROLLBACK) || defined(CONFIG_AML_AVB2_ANTIROLLBACK)
		uint32_t version;
		uint32_t lock_state;
#endif

		printf("ab_suffix: %s\n", out_data->ab_suffix);
		printf("vbmeta: name: %s, size: %zd, result: %d\n",
				out_data->vbmeta_images->partition_name,
				out_data->vbmeta_images->vbmeta_size,
				out_data->vbmeta_images->verify_result);
		printf("num of vbmeta: %zd\n", out_data->num_vbmeta_images);
		printf("loaded name: %s, size: %zd, preload: %d\n",
				out_data->loaded_partitions->partition_name,
				out_data->loaded_partitions->data_size,
				out_data->loaded_partitions->preloaded);
		printf("num of loaded: %zd\n", out_data->num_loaded_partitions);
		printf("cmdline: %s\n", out_data->cmdline);
		for (i = 0; i < AVB_MAX_NUMBER_OF_ROLLBACK_INDEX_LOCATIONS; i++)
			printf("rollback(%d) = %llu\n", i, out_data->rollback_indexes[i]);

#if defined(CONFIG_AML_ANTIROLLBACK) || defined(CONFIG_AML_AVB2_ANTIROLLBACK)
		for (i = 0; i < AVB_MAX_NUMBER_OF_ROLLBACK_INDEX_LOCATIONS; i++)
			if (get_avb_antirollback(i, &version))
				printf("rpmb rollback(%d) = %u\n", i, version);
		if (get_avb_lock_state(&lock_state))
			printf("rpmb lock state: %u\n", lock_state);
#endif

		avb_slot_verify_data_free(out_data);
	}

	return result;
}

uint32_t avb_get_boot_patchlevel_from_vbmeta(AvbSlotVerifyData *data)
{
		int i, j;
		AvbVBMetaData *p;
		const char *ret = NULL;
		size_t len = 0;
		char buff[9];
		unsigned long boot_patchlevel;

		if (!data)
			return 0;

		for (i = 0; i < data->num_vbmeta_images; i++) {
			p = &data->vbmeta_images[i];
			if (strcmp(p->partition_name, "vbmeta") == 0) { /* match */
				if (p->verify_result != AVB_VBMETA_VERIFY_RESULT_OK) {
					// not verified
					printf("vbmeta verify_result %d\n", p->verify_result);

					/*device lock, treat as error*/
					if (!is_device_unlocked()) {
						printf("device lock, but vbmeta verify fail\n");
						return 0;
					}
				}

				ret = avb_property_lookup(p->vbmeta_data,
					p->vbmeta_size,
					"com.android.build.boot.security_patch",
					0,
					&len);

				if (ret)
					break;
//				else
//					printf("not found com.android.build.boot.
//					security_patch,len = %d\n", (int)len);
			}
		}

		if (ret) {
			for (i = 0, j = 0; i < len; i++) {
				if (ret[i] != '-' && j < 9)
					buff[j++] = ret[i];
			}
			buff[8] = '\n';
			if (!strict_strtoul(buff, 10, &boot_patchlevel))
				return (uint32_t)boot_patchlevel;
		}

		return 0;
}

static cmd_tbl_t cmd_avb_sub[] = {
	U_BOOT_CMD_MKENT(verify, 4, 0, do_avb_verify, "", ""),
};

static int do_avb_ops(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	cmd_tbl_t *c;
	int ret = 0;

	/* Strip off leading 'bmp' command argument */
	argc--;
	argv++;

	c = find_cmd_tbl(argv[0], &cmd_avb_sub[0], ARRAY_SIZE(cmd_avb_sub));

	if (c) {
		ret = c->cmd(cmdtp, flag, argc, argv);
	} else {
		cmd_usage(cmdtp);
		ret = 1;
	}

	return ret;
}


U_BOOT_CMD(
		avb, 2, 0, do_avb_ops,
		"avb",
		"\nThis command will trigger related avb operations\n"
		);
