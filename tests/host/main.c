/* Runner for every host-side test: each TEST() in tests/host/ (and, later, other host tests)
 * links into this binary and self-registers before main() runs. */
#include "framework/test.h"

#include <stdio.h>

int main(void) {
    /* Unbuffered so RUN/PASS/FAIL interleave in order under CI's non-tty stdout, and so a
     * crashing test doesn't take its output down with it. */
    setvbuf(stdout, NULL, _IONBF, 0);

    int total = 0;
    int failedTests = 0;

    for (TestCase *tc = hostTestList; tc; tc = tc->next) {
        int failuresBefore = hostTestFailures;
        printf("RUN  %s\n", tc->name);
        tc->fn();
        total++;
        if (hostTestFailures != failuresBefore) {
            failedTests++;
            printf("FAIL %s\n", tc->name);
        } else {
            printf("PASS %s\n", tc->name);
        }
    }

    if (total == 0) {
        fprintf(stderr, "no tests registered\n");
        return 1;
    }

    printf("%d/%d tests passed\n", total - failedTests, total);
    return failedTests ? 1 : 0;
}
