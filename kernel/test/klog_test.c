/* ktests for the kernel's tiny printf (kernel/core/format.c), exercised through ksnprintf(). */
#include "cmdline.h"
#include "format.h"
#include "ktest.h"

KTEST(klog_format) {
    char buf[128];

    ksnprintf(buf, sizeof(buf), "%llx", 0xdeadbeefULL);
    KTEST_ASSERT(cmdlineStrEq(buf, "deadbeef"));

    ksnprintf(buf, sizeof(buf), "%d", -42);
    KTEST_ASSERT(cmdlineStrEq(buf, "-42"));

    ksnprintf(buf, sizeof(buf), "%s", "hello");
    KTEST_ASSERT(cmdlineStrEq(buf, "hello"));

    ksnprintf(buf, sizeof(buf), "%u", 12345u);
    KTEST_ASSERT(cmdlineStrEq(buf, "12345"));

    ksnprintf(buf, sizeof(buf), "%04d", 7);
    KTEST_ASSERT(cmdlineStrEq(buf, "0007"));

    ksnprintf(buf, sizeof(buf), "%016llx", 0xABCULL);
    KTEST_ASSERT(cmdlineStrEq(buf, "0000000000000abc"));

    /* A format string that ends mid-specifier ("%l", "%0", "%5", ...) must stop cleanly at the
     * string's own NUL instead of reading past it: this used to walk one byte past the format
     * string's terminator (confirmed under a host ASan build), because the length modifier/width
     * parsing left `p` pointing at the NUL and the switch's `default:` case then appended it and
     * advanced past it. */
    ksnprintf(buf, sizeof(buf), "%l");
    KTEST_ASSERT(cmdlineStrEq(buf, ""));
}
