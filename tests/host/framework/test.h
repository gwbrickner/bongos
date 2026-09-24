/* Tiny assert-based test framework for host-side unit tests (ARCHITECTURE §23: pure logic --
 * allocators' algorithms, bongfs core, crypto vectors, parsers). Tests self-register via a
 * constructor, so a new TEST() needs no separate registration list. An assert that fails
 * returns from the enclosing test immediately, so a later assert can't run against state a
 * failed precondition never established (e.g. dereferencing a pointer ASSERT_TRUE just
 * rejected). */
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

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        if (!((a) == (b))) {                                                                       \
            fprintf(stderr, "  FAIL %s:%d: ASSERT_EQ(%s, %s) -> %lld != %lld\n", __FILE__,         \
                    __LINE__, #a, #b, (long long)(a), (long long)(b));                             \
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
