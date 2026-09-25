/* See cmdline.h. */
#include "cmdline.h"

bool cmdlineStrEq(const char *s, const char *t) {
    while (*s != '\0' && *t != '\0') {
        if (*s != *t) {
            return false;
        }
        s++;
        t++;
    }
    return *s == *t;
}

static bool cmdlineIsSep(char c) {
    return c == ' ' || c == '\t';
}

bool cmdlineFindKtest(const char *cmdline, char *out, size_t outCap) {
    if (cmdline == NULL || out == NULL || outCap == 0) {
        return false;
    }
    static const char prefix[] = "ktest=";
    const size_t prefixLen = sizeof(prefix) - 1;

    const char *best = NULL;
    size_t bestLen = 0;
    const char *p = cmdline;
    while (*p != '\0') {
        while (cmdlineIsSep(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *tokStart = p;
        while (*p != '\0' && !cmdlineIsSep(*p)) {
            p++;
        }
        size_t tokLen = (size_t)(p - tokStart);

        if (tokLen >= prefixLen) {
            bool matches = true;
            for (size_t i = 0; i < prefixLen; i++) {
                if (tokStart[i] != prefix[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                best = tokStart + prefixLen;
                bestLen = tokLen - prefixLen;
            }
        }
    }
    if (best == NULL) {
        return false;
    }
    size_t copyLen = (bestLen < outCap - 1) ? bestLen : outCap - 1;
    for (size_t i = 0; i < copyLen; i++) {
        out[i] = best[i];
    }
    out[copyLen] = '\0';
    return true;
}

bool cmdlineHasToken(const char *cmdline, const char *token) {
    if (cmdline == NULL || token == NULL) {
        return false;
    }
    const char *p = cmdline;
    while (*p != '\0') {
        while (cmdlineIsSep(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *tokStart = p;
        while (*p != '\0' && !cmdlineIsSep(*p)) {
            p++;
        }
        size_t tokLen = (size_t)(p - tokStart);
        size_t i = 0;
        while (i < tokLen && token[i] != '\0' && tokStart[i] == token[i]) {
            i++;
        }
        if (i == tokLen && token[i] == '\0') {
            return true;
        }
    }
    return false;
}

/* Standard iterative wildcard match (a single backtrack point is enough for a single '*' class of
 * pattern): no recursion, bounded by strlen(name) iterations (ARCHITECTURE §4 forbids unbounded
 * recursion in the kernel). */
bool cmdlineGlobMatch(const char *pattern, const char *name) {
    const char *p = pattern;
    const char *n = name;
    const char *starP = NULL;
    const char *starN = NULL;

    while (*n != '\0') {
        if (*p == *n) {
            p++;
            n++;
        } else if (*p == '*') {
            starP = p;
            starN = n;
            p++;
        } else if (starP != NULL) {
            p = starP + 1;
            starN++;
            n = starN;
        } else {
            return false;
        }
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}
