#include "testutils.h"
#include <assert.h>
#include <criterion/criterion.h>
#include <criterion/logging.h>

/* ---------- Helpers ---------- */


static void init() {
  /*#region*/
  test_init();
  // DO STUFF
  /*#endregion*/
}

static void fini() {
  /*#region*/
  test_fini();
  // DO STUFF
  /*#endregion*/
}

/* ---------- Initialization ---------- */

TIMED_TEST(T, parse, init, fini)
/*#region*/
// TO STUFF
/*#endregion*/
END_TIMED_TEST
