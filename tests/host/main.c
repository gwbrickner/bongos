/* Runner for every host-side test: each TEST() in tests/host/ (and, later, other host tests)
 * links into this binary and self-registers before main() runs. */
#include "framework/test.h"

#include <stdio.h>

int main(void) {
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

    printf("%d/%d tests passed\n", total - failedTests, total);
    return failedTests ? 1 : 0;
}
