#include "test.h"

TestCase *hostTestList = NULL;
static TestCase *hostTestTail = NULL;
int hostTestFailures = 0;

void hostTestRegister(TestCase *tc) {
    tc->next = NULL;
    if (hostTestTail) {
        hostTestTail->next = tc;
    } else {
        hostTestList = tc;
    }
    hostTestTail = tc;
}
