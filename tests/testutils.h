#ifndef _NESTOR_TEST_UTILS_H
#define _NESTOR_TEST_UTILS_H
#include <criterion/criterion.h>
#include <stdint.h>

/* ---------- per-test state ---------- */

extern const char *current_test_name;

#define KB(x) 1024 * x
#define MB(x) 1024 * 1024 * x

typedef struct {
  bool expected_result;
  const char *source_code;
  const char *test_description;
} ParserTest;

/* ---------- helper macro ---------- */
void test_init(void);

void test_fini(void);

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
/*
 * Must be called at the beginning of each test body
 */
#define WARN_THRESHOLD_MS 1000

#define SET_TEST_NAME(suite, test) current_test_name = STR(suite) ":" STR(test)

#define TIMED_TEST(suite, name, f_init, f_fini)                                \
  Test(suite, name, .init = f_init, .fini = f_fini, .timeout = 10) {            \
    SET_TEST_NAME(suite, name);

#define END_TIMED_TEST }
#endif
