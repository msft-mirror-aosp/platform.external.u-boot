// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Google LLC
 */

#ifndef _BCC_H
#define _BCC_H

#include <avb_verify.h>

struct bcc_context;

enum bcc_mode {
	BCC_MODE_NORMAL,
	BCC_MODE_DEBUG,
	BCC_MODE_MAINTENANCE,
};

int bcc_init(void);
void bcc_set_handover(void *handover, size_t handover_size) ;

/**
 * Allocate and initialize a bcc_context
 *
 * Returns NULL on failure.
 */
struct bcc_context *bcc_context_alloc(void);

/**
 * Update the temporary hidden hash with arbitrary data
 *
 * Makes the final hidden hash used during BCC handover depend on the
 * provided data. Note that if this function is called more than once, the
 * resulting hash will also depend on the relative order of those calls.
 * Returns zero if successful, a negative error code otherwise.
 */
int bcc_update_hidden_hash(struct bcc_context *ctx, const uint8_t *buffer,
			   size_t size);

/**
 * Update the temporary code hash with arbitrary data
 *
 * Makes the final code hash used during BCC handover depend on the
 * provided data. Note that if this function is called more than once, the
 * resulting hash will also depend on the relative order of those calls.
 * Returns zero if successful, a negative error code otherwise.
 */
int bcc_update_code_hash(struct bcc_context *ctx, const uint8_t *buffer,
			 size_t size);

/**
 * Update the temporary authority hash with arbitrary data
 *
 * Makes the final authority hash used during BCC handover depend on the
 * provided data. Note that if this function is called more than once, the
 * resulting hash will also depend on the relative order of those calls.
 * Returns zero if successful, a negative error code otherwise.
 */
int bcc_update_authority_hash(struct bcc_context *ctx, const uint8_t *buffer,
			      size_t size);

/**
 * Perform a Boot Certificate Chain handover
 *
 * Takes the input BCC handover and measurement of all inputs loaded at this
 * boot stage, and generates the outgoing BCC handover for the next stage.
 * Returns zero if successful, a negative error code otherwise.
 */
int bcc_handover(struct bcc_context *ctx, const char *component_name,
		 enum bcc_mode mode);

int bcc_vm_instance_handover(const char *iface_str, int devnum,
			     const char *instance_uuid,
			     const char *component_name,
			     enum bcc_mode boot_mode,
			     const AvbSlotVerifyData *code_data,
			     const AvbSlotVerifyData *config_data,
			     const void *unverified_config,
			     size_t unverified_config_size);

/**
 * Get a sealing key derived from the sealing CDI.
 *
 * The sealing CDI should not be used directly as a key but should have keys
 * derived from it. This functions deterministically derives keys from the
 * sealing CDI based on the info parameter. Returns zero if successful, a
 * negative error code otherwise.
 */
int bcc_get_sealing_key(const uint8_t *info, size_t info_size,
			uint8_t *out_key, size_t out_key_size);

/**
 * Zero given memory buffer and flush dcache
 */
void bcc_clear_memory(void *data, size_t size);

#endif /* _BCC_H */