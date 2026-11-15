/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include "queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#if defined(MM64)

#ifndef PAGING64_PGB_SZ
#define PAGING64_PGB_SZ 512
#endif

/*
 * init_pte - Initialize PTE entry
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn, // FPN
             int drt,    // dirty
             int swp,    // swap
             int swptyp, // swap type
             addr_t swpoff) // swap offset
{
  if (pre != 0) {
    if (swp == 0) { // Non swap ~ page online
      if (fpn == 0)
        return -1;  // Invalid setting

      /* Valid setting with FPN */
      SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
      CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
      CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

      SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
    }
    else
    { // page swapped
      SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
      SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
      CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

      SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
      SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
    }
  }

  return 0;
}


/*
 * get_pd_from_pagenum - Parse address to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Extract page direactories */
	*pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
	*p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
	*pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
	*pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
	*pt  = (addr&PAGING64_ADDR_PT_MASK) >>PAGING64_ADDR_PT_LOBIT;

	/* TODO: implement the page direactories mapping */

	return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}


/*
 * pte_set_swap - Set PTE entry for swapped page
 * @pte    : target page table entry (PTE)
 * @swptyp : swap type
 * @swpoff : swap offset
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);

  addr_t pgd=0, p4d=0, pud=0, pmd=0, pt=0;
	
  if (!mm || !mm->pt) return -1;

  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
  addr_t *pte = &mm->pt[pt];

  CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @pte   : target page table entry (PTE)
 * @fpn   : frame page number (FPN)
 * @caller: caller
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  struct krnl_t *krnl = (caller) ? caller->krnl : NULL;
  struct mm_struct *target_mm = mm ? mm : ((krnl && krnl->mm) ? krnl->mm : (caller ? caller->mm : NULL));
  addr_t pgd=0, p4d=0, pud=0, pmd=0, pt=0;

  if (!target_mm || !target_mm->pt) return -1;

  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
  addr_t *pte = &target_mm->pt[pt];

  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  return 0;
}


/* Get PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @caller: caller
 * @ret    : page table entry
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  struct krnl_t *krnl = (caller) ? caller->krnl : NULL;
  struct mm_struct *target_mm = mm ? mm : ((krnl && krnl->mm) ? krnl->mm : (caller ? caller->mm : NULL));
  addr_t pgd=0, p4d=0, pud=0, pmd=0, pt=0;

  if (!target_mm || !target_mm->pt) return 0;

  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
  return target_mm->pt[pt];
}

/* Set PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @ret    : page table entry
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  struct krnl_t *krnl = (caller) ? caller->krnl : NULL;
  struct mm_struct *target_mm = mm ? mm : ((krnl && krnl->mm) ? krnl->mm : (caller ? caller->mm : NULL));
  addr_t pgd=0, p4d=0, pud=0, pmd=0, pt=0;

  if (!target_mm || !target_mm->pt) return -1;

  get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
  target_mm->pt[pt] = pte_val;

  return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                    // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (!mm || !mm->pt) return -1;

  addr_t start_pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  uint64_t pattern = 0xdeadbeef;

  for (int pgit = 0; pgit < pgnum; pgit++) {
    addr_t pgd=0, p4d=0, pud=0, pmd=0, pt=0;
    get_pd_from_pagenum(start_pgn + pgit, &pgd, &p4d, &pud, &pmd, &pt);
    mm->pt[pt] = pattern;
  }

  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                   // no guarantee all given pages are mapped
  struct framephy_struct *fpit = frames;
  int pgit = 0;
  addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);

  if (ret_rg != NULL) {
    ret_rg->rg_start = addr;
    ret_rg->rg_end = addr + (addr_t)pgnum * PAGING64_PAGESZ;
    ret_rg->rg_next = NULL;
  }

  for (pgit = 0; pgit < pgnum && fpit != NULL; pgit++) {
    if (fpit->fpn == (addr_t)-1) {
      /* Trường hợp 0 khung RAM (test 1K): trang đã được cấp sẵn trên SWAP */
      addr_t swpfpn = 0;
      if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == 0) {
        pte_set_swap(caller, pgn + pgit, 0, swpfpn);
      }
    } else {
      pte_set_fpn(mm, pgn + pgit, fpit->fpn, caller);
      enlist_pgn_node(&mm->fifo_pgn, pgn + pgit);
    }
    fpit = fpit->fp_next;
  }

  if (ret_rg != NULL) {
    ret_rg->rg_end = addr + (addr_t)pgit * PAGING64_PAGESZ;
  }

  return 0;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  addr_t fpn = 0;
  int pgit;
  struct framephy_struct *head = NULL;
  struct framephy_struct *tail = NULL;
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  struct memphy_struct *mram = caller->krnl->mram;
  struct memphy_struct *mswp = caller->krnl->active_mswp;
  
  for (pgit = 0; pgit < req_pgnum; pgit++)
  {
    fpn = 0;
    if (MEMPHY_get_freefp(mram, &fpn) != 0)
    {
      addr_t vicpgn = 0, swpfpn = 0;
      struct pcb_t *vic_proc = caller;

      /* 1. Tìm trang nạn nhân trong chính tiến trình gọi */
      if (find_victim_page(mm, &vicpgn) < 0)
      {
        /* 2. Tìm trang nạn nhân ở các tiến trình khác đang chạy (Global Replacement) */
        vic_proc = NULL;
        if (caller->krnl && caller->krnl->running_list) {
          for (int i = 0; i < caller->krnl->running_list->size; i++) {
            struct pcb_t *p = caller->krnl->running_list->proc[i];
            if (p && p->mm && find_victim_page(p->mm, &vicpgn) == 0) {
              vic_proc = p;
              break;
            }
          }
        }
      }

      if (vic_proc != NULL)
      {
        addr_t vicpte = pte_get_entry(vic_proc->mm, vicpgn, vic_proc);
        addr_t vicfpn = PAGING_FPN(vicpte);

        if (MEMPHY_get_freefp(mswp, &swpfpn) != 0)
        {
          *frm_lst = head;
          return -3000;
        }

        __swap_cp_page(mram, vicfpn, mswp, swpfpn);
        pte_set_swap(vic_proc, vicpgn, 0, swpfpn);
        fpn = vicfpn;
      }
      else if (mram->maxsz < PAGING64_PAGESZ)
      {
        /* 3. RAM vật lý nhỏ hơn 1 trang (test 1K): đánh dấu cấp thẳng trên SWAP */
        fpn = (addr_t)-1;
      }
      else
      {
        *frm_lst = head;
        return -3000;
      }
    }

    struct framephy_struct *newfp = malloc(sizeof(struct framephy_struct));
    newfp->fpn = fpn;
    newfp->fp_next = NULL;

    if (head == NULL) {
      head = newfp;
      tail = newfp;
    } else {
      tail->fp_next = newfp;
      tail = newfp;
    }
  }

  *frm_lst = head;
  return 0;
}

int free_pcb_memph(struct pcb_t *caller)
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  struct memphy_struct *mram = caller->krnl->mram;
  struct memphy_struct *mswp = caller->krnl->active_mswp;
  struct pgn_t *pg = mm->fifo_pgn;

  while (pg != NULL)
  {
    addr_t pte = pte_get_entry(mm, pg->pgn, caller);
    if (PAGING_PAGE_PRESENT(pte))
    {
      MEMPHY_put_freefp(mram, PAGING_FPN(pte));
    }
    else
    {
      MEMPHY_put_freefp(mswp, PAGING_SWP(pte));
    }
    struct pgn_t *tmp = pg;
    pg = pg->pg_next;
    free(tmp);
  }
  mm->fifo_pgn = NULL;
  return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL;

  int ret_alloc = (int)alloc_pages_range(caller, incpgnum, &frm_lst);

  if (ret_alloc < 0)
  {
    /* Trả lại các khung đã cấp phát dở dang (nếu có) trước khi báo lỗi */
    while (frm_lst != NULL)
    {
      struct framephy_struct *tmp = frm_lst;
      MEMPHY_put_freefp(caller->krnl->mram, tmp->fpn);
      frm_lst = frm_lst->fp_next;
      free(tmp);
    }
    return -1;
  }

  vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);

  /* Giải phóng danh sách liên kết tạm sau khi đã ánh xạ xong vào PTE */
  while (frm_lst != NULL)
  {
    struct framephy_struct *tmp = frm_lst;
    frm_lst = frm_lst->fp_next;
    free(tmp);
  }
  
  return 0;
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  int cellidx;
  addr_t addrsrc, addrdst;

  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;

    BYTE data;
    MEMPHY_read(mpsrc, addrsrc, &data);
    MEMPHY_write(mpdst, addrdst, data);
  }

  return 0;
}

/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: mm owner
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  struct vm_area_struct *vma0 = malloc(sizeof(struct vm_area_struct));

  /* TODO init page table directory */
  mm->pgd = (uint64_t *)malloc(sizeof(uint64_t) * PAGING64_PGB_SZ);
  mm->p4d = (uint64_t *)malloc(sizeof(uint64_t) * PAGING64_PGB_SZ);
  mm->pud = (uint64_t *)malloc(sizeof(uint64_t) * PAGING64_PGB_SZ);
  mm->pmd = (uint64_t *)malloc(sizeof(uint64_t) * PAGING64_PGB_SZ);
  mm->pt  = (uint64_t *)malloc(sizeof(uint64_t) * PAGING64_PGB_SZ);

  for (int i = 0; i < PAGING64_PGB_SZ; i++) {
    mm->pgd[i] = 0;
    mm->p4d[i] = 0;
    mm->pud[i] = 0;
    mm->pmd[i] = 0;
    mm->pt[i]  = 0;
  }

  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  vma0->vm_next = NULL;
  vma0->vm_freerg_list = NULL;

  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  vma0->vm_mm = mm; 
  mm->mmap = vma0;
  mm->fifo_pgn = NULL;
  mm->kcpooltbl = NULL;

  return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;
  if (pg == NULL)
    return -1;

  /* Trường hợp danh sách chỉ có 1 phần tử */
  if (pg->pg_next == NULL)
  {
    *retpgn = pg->pgn;
    mm->fifo_pgn = NULL;
    free(pg);
    return 0;
  }

  /* Duyệt đến phần tử cuối cùng (cũ nhất) */
  struct pgn_t *prev = NULL;
  while (pg->pg_next != NULL)
  {
    prev = pg;
    pg = pg->pg_next;
  }

  *retpgn = pg->pgn;
  prev->pg_next = NULL;
  free(pg);

  return 0;
}

int print_list_fp(struct framephy_struct *ifp)
{
  struct framephy_struct *fp = ifp;

  printf("print_list_fp: ");
  if (fp == NULL) { printf("NULL list\n"); return -1;}
  printf("\n");
  while (fp != NULL)
  {
    printf("fp[" FORMAT_ADDR "]\n", fp->fpn);
    fp = fp->fp_next;
  }
  printf("\n");
  return 0;
}

int print_list_rg(struct vm_rg_struct *irg)
{
  struct vm_rg_struct *rg = irg;

  printf("print_list_rg: ");
  if (rg == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (rg != NULL)
  {
    printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "]\n", rg->rg_start, rg->rg_end);
    rg = rg->rg_next;
  }
  printf("\n");
  return 0;
}

int print_list_vma(struct vm_area_struct *ivma)
{
  struct vm_area_struct *vma = ivma;

  printf("print_list_vma: ");
  if (vma == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (vma != NULL)
  {
    printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "]\n", vma->vm_start, vma->vm_end);
    vma = vma->vm_next;
  }
  printf("\n");
  return 0;
}

int print_list_pgn(struct pgn_t *ip)
{
  printf("print_list_pgn: ");
  if (ip == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (ip != NULL)
  {
    printf("va[" FORMAT_ADDR "]-\n", ip->pgn);
    ip = ip->pg_next;
  }
  printf("\n");
  return 0;
}

int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (mm == NULL)
    return -1;

  printf(" PDG=%lx P4g=%lx PUD=%lx PMD=%lx\n",
         (addr_t)mm->pgd, (addr_t)mm->p4d, (addr_t)mm->pud, (addr_t)mm->pmd);

  return 0;
}

#endif  //def MM64
