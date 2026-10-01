Crypto primitives, also compiled into the kernel. **EXPERIMENTAL and unaudited** (ARCHITECTURE §17); docs and user-facing text must say so.

The rest of the library arrives in M11.1, but three primitives arrive early (M2.6) because the kernel CSPRNG needs them:

| File | API (`include/crypto/`) | Vectors |
|---|---|---|
| `chacha20.c` | `chacha20Block`, `chacha20Xor` (RFC 8439; `STATUS_ERR_INVALID` if the 32-bit block counter would wrap) | RFC 8439 2.3.2, 2.4.2, A.1, A.2 |
| `sha256.c` | `sha256Init/Update/Final` (`Sha256Ctx`) | FIPS 180-4: "", "abc", the 448-bit message, 1,000,000 x 'a' |
| `wipe.c` | `cryptoWipe` (volatile stores; no inline asm, because assembly lives only in `kernel/arch/` and `boot/`) | host test |

Rules for every file here: freestanding C17 (only `stdint.h`/`stddef.h`, plus `uapi/status.h`), add/xor/rotate on secret data, no branch or table index on secret data (SHA-256's public round constants excepted), byte-wise loads and stores (no alignment or endianness assumptions), secrets wiped with `cryptoWipe` before returning, and a contract comment on every non-static function.

Tests: `libs/crypto/test/crypto-vectors.h` is the single vector file, used by the host tests (`tests/host/crypto_test.c`, `make host-tests`) and by the ktests (`kernel/test/crypto_test.c`: `chacha20_rfc8439_block`, `chacha20_rfc8439_encrypt`, `sha256_fips180_vectors`, required by `make test`). Provenance of the vectors is stated at the top of that header.

M11.1 lanes must build on these files (and keep the names above) rather than re-implementing them.
