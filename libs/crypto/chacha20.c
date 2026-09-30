/* ChaCha20 block function and stream XOR (RFC 8439 sections 2.1-2.4). EXPERIMENTAL crypto
 * (ARCHITECTURE §17): only add/xor/rotate touch key-derived words, nothing branches or indexes
 * memory on them, and the working state is wiped before every return. */
#include "crypto/chacha20.h"

#include "crypto/wipe.h"

/* Little-endian byte-wise load/store: no alignment or endianness assumption. The operands are
 * widened to uint32_t before shifting (an int shift into the sign bit is UB, and UBSan traps it).
 */
static uint32_t load32Le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32Le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n)))) /* 0 < n < 32 */

#define QUARTER_ROUND(a, b, c, d)                                                                  \
    do {                                                                                           \
        (a) += (b);                                                                                \
        (d) ^= (a);                                                                                \
        (d) = ROTL32((d), 16);                                                                     \
        (c) += (d);                                                                                \
        (b) ^= (c);                                                                                \
        (b) = ROTL32((b), 12);                                                                     \
        (a) += (b);                                                                                \
        (d) ^= (a);                                                                                \
        (d) = ROTL32((d), 8);                                                                      \
        (c) += (d);                                                                                \
        (b) ^= (c);                                                                                \
        (b) = ROTL32((b), 7);                                                                      \
    } while (0)

/* Contract: see crypto/chacha20.h. */
void chacha20Block(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                   const uint8_t nonce[CHACHA20_NONCE_SIZE], uint8_t out[CHACHA20_BLOCK_SIZE]) {
    uint32_t state[16];
    uint32_t x[16];

    /* "expand 32-byte k" */
    state[0] = 0x61707865u;
    state[1] = 0x3320646Eu;
    state[2] = 0x79622D32u;
    state[3] = 0x6B206574u;
    for (int i = 0; i < 8; i++) {
        state[4 + i] = load32Le(key + 4 * i);
    }
    state[12] = counter;
    for (int i = 0; i < 3; i++) {
        state[13 + i] = load32Le(nonce + 4 * i);
    }

    for (int i = 0; i < 16; i++) {
        x[i] = state[i];
    }
    for (int round = 0; round < 10; round++) { /* 10 double rounds = 20 rounds */
        QUARTER_ROUND(x[0], x[4], x[8], x[12]);
        QUARTER_ROUND(x[1], x[5], x[9], x[13]);
        QUARTER_ROUND(x[2], x[6], x[10], x[14]);
        QUARTER_ROUND(x[3], x[7], x[11], x[15]);
        QUARTER_ROUND(x[0], x[5], x[10], x[15]);
        QUARTER_ROUND(x[1], x[6], x[11], x[12]);
        QUARTER_ROUND(x[2], x[7], x[8], x[13]);
        QUARTER_ROUND(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) {
        store32Le(out + 4 * i, x[i] + state[i]);
    }

    cryptoWipe(state, sizeof(state));
    cryptoWipe(x, sizeof(x));
}

/* Contract: see crypto/chacha20.h. */
Status chacha20Xor(const uint8_t key[CHACHA20_KEY_SIZE], uint32_t counter,
                   const uint8_t nonce[CHACHA20_NONCE_SIZE], const uint8_t *in, uint8_t *out,
                   size_t len) {
    if (len == 0) {
        return STATUS_OK;
    }
    if (key == NULL || nonce == NULL || in == NULL || out == NULL) {
        return STATUS_ERR_INVALID;
    }
    /* Blocks needed, in 64 bits so a 32-bit size_t or a huge len can't wrap the arithmetic:
     * blocks * 64 >= len > (blocks - 1) * 64. The counter values used are counter .. counter +
     * blocks - 1, which must all be <= 0xFFFFFFFF. */
    uint64_t blocks = ((uint64_t)len >> 6) + (((uint64_t)len & 63u) != 0 ? 1u : 0u);
    if (blocks > 0x100000000ULL - (uint64_t)counter) {
        return STATUS_ERR_INVALID;
    }

    uint8_t keystream[CHACHA20_BLOCK_SIZE];
    size_t done = 0;
    while (done < len) {
        chacha20Block(key, counter, nonce, keystream);
        size_t n = len - done;
        if (n > CHACHA20_BLOCK_SIZE) {
            n = CHACHA20_BLOCK_SIZE;
        }
        for (size_t i = 0; i < n; i++) {
            out[done + i] = (uint8_t)(in[done + i] ^ keystream[i]);
        }
        done += n;
        counter++; /* may wrap to 0 only after the very last block, when it is no longer used */
    }
    cryptoWipe(keystream, sizeof(keystream));
    return STATUS_OK;
}
