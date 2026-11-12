#include "libmem.h"
#include "mm64.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int pg_getpage(struct mm_struct *mm, int pgn, int *fpn,
                      struct pcb_t *caller);

struct fixture
{
  struct mm_struct mm;
  struct pcb_t proc;
  struct krnl_t krnl;
  struct memphy_struct ram;
  struct memphy_struct swap;
};

struct worker_args
{
  struct pcb_t *proc;
  uint32_t reg_index;
  int thread_index;
  int failures;
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

static void init_fixture(struct fixture *f, int ram_frames,
                         int swap_frames, addr_t free_end)
{
  memset(f, 0, sizeof(*f));
  init_mm(&f->mm, &f->proc);
  free(f->mm.mmap->vm_freerg_list);
  f->mm.mmap->vm_freerg_list = init_vm_rg(0, free_end);
  f->proc.mm = &f->mm;
  f->proc.krnl = &f->krnl;
  f->krnl.mram = &f->ram;
  f->krnl.active_mswp = &f->swap;
  init_memphy(&f->ram, ram_frames * PAGING64_PAGESZ, 1);
  init_memphy(&f->swap, swap_frames * PAGING64_PAGESZ, 1);
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

static void test_swap_stress(void)
{
  struct fixture f;
  uint32_t readback = 0;
  const BYTE expected = 0x31;
  int allocations, writes;

  init_fixture(&f, 2, 4, 4096);
  allocations = liballoc(&f.proc, 256, 0) == 0 &&
                liballoc(&f.proc, 256, 1) == 0 &&
                liballoc(&f.proc, 256, 2) == 0;
  writes = libwrite(&f.proc, 0x31, 0, 0) == 0 &&
           libwrite(&f.proc, 0x42, 1, 0) == 0 &&
           libwrite(&f.proc, 0x53, 2, 0) == 0;

  addr_t first_pte = pte_get_entry(&f.mm, 0, &f.proc);
  int page_one_swapped = !(first_pte & PAGING_PTE_PRESENT_MASK) &&
                         (first_pte & PAGING_PTE_SWAPPED_MASK);
  int read_result = libread(&f.proc, 0, 0, &readback);
  check(allocations && writes && page_one_swapped && read_result == 0 &&
            readback == (uint32_t)(unsigned char)expected,
        "three pages exceed 2 RAM frames; page 1 swaps out and reloads intact");
  if (!page_one_swapped || read_result != 0 ||
      readback != (uint32_t)(unsigned char)expected)
    printf("    observed: swapped=%d, libread=%d, byte=0x%02x (expected 0x31)\n",
           page_one_swapped, read_result, readback & 0xFF);
  destroy_fixture(&f);
}

static void test_swap_exhaustion(void)
{
  struct fixture f;
  int allocated = 1;
  int writes = 1;
  int sixth_page_frame = -1;
  int seventh_page_frame = -1;

  init_fixture(&f, 2, 4, 4096);
  for (uint32_t region = 0; region < 7; region++)
    if (liballoc(&f.proc, 256, region) != 0)
      allocated = 0;

  for (uint32_t region = 0; region < 6; region++)
  {
    if (libwrite(&f.proc, (BYTE)(region + 1), region, 0) != 0)
      writes = 0;
  }

    int sixth_page_result = pg_getpage(&f.mm, 5, &sixth_page_frame, &f.proc);
    int seventh_page_result = pg_getpage(&f.mm, 6, &seventh_page_frame, &f.proc);
    check(allocated && writes && sixth_page_result == 0 && seventh_page_result == -1,
        "pg_getpage returns -1 after RAM and all 4 swap frames are exhausted");
    if (!allocated || !writes || sixth_page_result != 0 || seventh_page_result != -1)
  {
    int ram_free = 0, swap_free = 0;
    for (struct framephy_struct *node = f.ram.free_fp_list; node != NULL; node = node->fp_next)
      ram_free++;
    for (struct framephy_struct *node = f.swap.free_fp_list; node != NULL; node = node->fp_next)
      swap_free++;
        printf("    observed: allocations=%d, writes=%d, page6=%d/fpn%d, page7=%d/fpn%d\n",
          allocated, writes, sixth_page_result, sixth_page_frame,
          seventh_page_result, seventh_page_frame);
    printf("    free frames: RAM=%d, SWAP=%d; PTE[0..6]:", ram_free, swap_free);
    for (int pgn = 0; pgn <= 6; pgn++)
      printf(" %d:%llx", pgn, (unsigned long long)f.mm.pgd[pgn]);
    printf("\n");
  }
  destroy_fixture(&f);
}

static void *allocation_worker(void *opaque)
{
  struct worker_args *args = opaque;
  for (int i = 0; i < 10; i++)
  {
    BYTE expected = (BYTE)(args->thread_index * 30 + i % 30 + 1);
    uint32_t readback = 0;
    if (liballoc(args->proc, 8, args->reg_index) != 0 ||
        libwrite(args->proc, expected, args->reg_index, 0) != 0 ||
        libread(args->proc, args->reg_index, 0, &readback) != 0 ||
        readback != (uint32_t)(unsigned char)expected ||
        libfree(args->proc, args->reg_index) != 0)
      args->failures++;
  }
  return NULL;
}

static int free_list_is_sound(struct vm_rg_struct *head)
{
  struct vm_rg_struct *slow = head;
  struct vm_rg_struct *fast = head;
  for (struct vm_rg_struct *node = head; node != NULL; node = node->rg_next)
    if (node->rg_start > node->rg_end)
      return 0;

  while (fast != NULL && fast->rg_next != NULL)
  {
    slow = slow->rg_next;
    fast = fast->rg_next->rg_next;
    if (slow == fast)
      return 0;
  }
  return 1;
}

static void test_concurrent_alloc_free(void)
{
  struct fixture f;
  pthread_t threads[2];
  struct worker_args args[2];
  int created[2] = {0, 0};
  int thread_checks;

  init_fixture(&f, 2, 4, 4096);
  pte_set_fpn(&f.mm, 0, 0, &f.proc);
  for (int i = 0; i < 2; i++)
  {
    args[i].proc = &f.proc;
    args[i].reg_index = (uint32_t)i;
    args[i].thread_index = i;
    args[i].failures = 0;
    created[i] = pthread_create(&threads[i], NULL, allocation_worker, &args[i]) == 0;
  }
  for (int i = 0; i < 2; i++)
    if (created[i])
      pthread_join(threads[i], NULL);

  thread_checks = created[0] && created[1] && args[0].failures == 0 &&
                  args[1].failures == 0 && free_list_is_sound(f.mm.mmap->vm_freerg_list) &&
                  f.mm.symrgtbl[0].rg_start == 0 && f.mm.symrgtbl[0].rg_end == 0 &&
                  f.mm.symrgtbl[1].rg_start == 0 && f.mm.symrgtbl[1].rg_end == 0;
  check(thread_checks,
        "two threads repeatedly allocate, write, read, and free without list corruption");
  destroy_fixture(&f);
}

int main(void)
{
  printf("=== INTEGRATION MEMORY TESTS ===\n");
  test_swap_stress();
  test_swap_exhaustion();
  test_concurrent_alloc_free();
  printf("=== INTEGRATION RESULT: %d failure(s) ===\n", failures);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}