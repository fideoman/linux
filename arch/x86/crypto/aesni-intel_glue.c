// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Support for Intel AES-NI instructions. This file contains glue
 * code, the real AES implementation is in intel-aes_asm.S.
 *
 * Copyright (C) 2008, Intel Corp.
 *    Author: Huang Ying <ying.huang@intel.com>
 *
 * Added RFC4106 AES-GCM support for 128-bit keys under the AEAD
 * interface for 64-bit kernels.
 *    Authors: Adrian Hoban <adrian.hoban@intel.com>
 *             Gabriele Paoloni <gabriele.paoloni@intel.com>
 *             Tadeusz Struk (tadeusz.struk@intel.com)
 *             Aidan O'Mahony (aidan.o.mahony@intel.com)
 *    Copyright (c) 2010, Intel Corporation.
 */

#include <linux/hardirq.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/err.h>
#include <crypto/algapi.h>
#include <crypto/aes.h>
#include <crypto/ctr.h>
#include <crypto/b128ops.h>
#include <crypto/gcm.h>
#include <crypto/xts.h>
#include <asm/cpu_device_id.h>
#include <asm/crypto/aes.h>
#include <asm/simd.h>
#include <crypto/scatterwalk.h>
#include <crypto/internal/aead.h>
#include <crypto/internal/simd.h>
#include <crypto/internal/skcipher.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#ifdef CONFIG_X86_64
#include <asm/crypto/glue_helper.h>
#endif


#define AESNI_ALIGN	16
#define AESNI_ALIGN_ATTR __attribute__ ((__aligned__(AESNI_ALIGN)))
#define AES_BLOCK_MASK	(~(AES_BLOCK_SIZE - 1))
#define RFC4106_HASH_SUBKEY_SIZE 16
#define AESNI_ALIGN_EXTRA ((AESNI_ALIGN - 1) & ~(CRYPTO_MINALIGN - 1))
#define CRYPTO_AES_CTX_SIZE (sizeof(struct crypto_aes_ctx) + AESNI_ALIGN_EXTRA)
#define XTS_AES_CTX_SIZE (sizeof(struct aesni_xts_ctx) + AESNI_ALIGN_EXTRA)

/* This data is stored at the end of the crypto_tfm struct.
 * It's a type of per "session" data storage location.
 * This needs to be 16 byte aligned.
 */
struct aesni_rfc4106_gcm_ctx {
	u8 hash_subkey[16] AESNI_ALIGN_ATTR;
	struct crypto_aes_ctx aes_key_expanded AESNI_ALIGN_ATTR;
	u8 nonce[4];
};

struct generic_gcmaes_ctx {
	u8 hash_subkey[16] AESNI_ALIGN_ATTR;
	struct crypto_aes_ctx aes_key_expanded AESNI_ALIGN_ATTR;
};

struct aesni_xts_ctx {
	u8 raw_tweak_ctx[sizeof(struct crypto_aes_ctx)] AESNI_ALIGN_ATTR;
	u8 raw_crypt_ctx[sizeof(struct crypto_aes_ctx)] AESNI_ALIGN_ATTR;
};

#define GCM_BLOCK_LEN 16

struct gcm_context_data {
	/* init, update and finalize context data */
	u8 aad_hash[GCM_BLOCK_LEN];
	u64 aad_length;
	u64 in_length;
	u8 partial_block_enc_key[GCM_BLOCK_LEN];
	u8 orig_IV[GCM_BLOCK_LEN];
	u8 current_counter[GCM_BLOCK_LEN];
	u64 partial_block_len;
	u64 unused;
	u8 hash_keys[GCM_BLOCK_LEN * 16];
};

asmlinkage int aesni_set_key(struct crypto_aes_ctx *ctx, const u8 *in_key,
			     unsigned int key_len);
asmlinkage void aesni_enc(struct crypto_aes_ctx *ctx, u8 *out,
			  const u8 *in);
asmlinkage void aesni_dec(struct crypto_aes_ctx *ctx, u8 *out,
			  const u8 *in);
asmlinkage void aesni_ecb_enc(struct crypto_aes_ctx *ctx, u8 *out,
			      const u8 *in, unsigned int len);
asmlinkage void aesni_ecb_dec(struct crypto_aes_ctx *ctx, u8 *out,
			      const u8 *in, unsigned int len);
asmlinkage void aesni_cbc_enc(struct crypto_aes_ctx *ctx, u8 *out,
			      const u8 *in, unsigned int len, u8 *iv);
asmlinkage void aesni_cbc_dec(struct crypto_aes_ctx *ctx, u8 *out,
			      const u8 *in, unsigned int len, u8 *iv);

#define AVX_GEN2_OPTSIZE 640
#define AVX_GEN4_OPTSIZE 4096

#ifdef CONFIG_X86_64

static void (*aesni_ctr_enc_tfm)(struct crypto_aes_ctx *ctx, u8 *out,
			      const u8 *in, unsigned int len, u8 *iv);
asmlinkage void aesni_ctr_enc(struct crypto_aes_ctx *ctx, u8 *out,
			      const u8 *in, unsigned int len, u8 *iv);

asmlinkage void aesni_xts_crypt8(struct crypto_aes_ctx *ctx, u8 *out,
				 const u8 *in, bool enc, u8 *iv);

/* asmlinkage void aesni_gcm_enc()
 * void *ctx,  AES Key schedule. Starts on a 16 byte boundary.
 * struct gcm_context_data.  May be uninitialized.
 * u8 *out, Ciphertext output. Encrypt in-place is allowed.
 * const u8 *in, Plaintext input
 * unsigned long plaintext_len, Length of data in bytes for encryption.
 * u8 *iv, Pre-counter block j0: 12 byte IV concatenated with 0x00000001.
 *         16-byte aligned pointer.
 * u8 *hash_subkey, the Hash sub key input. Data starts on a 16-byte boundary.
 * const u8 *aad, Additional Authentication Data (AAD)
 * unsigned long aad_len, Length of AAD in bytes.
 * u8 *auth_tag, Authenticated Tag output.
 * unsigned long auth_tag_len), Authenticated Tag Length in bytes.
 *          Valid values are 16 (most likely), 12 or 8.
 */
asmlinkage void aesni_gcm_enc(void *ctx,
			struct gcm_context_data *gdata, u8 *out,
			const u8 *in, unsigned long plaintext_len, u8 *iv,
			u8 *hash_subkey, const u8 *aad, unsigned long aad_len,
			u8 *auth_tag, unsigned long auth_tag_len);

/* asmlinkage void aesni_gcm_dec()
 * void *ctx, AES Key schedule. Starts on a 16 byte boundary.
 * struct gcm_context_data.  May be uninitialized.
 * u8 *out, Plaintext output. Decrypt in-place is allowed.
 * const u8 *in, Ciphertext input
 * unsigned long ciphertext_len, Length of data in bytes for decryption.
 * u8 *iv, Pre-counter block j0: 12 byte IV concatenated with 0x00000001.
 *         16-byte aligned pointer.
 * u8 *hash_subkey, the Hash sub key input. Data starts on a 16-byte boundary.
 * const u8 *aad, Additional Authentication Data (AAD)
 * unsigned long aad_len, Length of AAD in bytes. With RFC4106 this is going
 * to be 8 or 12 bytes
 * u8 *auth_tag, Authenticated Tag output.
 * unsigned long auth_tag_len) Authenticated Tag Length in bytes.
 * Valid values are 16 (most likely), 12 or 8.
 */
asmlinkage void aesni_gcm_dec(void *ctx,
			struct gcm_context_data *gdata, u8 *out,
			const u8 *in, unsigned long ciphertext_len, u8 *iv,
			u8 *hash_subkey, const u8 *aad, unsigned long aad_len,
			u8 *auth_tag, unsigned long auth_tag_len);

/* Scatter / Gather routines, with args similar to above */
asmlinkage void aesni_gcm_init(void *ctx,
			       struct gcm_context_data *gdata,
			       u8 *iv,
			       u8 *hash_subkey, const u8 *aad,
			       unsigned long aad_len);
asmlinkage void aesni_gcm_enc_update(void *ctx,
				     struct gcm_context_data *gdata, u8 *out,
				     const u8 *in, unsigned long plaintext_len);
asmlinkage void aesni_gcm_dec_update(void *ctx,
				     struct gcm_context_data *gdata, u8 *out,
				     const u8 *in,
				     unsigned long ciphertext_len);
asmlinkage void aesni_gcm_finalize(void *ctx,
				   struct gcm_context_data *gdata,
				   u8 *auth_tag, unsigned long auth_tag_len);

static const struct aesni_gcm_tfm_s {
	void (*init)(void *ctx, struct gcm_context_data *gdata, u8 *iv,
		     u8 *hash_subkey, const u8 *aad, unsigned long aad_len);
	void (*enc_update)(void *ctx, struct gcm_context_data *gdata, u8 *out,
			   const u8 *in, unsigned long plaintext_len);
	void (*dec_update)(void *ctx, struct gcm_context_data *gdata, u8 *out,
			   const u8 *in, unsigned long ciphertext_len);
	void (*finalize)(void *ctx, struct gcm_context_data *gdata,
			 u8 *auth_tag, unsigned long auth_tag_len);
} *aesni_gcm_tfm;

static const struct aesni_gcm_tfm_s aesni_gcm_tfm_sse = {
	.init = &aesni_gcm_init,
	.enc_update = &aesni_gcm_enc_update,
	.dec_update = &aesni_gcm_dec_update,
	.finalize = &aesni_gcm_finalize,
};

#ifdef CONFIG_AS_AVX
asmlinkage void aes_ctr_enc_128_avx_by8(const u8 *in, u8 *iv,
		void *keys, u8 *out, unsigned int num_bytes);
asmlinkage void aes_ctr_enc_192_avx_by8(const u8 *in, u8 *iv,
		void *keys, u8 *out, unsigned int num_bytes);
asmlinkage void aes_ctr_enc_256_avx_by8(const u8 *in, u8 *iv,
		void *keys, u8 *out, unsigned int num_bytes);
/*
 * asmlinkage void aesni_gcm_init_avx_gen2()
 * gcm_data *my_ctx_data, context data
 * u8 *hash_subkey,  the Hash sub key input. Data starts on a 16-byte boundary.
 */
asmlinkage void aesni_gcm_init_avx_gen2(void *my_ctx_data,
					struct gcm_context_data *gdata,
					u8 *iv,
					u8 *hash_subkey,
					const u8 *aad,
					unsigned long aad_len);

asmlinkage void aesni_gcm_enc_update_avx_gen2(void *ctx,
				     struct gcm_context_data *gdata, u8 *out,
				     const u8 *in, unsigned long plaintext_len);
asmlinkage void aesni_gcm_dec_update_avx_gen2(void *ctx,
				     struct gcm_context_data *gdata, u8 *out,
				     const u8 *in,
				     unsigned long ciphertext_len);
asmlinkage void aesni_gcm_finalize_avx_gen2(void *ctx,
				   struct gcm_context_data *gdata,
				   u8 *auth_tag, unsigned long auth_tag_len);

asmlinkage void aesni_gcm_enc_avx_gen2(void *ctx,
				struct gcm_context_data *gdata, u8 *out,
			const u8 *in, unsigned long plaintext_len, u8 *iv,
			const u8 *aad, unsigned long aad_len,
			u8 *auth_tag, unsigned long auth_tag_len);

asmlinkage void aesni_gcm_dec_avx_gen2(void *ctx,
				struct gcm_context_data *gdata, u8 *out,
			const u8 *in, unsigned long ciphertext_len, u8 *iv,
			const u8 *aad, unsigned long aad_len,
			u8 *auth_tag, unsigned long auth_tag_len);

static const struct aesni_gcm_tfm_s aesni_gcm_tfm_avx_gen2 = {
	.init = &aesni_gcm_init_avx_gen2,
	.enc_update = &aesni_gcm_enc_update_avx_gen2,
	.dec_update = &aesni_gcm_dec_update_avx_gen2,
	.finalize = &aesni_gcm_finalize_avx_gen2,
};

#endif

#ifdef CONFIG_AS_AVX2
/*
 * asmlinkage void aesni_gcm_init_avx_gen4()
 * gcm_data *my_ctx_data, context data
 * u8 *hash_subkey,  the Hash sub key input. Data starts on a 16-byte boundary.
 */
asmlinkage void aesni_gcm_init_avx_gen4(void *my_ctx_data,
					struct gcm_context_data *gdata,
					u8 *iv,
					u8 *hash_subkey,
					const u8 *aad,
					unsigned long aad_len);

asmlinkage void aesni_gcm_enc_update_avx_gen4(void *ctx,
				     struct gcm_context_data *gdata, u8 *out,
				     const u8 *in, unsigned long plaintext_len);
asmlinkage void aesni_gcm_dec_update_avx_gen4(void *ctx,
				     struct gcm_context_data *gdata, u8 *out,
				     const u8 *in,
				     unsigned long ciphertext_len);
asmlinkage void aesni_gcm_finalize_avx_gen4(void *ctx,
				   struct gcm_context_data *gdata,
				   u8 *auth_tag, unsigned long auth_tag_len);

asmlinkage void aesni_gcm_enc_avx_gen4(void *ctx,
				struct gcm_context_data *gdata, u8 *out,
			const u8 *in, unsigned long plaintext_len, u8 *iv,
			const u8 *aad, unsigned long aad_len,
			u8 *auth_tag, unsigned long auth_tag_len);

asmlinkage void aesni_gcm_dec_avx_gen4(void *ctx,
				struct gcm_context_data *gdata, u8 *out,
			const u8 *in, unsigned long ciphertext_len, u8 *iv,
			const u8 *aad, unsigned long aad_len,
			u8 *auth_tag, unsigned long auth_tag_len);

static const struct aesni_gcm_tfm_s aesni_gcm_tfm_avx_gen4 = {
	.init = &aesni_gcm_init_avx_gen4,
	.enc_update = &aesni_gcm_enc_update_avx_gen4,
	.dec_update = &aesni_gcm_dec_update_avx_gen4,
	.finalize = &aesni_gcm_finalize_avx_gen4,
};

#endif

static inline struct
aesni_rfc4106_gcm_ctx *aesni_rfc4106_gcm_ctx_get(struct crypto_aead *tfm)
{
	unsigned long align = AESNI_ALIGN;

	if (align <= crypto_tfm_ctx_alignment())
		align = 1;
	return PTR_ALIGN(crypto_aead_ctx(tfm), align);
}

static inline struct
generic_gcmaes_ctx *generic_gcmaes_ctx_get(struct crypto_aead *tfm)
{
	unsigned long align = AESNI_ALIGN;

	if (align <= crypto_tfm_ctx_alignment())
		align = 1;
	return PTR_ALIGN(crypto_aead_ctx(tfm), align);
}
#endif

static inline struct crypto_aes_ctx *aes_ctx(void *raw_ctx)
{
	unsigned long addr = (unsigned long)raw_ctx;
	unsigned long align = AESNI_ALIGN;

	if (align <= crypto_tfm_ctx_alignment())
		align = 1;
	return (struct crypto_aes_ctx *)ALIGN(addr, align);
}

static int aes_set_key_common(struct crypto_tfm *tfm, void *raw_ctx,
			      const u8 *in_key, unsigned int key_len)
{
	struct crypto_aes_ctx *ctx = aes_ctx(raw_ctx);
	u32 *flags = &tfm->crt_flags;
	int err;

	if (key_len != AES_KEYSIZE_128 && key_len != AES_KEYSIZE_192 &&
	    key_len != AES_KEYSIZE_256) {
		*flags |= CRYPTO_TFM_RES_BAD_KEY_LEN;
		return -EINVAL;
	}

	if (!crypto_simd_usable())
		err = crypto_aes_expand_key(ctx, in_key, key_len);
	else {
		kernel_fpu_begin();
		err = aesni_set_key(ctx, in_key, key_len);
		kernel_fpu_end();
	}

	return err;
}

static int aes_set_key(struct crypto_tfm *tfm, const u8 *in_key,
		       unsigned int key_len)
{
	return aes_set_key_common(tfm, crypto_tfm_ctx(tfm), in_key, key_len);
}

static void aes_encrypt(struct crypto_tfm *tfm, u8 *dst, const u8 *src)
{
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_tfm_ctx(tfm));

	if (!crypto_simd_usable())
		crypto_aes_encrypt_x86(ctx, dst, src);
	else {
		kernel_fpu_begin();
		aesni_enc(ctx, dst, src);
		kernel_fpu_end();
	}
}

static void aes_decrypt(struct crypto_tfm *tfm, u8 *dst, const u8 *src)
{
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_tfm_ctx(tfm));

	if (!crypto_simd_usable())
		crypto_aes_decrypt_x86(ctx, dst, src);
	else {
		kernel_fpu_begin();
		aesni_dec(ctx, dst, src);
		kernel_fpu_end();
	}
}

static void __aes_encrypt(struct crypto_tfm *tfm, u8 *dst, const u8 *src)
{
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_tfm_ctx(tfm));

	aesni_enc(ctx, dst, src);
}

static void __aes_decrypt(struct crypto_tfm *tfm, u8 *dst, const u8 *src)
{
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_tfm_ctx(tfm));

	aesni_dec(ctx, dst, src);
}

static int aesni_skcipher_setkey(struct crypto_skcipher *tfm, const u8 *key,
			         unsigned int len)
{
	return aes_set_key_common(crypto_skcipher_tfm(tfm),
				  crypto_skcipher_ctx(tfm), key, len);
}

static int ecb_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_skcipher_ctx(tfm));
	struct skcipher_walk walk;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, true);

	while ((nbytes = walk.nbytes)) {
		kernel_fpu_begin();
		aesni_ecb_enc(ctx, walk.dst.virt.addr, walk.src.virt.addr,
			      nbytes & AES_BLOCK_MASK);
		kernel_fpu_end();
		nbytes &= AES_BLOCK_SIZE - 1;
		err = skcipher_walk_done(&walk, nbytes);
	}

	return err;
}

static int ecb_decrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_skcipher_ctx(tfm));
	struct skcipher_walk walk;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, true);

	while ((nbytes = walk.nbytes)) {
		kernel_fpu_begin();
		aesni_ecb_dec(ctx, walk.dst.virt.addr, walk.src.virt.addr,
			      nbytes & AES_BLOCK_MASK);
		kernel_fpu_end();
		nbytes &= AES_BLOCK_SIZE - 1;
		err = skcipher_walk_done(&walk, nbytes);
	}

	return err;
}

static int cbc_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_skcipher_ctx(tfm));
	struct skcipher_walk walk;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, true);

	while ((nbytes = walk.nbytes)) {
		kernel_fpu_begin();
		aesni_cbc_enc(ctx, walk.dst.virt.addr, walk.src.virt.addr,
			      nbytes & AES_BLOCK_MASK, walk.iv);
		kernel_fpu_end();
		nbytes &= AES_BLOCK_SIZE - 1;
		err = skcipher_walk_done(&walk, nbytes);
	}

	return err;
}

static int cbc_decrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_skcipher_ctx(tfm));
	struct skcipher_walk walk;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, true);

	while ((nbytes = walk.nbytes)) {
		kernel_fpu_begin();
		aesni_cbc_dec(ctx, walk.dst.virt.addr, walk.src.virt.addr,
			      nbytes & AES_BLOCK_MASK, walk.iv);
		kernel_fpu_end();
		nbytes &= AES_BLOCK_SIZE - 1;
		err = skcipher_walk_done(&walk, nbytes);
	}

	return err;
}

#ifdef CONFIG_X86_64
static void ctr_crypt_final(struct crypto_aes_ctx *ctx,
			    struct skcipher_walk *walk)
{
	u8 *ctrblk = walk->iv;
	u8 keystream[AES_BLOCK_SIZE];
	u8 *src = walk->src.virt.addr;
	u8 *dst = walk->dst.virt.addr;
	unsigned int nbytes = walk->nbytes;

	aesni_enc(ctx, keystream, ctrblk);
	crypto_xor_cpy(dst, keystream, src, nbytes);

	crypto_inc(ctrblk, AES_BLOCK_SIZE);
}

#ifdef CONFIG_AS_AVX
static void aesni_ctr_enc_avx_tfm(struct crypto_aes_ctx *ctx, u8 *out,
			      const u8 *in, unsigned int len, u8 *iv)
{
	/*
	 * based on key length, override with the by8 version
	 * of ctr mode encryption/decryption for improved performance
	 * aes_set_key_common() ensures that key length is one of
	 * {128,192,256}
	 */
	if (ctx->key_length == AES_KEYSIZE_128)
		aes_ctr_enc_128_avx_by8(in, iv, (void *)ctx, out, len);
	else if (ctx->key_length == AES_KEYSIZE_192)
		aes_ctr_enc_192_avx_by8(in, iv, (void *)ctx, out, len);
	else
		aes_ctr_enc_256_avx_by8(in, iv, (void *)ctx, out, len);
}
#endif

static int ctr_crypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct crypto_aes_ctx *ctx = aes_ctx(crypto_skcipher_ctx(tfm));
	struct skcipher_walk walk;
	unsigned int nbytes;
	int err;

	err = skcipher_walk_virt(&walk, req, true);

	while ((nbytes = walk.nbytes) >= AES_BLOCK_SIZE) {
		kernel_fpu_begin();
		aesni_ctr_enc_tfm(ctx, walk.dst.virt.addr, walk.src.virt.addr,
			              nbytes & AES_BLOCK_MASK, walk.iv);
		kernel_fpu_end();
		nbytes &= AES_BLOCK_SIZE - 1;
		err = skcipher_walk_done(&walk, nbytes);
	}
	if (walk.nbytes) {
		kernel_fpu_begin();
		ctr_crypt_final(ctx, &walk);
		kernel_fpu_end();
		err = skcipher_walk_done(&walk, 0);
	}

	return err;
}

static int xts_aesni_setkey(struct crypto_skcipher *tfm, const u8 *key,
			    unsigned int keylen)
{
	struct aesni_xts_ctx *ctx = crypto_skcipher_ctx(tfm);
	int err;

	err = xts_verify_key(tfm, key, keylen);
	if (err)
		return err;

	keylen /= 2;

	/* first half of xts-key is for crypt */
	err = aes_set_key_common(crypto_skcipher_tfm(tfm), ctx->raw_crypt_ctx,
				 key, keylen);
	if (err)
		return err;

	/* second half of xts-key is for tweak */
	return aes_set_key_common(crypto_skcipher_tfm(tfm), ctx->raw_tweak_ctx,
				  key + keylen, keylen);
}


static void aesni_xts_tweak(void *ctx, u8 *out, const u8 *in)
{
	aesni_enc(ctx, out, in);
}

static void aesni_xts_enc(void *ctx, u128 *dst, const u128 *src, le128 *iv)
{
	glue_xts_crypt_128bit_one(ctx, dst, src, iv, GLUE_FUNC_CAST(aesni_enc));
}

static void aesni_xts_dec(void *ctx, u128 *dst, const u128 *src, le128 *iv)
{
	glue_xts_crypt_128bit_one(ctx, dst, src, iv, GLUE_FUNC_CAST(aesni_dec));
}

static void aesni_xts_enc8(void *ctx, u128 *dst, const u128 *src, le128 *iv)
{
	aesni_xts_crypt8(ctx, (u8 *)dst, (const u8 *)src, true, (u8 *)iv);
}

static void aesni_xts_dec8(void *ctx, u128 *dst, const u128 *src, le128 *iv)
{
	aesni_xts_crypt8(ctx, (u8 *)dst, (const u8 *)src, false, (u8 *)iv);
}

static const struct common_glue_ctx aesni_enc_xts = {
	.num_funcs = 2,
	.fpu_blocks_limit = 1,

	.funcs = { {
		.num_blocks = 8,
		.fn_u = { .xts = GLUE_XTS_FUNC_CAST(aesni_xts_enc8) }
	}, {
		.num_blocks = 1,
		.fn_u = { .xts = GLUE_XTS_FUNC_CAST(aesni_xts_enc) }
	} }
};

static const struct common_glue_ctx aesni_dec_xts = {
	.num_funcs = 2,
	.fpu_blocks_limit = 1,

	.funcs = { {
		.num_blocks = 8,
		.fn_u = { .xts = GLUE_XTS_FUNC_CAST(aesni_xts_dec8) }
	}, {
		.num_blocks = 1,
		.fn_u = { .xts = GLUE_XTS_FUNC_CAST(aesni_xts_dec) }
	} }
};

static int xts_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct aesni_xts_ctx *ctx = crypto_skcipher_ctx(tfm);

	return glue_xts_req_128bit(&aesni_enc_xts, req,
				   XTS_TWEAK_CAST(aesni_xts_tweak),
				   aes_ctx(ctx->raw_tweak_ctx),
				   aes_ctx(ctx->raw_crypt_ctx));
}

static int xts_decrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct aesni_xts_ctx *ctx = crypto_skcipher_ctx(tfm);

	return glue_xts_req_128bit(&aesni_dec_xts, req,
				   XTS_TWEAK_CAST(aesni_xts_tweak),
				   aes_ctx(ctx->raw_tweak_ctx),
				   aes_ctx(ctx->raw_crypt_ctx));
}

static int
rfc4106_set_hash_subkey(u8 *hash_subkey, const u8 *key, unsigned int key_len)
{
	struct crypto_cipher *tfm;
	int ret;

	tfm = crypto_alloc_cipher("aes", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);

	ret = crypto_cipher_setkey(tfm, key, key_len);
	if (ret)
		goto out_free_cipher;

	/* Clear the data in the hash sub key container to zero.*/
	/* We want to cipher all zeros to create the hash sub key. */
	memset(hash_subkey, 0, RFC4106_HASH_SUBKEY_SIZE);

	crypto_cipher_encrypt_one(tfm, hash_subkey, hash_subkey);

out_free_cipher:
	crypto_free_cipher(tfm);
	return ret;
}

static int common_rfc4106_set_key(struct crypto_aead *aead, const u8 *key,
				  unsigned int key_len)
{
	struct aesni_rfc4106_gcm_ctx *ctx = aesni_rfc4106_gcm_ctx_get(aead);

	if (key_len < 4) {
		crypto_aead_set_flags(aead, CRYPTO_TFM_RES_BAD_KEY_LEN);
		return -EINVAL;
	}
	/*Account for 4 byte nonce at the end.*/
	key_len -= 4;

	memcpy(ctx->nonce, key + key_len, sizeof(ctx->nonce));

	return aes_set_key_common(crypto_aead_tfm(aead),
				  &ctx->aes_key_expanded, key, key_len) ?:
	       rfc4106_set_hash_subkey(ctx->hash_subkey, key, key_len);
}

/* This is the Integrity Check Value (aka the authentication tag) length and can
 * be 8, 12 or 16 bytes long. */
static int common_rfc4106_set_authsize(struct crypto_aead *aead,
				       unsigned int authsize)
{
	switch (authsize) {
	case 8:
	case 12:
	case 16:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int generic_gcmaes_set_authsize(struct crypto_aead *tfm,
				       unsigned int authsize)
{
	switch (authsize) {
	case 4:
	case 8:
	case 12:
	case 13:
	case 14:
	case 15:
	case 16:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int gcmaes_crypt_by_sg(bool enc, struct aead_request *req,
			      unsigned int assoclen, u8 *hash_subkey,
			      u8 *iv, void *aes_ctx)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	unsigned long auth_tag_len = crypto_aead_authsize(tfm);
	const struct aesni_gcm_tfm_s *gcm_tfm = aesni_gcm_tfm;
	struct gcm_context_data data AESNI_ALIGN_ATTR;
	struct scatter_walk dst_sg_walk = {};
	unsigned long left = req->cryptlen;
	unsigned long len, srclen, dstlen;
	struct scatter_walk assoc_sg_walk;
	struct scatter_walk src_sg_walk;
	struct scatterlist src_start[2];
	struct scatterlist dst_start[2];
	struct scatterlist *src_sg;
	struct scatterlist *dst_sg;
	u8 *src, *dst, *assoc;
	u8 *assocmem = NULL;
	u8 authTag[16];

	if (!enc)
		left -= auth_tag_len;

#ifdef CONFIG_AS_AVX2
	if (left < AVX_GEN4_OPTSIZE && gcm_tfm == &aesni_gcm_tfm_avx_gen4)
		gcm_tfm = &aesni_gcm_tfm_avx_gen2;
#endif
#ifdef CONFIG_AS_AVX
	if (left < AVX_GEN2_OPTSIZE && gcm_tfm == &aesni_gcm_tfm_avx_gen2)
		gcm_tfm = &aesni_gcm_tfm_sse;
#endif

	/* Linearize assoc, if not already linear */
	if (req->src->length >= assoclen && req->src->length &&
		(!PageHighMem(sg_page(req->src)) ||
			req->src->offset + req->src->length <= PAGE_SIZE)) {
		scatterwalk_start(&assoc_sg_walk, req->src);
		assoc = scatterwalk_map(&assoc_sg_walk);
	} else {
		/* assoc can be any length, so must be on heap */
		assocmem = kmalloc(assoclen, GFP_ATOMIC);
		if (unlikely(!assocmem))
			return -ENOMEM;
		assoc = assocmem;

		scatterwalk_map_and_copy(assoc, req->src, 0, assoclen, 0);
	}

	if (left) {
		src_sg = scatterwalk_ffwd(src_start, req->src, req->assoclen);
		scatterwalk_start(&src_sg_walk, src_sg);
		if (req->src != req->dst) {
			dst_sg = scatterwalk_ffwd(dst_start, req->dst,
						  req->assoclen);
			scatterwalk_start(&dst_sg_walk, dst_sg);
		}
	}

	kernel_fpu_begin();
	gcm_tfm->init(aes_ctx, &data, iv,
		hash_subkey, assoc, assoclen);
	if (req->src != req->dst) {
		while (left) {
			src = scatterwalk_map(&src_sg_walk);
			dst = scatterwalk_map(&dst_sg_walk);
			srclen = scatterwalk_clamp(&src_sg_walk, left);
			dstlen = scatterwalk_clamp(&dst_sg_walk, left);
			len = min(srclen, dstlen);
			if (len) {
				if (enc)
					gcm_tfm->enc_update(aes_ctx, &data,
							     dst, src, len);
				else
					gcm_tfm->dec_update(aes_ctx, &data,
							     dst, src, len);
			}
			left -= len;

			scatterwalk_unmap(src);
			scatterwalk_unmap(dst);
			scatterwalk_advance(&src_sg_walk, len);
			scatterwalk_advance(&dst_sg_walk, len);
			scatterwalk_done(&src_sg_walk, 0, left);
			scatterwalk_done(&dst_sg_walk, 1, left);
		}
	} else {
		while (left) {
			dst = src = scatterwalk_map(&src_sg_walk);
			len = scatterwalk_clamp(&src_sg_walk, left);
			if (len) {
				if (enc)
					gcm_tfm->enc_update(aes_ctx, &data,
							     src, src, len);
				else
					gcm_tfm->dec_update(aes_ctx, &data,
							     src, src, len);
			}
			left -= len;
			scatterwalk_unmap(src);
			scatterwalk_advance(&src_sg_walk, len);
			scatterwalk_done(&src_sg_walk, 1, left);
		}
	}
	gcm_tfm->finalize(aes_ctx, &data, authTag, auth_tag_len);
	kernel_fpu_end();

	if (!assocmem)
		scatterwalk_unmap(assoc);
	else
		kfree(assocmem);

	if (!enc) {
		u8 authTagMsg[16];

		/* Copy out original authTag */
		scatterwalk_map_and_copy(authTagMsg, req->src,
					 req->assoclen + req->cryptlen -
					 auth_tag_len,
					 auth_tag_len, 0);

		/* Compare generated tag with passed in tag. */
		return crypto_memneq(authTagMsg, authTag, auth_tag_len) ?
			-EBADMSG : 0;
	}

	/* Copy in the authTag */
	scatterwalk_map_and_copy(authTag, req->dst,
				 req->assoclen + req->cryptlen,
				 auth_tag_len, 1);

	return 0;
}

static int gcmaes_encrypt(struct aead_request *req, unsigned int assoclen,
			  u8 *hash_subkey, u8 *iv, void *aes_ctx)
{
	return gcmaes_crypt_by_sg(true, req, assoclen, hash_subkey, iv,
				aes_ctx);
}

static int gcmaes_decrypt(struct aead_request *req, unsigned int assoclen,
			  u8 *hash_subkey, u8 *iv, void *aes_ctx)
{
	return gcmaes_crypt_by_sg(false, req, assoclen, hash_subkey, iv,
				aes_ctx);
}

static int helper_rfc4106_encrypt(struct aead_request *req)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct aesni_rfc4106_gcm_ctx *ctx = aesni_rfc4106_gcm_ctx_get(tfm);
	void *aes_ctx = &(ctx->aes_key_expanded);
	u8 iv[16] __attribute__ ((__aligned__(AESNI_ALIGN)));
	unsigned int i;
	__be32 counter = cpu_to_be32(1);

	/* Assuming we are supporting rfc4106 64-bit extended */
	/* sequence numbers We need to have the AAD length equal */
	/* to 16 or 20 bytes */
	if (unlikely(req->assoclen != 16 && req->assoclen != 20))
		return -EINVAL;

	/* IV below built */
	for (i = 0; i < 4; i++)
		*(iv+i) = ctx->nonce[i];
	for (i = 0; i < 8; i++)
		*(iv+4+i) = req->iv[i];
	*((__be32 *)(iv+12)) = counter;

	return gcmaes_encrypt(req, req->assoclen - 8, ctx->hash_subkey, iv,
			      aes_ctx);
}

static int helper_rfc4106_decrypt(struct aead_request *req)
{
	__be32 counter = cpu_to_be32(1);
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct aesni_rfc4106_gcm_ctx *ctx = aesni_rfc4106_gcm_ctx_get(tfm);
	void *aes_ctx = &(ctx->aes_key_expanded);
	u8 iv[16] __attribute__ ((__aligned__(AESNI_ALIGN)));
	unsigned int i;

	if (unlikely(req->assoclen != 16 && req->assoclen != 20))
		return -EINVAL;

	/* Assuming we are supporting rfc4106 64-bit extended */
	/* sequence numbers We need to have the AAD length */
	/* equal to 16 or 20 bytes */

	/* IV below built */
	for (i = 0; i < 4; i++)
		*(iv+i) = ctx->nonce[i];
	for (i = 0; i < 8; i++)
		*(iv+4+i) = req->iv[i];
	*((__be32 *)(iv+12)) = counter;

	return gcmaes_decrypt(req, req->assoclen - 8, ctx->hash_subkey, iv,
			      aes_ctx);
}
#endif

static struct crypto_alg aesni_algs[] = { {
	.cra_name		= "aes",
	.cra_driver_name	= "aes-aesni",
	.cra_priority		= 300,
	.cra_flags		= CRYPTO_ALG_TYPE_CIPHER,
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= CRYPTO_AES_CTX_SIZE,
	.cra_module		= THIS_MODULE,
	.cra_u	= {
		.cipher	= {
			.cia_min_keysize	= AES_MIN_KEY_SIZE,
			.cia_max_keysize	= AES_MAX_KEY_SIZE,
			.cia_setkey		= aes_set_key,
			.cia_encrypt		= aes_encrypt,
			.cia_decrypt		= aes_decrypt
		}
	}
}, {
	.cra_name		= "__aes",
	.cra_driver_name	= "__aes-aesni",
	.cra_priority		= 300,
	.cra_flags		= CRYPTO_ALG_TYPE_CIPHER | CRYPTO_ALG_INTERNAL,
	.cra_blocksize		= AES_BLOCK_SIZE,
	.cra_ctxsize		= CRYPTO_AES_CTX_SIZE,
	.cra_module		= THIS_MODULE,
	.cra_u	= {
		.cipher	= {
			.cia_min_keysize	= AES_MIN_KEY_SIZE,
			.cia_max_keysize	= AES_MAX_KEY_SIZE,
			.cia_setkey		= aes_set_key,
			.cia_encrypt		= __aes_encrypt,
			.cia_decrypt		= __aes_decrypt
		}
	}
} };

static struct skcipher_alg aesni_skciphers[] = {
	{
		.base = {
			.cra_name		= "__ecb(aes)",
			.cra_driver_name	= "__ecb-aes-aesni",
			.cra_priority		= 400,
			.cra_flags		= CRYPTO_ALG_INTERNAL,
			.cra_blocksize		= AES_BLOCK_SIZE,
			.cra_ctxsize		= CRYPTO_AES_CTX_SIZE,
			.cra_module		= THIS_MODULE,
		},
		.min_keysize	= AES_MIN_KEY_SIZE,
		.max_keysize	= AES_MAX_KEY_SIZE,
		.setkey		= aesni_skcipher_setkey,
		.encrypt	= ecb_encrypt,
		.decrypt	= ecb_decrypt,
	}, {
		.base = {
			.cra_name		= "__cbc(aes)",
			.cra_driver_name	= "__cbc-aes-aesni",
			.cra_priority		= 400,
			.cra_flags		= CRYPTO_ALG_INTERNAL,
			.cra_blocksize		= AES_BLOCK_SIZE,
			.cra_ctxsize		= CRYPTO_AES_CTX_SIZE,
			.cra_module		= THIS_MODULE,
		},
		.min_keysize	= AES_MIN_KEY_SIZE,
		.max_keysize	= AES_MAX_KEY_SIZE,
		.ivsize		= AES_BLOCK_SIZE,
		.setkey		= aesni_skcipher_setkey,
		.encrypt	= cbc_encrypt,
		.decrypt	= cbc_decrypt,
#ifdef CONFIG_X86_64
	}, {
		.base = {
			.cra_name		= "__ctr(aes)",
			.cra_driver_name	= "__ctr-aes-aesni",
			.cra_priority		= 400,
			.cra_flags		= CRYPTO_ALG_INTERNAL,
			.cra_blocksize		= 1,
			.cra_ctxsize		= CRYPTO_AES_CTX_SIZE,
			.cra_module		= THIS_MODULE,
		},
		.min_keysize	= AES_MIN_KEY_SIZE,
		.max_keysize	= AES_MAX_KEY_SIZE,
		.ivsize		= AES_BLOCK_SIZE,
		.chunksize	= AES_BLOCK_SIZE,
		.setkey		= aesni_skcipher_setkey,
		.encrypt	= ctr_crypt,
		.decrypt	= ctr_crypt,
	}, {
		.base = {
			.cra_name		= "__xts(aes)",
			.cra_driver_name	= "__xts-aes-aesni",
			.cra_priority		= 401,
			.cra_flags		= CRYPTO_ALG_INTERNAL,
			.cra_blocksize		= AES_BLOCK_SIZE,
			.cra_ctxsize		= XTS_AES_CTX_SIZE,
			.cra_module		= THIS_MODULE,
		},
		.min_keysize	= 2 * AES_MIN_KEY_SIZE,
		.max_keysize	= 2 * AES_MAX_KEY_SIZE,
		.ivsize		= AES_BLOCK_SIZE,
		.setkey		= xts_aesni_setkey,
		.encrypt	= xts_encrypt,
		.decrypt	= xts_decrypt,
#endif
	}
};

static
struct simd_skcipher_alg *aesni_simd_skciphers[ARRAY_SIZE(aesni_skciphers)];

#ifdef CONFIG_X86_64
static int generic_gcmaes_set_key(struct crypto_aead *aead, const u8 *key,
				  unsigned int key_len)
{
	struct generic_gcmaes_ctx *ctx = generic_gcmaes_ctx_get(aead);

	return aes_set_key_common(crypto_aead_tfm(aead),
				  &ctx->aes_key_expanded, key, key_len) ?:
	       rfc4106_set_hash_subkey(ctx->hash_subkey, key, key_len);
}

static int generic_gcmaes_encrypt(struct aead_request *req)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct generic_gcmaes_ctx *ctx = generic_gcmaes_ctx_get(tfm);
	void *aes_ctx = &(ctx->aes_key_expanded);
	u8 iv[16] __attribute__ ((__aligned__(AESNI_ALIGN)));
	__be32 counter = cpu_to_be32(1);

	memcpy(iv, req->iv, 12);
	*((__be32 *)(iv+12)) = counter;

	return gcmaes_encrypt(req, req->assoclen, ctx->hash_subkey, iv,
			      aes_ctx);
}

static int generic_gcmaes_decrypt(struct aead_request *req)
{
	__be32 counter = cpu_to_be32(1);
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);
	struct generic_gcmaes_ctx *ctx = generic_gcmaes_ctx_get(tfm);
	void *aes_ctx = &(ctx->aes_key_expanded);
	u8 iv[16] __attribute__ ((__aligned__(AESNI_ALIGN)));

	memcpy(iv, req->iv, 12);
	*((__be32 *)(iv+12)) = counter;

	return gcmaes_decrypt(req, req->assoclen, ctx->hash_subkey, iv,
			      aes_ctx);
}

static struct aead_alg aesni_aeads[] = { {
	.setkey			= common_rfc4106_set_key,
	.setauthsize		= common_rfc4106_set_authsize,
	.encrypt		= helper_rfc4106_encrypt,
	.decrypt		= helper_rfc4106_decrypt,
	.ivsize			= GCM_RFC4106_IV_SIZE,
	.maxauthsize		= 16,
	.base = {
		.cra_name		= "__rfc4106(gcm(aes))",
		.cra_driver_name	= "__rfc4106-gcm-aesni",
		.cra_priority		= 400,
		.cra_flags		= CRYPTO_ALG_INTERNAL,
		.cra_blocksize		= 1,
		.cra_ctxsize		= sizeof(struct aesni_rfc4106_gcm_ctx),
		.cra_alignmask		= AESNI_ALIGN - 1,
		.cra_module		= THIS_MODULE,
	},
}, {
	.setkey			= generic_gcmaes_set_key,
	.setauthsize		= generic_gcmaes_set_authsize,
	.encrypt		= generic_gcmaes_encrypt,
	.decrypt		= generic_gcmaes_decrypt,
	.ivsize			= GCM_AES_IV_SIZE,
	.maxauthsize		= 16,
	.base = {
		.cra_name		= "__gcm(aes)",
		.cra_driver_name	= "__generic-gcm-aesni",
		.cra_priority		= 400,
		.cra_flags		= CRYPTO_ALG_INTERNAL,
		.cra_blocksize		= 1,
		.cra_ctxsize		= sizeof(struct generic_gcmaes_ctx),
		.cra_alignmask		= AESNI_ALIGN - 1,
		.cra_module		= THIS_MODULE,
	},
} };
#else
static struct aead_alg aesni_aeads[0];
#endif

static struct simd_aead_alg *aesni_simd_aeads[ARRAY_SIZE(aesni_aeads)];

static const struct x86_cpu_id aesni_cpu_id[] = {
	X86_FEATURE_MATCH(X86_FEATURE_AES),
	{}
};
MODULE_DEVICE_TABLE(x86cpu, aesni_cpu_id);

static int __init aesni_init(void)
{
	int err;

	if (!x86_match_cpu(aesni_cpu_id))
		return -ENODEV;
#ifdef CONFIG_X86_64
#ifdef CONFIG_AS_AVX2
	if (boot_cpu_has(X86_FEATURE_AVX2)) {
		pr_info("AVX2 version of gcm_enc/dec engaged.\n");
		aesni_gcm_tfm = &aesni_gcm_tfm_avx_gen4;
	} else
#endif
#ifdef CONFIG_AS_AVX
	if (boot_cpu_has(X86_FEATURE_AVX)) {
		pr_info("AVX version of gcm_enc/dec engaged.\n");
		aesni_gcm_tfm = &aesni_gcm_tfm_avx_gen2;
	} else
#endif
	{
		pr_info("SSE version of gcm_enc/dec engaged.\n");
		aesni_gcm_tfm = &aesni_gcm_tfm_sse;
	}
	aesni_ctr_enc_tfm = aesni_ctr_enc;
#ifdef CONFIG_AS_AVX
	if (boot_cpu_has(X86_FEATURE_AVX)) {
		/* optimize performance of ctr mode encryption transform */
		aesni_ctr_enc_tfm = aesni_ctr_enc_avx_tfm;
		pr_info("AES CTR mode by8 optimization enabled\n");
	}
#endif
#endif

	err = crypto_register_algs(aesni_algs, ARRAY_SIZE(aesni_algs));
	if (err)
		return err;

	err = simd_register_skciphers_compat(aesni_skciphers,
					     ARRAY_SIZE(aesni_skciphers),
					     aesni_simd_skciphers);
	if (err)
		goto unregister_algs;

	err = simd_register_aeads_compat(aesni_aeads, ARRAY_SIZE(aesni_aeads),
					 aesni_simd_aeads);
	if (err)
		goto unregister_skciphers;

	return 0;

unregister_skciphers:
	simd_unregister_skciphers(aesni_skciphers, ARRAY_SIZE(aesni_skciphers),
				  aesni_simd_skciphers);
unregister_algs:
	crypto_unregister_algs(aesni_algs, ARRAY_SIZE(aesni_algs));
	return err;
}

static void __exit aesni_exit(void)
{
	simd_unregister_aeads(aesni_aeads, ARRAY_SIZE(aesni_aeads),
			      aesni_simd_aeads);
	simd_unregister_skciphers(aesni_skciphers, ARRAY_SIZE(aesni_skciphers),
				  aesni_simd_skciphers);
	crypto_unregister_algs(aesni_algs, ARRAY_SIZE(aesni_algs));
}

late_initcall(aesni_init);
module_exit(aesni_exit);

MODULE_DESCRIPTION("Rijndael (AES) Cipher Algorithm, Intel AES-NI instructions optimized");
MODULE_LICENSE("GPL");
MODULE_ALIAS_CRYPTO("aes");
