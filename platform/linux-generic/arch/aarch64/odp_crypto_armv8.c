/* Copyright (c) 2014-2018, Linaro Limited
 * Copyright (c) 2021, ARM Limited
 * Copyright (c) 2022, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <odp_posix_extensions.h>
#include <odp/api/crypto.h>
#include <odp_init_internal.h>
#include <odp/api/spinlock.h>
#include <odp/api/sync.h>
#include <odp/api/debug.h>
#include <odp/api/align.h>
#include <odp/api/shared_memory.h>
#include <odp_debug_internal.h>
#include <odp/api/hints.h>
#include <odp/api/random.h>
#include <odp/api/plat/packet_inlines.h>
#include <odp/api/plat/thread_inlines.h>
#include <odp_packet_internal.h>
#include <odp/api/plat/queue_inlines.h>

#include "AArch64cryptolib.h"

/* Inlined API functions */
#include <odp/api/plat/event_inlines.h>

#define MAX_SESSIONS 4000
/* Length in bytes */
#define ARM_CRYPTO_MAX_CIPHER_KEY_LENGTH      32
#define ARM_CRYPTO_MAX_AUTH_KEY_LENGTH        32
#define ARM_CRYPTO_MAX_IV_LENGTH              16
#define ARM_CRYPTO_MAX_AAD_LENGTH             16
#define ARM_CRYPTO_MAX_DATA_LENGTH	      65536
#define ARM_CRYPTO_MAX_DIGEST_LENGTH          16

/*
 * ARM crypto library may read up to 15 bytes past the end of input
 * data and AAD and write up to 15 bytes past the end of output data.
 */
#define OOB_WRITE_LEN 16 /* rounded up to 16 bytes for efficiency */

/*
 * Data buffer size must be a multiple of 16, because the ARM crypto
 * library will write full 16 byte blocks even if the last data block
 * is not a full block.
 */
ODP_STATIC_ASSERT(ARM_CRYPTO_MAX_DATA_LENGTH % 16 == 0,
		  "Data buffer size not a multiple of 16");

/*
 * IV buffer size must be a multiple of 16, because the ARM crypto
 * library will read in 16 byte blocks even if the last data block
 * is not a full block.
 */
ODP_STATIC_ASSERT(ARM_CRYPTO_MAX_IV_LENGTH % 16 == 0,
		  "IV buffer size not a multiple of 16");

/*
 * Cipher algorithm capabilities
 *
 * Keep sorted: first by key length, then by IV length
 */
static const odp_crypto_cipher_capability_t cipher_capa_null[] = {
{.key_len = 0, .iv_len = 0} };

#ifdef __ARM_FEATURE_AES
static const odp_crypto_cipher_capability_t cipher_capa_aes_gcm[] = {
{.key_len = 16, .iv_len = 12},
{.key_len = 24, .iv_len = 12},
{.key_len = 32, .iv_len = 12} };
#endif

/*
 * Authentication algorithm capabilities
 *
 * Keep sorted: first by digest length, then by key length
 */
static const odp_crypto_auth_capability_t auth_capa_null[] = {
{.digest_len = 0, .key_len = 0, .aad_len = {.min = 0, .max = 0, .inc = 0} } };

#define AES_GCM_TAG_LEN 16

#ifdef __ARM_FEATURE_AES
static const odp_crypto_auth_capability_t auth_capa_aes_gcm[] = {
{.digest_len = AES_GCM_TAG_LEN, .key_len = 0, .aad_len = {.min = 8, .max = 12, .inc = 4} } };
#endif

/** Forward declaration of session structure */
typedef struct odp_crypto_generic_session_t odp_crypto_generic_session_t;

/**
 * Algorithm handler function prototype
 */
typedef
void (*crypto_func_t)(odp_packet_t pkt,
		      const odp_crypto_packet_op_param_t *param,
		      odp_crypto_generic_session_t *session);

/**
 * Per crypto session data structure
 */
struct odp_crypto_generic_session_t {
	odp_crypto_generic_session_t *next;

	/* Session creation parameters */
	odp_crypto_session_param_t p;

	struct {
#if ODP_DEPRECATED_API
		/* Copy of session IV data */
		uint8_t iv_data[ARM_CRYPTO_MAX_IV_LENGTH];
#endif
		uint8_t key_data[ARM_CRYPTO_MAX_CIPHER_KEY_LENGTH];
	} cipher;

	struct {
		uint8_t  key[ARM_CRYPTO_MAX_AUTH_KEY_LENGTH];
#if ODP_DEPRECATED_API
		uint8_t  iv_data[ARM_CRYPTO_MAX_IV_LENGTH];
#endif
	} auth;

	crypto_func_t func;
	unsigned int idx;
	armv8_cipher_constants_t cc;
};

typedef struct odp_crypto_global_s odp_crypto_global_t;

struct odp_crypto_global_s {
	odp_spinlock_t                lock;
	odp_crypto_generic_session_t *free;
	odp_crypto_generic_session_t  sessions[MAX_SESSIONS];
};

static odp_crypto_global_t *global;

typedef struct crypto_local_t {
	uint8_t buffer[ARM_CRYPTO_MAX_DATA_LENGTH];
} crypto_local_t;

static __thread crypto_local_t local;

static
odp_crypto_generic_session_t *alloc_session(void)
{
	odp_crypto_generic_session_t *session = NULL;

	odp_spinlock_lock(&global->lock);
	session = global->free;
	if (session) {
		global->free = session->next;
		session->next = NULL;
	}
	odp_spinlock_unlock(&global->lock);

	if (!session)
		return NULL;

	session->idx = session - global->sessions;

	return session;
}

static
void free_session(odp_crypto_generic_session_t *session)
{
	odp_spinlock_lock(&global->lock);
	session->next = global->free;
	global->free = session;
	odp_spinlock_unlock(&global->lock);
}

static
odp_crypto_packet_result_t *get_op_result_from_packet(odp_packet_t pkt)
{
	odp_packet_hdr_t *hdr = packet_hdr(pkt);

	return &hdr->crypto_op_result;
}

static inline void set_crypto_op_result(odp_packet_t pkt,
					odp_crypto_alg_err_t cipher_err,
					odp_crypto_alg_err_t auth_err)
{
	odp_crypto_packet_result_t *op_result;

	op_result = get_op_result_from_packet(pkt);
	op_result->cipher_status.alg_err = cipher_err;
	op_result->cipher_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	op_result->auth_status.alg_err = auth_err;
	op_result->auth_status.hw_err = ODP_CRYPTO_HW_ERR_NONE;
	op_result->ok = (cipher_err == ODP_CRYPTO_ALG_ERR_NONE &&
			 auth_err   == ODP_CRYPTO_ALG_ERR_NONE);
}

static inline void set_crypto_op_result_ok(odp_packet_t pkt)
{
	set_crypto_op_result(pkt,
			     ODP_CRYPTO_ALG_ERR_NONE,
			     ODP_CRYPTO_ALG_ERR_NONE);
}

static void
null_crypto_routine(odp_packet_t pkt ODP_UNUSED,
		    const odp_crypto_packet_op_param_t *param ODP_UNUSED,
		    odp_crypto_generic_session_t *session ODP_UNUSED)
{
	set_crypto_op_result_ok(pkt);
}

static inline void copy_aad(uint8_t *dst, uint8_t *src, uint32_t len)
{
	ODP_ASSERT(len == 8 || len == 12);

	/* Use constant length memcpy for better optimization result */
	if (len == 8)
		memcpy(dst, src, 8);
	else
		memcpy(dst, src, 12);
}

static
void aes_gcm_encrypt(odp_packet_t pkt,
		     const odp_crypto_packet_op_param_t *param,
		     odp_crypto_generic_session_t *session)
{
	armv8_cipher_state_t cs = {
		.counter = {
			.d = {0, 0}
		}
	};
	uint8_t *iv_ptr;
	uint64_t iv_bit_length = session->p.cipher_iv_len * 8;
	uint64_t plaintext_bit_length = param->cipher_range.length * 8;
	uint64_t aad_bit_length = session->p.auth_aad_len * 8;
	uint32_t in_pos = param->cipher_range.offset;
	uint32_t in_len = param->cipher_range.length;
	odp_bool_t continuous_data;
	uint16_t saved_tail[OOB_WRITE_LEN];
	uint8_t tag[AES_GCM_TAG_LEN];
	int rc;

	/* Fail early if cipher_range is too large */
	if (odp_unlikely(in_len > ARM_CRYPTO_MAX_DATA_LENGTH)) {
		ODP_DBG("ARM Crypto: Packet size too large for requested operation\n");
		goto err;
	}

#if ODP_DEPRECATED_API
	if (param->cipher_iv_ptr)
		iv_ptr = param->cipher_iv_ptr;
	else if (session->p.cipher_iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		goto err;
#else
	iv_ptr = param->cipher_iv_ptr;
	ODP_ASSERT(session->p.cipher_iv_len == 0 || iv_ptr != NULL);
#endif

	cs.constants = &session->cc;

	rc = armv8_aes_gcm_set_counter(iv_ptr, iv_bit_length, &cs);
	if (odp_unlikely(rc)) {
		ODP_DBG("ARM Crypto: Failure while setting nonce\n");
		goto err;
	}

	/* Copy AAD in a stack to make sure that the ARM crypto library can
	 * read it in 16 byte chunks. */
	uint8_t aad[ARM_CRYPTO_MAX_AAD_LENGTH];

	copy_aad(aad, param->aad_ptr, session->p.auth_aad_len);

	uint32_t seg_len = 0;
	uint8_t *data = odp_packet_offset(pkt, in_pos, &seg_len, NULL);

	if (odp_unlikely(odp_packet_is_segmented(pkt)) ||
	    odp_unlikely(odp_packet_tailroom(pkt) < OOB_WRITE_LEN)) {
		/* Packet is segmented or it may not be safe to read and write
		 * beyond the end of packet data. Copy the cipher range to a
		 * contiguous buffer. */
		odp_packet_copy_to_mem(pkt, in_pos, in_len, local.buffer);

		data = local.buffer;
		continuous_data = false;
	} else {
		/* Save data that might get overwritten */
		memcpy(saved_tail, data + in_len, OOB_WRITE_LEN);
		continuous_data = true;
	}

	rc = armv8_enc_aes_gcm_from_state(&cs,
					  aad, aad_bit_length,
					  data, plaintext_bit_length,
					  data,
					  tag);
	if (odp_unlikely(rc)) {
		ODP_DBG("ARM Crypto: AES GCM Encoding failed\n");
		goto err;
	}

	if (odp_likely(continuous_data)) {
		memcpy(data + in_len, saved_tail, OOB_WRITE_LEN);
		memcpy(data - in_pos + param->hash_result_offset,
		       tag, AES_GCM_TAG_LEN);
	} else {
		odp_packet_copy_from_mem(pkt, in_pos, in_len, data);
		odp_packet_copy_from_mem(pkt, param->hash_result_offset,
					 AES_GCM_TAG_LEN, tag);
	}

	set_crypto_op_result_ok(pkt);
	return;

err:
	set_crypto_op_result(pkt,
			     ODP_CRYPTO_ALG_ERR_DATA_SIZE,
			     ODP_CRYPTO_ALG_ERR_NONE);
}

static
void aes_gcm_decrypt(odp_packet_t pkt,
		     const odp_crypto_packet_op_param_t *param,
		     odp_crypto_generic_session_t *session)
{
	armv8_cipher_state_t cs = {
		.counter = {
			.d = {0, 0}
		}
	};
	uint8_t *iv_ptr;
	uint8_t tag[AES_GCM_TAG_LEN];
	uint64_t iv_bit_length = session->p.cipher_iv_len * 8;
	uint64_t plaintext_bit_length = param->cipher_range.length * 8;
	uint64_t aad_bit_length = session->p.auth_aad_len * 8;
	uint32_t in_pos = param->cipher_range.offset;
	uint32_t in_len = param->cipher_range.length;
	odp_bool_t continuous_data;
	uint16_t saved_tail[OOB_WRITE_LEN];
	int rc;

	/* Fail early if cipher_range is too large */
	if (odp_unlikely(in_len > ARM_CRYPTO_MAX_DATA_LENGTH)) {
		ODP_DBG("ARM Crypto: Packet size too large for requested operation\n");
		goto err;
	}

#if ODP_DEPRECATED_API
	if (param->cipher_iv_ptr)
		iv_ptr = param->cipher_iv_ptr;
	else if (session->p.cipher_iv.data)
		iv_ptr = session->cipher.iv_data;
	else
		goto err;
#else
	iv_ptr = param->cipher_iv_ptr;
	ODP_ASSERT(session->p.cipher_iv_len == 0 || iv_ptr != NULL);
#endif

	cs.constants = &session->cc;

	rc = armv8_aes_gcm_set_counter(iv_ptr, iv_bit_length, &cs);
	if (odp_unlikely(rc)) {
		ODP_DBG("ARM Crypto: Failure while setting nonce\n");
		goto err;
	}

	/* Copy AAD in a stack to make sure that the ARM crypto library can
	 * read it in 16 byte chunks. */
	uint8_t aad[ARM_CRYPTO_MAX_AAD_LENGTH];

	copy_aad(aad, param->aad_ptr, session->p.auth_aad_len);

	uint32_t seg_len = 0;
	uint8_t *data = odp_packet_offset(pkt, in_pos, &seg_len, NULL);

	if (odp_unlikely(odp_packet_is_segmented(pkt)) ||
	    odp_unlikely(odp_packet_tailroom(pkt) < OOB_WRITE_LEN)) {
		/* Packet is segmented or it may not be safe to read and write
		 * beyond the end of packet data. Copy the cipher range to a
		 * contiguous buffer. */
		odp_packet_copy_to_mem(pkt, in_pos, in_len, local.buffer);
		data = local.buffer;
		/* Copy tag from the packet to a buffer */
		odp_packet_copy_to_mem(pkt, param->hash_result_offset,
				       AES_GCM_TAG_LEN, tag);
		continuous_data = false;
	} else {
		/* Save data that might get overwritten */
		memcpy(saved_tail, data + in_len, OOB_WRITE_LEN);
		/* Copy tag from the packet to a buffer */
		memcpy(tag, data - in_pos + param->hash_result_offset, AES_GCM_TAG_LEN);
		continuous_data = true;
	}

	rc = armv8_dec_aes_gcm_from_state(&cs,
					  aad, aad_bit_length,
					  data, plaintext_bit_length,
					  tag,
					  data);
	if (odp_unlikely(rc)) {
		ODP_DBG("ARM Crypto: AES GCM Decoding failed\n");
		goto err;
	}

	if (odp_likely(continuous_data))
		memcpy(data + in_len, saved_tail, OOB_WRITE_LEN);
	else
		odp_packet_copy_from_mem(pkt, in_pos, in_len, data);

	set_crypto_op_result_ok(pkt);
	return;

err:
	set_crypto_op_result(pkt,
			     ODP_CRYPTO_ALG_ERR_NONE,
			     ODP_CRYPTO_ALG_ERR_ICV_CHECK);
}

static int process_aes_gcm_param(odp_crypto_generic_session_t *session)
{
	/* Verify Key len is valid */
	if (16 != session->p.cipher_key.length &&
	    24 != session->p.cipher_key.length &&
	    32 != session->p.cipher_key.length)
		return -1;

	/* Verify IV len is correct */
	if (12 != session->p.cipher_iv_len)
		return -1;

	if (ARM_CRYPTO_MAX_CIPHER_KEY_LENGTH < session->p.cipher_key.length)
		return -1;

	memcpy(session->cipher.key_data, session->p.cipher_key.data,
	       session->p.cipher_key.length);

	/* Set function */
	if (ODP_CRYPTO_OP_ENCODE == session->p.op)
		session->func = aes_gcm_encrypt;
	else
		session->func = aes_gcm_decrypt;

	return 0;
}

int odp_crypto_capability(odp_crypto_capability_t *capa)
{
	if (NULL == capa)
		return -1;

	/* Initialize crypto capability structure */
	memset(capa, 0, sizeof(odp_crypto_capability_t));

	capa->sync_mode = ODP_SUPPORT_PREFERRED;
	capa->async_mode = ODP_SUPPORT_YES;
	capa->queue_type_plain = 1;
	capa->queue_type_sched = 1;

	capa->ciphers.bit.null       = 1;
	capa->auths.bit.null         = 1;

#ifdef __ARM_FEATURE_AES
	capa->ciphers.bit.aes_gcm    = 1;
	capa->auths.bit.aes_gcm      = 1;
#endif

	capa->max_sessions = MAX_SESSIONS;

	return 0;
}

int odp_crypto_cipher_capability(odp_cipher_alg_t cipher,
				 odp_crypto_cipher_capability_t dst[],
				 int num_copy)
{
	const odp_crypto_cipher_capability_t *src;
	int num;
	int size = sizeof(odp_crypto_cipher_capability_t);

	switch (cipher) {
	case ODP_CIPHER_ALG_NULL:
		src = cipher_capa_null;
		num = sizeof(cipher_capa_null) / size;
		break;
#ifdef __ARM_FEATURE_AES
	case ODP_CIPHER_ALG_AES_GCM:
		src = cipher_capa_aes_gcm;
		num = sizeof(cipher_capa_aes_gcm) / size;
		break;
#endif
	default:
		return -1;
	}

	if (num < num_copy)
		num_copy = num;

	memcpy(dst, src, num_copy * size);

	return num;
}

int odp_crypto_auth_capability(odp_auth_alg_t auth,
			       odp_crypto_auth_capability_t dst[], int num_copy)
{
	const odp_crypto_auth_capability_t *src;
	int num;
	int size = sizeof(odp_crypto_auth_capability_t);

	switch (auth) {
	case ODP_AUTH_ALG_NULL:
		src = auth_capa_null;
		num = sizeof(auth_capa_null) / size;
		break;
#ifdef __ARM_FEATURE_AES
	case ODP_AUTH_ALG_AES_GCM:
		src = auth_capa_aes_gcm;
		num = sizeof(auth_capa_aes_gcm) / size;
		break;
#endif
	default:
		return -1;
	}

	if (num < num_copy)
		num_copy = num;

	memcpy(dst, src, num_copy * size);

	return num;
}

int
odp_crypto_session_create(const odp_crypto_session_param_t *param,
			  odp_crypto_session_t *session_out,
			  odp_crypto_ses_create_err_t *status)
{
	int rc = 0;
	odp_crypto_generic_session_t *session;

	if (odp_global_ro.disable.crypto) {
		ODP_ERR("Crypto is disabled\n");
		/* Dummy output to avoid compiler warning about uninitialized
		 * variables */
		*status = ODP_CRYPTO_SES_ERR_ENOMEM;
		*session_out = ODP_CRYPTO_SESSION_INVALID;
		return -1;
	}

	/* Allocate memory for this session */
	session = alloc_session();
	if (NULL == session) {
		*status = ODP_CRYPTO_SES_ERR_ENOMEM;
		goto err;
	}

	/* Copy parameters */
	session->p = *param;

	if (session->p.cipher_iv_len > ARM_CRYPTO_MAX_IV_LENGTH) {
		ODP_DBG("Maximum IV length exceeded\n");
		*status = ODP_CRYPTO_SES_ERR_CIPHER;
		goto err;
	}

	if (session->p.auth_iv_len > ARM_CRYPTO_MAX_IV_LENGTH) {
		ODP_DBG("Maximum auth IV length exceeded\n");
		*status = ODP_CRYPTO_SES_ERR_CIPHER;
		goto err;
	}

#if ODP_DEPRECATED_API
	/* Copy IV data */
	if (session->p.cipher_iv.data)
		memcpy(session->cipher.iv_data, session->p.cipher_iv.data,
		       session->p.cipher_iv.length);

	if (session->p.auth_iv.data)
		memcpy(session->auth.iv_data, session->p.auth_iv.data,
		       session->p.auth_iv.length);
#endif

	/* Process based on cipher */
	switch (param->cipher_alg) {
	case ODP_CIPHER_ALG_NULL:
		session->func = null_crypto_routine;
		rc = 0;
		break;
	case ODP_CIPHER_ALG_AES_GCM:
	{
		/* Set cipher mode for AES-GCM */
		armv8_cipher_mode_t mode = 0;

		switch (param->cipher_key.length) {
		case 16:
			mode = AES_GCM_128;
			break;
		case 24:
			mode = AES_GCM_192;
			break;
		case 32:
			mode = AES_GCM_256;
			break;
		default:
			rc = -1;
			break;
		}

		/* AES-GCM requires to do both auth and
		 * cipher at the same time */
		if (param->auth_alg != ODP_AUTH_ALG_AES_GCM) {
			rc = -1;
		} else if (mode == AES_GCM_128 || mode == AES_GCM_192 ||
			   mode == AES_GCM_256) {
			if (armv8_aes_gcm_set_constants(mode,
							session->p.auth_digest_len,
							session->p.cipher_key.data,
							&session->cc) != 0) {
				ODP_DBG("ARM Crypto: Failure in setting constants\n");
				rc = -1;
			}
			rc = process_aes_gcm_param(session);
		} else {
			rc = -1;
		}
		break;
	}
	default:
		rc = -1;
	}

	/* Check result */
	if (rc) {
		*status = ODP_CRYPTO_SES_ERR_CIPHER;
		goto err;
	}

	/* Process based on auth */
	switch (param->auth_alg) {
	case ODP_AUTH_ALG_NULL:
		if (param->cipher_alg == ODP_CIPHER_ALG_NULL)
			rc = 0;
		else
			rc = -1;
		break;
	case ODP_AUTH_ALG_AES_GCM:
		/* AES-GCM requires to do both auth and
		 * cipher at the same time */
		if (param->cipher_alg == ODP_CIPHER_ALG_AES_GCM) {
			rc = 0;
		} else {
			rc = -1;
		}
		break;
	default:
		rc = -1;
	}

	/* Check result */
	if (rc) {
		*status = ODP_CRYPTO_SES_ERR_AUTH;
		goto err;
	}

	/* We're happy */
	*session_out = (intptr_t)session;
	*status = ODP_CRYPTO_SES_ERR_NONE;
	return 0;

err:
	/* error status should be set at this moment */
	if (session != NULL)
		free_session(session);
	*session_out = ODP_CRYPTO_SESSION_INVALID;
	return -1;
}

int odp_crypto_session_destroy(odp_crypto_session_t session)
{
	odp_crypto_generic_session_t *generic;

	generic = (odp_crypto_generic_session_t *)(intptr_t)session;
	memset(generic, 0, sizeof(*generic));
	free_session(generic);
	return 0;
}

#if ODP_DEPRECATED_API
int
odp_crypto_operation(odp_crypto_op_param_t *param,
		     odp_bool_t *posted,
		     odp_crypto_op_result_t *result)
{
	odp_crypto_packet_op_param_t packet_param;
	odp_packet_t out_pkt = param->out_pkt;
	odp_crypto_packet_result_t packet_result;
	odp_crypto_op_result_t local_result;
	int rc;

	packet_param.session = param->session;
	packet_param.cipher_iv_ptr = param->cipher_iv_ptr;
	packet_param.auth_iv_ptr = param->auth_iv_ptr;
	packet_param.hash_result_offset = param->hash_result_offset;
	packet_param.aad_ptr = param->aad_ptr;
	packet_param.cipher_range = param->cipher_range;
	packet_param.auth_range = param->auth_range;

	rc = odp_crypto_op(&param->pkt, &out_pkt, &packet_param, 1);
	if (rc <= 0)
		return -1;

	rc = odp_crypto_result(&packet_result, out_pkt);
	if (rc < 0) {
		/*
		 * We cannot fail since odp_crypto_op() has already processed
		 * the packet. Let's indicate error in the result instead.
		 */
		packet_result.ok = false;
	}

	/* Indicate to caller operation was sync */
	*posted = 0;

	packet_subtype_set(out_pkt, ODP_EVENT_PACKET_BASIC);

	/* Fill in result */
	local_result.ctx = param->ctx;
	local_result.pkt = out_pkt;
	local_result.cipher_status = packet_result.cipher_status;
	local_result.auth_status = packet_result.auth_status;
	local_result.ok = packet_result.ok;

	/*
	 * Be bug-to-bug compatible. Return output packet also through params.
	 */
	param->out_pkt = out_pkt;

	*result = local_result;

	return 0;
}
#endif

int _odp_crypto_init_global(void)
{
	size_t mem_size;
	odp_shm_t shm;
	int idx;

	if (odp_global_ro.disable.crypto) {
		ODP_PRINT("\nODP crypto is DISABLED\n");
		return 0;
	}

	/* Calculate the memory size we need */
	mem_size  = sizeof(odp_crypto_global_t);

	/* Allocate our globally shared memory */
	shm = odp_shm_reserve("_odp_crypto_pool_armv8crypto", mem_size,
			      ODP_CACHE_LINE_SIZE,
			      0);
	if (ODP_SHM_INVALID == shm) {
		ODP_ERR("unable to allocate crypto pool\n");
		return -1;
	}

	global = odp_shm_addr(shm);

	/* Clear it out */
	memset(global, 0, mem_size);

	/* Initialize free list and lock */
	for (idx = 0; idx < MAX_SESSIONS; idx++) {
		global->sessions[idx].next = global->free;
		global->free = &global->sessions[idx];
	}
	odp_spinlock_init(&global->lock);

	return 0;
}

int _odp_crypto_term_global(void)
{
	int rc = 0;
	int ret;
	int count = 0;
	odp_crypto_generic_session_t *session;

	if (odp_global_ro.disable.crypto)
		return 0;

	for (session = global->free; session != NULL; session = session->next)
		count++;
	if (count != MAX_SESSIONS) {
		ODP_ERR("crypto sessions still active\n");
		rc = -1;
	}

	ret = odp_shm_free(odp_shm_lookup("_odp_crypto_pool_armv8crypto"));
	if (ret < 0) {
		ODP_ERR("shm free failed for _odp_crypto_pool_armv8crypto\n");
		rc = -1;
	}

	return rc;
}

int _odp_crypto_init_local(void)
{
	if (odp_global_ro.disable.crypto)
		return 0;

	memset(&local, 0, sizeof(local));

	return 0;
}

int _odp_crypto_term_local(void)
{
	return 0;
}

#if ODP_DEPRECATED_API
odp_crypto_compl_t odp_crypto_compl_from_event(odp_event_t ev)
{
	/* This check not mandated by the API specification */
	if (odp_event_type(ev) != ODP_EVENT_CRYPTO_COMPL)
		ODP_ABORT("Event not a crypto completion");
	return (odp_crypto_compl_t)ev;
}

odp_event_t odp_crypto_compl_to_event(odp_crypto_compl_t completion_event)
{
	return (odp_event_t)completion_event;
}

void
odp_crypto_compl_result(odp_crypto_compl_t completion_event,
			odp_crypto_op_result_t *result)
{
	(void)completion_event;
	(void)result;

	/* We won't get such events anyway, so there can be no result */
	ODP_ASSERT(0);
}

void
odp_crypto_compl_free(odp_crypto_compl_t completion_event)
{
	odp_event_t ev = odp_crypto_compl_to_event(completion_event);

	odp_buffer_free(odp_buffer_from_event(ev));
}

uint64_t odp_crypto_compl_to_u64(odp_crypto_compl_t hdl)
{
	return _odp_pri(hdl);
}
#endif /* ODP_DEPRECATED_API */

void odp_crypto_session_param_init(odp_crypto_session_param_t *param)
{
	memset(param, 0, sizeof(odp_crypto_session_param_t));
}

uint64_t odp_crypto_session_to_u64(odp_crypto_session_t hdl)
{
	return (uint64_t)hdl;
}

odp_packet_t odp_crypto_packet_from_event(odp_event_t ev)
{
	/* This check not mandated by the API specification */
	ODP_ASSERT(odp_event_type(ev) == ODP_EVENT_PACKET);
	ODP_ASSERT(odp_event_subtype(ev) == ODP_EVENT_PACKET_CRYPTO);

	return odp_packet_from_event(ev);
}

odp_event_t odp_crypto_packet_to_event(odp_packet_t pkt)
{
	return odp_packet_to_event(pkt);
}

int odp_crypto_result(odp_crypto_packet_result_t *result,
		      odp_packet_t packet)
{
	odp_crypto_packet_result_t *op_result;

	ODP_ASSERT(odp_event_subtype(odp_packet_to_event(packet)) ==
		   ODP_EVENT_PACKET_CRYPTO);

	op_result = get_op_result_from_packet(packet);

	memcpy(result, op_result, sizeof(*result));

	return 0;
}

static
int crypto_int(odp_packet_t pkt_in,
	       odp_packet_t *pkt_out,
	       const odp_crypto_packet_op_param_t *param)
{
	odp_crypto_generic_session_t *session;
	odp_bool_t allocated = false;
	odp_packet_t out_pkt = *pkt_out;

	session = (odp_crypto_generic_session_t *)(intptr_t)param->session;

	/* Resolve output buffer */
	if (odp_unlikely(ODP_PACKET_INVALID == out_pkt) &&
	    ODP_POOL_INVALID != session->p.output_pool) {
		out_pkt = odp_packet_alloc(session->p.output_pool,
					   odp_packet_len(pkt_in));
		allocated = true;
	}

	if (odp_unlikely(ODP_PACKET_INVALID == out_pkt)) {
		ODP_DBG("Alloc failed.\n");
		return -1;
	}

	if (odp_unlikely(pkt_in != out_pkt)) {
		int ret;
		int md_copy;

		md_copy = _odp_packet_copy_md_possible(session->p.output_pool,
						       odp_packet_pool(pkt_in));
		if (odp_unlikely(md_copy < 0)) {
			ODP_ERR("Unable to copy packet metadata\n");
			goto err;
		}

		ret = odp_packet_copy_from_pkt(out_pkt,
					       0,
					       pkt_in,
					       0,
					       odp_packet_len(pkt_in));
		if (odp_unlikely(ret < 0))
			goto err;

		_odp_packet_copy_md(packet_hdr(out_pkt), packet_hdr(pkt_in), md_copy);
		odp_packet_free(pkt_in);
		pkt_in = ODP_PACKET_INVALID;
	}

	/* Invoke the crypto function */
	session->func(out_pkt, param, session);

	packet_subtype_set(out_pkt, ODP_EVENT_PACKET_CRYPTO);

	/* Synchronous, simply return results */
	*pkt_out = out_pkt;

	return 0;

err:
	if (allocated) {
		odp_packet_free(out_pkt);
		*pkt_out = ODP_PACKET_INVALID;
	}

	return -1;
}

int odp_crypto_op(const odp_packet_t pkt_in[],
		  odp_packet_t pkt_out[],
		  const odp_crypto_packet_op_param_t param[],
		  int num_pkt)
{
	int i, rc;
	odp_crypto_generic_session_t *session;

	for (i = 0; i < num_pkt; i++) {
		session = (odp_crypto_generic_session_t *)(intptr_t)param[i].session;
		ODP_ASSERT(ODP_CRYPTO_SYNC == session->p.op_mode);

		rc = crypto_int(pkt_in[i], &pkt_out[i], &param[i]);
		if (rc < 0)
			break;
	}

	return i;
}

int odp_crypto_op_enq(const odp_packet_t pkt_in[],
		      const odp_packet_t pkt_out[],
		      const odp_crypto_packet_op_param_t param[],
		      int num_pkt)
{
	odp_packet_t pkt;
	odp_event_t event;
	odp_crypto_generic_session_t *session;
	int i, rc;

	for (i = 0; i < num_pkt; i++) {
		session = (odp_crypto_generic_session_t *)(intptr_t)param[i].session;
		ODP_ASSERT(ODP_CRYPTO_ASYNC == session->p.op_mode);
		ODP_ASSERT(ODP_QUEUE_INVALID != session->p.compl_queue);

		pkt = pkt_out[i];
		rc = crypto_int(pkt_in[i], &pkt, &param[i]);
		if (rc < 0)
			break;

		event = odp_packet_to_event(pkt);
		if (odp_queue_enq(session->p.compl_queue, event)) {
			odp_event_free(event);
			break;
		}
	}

	return i;
}
