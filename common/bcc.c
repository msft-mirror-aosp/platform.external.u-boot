// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Google LLC
 */

#include <bcc.h>
#include <malloc.h>
#include <u-boot/sha256.h>
#include <vm_instance.h>

#include <dice/android/bcc.h>
#include <dice/cbor_reader.h>
#include <dice/cbor_writer.h>
#include <dice/ops.h>

#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/uclass.h>
#include <dm/read.h>
#include <linux/ioport.h>

#include <openssl/evp.h>
#include <openssl/hkdf.h>

#define BCC_CONFIG_DESC_SIZE	64

struct boot_measurement {
	const uint8_t *public_key;
	size_t public_key_size;
	uint8_t digest[AVB_SHA256_DIGEST_SIZE];
};

static uint8_t *bcc_handover_buffer;
static size_t bcc_handover_buffer_size;

static const DiceMode bcc_to_dice_mode[] = {
	[BCC_MODE_NORMAL] = kDiceModeNormal,
	[BCC_MODE_MAINTENANCE] = kDiceModeMaintenance,
	[BCC_MODE_DEBUG] = kDiceModeDebug,
};

struct bcc_context {
	sha256_context auth_hash, code_hash, hidden_hash;
};

static void *find_bcc_handover(size_t *size)
{
	struct udevice *dev;
	struct resource res;

	/* Probe drivers that provide a BCC handover buffer. */
	for (uclass_first_device(UCLASS_DICE, &dev); dev; uclass_next_device(&dev)) {
		if (!dev_read_resource(dev, 0, &res)) {
			*size = resource_size(&res);
			return (void *)res.start;
		}
	}

	return NULL;
}

int bcc_init(void)
{
	/* Idempotent initialization to allow indepedent client modules. */
	if (bcc_handover_buffer && bcc_handover_buffer_size)
		return 0;
	/* If a BCC handover wasn't already set, look for a driver. */
	bcc_handover_buffer = find_bcc_handover(&bcc_handover_buffer_size);
	if (!bcc_handover_buffer || !bcc_handover_buffer_size)
		return -ENOENT;
	return 0;
}

void bcc_set_handover(void *handover, size_t handover_size)
{
	bcc_handover_buffer = handover;
	bcc_handover_buffer_size = handover_size;
}

void bcc_clear_memory(void *data, size_t size)
{
	DiceClearMemory(NULL, size, data);
}

struct bcc_context *bcc_context_alloc(void)
{
	struct bcc_context *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx) {
		sha256_starts(&ctx->hidden_hash);
		sha256_starts(&ctx->code_hash);
		sha256_starts(&ctx->auth_hash);
	}

	return ctx;
}

static int bcc_update_hash(sha256_context *ctx, const uint8_t *input,
			   size_t size)
{
	sha256_update(ctx, input, size);
	return 0;
}

static int bcc_finish_hash(sha256_context *ctx, void *digest, size_t size)
{
	memset(digest, 0, size);
	sha256_finish(ctx, digest);
	return 0;
}

int bcc_update_hidden_hash(struct bcc_context *ctx, const uint8_t *input,
			   size_t size)
{
	return bcc_update_hash(&ctx->hidden_hash, input, size);
}

int bcc_update_authority_hash(struct bcc_context *ctx, const uint8_t *input,
			      size_t size)
{
	return bcc_update_hash(&ctx->auth_hash, input, size);
}

int bcc_update_code_hash(struct bcc_context *ctx, const uint8_t *input,
			 size_t size)
{
	return bcc_update_hash(&ctx->code_hash, input, size);
}

int bcc_handover(struct bcc_context *ctx, const char *component_name,
		 enum bcc_mode mode)
{
	uint8_t *new_handover;
	uint8_t cfg_desc[BCC_CONFIG_DESC_SIZE];
	size_t cfg_desc_size;
	BccConfigValues cfg_vals;
	DiceInputValues input_vals;
	DiceResult res;
	int ret;

	/* Make sure initialization is complete. */
	ret = bcc_init();
	if (ret)
		return ret;

	cfg_vals = (BccConfigValues){
		.inputs = BCC_INPUT_COMPONENT_NAME,
		.component_name = component_name,
	};

	res = BccFormatConfigDescriptor(&cfg_vals, BCC_CONFIG_DESC_SIZE,
					cfg_desc, &cfg_desc_size);
	if (res != kDiceResultOk)
		return -EINVAL;

	input_vals = (DiceInputValues){
		.config_type = kDiceConfigTypeDescriptor,
		.config_descriptor = cfg_desc,
		.config_descriptor_size = cfg_desc_size,
		.mode = bcc_to_dice_mode[mode],
	};

	bcc_finish_hash(&ctx->auth_hash, input_vals.authority_hash, DICE_HASH_SIZE);
	bcc_finish_hash(&ctx->code_hash, input_vals.code_hash, DICE_HASH_SIZE);
	bcc_finish_hash(&ctx->hidden_hash, input_vals.hidden, DICE_HIDDEN_SIZE);

	new_handover = calloc(1, bcc_handover_buffer_size);
	if (!new_handover)
		return -ENOMEM;

	res = BccHandoverMainFlow(/*context=*/NULL, bcc_handover_buffer,
				  bcc_handover_buffer_size, &input_vals,
				  bcc_handover_buffer_size, new_handover, NULL);
	if (res != kDiceResultOk) {
		ret = -EINVAL;
		goto out;
	}

	/* Update the handover buffer with the new data. */
	memcpy(bcc_handover_buffer, new_handover, bcc_handover_buffer_size);

out:
	bcc_clear_memory(new_handover, bcc_handover_buffer_size);
	free(new_handover);
	return ret;
}

/**
 * Format the VM instance data into a CBOR record.
 *
 * The salt is left uninitialized and a pointer to it is returned.
 *
 *  Measurement = [
 *    1: bstr,		// public key
 *    2: bstr,		// digest
 *  ]
 *
 *  InstanceData = {
 *    1: bstr .size 64,	// salt
 *    2: Measurement,	// code
 *    ? 3: Measurement,	// config
 *  }
 */
static int vm_instance_format(const struct boot_measurement *code,
			      const struct boot_measurement *config,
			      size_t buffer_size, uint8_t *buffer,
			      size_t *formatted_size, uint8_t **salt)
{
	const int64_t salt_label = 1;
	const int64_t code_label = 2;
	const int64_t config_label = 3;
	const int64_t public_key_label = 1;
	const int64_t digest_label = 2;

	struct CborOut out;

	if (!code || !formatted_size)
		return -EINVAL;

	CborOutInit(buffer, buffer_size, &out);
	CborWriteMap(/*num_pairs=*/config ? 3 : 1, &out);

	CborWriteInt(salt_label, &out);
	*salt = CborAllocBstr(VM_INSTANCE_SALT_SIZE, &out);

	CborWriteInt(code_label, &out);
	CborWriteMap(/*num_pairs=*/2, &out);
	CborWriteInt(public_key_label, &out);
	CborWriteBstr(code->public_key_size, code->public_key, &out);
	CborWriteInt(digest_label, &out);
	CborWriteBstr(AVB_SHA256_DIGEST_SIZE, code->digest, &out);

	if (config) {
		CborWriteInt(config_label, &out);
		CborWriteMap(/*num_pairs=*/2, &out);
		CborWriteInt(public_key_label, &out);
		CborWriteBstr(config->public_key_size, config->public_key, &out);
		CborWriteInt(digest_label, &out);
		CborWriteBstr(AVB_SHA256_DIGEST_SIZE, config->digest, &out);
	}

	*formatted_size = CborOutSize(&out);

	return CborOutOverflowed(&out) ? -E2BIG : 0;
}

/**
 * Verify the measurements are the same as previously recorded for the VM
 * instance or create a new VM instance with the measurements.
 *
 * The salt tha is saved with the instance data is added as a hidden input to
 * bcc_ctx if the measurements don't conflict with the previously recorded
 * measurements.
 */
static int vm_instance_verify(const char *iface_str, int devnum,
			      const char *instance_uuid,
			      struct bcc_context *bcc_ctx,
			      const struct boot_measurement *code,
			      const struct boot_measurement *config)
{
	const uint8_t key_info[] = {
			'v', 'm', '-', 'i', 'n', 's', 't', 'a', 'n', 'c', 'e' };
	uint8_t *record;
	uint8_t *record_salt;
	uint8_t *saved_record;
	size_t record_size;
	struct AvbOps *ops;
	char devnum_str[3];
	struct uuid uuid;
	uint8_t sealing_key[32];
	int ret;

	if (uuid_str_to_bin(instance_uuid, (unsigned char *)&uuid,
			    UUID_STR_FORMAT_STD))
		return -EINVAL;

	/* Measure the formatted record to allocate large enough buffers. */
	ret = vm_instance_format(code, config, /*buffer_size=*/0,
				 /*buffer=*/NULL, &record_size, &record_salt);
	if (ret && ret != -E2BIG)
		return ret;
	record = malloc(record_size);
	if (!record)
		return -ENOMEM;

	saved_record = malloc(record_size);
	if (!saved_record) {
		free(record);
		return -ENOMEM;
	}

	/* Format the record into the buffer. */
	ret = vm_instance_format(code, config, record_size, record,
				 &record_size, &record_salt);
	if (ret)
		return ret;

	snprintf(devnum_str, sizeof(devnum_str), "%d", devnum);
	ops = avb_ops_alloc(iface_str, devnum_str);
	if (!ops) {
		ret = -EINVAL;
		goto out;
	}

	ret = bcc_get_sealing_key(key_info, sizeof(key_info),
				  sealing_key, sizeof(sealing_key));
	if (ret)
		goto out;

	/* Load any previous data, failing if it's a different size. */
	ret = vm_instance_load_entry(ops, &uuid,
				     sealing_key, sizeof(sealing_key),
				     saved_record, record_size);
	if (ret == 0) {
		ptrdiff_t salt_offset = record_salt - record;

		/*
		 * Copy the assumed saved salt so the records will be
		 * byte-for-byte identical if they match.
		 */
		memcpy(record_salt, saved_record + salt_offset,
		       VM_INSTANCE_SALT_SIZE);
		if (memcmp(record, saved_record, record_size) != 0) {
			log_err("VM instance data mismatch.\n");
			ret = -EINVAL;
			goto out;
		}
	} else if (ret == -ENOENT) {
		/* No previous entry so create a fresh one. */
		printf("Creating new VM instance.\n");

		ret = vm_instance_new_salt(record_salt);
		if (ret)
			goto out;

		ret = vm_instance_save_entry(ops, &uuid,
					     sealing_key, sizeof(sealing_key),
					     record, record_size);
		if (ret) {
			log_err("Failed to create VM instance.\n");
			goto out;
		}
	}

	bcc_update_hidden_hash(bcc_ctx, record_salt, VM_INSTANCE_SALT_SIZE);

out:
	avb_ops_free(ops);
	bcc_clear_memory(sealing_key, sizeof(sealing_key));
	bcc_clear_memory(record, record_size);
	bcc_clear_memory(saved_record, record_size);
	free(saved_record);
	free(record);
	return ret;
}

static int boot_measurement_from_avb_data(const AvbSlotVerifyData *data,
					  struct boot_measurement *measurement)
{
	/*
	 * Use the top-level vbmeta public key as the authority. Any chained
	 * partitions will have their public key captures in the vbmeta digest.
	 */
	if (avb_find_main_pubkey(data, &measurement->public_key,
				 &measurement->public_key_size)
			== CMD_RET_FAILURE)
		return -EINVAL;

	avb_slot_verify_data_calculate_vbmeta_digest(data,
						     AVB_DIGEST_TYPE_SHA256,
						     measurement->digest);
	return 0;
}

int bcc_vm_instance_handover(const char *iface_str, int devnum,
			     const char *instance_uuid,
			     const char *component_name,
			     enum bcc_mode bcc_mode,
			     const AvbSlotVerifyData *code_data,
			     const AvbSlotVerifyData *config_data,
			     const void *unverified_config,
			     size_t unverified_config_size)
{
	struct boot_measurement code;
	struct boot_measurement config;
	struct bcc_context *bcc_ctx;
	int ret;

	/* Continue without the BCC if it wasn't found. */
	ret = bcc_init();
	if (ret == -ENOENT)
		return 0;
	if (ret)
		return ret;

	ret = boot_measurement_from_avb_data(code_data, &code);
	if (ret)
		return ret;

	if (config_data) {
		ret = boot_measurement_from_avb_data(config_data, &config);
		if (ret)
			return ret;
	}

	bcc_ctx = bcc_context_alloc();
	if (!bcc_ctx)
		return -ENOMEM;

	ret = vm_instance_verify(iface_str, devnum, instance_uuid, bcc_ctx,
				 &code, config_data ? &config : NULL);
	if (ret) {
		log_err("Failed to validate instance.\n");
		goto out;
	}
	printf("Booting VM instance.\n");

	/* TODO: format details nicely/usefully for BCC and use the config input */
	bcc_update_authority_hash(bcc_ctx, code.public_key, code.public_key_size);
	bcc_update_code_hash(bcc_ctx, code.digest, AVB_SHA256_DIGEST_SIZE);
	if (config_data) {
		bcc_update_authority_hash(bcc_ctx, config.public_key, config.public_key_size);
		bcc_update_code_hash(bcc_ctx, config.digest, AVB_SHA256_DIGEST_SIZE);
	}
	if (unverified_config)
		bcc_update_code_hash(bcc_ctx, unverified_config, unverified_config_size);

	ret = bcc_handover(bcc_ctx, component_name, bcc_mode);

out:
	free(bcc_ctx);
	return ret;
}

int bcc_get_sealing_key(const uint8_t *info, size_t info_size,
			uint8_t *out_key, size_t out_key_size)
{
	const uint64_t cdi_attest_label = 1;
	const uint64_t cdi_seal_label = 2;
	struct CborIn in;
	int64_t label;
	size_t item_size;
	const uint8_t *ptr;
	int ret;

	/* Make sure initialization is complete. */
	ret = bcc_init();
	if (ret)
		return ret;

	CborInInit(bcc_handover_buffer, bcc_handover_buffer_size, &in);
	if (CborReadMap(&in, &item_size) != CBOR_READ_RESULT_OK ||
	    item_size < 3 ||
	    // Read the attestation CDI.
	    CborReadInt(&in, &label) != CBOR_READ_RESULT_OK ||
	    label != cdi_attest_label ||
	    CborReadBstr(&in, &item_size, &ptr) != CBOR_READ_RESULT_OK ||
	    item_size != DICE_CDI_SIZE ||
	    // Read the sealing CDI.
	    CborReadInt(&in, &label) != CBOR_READ_RESULT_OK ||
	    label != cdi_seal_label ||
	    CborReadBstr(&in, &item_size, &ptr) != CBOR_READ_RESULT_OK ||
	    item_size != DICE_CDI_SIZE)
		return -EINVAL;

	if (!HKDF(out_key, out_key_size, EVP_sha512(),
		  ptr, DICE_CDI_SIZE, /*salt=*/NULL, /*salt_len=*/0,
		  info, info_size)) {
		return -EIO;
	}

	return 0;
}