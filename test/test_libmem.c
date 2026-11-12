#include "libmem.h"
#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture
{
  struct mm_struct mm;
  struct pcb_t proc;
  struct krnl_t krnl;
  struct memphy_struct ram;
  struct memphy_struct swap;
};

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

static void release_rg_list(struct vm_rg_struct *node)
{
  while (node != NULL)
  {
    struct vm_rg_struct *next = node->rg_next;
    free(node);
    node = next;
  }
}

static void init_fixture(struct fixture *f, addr_t free_end)
{
  memset(f, 0, sizeof(*f));
  init_mm(&f->mm, &f->proc);
  free(f->mm.mmap->vm_freerg_list);
  f->mm.mmap->vm_freerg_list = init_vm_rg(0, free_end);
  f->proc.mm = &f->mm;
  f->proc.krnl = &f->krnl;
  f->krnl.mram = &f->ram;
  f->krnl.active_mswp = &f->swap;
  init_memphy(&f->ram, 8 * PAGING64_PAGESZ, 1);
  init_memphy(&f->swap, 8 * PAGING64_PAGESZ, 1);
}

static void destroy_fixture(struct fixture *f)
{
  release_memphy(&f->ram);
  release_memphy(&f->swap);
  release_rg_list(f->mm.mmap->vm_freerg_list);
  free(f->mm.mmap);
  free(f->mm.pgd);
  while (f->mm.fifo_pgn != NULL)
  {
    struct pgn_t *next = f->mm.fifo_pgn->pg_next;
    free(f->mm.fifo_pgn);
    f->mm.fifo_pgn = next;
  }
}

static void test_alloc_free_reuse(void)
{
  struct fixture f;
  addr_t start0, start1, start2;

  init_fixture(&f, 4096);
  int result = liballoc(&f.proc, 64, 0) == 0 &&
               liballoc(&f.proc, 64, 1) == 0;
  start0 = f.mm.symrgtbl[0].rg_start;
  start1 = f.mm.symrgtbl[1].rg_start;
  result = result && libfree(&f.proc, 0) == 0 &&
           liballoc(&f.proc, 32, 2) == 0;
  start2 = f.mm.symrgtbl[2].rg_start;

  check(result && start0 == 0 && start1 == 64 && start2 == start0 &&
            f.mm.symrgtbl[2].rg_end == start0 + 32,
        "allocation, free, and reuse return the freed address range");
  destroy_fixture(&f);
}

static void test_invalid_and_double_free(void)
{
  struct fixture f;
  int allocated, first_free, double_free, negative_index, high_index;

  init_fixture(&f, 4096);
  allocated = liballoc(&f.proc, 16, 4) == 0;
  first_free = libfree(&f.proc, 4);
  double_free = libfree(&f.proc, 4);
  negative_index = libfree(&f.proc, (uint32_t)-1);
  high_index = libfree(&f.proc, PAGING_MAX_SYMTBL_SZ + 1U);

  check(allocated && first_free == 0 && double_free == -1 &&
            negative_index == -1 && high_index == -1,
        "double free and invalid region indices are rejected");
  destroy_fixture(&f);
}

static void test_read_write(void)
{
  struct fixture f;
  uint32_t value0 = 0, value1 = 0;
  int operations;

  init_fixture(&f, 256);
  pte_set_fpn(&f.mm, 0, 1, &f.proc);
  operations = liballoc(&f.proc, 32, 0) == 0 &&
               libwrite(&f.proc, 0x23, 0, 3) == 0 &&
               libwrite(&f.proc, 0x7E, 0, 17) == 0 &&
               libread(&f.proc, 0, 3, &value0) == 0 &&
               libread(&f.proc, 0, 17, &value1) == 0 &&
               value0 == 0x23 && value1 == 0x7E;

  check(operations, "libread/libwrite preserve bytes at distinct offsets");
  destroy_fixture(&f);
}

static void test_unallocated_access(void)
{
  struct fixture f;
  uint32_t readback = 0;
  int unallocated_read, unallocated_write, freed_read, freed_write;

  init_fixture(&f, 256);
  pte_set_fpn(&f.mm, 0, 1, &f.proc);
  unallocated_read = libread(&f.proc, 2, 0, &readback);
  unallocated_write = libwrite(&f.proc, 0x33, 2, 0);
  liballoc(&f.proc, 16, 1);
  libfree(&f.proc, 1);
  freed_read = libread(&f.proc, 1, 0, &readback);
  freed_write = libwrite(&f.proc, 0x44, 1, 0);

  check(unallocated_read == -1 && unallocated_write == -1 &&
            freed_read == -1 && freed_write == -1,
        "reads and writes to never-allocated or freed regions are rejected");
  if (unallocated_read != -1 || unallocated_write != -1 ||
      freed_read != -1 || freed_write != -1)
    printf("    observed returns: unallocated read/write=%d/%d, freed read/write=%d/%d\n",
           unallocated_read, unallocated_write, freed_read, freed_write);
  destroy_fixture(&f);
}

static void test_fifo_victim(void)
{
  struct fixture f;
  addr_t victim = (addr_t)-1;

  init_fixture(&f, 256);
  enlist_pgn_node(&f.mm.fifo_pgn, 1);
  enlist_pgn_node(&f.mm.fifo_pgn, 2);
  enlist_pgn_node(&f.mm.fifo_pgn, 3);
  check(find_victim_page(&f.mm, &victim) == 0 && victim == 1,
        "FIFO victim selection removes the oldest page");
  destroy_fixture(&f);
}

int main(void)
{
  printf("=== LIBMEM TESTS ===\n");
  test_alloc_free_reuse();
  test_invalid_and_double_free();
  test_read_write();
  test_unallocated_access();
  test_fifo_victim();
  printf("=== LIBMEM RESULT: %d failure(s) ===\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}