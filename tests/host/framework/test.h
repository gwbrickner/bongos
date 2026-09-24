/* Tiny assert-based test framework for host-side unit tests (ARCHITECTURE §23: pure logic --
 * allocators' algorithms, bongfs core, crypto vectors, parsers). Tests self-register via a
 * constructor, so a new TEST() needs no separate registration list. An assert that fails
 * returns from its enclosing function immediately, so a later assert can't run against state a
 * failed precondition never established (e.g. dereferencing a pointer ASSERT_TRUE just
 * rejected). Used inside a helper called from a TEST(), it only returns from that helper --
 * the failure is still recorded, but the test body keeps running after the helper returns. */
#ifndef HOST_TEST_H
#define HOST_TEST_H

#include <stdio.h>
#include <string.h>

typedef void (*TestFn)(void);

typedef struct TestCase {
    const char *name;
    TestFn fn;
    struct TestCase *next;
} TestCase;

extern TestCase *hostTestList;
extern int hostTestFailures;

void hostTestRegister(TestCase *tc);

/* testName becomes the identifier prefix (testNameTest/Case/Register), not the test's own
 * function, so TEST(pmmAllocPages) can't collide with a real pmmAllocPages() under test. */
#define TEST(testName)                                                                             \
    static void testName##Test(void);                                                              \
    static TestCase testName##Case = {#testName, testName##Test, NULL};                            \
    __attribute__((constructor)) static void testName##Register(void) {                            \
        hostTestRegister(&testName##Case);                                                         \
    }                                                                                              \
    static void testName##Test(void)

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #cond);         \
            hostTestFailures++;                                                                    \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* Evaluates a and b exactly once into their own types (__auto_type; a clang/gcc extension) so
 * an operand with side effects (a counter, an allocator call) isn't re-run for the failure
 * message with a different result. The message casts to long long for printing, which is
 * exact for anything that fits (the common case: sizes, counts, small pointers) and only
 * approximate for huge unsigned values or floats -- good enough for a diagnostic, not used for
 * the pass/fail decision itself. */
#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        __auto_type a_ = (a);                                                                      \
        __auto_type b_ = (b);                                                                      \
        if (!(a_ == b_)) {                                                                         \
            fprintf(stderr, "  FAIL %s:%d: ASSERT_EQ(%s, %s) -> %lld != %lld\n", __FILE__,         \
                    __LINE__, #a, #b, (long long)a_, (long long)b_);                               \
            hostTestFailures++;                                                                    \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_STREQ(a, b)                                                                         \
    do {                                                                                           \
        const char *sa_ = (a);                                                                     \
        const char *sb_ = (b);                                                                     \
        int mismatch_ = (sa_ == NULL || sb_ == NULL) ? (sa_ != sb_) : (strcmp(sa_, sb_) != 0);     \
        if (mismatch_) {                                                                           \
            fprintf(stderr, "  FAIL %s:%d: ASSERT_STREQ(%s, %s) -> \"%s\" != \"%s\"\n", __FILE__,  \
                    __LINE__, #a, #b, sa_ ? sa_ : "(null)", sb_ ? sb_ : "(null)");                 \
            hostTestFailures++;                                                                    \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#endif
