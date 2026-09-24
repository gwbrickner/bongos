#include "test.h"

TestCase *hostTestList = NULL;
int hostTestFailures = 0;

void hostTestRegister(TestCase *tc) {
    tc->next = hostTestList;
    hostTestList = tc;
}
