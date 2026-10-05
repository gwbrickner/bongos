/* Kernel command-line helpers (ARCHITECTURE §5.3's cmdline, used today for `ktest=`). Freestanding
 * (no libc string.h), so this also provides the tiny string helpers the cmdline/ktest code needs.
 */
#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Splits `cmdline` on spaces/tabs and copies the value of the *last* "<key>=" token into `out`
 * (capacity `outCap`, truncated if needed); returns false (leaving `out` untouched) if no such
 * token exists. `key` is given without the '='. No locks, boot-time only; pure. */
bool cmdlineFindValue(const char *cmdline, const char *key, char *out, size_t outCap);

/* Like cmdlineFindValue(), but returns the value of the last "<key>=" token in place: `*outValue`
 * points into `cmdline` (not NUL-terminated at the value's end) and `*outLen` is its full length,
 * so nothing is truncated. false (outputs untouched) if there is no such token. No locks; pure. */
bool cmdlineFindValueSpan(const char *cmdline, const char *key, const char **outValue,
                          size_t *outLen);

/* cmdlineFindValue(cmdline, "ktest", ...). */
bool cmdlineFindKtest(const char *cmdline, char *out, size_t outCap);

/* Whole-string glob match: '*' in `pattern` matches any substring (including empty), every other
 * character must match `name` literally. The match is anchored at both ends (the pattern must
 * describe the whole name, not a substring of it). No locks, boot-time only; pure. */
bool cmdlineGlobMatch(const char *pattern, const char *name);

/* True if `s` and `t` are equal C strings. Freestanding replacement for strcmp()==0 (ARCHITECTURE
 * §4: only stdint/stddef/stdbool/stdarg/stdatomic in the kernel). No locks; pure. */
bool cmdlineStrEq(const char *s, const char *t);

/* True if `token` (e.g. "fbcon=off") appears as a whole space/tab-separated token anywhere in
 * `cmdline` (not as a substring of a longer token). No locks, boot-time only; pure. */
bool cmdlineHasToken(const char *cmdline, const char *token);

#endif
