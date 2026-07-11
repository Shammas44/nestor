#include "assert.h"
const Except Assert_Failed = {"Assertion failed", Assertion_Failed};
void(assert)(int e) { assert(e); }
