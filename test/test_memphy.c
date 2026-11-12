#include "mm.h"
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef MM64
#define TEST_PAGE_SIZE PAGING64_PAGESZ
#else
#define TEST_PAGE_SIZE PAGING_PAGESZ
#endif
#define TEST_RAM_SIZE (16 * TEST_PAGE_SIZE)
#define TEST_FRAME_COUNT (TEST_RAM_SIZE / TEST_PAGE_SIZE)

static int failures;

static void check(int condition, const char *message)
{
  if (condition)
    printf("  [PASS] %s\n", message);
  else
  {
    printf("  [FAIL] %s\n", message);
    failures++;
  }
}

static void release_memphy(struct memphy_struct *mp)
{
  struct framephy_struct *node = mp->free_fp_list;
  while (node != NULL)
  {
    struct framephy_struct *next = node->fp_next;
    free(node);
    node = next;
  }
  free(mp->storage);
}

static void test_init_and_format(struct memphy_struct *mp)
{
  struct framephy_struct *node = mp->free_fp_list;
  int count = 0;
  int ordered = 1;

  while (node != NULL)
  {
    if (node->fpn != (addr_t)count)
      ordered = 0;
    count++;
    node = node->fp_next;
  }

  if (count != TEST_FRAME_COUNT || !ordered)
    printf("    detail: found %d frames, expected %d, ascending=%s\n",
           count, TEST_FRAME_COUNT, ordered ? "yes" : "no");
  check(count == TEST_FRAME_COUNT && ordered,
        "init_memphy formats 16 free frames in ascending order");
}

static void test_get_until_empty(struct memphy_struct *mp)
{
  int seen[TEST_FRAME_COUNT] = {0};
  int unique = 1;
  addr_t fpn;

  for (int i = 0; i < TEST_FRAME_COUNT; i++)
  {
    int status = MEMPHY_get_freefp(mp, &fpn);
    if (status != 0 || fpn >= TEST_FRAME_COUNT || seen[fpn])
    {
      printf("    detail: request %d returned status=%d, fpn=%lu\n",
             i + 1, status, (unsigned long)fpn);
      unique = 0;
      continue;
    }
    seen[fpn] = 1;
  }

  check(unique, "getting all 16 frames returns unique frame numbers");
  check(MEMPHY_get_freefp(mp, &fpn) == -1,
        "the 17th frame request reports exhaustion");
}

static void test_put_and_reuse(struct memphy_struct *mp)
{
  addr_t first;
  addr_t second;

  check(MEMPHY_put_freefp(mp, 3) == 0 && MEMPHY_put_freefp(mp, 7) == 0,
        "two frames can be returned to the free list");
  check(MEMPHY_get_freefp(mp, &first) == 0 && first == 7 &&
            MEMPHY_get_freefp(mp, &second) == 0 && second == 3 &&
            mp->free_fp_list == NULL,
        "returned frames are reused in LIFO order without breaking the list");
}

static void test_random_read_write(struct memphy_struct *mp)
{
  BYTE written = (BYTE)0xA7;
  BYTE readback = 0;
  addr_t address = 123;

  check(MEMPHY_write(mp, address, written) == 0 &&
            MEMPHY_read(mp, address, &readback) == 0 && readback == written,
        "random-access write/read preserves the byte value");
}

static void test_sequential_and_null(void)
{
  struct memphy_struct sequential = {0};
  BYTE written = (BYTE)0x5C;
  BYTE readback = 0;
  int null_checks;
  int sequential_checks;

  null_checks = MEMPHY_write(NULL, 0, written) == -1 &&
                MEMPHY_read(NULL, 0, &readback) == -1 &&
                MEMPHY_seq_write(NULL, 0, written) == -1 &&
                MEMPHY_seq_read(NULL, 0, &readback) == -1;
  check(null_checks, "all MEMPHY read/write APIs reject a NULL device");

  init_memphy(&sequential, TEST_RAM_SIZE, 0);
  sequential_checks = MEMPHY_seq_write(&sequential, 11, written) == 0 &&
                      MEMPHY_seq_read(&sequential, 11, &readback) == 0 &&
                      readback == written &&
                      MEMPHY_write(&sequential, 12, written) == 0 &&
                      MEMPHY_read(&sequential, 12, &readback) == 0 &&
                      readback == written;
  check(sequential_checks,
        "sequential mode (rdmflg=0) supports direct and wrapper I/O");
  release_memphy(&sequential);
}

int main(void)
{
  struct memphy_struct ram = {0};

  printf("=== MEMPHY TESTS ===\n");
  check(init_memphy(&ram, TEST_RAM_SIZE, 1) == 0,
        "random-access device initialization succeeds");
  test_init_and_format(&ram);
  test_get_until_empty(&ram);
  test_put_and_reuse(&ram);
  test_random_read_write(&ram);
  test_sequential_and_null();
  release_memphy(&ram);

  printf("=== MEMPHY RESULT: %d failure(s) ===\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}