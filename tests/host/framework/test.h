/* Tiny assert-based test framework for host-side unit tests (ARCHITECTURE §23: pure logic --
 * allocators' algorithms, bongfs core, crypto vectors, parsers). Tests self-register via a
 * constructor, so a new TEST() needs no separate registration list. */
#ifndef BONGOS_HOST_TEST_H
#define BONGOS_HOST_TEST_H

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

#define TEST(testName)                                                                             \
    static void testName(void);                                                                    \
    static TestCase testName##_case = {#testName, testName, NULL};                                 \
    __attribute__((constructor)) static void testName##_register(void) {                           \
        hostTestRegister(&testName##_case);                                                        \
    }                                                                                              \
    static void testName(void)

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL %s:%d: ASSERT_TRUE(%s)\n", __FILE__, __LINE__, #cond);         \
            hostTestFailures++;                                                                    \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        long long va_ = (long long)(a);                                                            \
        long long vb_ = (long long)(b);                                                            \
        if (va_ != vb_) {                                                                          \
            fprintf(stderr, "  FAIL %s:%d: ASSERT_EQ(%s, %s) -> %lld != %lld\n", __FILE__,         \
                    __LINE__, #a, #b, va_, vb_);                                                   \
            hostTestFailures++;                                                                    \
        }                                                                                          \
    } while (0)

#define ASSERT_STREQ(a, b)                                                                         \
    do {                                                                                           \
        const char *sa_ = (a);                                                                     \
        const char *sb_ = (b);                                                                     \
        if (strcmp(sa_, sb_) != 0) {                                                               \
            fprintf(stderr, "  FAIL %s:%d: ASSERT_STREQ(%s, %s) -> \"%s\" != \"%s\"\n", __FILE__,  \
                    __LINE__, #a, #b, sa_, sb_);                                                   \
            hostTestFailures++;                                                                    \
        }                                                                                          \
    } while (0)

#endif
