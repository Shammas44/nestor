#include "testutils.h"
#include "arena.h"
#include "stringview.h"
#include "bitstack.h"
#include "error_codes.h"
#include <criterion/criterion.h>

static void init() {
  /*#region*/
  test_init();
  /*#endregion*/
}

static void fini() {
  /*#region*/
  test_fini();
  /*#endregion*/
}

TIMED_TEST(stage1, arena_alignment_and_chaining, init, fini)
/*#region*/
  // Create an arena with a small chunk size to trigger chaining
  Arena *arena = arena_create(16);
  cr_assert_not_null(arena);

  // Allocate 100 blocks of various sizes (1 to 50 bytes)
  for (int i = 0; i < 100; i++) {
    size_t size = (i % 50) + 1;
    void *ptr = na_alloc(arena, size);
    cr_assert_not_null(ptr);
    
    // Check that ptr is 8-byte aligned
    uintptr_t addr = (uintptr_t)ptr;
    cr_assert_eq(addr % 8, 0, "Address %p is not 8-byte aligned (i = %d, size = %zu)", ptr, i, size);
  }

  // Reset the arena
  arena_reset(arena);

  // Re-allocate to check that we can reuse the memory
  for (int i = 0; i < 100; i++) {
    size_t size = (i % 50) + 1;
    void *ptr = na_alloc(arena, size);
    cr_assert_not_null(ptr);
    uintptr_t addr = (uintptr_t)ptr;
    cr_assert_eq(addr % 8, 0);
  }

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage1, arena_checkpoint_restore, init, fini)
/*#region*/
  Arena *arena = arena_create(32);
  cr_assert_not_null(arena);

  void *p1 = na_alloc(arena, 10);
  cr_assert_not_null(p1);

  size_t chk = arena_checkpoint(arena);

  void *p2 = na_alloc(arena, 20);
  cr_assert_not_null(p2);

  // Restore to checkpoint
  arena_restore(arena, chk);

  // Next allocation should overwrite/use the same space as p2
  void *p3 = na_alloc(arena, 20);
  cr_assert_eq(p2, p3, "p2 (%p) and p3 (%p) should be at the same address after restore", p2, p3);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage1, stringview_helpers, init, fini)
/*#region*/
  Arena *arena = arena_create(1024);
  cr_assert_not_null(arena);

  StringView sv1 = { "hello world", 11 };
  StringView sv2 = { "hello", 5 };
  StringView sv3 = { "world", 5 };
  StringView sv4 = { "hello world", 11 };

  cr_assert(sv_equals_cstr(sv1, "hello world"));
  cr_assert(!sv_equals_cstr(sv1, "hello"));

  cr_assert_eq(sv_compare(sv1, sv4), 0);
  cr_assert(sv_compare(sv1, sv2) > 0);
  cr_assert(sv_compare(sv2, sv1) < 0);

  cr_assert(sv_starts_with(sv1, sv2));
  cr_assert(!sv_starts_with(sv1, sv3));

  cr_assert_eq(sv_find_char(sv1, 'o'), 4);
  cr_assert_eq(sv_find_char(sv1, 'z'), -1);

  char *cstr = sv_to_cstring(arena, sv1);
  cr_assert_not_null(cstr);
  cr_assert_str_eq(cstr, "hello world");

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST

TIMED_TEST(stage1, bitstack_operations, init, fini)
/*#region*/
  Arena *arena = arena_create(1024);
  cr_assert_not_null(arena);

  BitStack stack;
  int32_t status = bitstack_init(&stack, arena, 5);
  cr_assert_eq(status, ERR_SUCCESS);
  cr_assert_eq(bitstack_size(&stack), 0);

  // Push elements
  cr_assert_eq(bitstack_push(&stack, 42), ERR_SUCCESS);
  cr_assert_eq(bitstack_push(&stack, 100), ERR_SUCCESS);
  cr_assert_eq(bitstack_size(&stack), 2);

  // Peek
  uint64_t val = 0;
  cr_assert_eq(bitstack_peek(&stack, &val), ERR_SUCCESS);
  cr_assert_eq(val, 100);

  // Pop
  cr_assert_eq(bitstack_pop(&stack, &val), ERR_SUCCESS);
  cr_assert_eq(val, 100);
  cr_assert_eq(bitstack_size(&stack), 1);

  cr_assert_eq(bitstack_pop(&stack, &val), ERR_SUCCESS);
  cr_assert_eq(val, 42);
  cr_assert_eq(bitstack_size(&stack), 0);

  // Underflow
  cr_assert_eq(bitstack_pop(&stack, &val), ERR_OOM);

  // Push up to capacity
  cr_assert_eq(bitstack_push(&stack, 1), ERR_SUCCESS);
  cr_assert_eq(bitstack_push(&stack, 2), ERR_SUCCESS);
  cr_assert_eq(bitstack_push(&stack, 3), ERR_SUCCESS);
  cr_assert_eq(bitstack_push(&stack, 4), ERR_SUCCESS);
  cr_assert_eq(bitstack_push(&stack, 5), ERR_SUCCESS);
  // Overflow
  cr_assert_eq(bitstack_push(&stack, 6), ERR_OOM);

  arena_destroy(arena);
/*#endregion*/
END_TIMED_TEST
