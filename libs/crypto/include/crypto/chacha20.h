/* ChaCha20 (RFC 8439 section 2: 96-bit nonce, 32-bit block counter). EXPERIMENTAL, unaudited
 * crypto (ARCHITECTURE §17). Used first by the kernel CSPRNG (M2.6), later by TLS/SSH. */
#ifndef CRYPTO_CHACHA20_H
#define CRYPTO_CHACHA20_H

#include "uapi/status.h"

#include <stddef.h>
#include <stdint.h>

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

/* Writes the 64-byte keystream block for (`key`, `counter`, `nonce`) to `out` (RFC 8439 2.3):
 * words are read/written little-endian byte-wise, so it is endian- and alignment-independent.
 * `out` must not overlap `key` or `nonce`. Only add/xor/rotate on secret data; no secret-dependent
 * branch or memory index. Wipes its internal state before returning. No locks, may not sleep,
 * IRQ-safe, cannot fail. */
void chacha20Block(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                   const uint8_t nonce[CHACHA20_NONCE_SIZE], uint8_t out[CHACHA20_BLOCK_SIZE]);

/* out[i] = in[i] ^ keystream[i] for i < `len`, starting at block `counter` (RFC 8439 2.4). Encrypt
 * and decrypt are the same operation. `in` == `out` (in place) is fine; any other overlap of `in`
 * and `out` is not supported, and `out` must not overlap `key` or `nonce` (they are re-read for
 * every block, so the keystream after the first block would silently be wrong). Fails with
 * STATUS_ERR_INVALID (writing nothing) if the message would need a block number above 0xFFFFFFFF
 * (the 32-bit counter must never wrap, or the keystream would repeat), or if `len` > 0 and `key`,
 * `nonce`, `in` or `out` is NULL. `len` == 0 succeeds for any `counter`, and then `in` and `out`
 * are not touched (may be NULL). Wipes the keystream before returning. No locks, may not sleep,
 * IRQ-safe. */
Status chacha20Xor(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                   const uint8_t nonce[CHACHA20_NONCE_SIZE], const uint8_t *in, uint8_t *out,
                   size_t len);

#endif
