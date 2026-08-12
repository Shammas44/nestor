#include "testutils.h"
#include <criterion/criterion.h>
#include <unistd.h>
#include <stdlib.h>

/* ---------- timing helpers ---------- */

static uint64_t now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static const uint64_t WARN_THRESHOLD_NS = WARN_THRESHOLD_MS * 1000000ull;

/* ---------- per-test state ---------- */

static uint64_t test_start_ns;
const char *current_test_name;

/* ---------- hooks ---------- */

void test_init(void) { test_start_ns = now_ns(); }

void test_fini(void) {
  uint64_t end = now_ns();

  // Safety check in case clock_gettime failed or wrapped (unlikely but safe)
  if (end < test_start_ns)
    return;

  uint64_t elapsed = end - test_start_ns;

  if (elapsed > WARN_THRESHOLD_NS) {
    cr_log_warn("Slow test: %.3f ms [%s]", elapsed / 1.0e6,
                current_test_name ? current_test_name : "unknown");
  }
}

__attribute__((constructor))
static void set_global_timeout(void) {
  /*#region*/
  setenv("CRITERION_TIMEOUT", "10", 1);
  /*#endregion*/
}
