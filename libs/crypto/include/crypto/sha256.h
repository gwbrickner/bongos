/* SHA-256 (FIPS 180-4 / RFC 6234). EXPERIMENTAL, unaudited crypto (ARCHITECTURE §17). */
#ifndef CRYPTO_SHA256_H
#define CRYPTO_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_BLOCK_SIZE  64

/* Incremental hashing state. Treat as opaque; it holds message-derived data (possibly secret), so
 * sha256Final wipes it, and callers who abandon a context early should cryptoWipe it. */
typedef struct Sha256Ctx {
    uint32_t h[8];                  /* chaining value */
    uint64_t totalBytes;            /* bytes absorbed so far */
    uint8_t buf[SHA256_BLOCK_SIZE]; /* pending partial block */
    uint32_t bufLen;                /* bytes in buf, < SHA256_BLOCK_SIZE between calls */
} Sha256Ctx;

/* Starts a new hash in `ctx` (fully overwritten). No locks, may not sleep, IRQ-safe, cannot fail.
 */
void sha256Init(Sha256Ctx *ctx);

/* Absorbs `len` bytes at `data` (any alignment, any split into calls gives the same digest).
 * `len` == 0 is a no-op and `data` may then be NULL. `ctx` must have been initialised and not yet
 * finalised. Messages up to 2^61 - 1 bytes are supported (the length field is 64 bits of bits;
 * beyond that the encoded length wraps as the standard's does not define it). No locks, may not
 * sleep, IRQ-safe, cannot fail. */
void sha256Update(Sha256Ctx *ctx, const void *data, size_t len);

/* Pads, finishes, writes the 32-byte big-endian digest to `out`, and wipes `ctx` (start over with
 * sha256Init to reuse it). `out` must not overlap `ctx` (the wipe runs after the digest is
 * written, so e.g. `out` == ctx->buf would come back all zero). No locks, may not sleep,
 * IRQ-safe, cannot fail. */
void sha256Final(Sha256Ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

#endif
