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
static void test_boundary_and_integrity(struct memphy_struct *mp)
{
  BYTE readback = 0;

  /* 1. Biên trên hợp lệ (byte cuối cùng của RAM) */
  check(MEMPHY_write(mp, TEST_RAM_SIZE - 1, (BYTE)0xFE) == 0 &&
            MEMPHY_read(mp, TEST_RAM_SIZE - 1, &readback) == 0 &&
            readback == (BYTE)0xFE,
        "read/write at the exact last valid byte (maxsz - 1) succeeds");

  /* 2. Vượt biên địa chỉ vật lý */
  check(MEMPHY_write(mp, TEST_RAM_SIZE, (BYTE)0x11) == -1 &&
            MEMPHY_read(mp, TEST_RAM_SIZE, &readback) == -1 &&
            MEMPHY_write(mp, TEST_RAM_SIZE + 100, (BYTE)0x22) == -1 &&
            MEMPHY_read(mp, TEST_RAM_SIZE + 100, &readback) == -1,
        "out-of-bounds read/write (addr >= maxsz) is rejected");

  /* 3. Ghi/đọc xuyên ranh giới 2 frame liền kề (Frame 0 -> Frame 1) */
  addr_t boundary = TEST_PAGE_SIZE - 1;
  check(MEMPHY_write(mp, boundary, (BYTE)0x80) == 0 &&
            MEMPHY_write(mp, boundary + 1, (BYTE)0xFF) == 0 &&
            MEMPHY_read(mp, boundary, &readback) == 0 && readback == (BYTE)0x80 &&
            MEMPHY_read(mp, boundary + 1, &readback) == 0 && readback == (BYTE)0xFF,
        "cross-frame boundary bytes (0x80, 0xFF) do not overwrite each other");

  /* 4. Ghi/đọc toàn bộ 256 giá trị byte (0x00 -> 0xFF) */
  int all_bytes_ok = 1;
  for (int b = 0; b < 256; b++)
  {
    if (MEMPHY_write(mp, (addr_t)b, (BYTE)b) != 0 ||
        MEMPHY_read(mp, (addr_t)b, &readback) != 0 ||
        readback != (BYTE)b)
    {
      all_bytes_ok = 0;
      break;
    }
  }
  check(all_bytes_ok, "full 8-bit byte range (0x00..0xFF) is preserved accurately");
}

static void test_invalid_arguments(struct memphy_struct *mp)
{
  struct memphy_struct bad_mp = {0};
  addr_t fpn = 0;

  check(init_memphy(NULL, TEST_RAM_SIZE, 1) == -1 &&
            init_memphy(&bad_mp, 0, 1) == -1,
        "init_memphy rejects NULL struct or zero size");

  check(MEMPHY_get_freefp(NULL, &fpn) == -1 &&
            MEMPHY_get_freefp(mp, NULL) == -1 &&
            MEMPHY_put_freefp(NULL, 0) == -1,
        "frame management APIs reject NULL pointers");

  check(MEMPHY_dump(mp) == 0 && MEMPHY_dump(NULL) == -1,
        "MEMPHY_dump handles valid and NULL devices safely");
}
int main(void)
{
  struct memphy_struct ram = {0};

  printf("=== MEMPHY EXTENDED TESTS ===\n");
  check(init_memphy(&ram, TEST_RAM_SIZE, 1) == 0,
        "random-access device initialization succeeds");
  test_init_and_format(&ram);
  test_get_until_empty(&ram);
  test_put_and_reuse(&ram);
  test_random_read_write(&ram);
  test_boundary_and_integrity(&ram);
  test_sequential_and_null();
  test_invalid_arguments(&ram);
  MEMPHY_destroy(&ram);

  printf("=== MEMPHY RESULT: %d failure(s) ===\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}