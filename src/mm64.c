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

#include "mm64.h"   // Quan ly co che phan trang va khung vat ly
#include "queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h> // De dung kieu uintptr_t an toan khi ep kieu dia chi bang trang

#if defined(MM64)

#ifndef PAGING64_PGB_SZ
// So luong entry trong moi bang phan trang
#define PAGING64_PGB_SZ 512   
#endif
// Danh dau mot trang dang trong trang thai cho bi day ra Swap
#define PENDING_SWP_FLAG (1ULL << 63)   
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
  if(pte==NULL) return -1;
  *pte=0;
  if (swp != 0) {
    // Page nam tre  SWAP 
    CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
    SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
    if (drt) SETBIT(*pte, PAGING_PTE_DIRTY_MASK);

    SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
    SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);
  } else if (pre != 0) {
    // Trang nam tren RAM 
    SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
    CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);
    if (drt) SETBIT(*pte, PAGING_PTE_DIRTY_MASK);

    SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);
  }

  return 0;
}

/*
 * get_pd_from_address - Parse address to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
  if (pgd == NULL || p4d == NULL || pud == NULL || pmd == NULL || pt == NULL)
		return -1;

	/* Tu choi dia chi ao nam ngoai khong gian 57-bit (2^57) */
	if (addr >= ((addr_t)1ULL << 57))
		return -1;

	/* Extract page direactories */
	*pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
	*p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
	*pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
	*pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
	*pt  = (addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT;

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
  /* Tu choi so hieu trang vuot qua 2^45 (tranh tran bit khi dich trai 12 bit) */
	if (pgn >= PAGING64_MAX_PGN)
		return -1;

	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}

/* Ham phu tro lay hoac cap phat bang phan trang tang ke tiep */
static uint64_t* next_pgtbl(uint64_t *tbl, addr_t idx, int alloc)
{
  if (tbl[idx] == 0) {
    if (!alloc) return NULL;
    uint64_t *new_tbl = (uint64_t *)calloc(PAGING64_PGB_SZ, sizeof(uint64_t));
    if (new_tbl == NULL) return NULL;
    tbl[idx] = (uint64_t)(uintptr_t)new_tbl;
  }
  return (uint64_t *)(uintptr_t)tbl[idx];
}

/* Duyet du 5 cap PGD -> P4D -> PUD -> PMD -> PT de lay con tro PTE
 * alloc = 1: tu dong cap phat bang con neu chua ton tai (dung cho ham set/map)
 * alloc = 0: chi doc, tra ve NULL neu chua duoc map (dung cho ham get)
 */
static addr_t* get_pte_5level(struct mm_struct *mm, addr_t pgn, int alloc)
{
  if (mm == NULL || mm->pgd == NULL) return NULL;

  addr_t pgd = 0, p4d = 0, pud = 0, pmd = 0, pt = 0;
  if (get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt) < 0)
    return NULL;

  uint64_t *p4d_tbl = next_pgtbl(mm->pgd, pgd, alloc);
  if (p4d_tbl == NULL) return NULL;

  uint64_t *pud_tbl = next_pgtbl(p4d_tbl, p4d, alloc);
  if (pud_tbl == NULL) return NULL;

  uint64_t *pmd_tbl = next_pgtbl(pud_tbl, pud, alloc);
  if (pmd_tbl == NULL) return NULL;

  uint64_t *pt_tbl  = next_pgtbl(pmd_tbl, pmd, alloc);
  if (pt_tbl == NULL) return NULL;

  /* Cap nhat con tro tham chieu de print_pgtbl hien thi dung dia chi cac tang */
  mm->p4d = p4d_tbl;
  mm->pud = pud_tbl;
  mm->pmd = pmd_tbl;
  mm->pt  = pt_tbl;

  return &pt_tbl[pt];
}

/*
 * pte_set_swap - Set PTE entry for swapped page
 * @pte    : target page table entry (PTE)
 * @swptyp : swap type
 * @swpoff : swap offset
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  // Lay con tro mm tu caller (an toan cho User va Kernel)
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (mm == NULL) return -1;

  addr_t *pte = NULL;

#ifdef MM64    
  pte = get_pte_5level(mm, pgn, 1);
  if (pte == NULL) return -1;
#else
  if (mm->pgd == NULL) return -1;
  pte = &mm->pgd[pgn];
#endif

  CLRBIT(*pte, PAGING_PTE_PRESENT_MASK); // Xoa bit present khi day ra khoi swap
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);
  
  CLRBIT(*pte, PAGING_PTE_DIRTY_MASK);

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
  // Uu tien mm truyen vao -> caller->mm (User) -> krnl->mm (Kernel)
  mm = mm ? mm : ((caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL));
  if (mm == NULL) return -1;
  
  addr_t *pte = NULL;

#ifdef MM64    
  pte = get_pte_5level(mm, pgn, 1);
  if (pte == NULL) return -1;
#else
  if (mm->pgd == NULL) return -1;
  pte = &mm->pgd[pgn];
#endif

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
  // Uu tien mm truyen vao -> caller->mm (User) -> krnl->mm (Kernel)
  mm = mm ? mm : ((caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL));
  if (mm == NULL) return 0;
  
  addr_t *pte = NULL;

#ifdef MM64    
  pte = get_pte_5level(mm, pgn, 0);
  if (pte == NULL) return 0;
#else
  if (mm->pgd == NULL) return 0;
  pte = &mm->pgd[pgn];
#endif
  
  return *pte;
}

/* Set PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @ret    : page table entry
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  mm = mm ? mm : ((caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL));
  if (mm == NULL) return -1;
  
  addr_t *pte = NULL;

#ifdef MM64    
  pte = get_pte_5level(mm, pgn, 1);
  if (pte == NULL) return -1;
#else
  if (mm->pgd == NULL) return -1;
  pte = &mm->pgd[pgn];
#endif

  // Ghi de toan bo pte_val vao vung nho ma con tro pte dang giu
  *pte = pte_val;

  return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                    // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  // 1. Lay mm tu caller an toan
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (mm == NULL) return -1;
  
  // 2. Tinh so trang bat dau (start_pgn) tu dia chi ao (addr)
  addr_t start_pgn = addr >> PAGING64_ADDR_PT_SHIFT;

  // 3. Lap qua pgnum trang và set pattern
  uint64_t pattern = 0xdeadbeef;
  for (int pgit = 0; pgit < pgnum; pgit++) {
    addr_t *pte = get_pte_5level(mm, start_pgn + pgit, 1);
    if (pte == NULL) return -1;
    *pte = pattern;
  }

  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller,        // process call
                    addr_t addr,                    // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                   // no guarantee all given
  struct framephy_struct *fpit = frames;
  int pgit = 0;
  addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);

  if (ret_rg != NULL) {
    ret_rg->rg_start = addr;
    ret_rg->rg_end = addr + (addr_t)pgnum * PAGING64_PAGESZ;
    ret_rg->rg_next = NULL;
  }

  // B1 : Anh xa tung trang vao PTE va ghi nhan vao danh sach quan ly trang fifo_pgn
  for (pgit = 0; pgit < pgnum && fpit != NULL; pgit++) {
    if (fpit->fpn == (addr_t)-1) {
      // B1.1 : Truong hop RAM < 4KB (test 1K) -> xin frame SWAP va dua vao fifo_pgn
      addr_t swpfpn = 0;
      if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == 0) {
        pte_set_swap(caller, pgn + pgit, 0, swpfpn);
       // enlist_pgn_node(&mm->fifo_pgn, pgn + pgit);
      }
    } else if (fpit->fpn & PENDING_SWP_FLAG) {
      // B1.2 : Trang da duoc alloc_pages_range day san ra SWAP -> gan PTE SWAP va dua vao fifo_pgn
      addr_t swpfpn = fpit->fpn & ~PENDING_SWP_FLAG;
      pte_set_swap(caller, pgn + pgit, 0, swpfpn);
     // enlist_pgn_node(&mm->fifo_pgn, pgn + pgit);
    } else {
      // B1.3 : Trang nam tren RAM -> gan FPN va dua vao fifo_pgn
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
  
  // B1 : Duyet qua tung trang can cap phat trong req_pgnum
  for (pgit = 0; pgit < req_pgnum; pgit++)
  {
    fpn = 0;
    // B2 : Uu tien xin frame trong truc tiep tu RAM
    if (MEMPHY_get_freefp(mram, &fpn) != 0)
    {
      addr_t vicpgn = 0, swpfpn = 0;
      struct pcb_t *vic_proc = caller;

      // B3 : Neu het RAM, tim trang nan nhan (victim) trong FIFO cua tien trinh hien tai
      /* 1. Tìm trang nạn nhân trong chính tiến trình gọi */
      if (find_victim_page(mm, &vicpgn) < 0)
      {
        // B3.1 : Tim victim o cac tien trinh khac trong running_list
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
        // B4 : Day trang victim da map tu RAM sang SWAP de lay lai vicfpn
        addr_t vicpte = pte_get_entry(vic_proc->mm, vicpgn, vic_proc);
        addr_t vicfpn = PAGING_FPN(vicpte);

        if (mswp == NULL || MEMPHY_get_freefp(mswp, &swpfpn) != 0)
        {
          enlist_pgn_node(&vic_proc->mm->fifo_pgn, vicpgn);
          *frm_lst = head;
          return -3000;
        }

        __swap_cp_page(mram, vicfpn, mswp, swpfpn);
        pte_set_swap(vic_proc, vicpgn, 0, swpfpn);
        fpn = vicfpn;
      }
      else
      {
        // B5 : Truong hop cac trang vua xin o vong truoc chua map vao PTE -> day bot 1 frame trong head ra SWAP
        struct framephy_struct *pending_vic = head;
        while (pending_vic != NULL &&
               (pending_vic->fpn == (addr_t)-1 || (pending_vic->fpn & PENDING_SWP_FLAG)))
        {
          pending_vic = pending_vic->fp_next;
        }

        if (pending_vic != NULL && mswp != NULL && MEMPHY_get_freefp(mswp, &swpfpn) == 0)
        {
          // B5.1 : Chuyen frame dau tien trong head sang SWAP va giu cho swpfpn (khong tra lai free list)
          __swap_cp_page(mram, pending_vic->fpn, mswp, swpfpn);
          fpn = pending_vic->fpn;
          pending_vic->fpn = PENDING_SWP_FLAG | swpfpn;
        }
        else if (mram->maxsz < PAGING64_PAGESZ)
        {
          // B5.2 : RAM vat ly nho hon 1 trang (test 1K): danh dau cap thang tren SWAP
          /* 3. RAM vật lý nhỏ hơn 1 trang (test 1K): đánh dấu cấp thẳng trên SWAP */
          fpn = (addr_t)-1;
        }
        else
        {
          // B5.3 : Het ca RAM lan SWAP -> bao loi de vm_map_ram rollback
          *frm_lst = head;
          return -3000;
        }
      }
    }

    // B6 : Tao node frame moi va noi vao danh sach ket qua frm_lst
    struct framephy_struct *newfp = malloc(sizeof(struct framephy_struct));
    if (newfp == NULL)
    {
      if (fpn != (addr_t)-1 && !(fpn & PENDING_SWP_FLAG))
        MEMPHY_put_freefp(mram, fpn);
      *frm_lst = head;
      return -3000;
    }
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

  // B7 : Tra danh sach frame da cap phat ve cho vm_map_ram
  *frm_lst = head;

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
  int ret_alloc = 0;

  /*@bksysnet: author provides a feasible solution of getting frames
   *FATAL logic in here, wrong behaviour if we have not enough page
   *i.e. we request 1000 frames meanwhile our RAM has size of 3 frames
   *Don't try to perform that case in this simple work, it will result
   *in endless procedure of swap-off to get frame and we have not provide
   *duplicate control mechanism, keep it simple
   */

  ret_alloc = alloc_pages_range(caller, incpgnum, &frm_lst);

  if (ret_alloc < 0)
  {
    /* Tra lai cac khung da cap phat do dang (neu co) truoc khi bao loi */
    while (frm_lst != NULL)
    {
      struct framephy_struct *tmp = frm_lst;
      if (tmp->fpn & PENDING_SWP_FLAG) {
        /* Tra lai cho SWAP neu khung nay dang nam tren SWAP */
        addr_t swpfpn = tmp->fpn & ~PENDING_SWP_FLAG;
        MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
      } else if (tmp->fpn != (addr_t)-1) {
        /* Tra lai cho RAM neu day la khung RAM binh thuong */
        MEMPHY_put_freefp(caller->krnl->mram, tmp->fpn);
      }
      frm_lst = frm_lst->fp_next;
      free(tmp);
    }
    return -1;
  }

  /* it leaves the case of memory is enough but half in ram, half in swap
   * do the swaping all to swapper to get the all in ram */
  vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);

  // Giai phong danh sach lien ket tam sau khi da anh xa xong vao PTE
  while (frm_lst != NULL)
  {
    struct framephy_struct *tmp = frm_lst;
    frm_lst = frm_lst->fp_next;
    free(tmp);
  }

  return 0;
}

/*
 * vm_map_kernel - map kernel virtual pages to RAM without SWAP/FIFO
 */
addr_t vm_map_kernel(struct pcb_t *caller, addr_t astart, addr_t aend,
                     addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  // B1: Kiem tra Kernel MM, RAM va tham so mapping
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      caller->krnl->mram == NULL || ret_rg == NULL || incpgnum <= 0 ||
      mapstart % PAGING64_PAGESZ != 0)
    return -1;

  struct mm_struct *kmm = caller->krnl->mm;
  addr_t first_pgn = mapstart >> PAGING64_ADDR_PT_SHIFT;
  if (kmm->pgd == NULL || first_pgn + (addr_t)incpgnum > PAGING64_MAX_PGN)
    return -1;

  // B2: Cap phat truoc tat ca frame RAM; neu thieu thi rollback
  addr_t *fpns = malloc((size_t)incpgnum * sizeof(addr_t));
  if (fpns == NULL)
    return -1;

  int allocated = 0;
  for (; allocated < incpgnum; allocated++)
  {
    if (MEMPHY_get_freefp(caller->krnl->mram, &fpns[allocated]) < 0)
      break;
  }

  if (allocated != incpgnum)
  {
    // B2.1: Tra lai cac frame da lay neu khong du RAM cho Kernel
    for (int i = 0; i < allocated; i++)
      MEMPHY_put_freefp(caller->krnl->mram, fpns[i]);
    free(fpns);
    return -1;
  }

  // B3: Map tung frame vao Kernel page table, khong dua vao FIFO User
  for (int i = 0; i < incpgnum; i++)
  {
    addr_t pgn = first_pgn + (addr_t)i;
    if (pte_set_fpn(kmm, pgn, fpns[i], caller) < 0)
    {
      // B3.1: Rollback PTE va frame neu mapping gap loi
      for (int j = 0; j < i; j++)
        pte_set_entry(kmm, first_pgn + (addr_t)j, 0, caller);
      for (int j = 0; j < incpgnum; j++)
        MEMPHY_put_freefp(caller->krnl->mram, fpns[j]);
      free(fpns);
      return -1;
    }
  }

  // B4: Tra ve vung dia chi ao da duoc map
  ret_rg->rg_start = mapstart;
  ret_rg->rg_end = mapstart + (addr_t)incpgnum * PAGING64_PAGESZ;
  ret_rg->rg_next = NULL;
  free(fpns);
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

#ifdef MM64
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#else
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#endif

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
  if(mm==NULL ) return -1;

  
  /* init page table directory */
  mm->pgd = NULL;
  mm->p4d = NULL;
  mm->pud = NULL;
  mm->pmd = NULL;
  mm->pt  = NULL;
  mm->mmap = NULL;
  mm->kcpooltbl = NULL;
  mm->fifo_pgn = NULL;

  struct vm_area_struct *vma0 = malloc(sizeof(struct vm_area_struct));

  if(vma0==NULL) {
    return -1;
  }
   mm->pgd = (uint64_t *)calloc(PAGING64_PGB_SZ, sizeof(uint64_t));

   if (mm->pgd == NULL) {
    free(vma0);
    return -1;
  }
  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;

  // Khoi tao danh sach vung nho rong (free region list)
  vma0->vm_freerg_list = NULL;
  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
    if (first_rg == NULL) {
    free(mm->pgd);
    mm->pgd = NULL;
    free(vma0);
    return -1;
  }
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  /* update VMA0 next */
  vma0->vm_next = NULL;

  /* Point vma owner backward */
  vma0->vm_mm = mm; 

  /* update mmap */
  mm->mmap = vma0;
  mm->kcpooltbl = NULL;
  
  // Can thiet cho thuat toan doi trang
  mm->fifo_pgn = NULL;   

  return 0;
}

/* Ham de quy giai phong cay bang phan trang tu tang `level` tro xuong
 * level = 5: PGD -> level = 4: P4D -> level = 3: PUD -> level = 2: PMD -> level = 1: PT
 */
static void free_pgtbl_level(addr_t *table, int level)
{
  if (table == NULL) return;

  /* Neu chua phai tang la cuoi cung (PT - level 1), duyet 512 entry de giai phong bang con */
  if (level > 1) {
    for (int i = 0; i < PAGING64_PGB_SZ; i++) {
      if (table[i] != 0) {
        addr_t *child_tbl = (addr_t *)(uintptr_t)table[i];
        free_pgtbl_level(child_tbl, level - 1);
        table[i] = 0;
      }
    }
  }

  /* Giai phong chinh bang hien tai (sau khi da giai phong het cac bang con ben duoi) */
  free(table);
}

/* Giai phong toan bo he thong bang phan trang 5 cap cua tien trinh */
int mm_free_page_tables(struct mm_struct *mm)
{
  if (mm == NULL) return -1;

  if (mm->pgd != NULL) {
    free_pgtbl_level((addr_t *)mm->pgd, 5);
  }

  /* Dat tat ca con tro ve NULL de tranh loi dangling pointer */
  mm->pgd = NULL;
  mm->p4d = NULL;
  mm->pud = NULL;
  mm->pmd = NULL;
  mm->pt  = NULL;

  return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));
  // Them doan nay se nhin tuong minh hon mot chut
  if (rgnode == NULL) {
    return NULL;
  }
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
  if(plist==NULL) return -1;
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));
  if(pnode==NULL) {
    return -1;
  }
  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

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
  if (mm == NULL) return -1;

  printf(" PDG=%lx P4g=%lx PUD=%lx PMD=%lx\n",
         (addr_t)mm->pgd, (addr_t)mm->p4d, (addr_t)mm->pud, (addr_t)mm->pmd);

  return 0;
}

#endif  //def MM64
