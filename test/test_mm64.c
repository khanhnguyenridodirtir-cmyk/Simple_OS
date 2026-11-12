#include "mm64.h"
#include <stdio.h>
#include <stdlib.h>

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

static void test_init_mm(void)
{
  struct mm_struct mm = {0};
  int result = init_mm(&mm, NULL);
  int zeroed = result == 0 && mm.pgd != NULL;

  if (zeroed)
    for (int i = 0; i < PAGING_MAX_PGN; i++)
      if (mm.pgd[i] != 0)
      {
        zeroed = 0;
        break;
      }

  check(zeroed && mm.mmap != NULL && mm.mmap->vm_start == 0 &&
            mm.mmap->vm_end == 0 && mm.mmap->sbrk == 0,
        "init_mm allocates a zeroed page table and an empty vma0");
  free(mm.pgd);
  if (mm.mmap != NULL)
  {
    free(mm.mmap->vm_freerg_list);
    free(mm.mmap);
  }
}

static void test_address_split(void)
{
  const addr_t address = (0x12ULL << 48) | (0x34ULL << 39) |
                         (0x56ULL << 30) | (0x78ULL << 21) |
                         (0x9AULL << 12) | 0xABCULL;
  addr_t pgd, p4d, pud, pmd, pt;

  check(get_pd_from_address(address, &pgd, &p4d, &pud, &pmd, &pt) == 0 &&
            pgd == 0x12 && p4d == 0x34 && pud == 0x56 &&
            pmd == 0x78 && pt == 0x9A,
        "get_pd_from_address extracts all five 9-bit indices");
}

static void test_pte_online_and_swap(void)
{
  struct mm_struct mm = {0};
  struct pcb_t caller = {0};
  int online;
  int swapped;

  init_mm(&mm, &caller);
  caller.mm = &mm;
  pte_set_fpn(&mm, 2, 5, &caller);
  addr_t pte = pte_get_entry(&mm, 2, &caller);
  online = (pte & PAGING_PTE_PRESENT_MASK) != 0 &&
           (pte & PAGING_PTE_SWAPPED_MASK) == 0 && PAGING_FPN(pte) == 5;

  pte_set_swap(&caller, 2, 0, 3);
  pte = pte_get_entry(&mm, 2, &caller);
  swapped = (pte & PAGING_PTE_PRESENT_MASK) == 0 &&
            (pte & PAGING_PTE_SWAPPED_MASK) != 0 && PAGING_SWP(pte) == 3;
  check(online && swapped,
        "PTE transitions from an online frame to a swapped page");

  free(mm.pgd);
  free(mm.mmap->vm_freerg_list);
  free(mm.mmap);
}

static void test_swap_copy(void)
{
  struct memphy_struct ram = {0};
  struct memphy_struct swap = {0};
  int identical = 1;

  init_memphy(&ram, 3 * PAGING64_PAGESZ, 1);
  init_memphy(&swap, 3 * PAGING64_PAGESZ, 1);
  for (addr_t i = 0; i < PAGING64_PAGESZ; i++)
    MEMPHY_write(&ram, PAGING64_PAGESZ + i, (BYTE)(i * 37 + 11));

  __swap_cp_page(&ram, 1, &swap, 2);
  for (addr_t i = 0; i < PAGING64_PAGESZ; i++)
  {
    BYTE source, destination;
    MEMPHY_read(&ram, PAGING64_PAGESZ + i, &source);
    MEMPHY_read(&swap, 2 * PAGING64_PAGESZ + i, &destination);
    if (source != destination)
    {
      identical = 0;
      break;
    }
  }

  check(identical, "swap copy preserves every byte in a 4KB page");
  release_memphy(&ram);
  release_memphy(&swap);
}

int main(void)
{
  printf("=== MM64 TESTS ===\n");
  test_init_mm();
  test_address_split();
  test_pte_online_and_swap();
  test_swap_copy();
  printf("=== MM64 RESULT: %d failure(s) ===\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}