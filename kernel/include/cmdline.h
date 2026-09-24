/* Kernel command-line helpers (ARCHITECTURE §5.3's cmdline, used today for `ktest=`). Freestanding
 * (no libc string.h), so this also provides the tiny string helpers the cmdline/ktest code needs.
 */
#ifndef KERNEL_CMDLINE_H
#define KERNEL_CMDLINE_H

#include <stdbool.h>
#include <stddef.h>

/* Splits `cmdline` on spaces/tabs and copies the value of the *last* "ktest=" token into `out`
 * (capacity `outCap`, truncated if needed); returns false (leaving `out` untouched) if no such
 * token exists. No locks, boot-time only; pure. */
bool cmdlineFindKtest(const char *cmdline, char *out, size_t outCap);

/* Whole-string glob match: '*' in `pattern` matches any substring (including empty), every other
 * character must match `name` literally. The match is anchored at both ends (the pattern must
 * describe the whole name, not a substring of it). No locks, boot-time only; pure. */
bool cmdlineGlobMatch(const char *pattern, const char *name);

/* True if `s` and `t` are equal C strings. Freestanding replacement for strcmp()==0 (ARCHITECTURE
 * §4: only stdint/stddef/stdbool/stdarg/stdatomic in the kernel). No locks; pure. */
bool cmdlineStrEq(const char *s, const char *t);

#endif
