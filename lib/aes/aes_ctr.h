/*
 * Minimal AES-256 in CTR mode. Public domain / CC0.
 *
 * Self-contained (no OpenSSL) so content packs decrypt identically on every
 * platform the engine builds for. CTR is symmetric: the same call encrypts
 * and decrypts. Not constant-time; it protects bundled content, not secrets
 * against local side channels.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t round_keys[240]; /* 15 round keys x 16 bytes */
} aes256_key;

/* Expands a 32-byte key. */
void aes256_set_key(aes256_key *ctx, const uint8_t key[32]);

/*
 * XORs `len` bytes of `in` into `out` (may alias) with the AES-CTR keystream
 * that starts at the 16-byte big-endian counter block `iv` — the block that
 * corresponds to offset 0 of the object. The counter increments once per
 * 16-byte block, big-endian over the whole 128 bits (NIST SP 800-38A / the
 * same convention as OpenSSL EVP_aes_256_ctr and Python `cryptography`
 * modes.CTR), so any object can be decrypted in one call from its start.
 */
void aes256_ctr_xor(const aes256_key *ctx, const uint8_t iv[16],
		const uint8_t *in, uint8_t *out, size_t len);

/*
 * Same as aes256_ctr_xor but starting at byte offset `offset` into the
 * object's keystream — allows decrypting a slice of an entry without
 * processing everything before it.
 */
void aes256_ctr_xor_at(const aes256_key *ctx, const uint8_t iv[16],
		uint64_t offset, const uint8_t *in, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
