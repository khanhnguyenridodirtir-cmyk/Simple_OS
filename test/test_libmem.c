#include "libmem.h"
#include "mm64.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE PAGING64_PAGESZ
extern int pg_getpage(struct mm_struct *, int, int *, struct pcb_t *);
struct fixture { struct mm_struct mm; struct pcb_t proc; struct krnl_t krnl; struct queue_t running; struct memphy_struct ram, swap; };
static int total, failed;

static void check(int ok, const char *name)
{
  ++total;
  printf("%02d. [%s] %s\n", total, ok ? "PASS" : "FAIL", name);
  if (!ok) ++failed;
}

static int fixture_init(struct fixture *f, int ram_frames, int swap_frames, addr_t free_bytes)
{
  memset(f, 0, sizeof(*f));
  if (init_mm(&f->mm, &f->proc) < 0) return -1;
  free(f->mm.mmap->vm_freerg_list);
  f->mm.mmap->vm_freerg_list = init_vm_rg(0, free_bytes);
  f->proc.mm = &f->mm;
  f->proc.pid = 1;
  f->proc.krnl = &f->krnl;
  f->krnl.running_list = &f->running;
  f->running.size = 1;
  f->running.proc[0] = &f->proc;
  f->krnl.mram = &f->ram;
  f->krnl.active_mswp = &f->swap;
  if (init_memphy(&f->ram, (addr_t)ram_frames * PAGE, 1) < 0) return -1;
  if (init_memphy(&f->swap, (addr_t)swap_frames * PAGE, 1) < 0) return -1;
  return 0;
}

static void fixture_destroy(struct fixture *f)
{
  MEMPHY_destroy(&f->ram);
  MEMPHY_destroy(&f->swap);
  struct vm_rg_struct *r = f->mm.mmap ? f->mm.mmap->vm_freerg_list : NULL;
  while (r) { struct vm_rg_struct *n = r->rg_next; free(r); r = n; }
  if (f->mm.mmap) free(f->mm.mmap);
  free(f->mm.pgd);
  struct pgn_t *p = f->mm.fifo_pgn;
  while (p) { struct pgn_t *n = p->pg_next; free(p); p = n; }
}

static int alloc_one(struct fixture *f, uint32_t id, addr_t size)
{ return liballoc(&f->proc, size, id); }

int main(void)
{
  struct fixture f; uint32_t out = 0; addr_t addr = 999; int rc;
  printf("========== LIBMEM 30 TESTS (MM64) ==========\n");

  /* 01-10: allocation/free and symbol table */
  fixture_init(&f, 4, 4, PAGE * 8);
  rc = alloc_one(&f, 0, 64); check(rc == 0 && f.mm.symrgtbl[0].rg_end - f.mm.symrgtbl[0].rg_start == 64, "T01 allocate a valid region");
  check(liballoc(&f.proc, 0, 1) == -1, "T02 reject zero-sized allocation");
  check(liballoc(NULL, 8, 1) == -1, "T03 reject NULL process");
  check(liballoc(&f.proc, 8, PAGING_MAX_SYMTBL_SZ) == -1, "T04 reject symbol index out of range");
  check(liballoc(&f.proc, 8, 0) == -1, "T05 reject allocation into an occupied symbol slot");
  check(libfree(&f.proc, 0) == 0, "T06 free an allocated region");
  check(libfree(&f.proc, 1) == -1, "T07 reject freeing a never-allocated region");
  alloc_one(&f, 1, 32); rc = libfree(&f.proc, 1); check(rc == 0 && libfree(&f.proc, 1) == -1, "T08 reject double free");
  check(libfree(&f.proc, (uint32_t)-1) == -1, "T09 reject negative/out-of-range free index");
  alloc_one(&f, 2, 64); addr = f.mm.symrgtbl[2].rg_start; libfree(&f.proc, 2); alloc_one(&f, 3, 16);
  check(f.mm.symrgtbl[3].rg_start == addr, "T10 reuse the freed address range");
  fixture_destroy(&f);

  /* 11-20: read/write and access validation */
  fixture_init(&f, 4, 4, PAGE * 8); alloc_one(&f, 0, 32); pte_set_fpn(&f.mm, 0, 1, &f.proc);
  check(libwrite(&f.proc, 0x5A, 0, 0) == 0 && libread(&f.proc, 0, 0, &out) == 0 && out == 0x5A, "T11 write then read one byte at offset zero");
  rc = libwrite(&f.proc, 0x12, 0, 3); int r2 = libwrite(&f.proc, 0x67, 0, 17); int r3 = libread(&f.proc, 0, 3, &out); int v3 = (int)out; int r4 = libread(&f.proc, 0, 17, &out); check(rc == 0 && r2 == 0 && r3 == 0 && v3 == 0x12 && r4 == 0 && out == 0x67, "T12 preserve distinct values at distinct offsets");
  check(libread(&f.proc, 0, 32, &out) == -1, "T13 reject read exactly at region size");
  check(libwrite(&f.proc, 0xAA, 0, 32) == -1, "T14 reject write exactly at region size");
  check(libread(&f.proc, 0, 40, &out) == -1, "T15 reject read beyond region size");
  check(libwrite(&f.proc, 0xBB, 0, 40) == -1, "T16 reject write beyond region size");
  libfree(&f.proc, 0); check(libread(&f.proc, 0, 0, &out) == -1, "T17 reject read after free");
  check(libwrite(&f.proc, 1, 0, 0) == -1, "T18 reject write after free");
  check(libread(&f.proc, PAGING_MAX_SYMTBL_SZ, 0, &out) == -1, "T19 reject invalid read symbol index");
  alloc_one(&f, 2, 8); libwrite(&f.proc, 0xE7, 2, 0); out = 0; rc = libread(&f.proc, 2, 0, &out); check(rc == 0 && out == 0xE7, "T20 preserve byte values with the high bit set (0xE7)");
  fixture_destroy(&f);

  /* 21-27: page mapping, FIFO, swap-in/out, exhaustion */
  fixture_init(&f, 3, 3, PAGE * 8); alloc_one(&f, 0, PAGE + 16);
  check(libwrite(&f.proc, 0x21, 0, 0) == 0 && PAGING_PAGE_PRESENT(pte_get_entry(&f.mm, 0, &f.proc)), "T21 first user page is mapped into RAM on demand");
  check(libwrite(&f.proc, 0x22, 0, PAGE) == 0 && PAGING_PAGE_PRESENT(pte_get_entry(&f.mm, PAGE / PAGING_PAGESZ, &f.proc)), "T22 second virtual page is mapped independently");
  fixture_destroy(&f);

  fixture_init(&f, 1, 2, PAGE * 8); alloc_one(&f, 0, PAGE * 2);
  rc = libwrite(&f.proc, 0x31, 0, 0); rc |= libwrite(&f.proc, 0x42, 0, PAGE);
  { uint64_t p0 = pte_get_entry(&f.mm, 0, &f.proc); check(rc == 0 && !(p0 & PAGING_PTE_PRESENT_MASK) && (p0 & PAGING_PTE_SWAPPED_MASK), "T23 evict the oldest resident page to SWAP when RAM is full"); }
  check(libread(&f.proc, 0, 0, &out) == 0 && out == 0x31, "T24 swap a page back into RAM without losing its byte");
  fixture_destroy(&f);

  fixture_init(&f, 1, 1, PAGE * 8); alloc_one(&f, 0, PAGE * 3);
  rc = libwrite(&f.proc, 1, 0, 0); rc |= libwrite(&f.proc, 2, 0, PAGE);
  check(rc == 0 && libwrite(&f.proc, 3, 0, PAGE * 2) == -1, "T25 fail cleanly when RAM and the only SWAP frame are occupied");
  fixture_destroy(&f);

  fixture_init(&f, 2, 2, PAGE * 8); enlist_pgn_node(&f.mm.fifo_pgn, 5); enlist_pgn_node(&f.mm.fifo_pgn, 6); enlist_pgn_node(&f.mm.fifo_pgn, 7);
  { addr_t victim = (addr_t)-1; check(find_victim_page(&f.mm, &victim) == 0 && victim == 5, "T26 FIFO chooses the oldest queued page"); }
  { addr_t victim = 0; while (f.mm.fifo_pgn) { struct pgn_t *n=f.mm.fifo_pgn->pg_next; free(f.mm.fifo_pgn); f.mm.fifo_pgn=n; } check(find_victim_page(&f.mm, &victim) == -1, "T27 FIFO reports no victim when the queue is empty"); }
  fixture_destroy(&f);

  /* 28-30: API argument and address-space failure cases */
  fixture_init(&f, 2, 2, PAGE * 8); alloc_one(&f, 0, 32); pte_set_fpn(&f.mm, 0, 1, &f.proc);
  check(libread(&f.proc, 0, 0, NULL) == -1, "T28 reject NULL read destination");
  check(libwrite(NULL, 0x11, 0, 0) == -1, "T29 reject NULL process on write");
  { int fpn = -1; check(pg_getpage(NULL, 0, &fpn, &f.proc) == -1, "T30 reject NULL page-table argument"); }
  fixture_destroy(&f);

  printf("========== RESULT: %d/%d PASS, %d FAIL ==========\n", total - failed, total, failed);
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
