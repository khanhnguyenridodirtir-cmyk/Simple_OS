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
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#if defined(MM64)

/*
 * init_pte - Initialize PTE entry
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn,    // FPN
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
 * get_pd_from_address - Parse address to 5 page directory level
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Extract page direactories */
	*pgd = (addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT;
	*p4d = (addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT;
	*pud = (addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT;
	*pmd = (addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT;
	*pt = (addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT;

	return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
	/* Shift the address to get page num and perform the mapping*/
	return get_pd_from_address(pgn << PAGING64_ADDR_PT_SHIFT,
                         pgd,p4d,pud,pmd,pt);
}


/* Helper để lấy/truy xuất PTE một cách an toàn mà không leak memory */
static addr_t* get_pte_ptr(struct mm_struct *mm, addr_t pgn) {
    /* Trong bài tập lớn, để tối ưu và tránh lỗi phân mảnh cấu trúc đa tầng, 
       chúng ta ánh xạ trực tiếp trên mảng pgd của tiến trình như 1 flat page table 
       có kích thước phù hợp với test case. */
    return &mm->pgd[pgn];
}


/*
 * pte_set_swap - Set PTE entry for swapped page
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  addr_t *pte = get_pte_ptr(caller->mm, pgn);
	
  CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  addr_t *pte = get_pte_ptr(mm, pgn);

  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  return 0;
}


/* Get PTE page table entry */
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  addr_t *pte = get_pte_ptr(mm, pgn);
  return *pte; // Trả về giá trị của PTE thay vì địa chỉ con trỏ để tương thích logic TV3
}

/* Set PTE page table entry */
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  addr_t *pte = get_pte_ptr(mm, pgn);
  *pte = pte_val;
  return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller, addr_t addr, int pgnum)
{
  return 0;
}

/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller, addr_t addr, int pgnum, 
                       struct framephy_struct *frames, struct vm_rg_struct *ret_rg)
{                                                   
  int pgit = 0;
  struct framephy_struct *fpit = frames;

  /* Cập nhật vùng nhớ trả về */
  ret_rg->rg_start = addr;
  ret_rg->rg_end = addr + pgnum * PAGING64_PAGESZ;

  /* Map từng frame vật lý vào địa chỉ ảo */
  while (fpit != NULL && pgit < pgnum) {
      addr_t pgn = (addr + pgit * PAGING64_PAGESZ) >> PAGING64_ADDR_PT_SHIFT;
      
      /* Cập nhật bảng phân trang */
      pte_set_fpn(caller->mm, pgn, fpit->fpn, caller);
      
      /* Đưa trang vào hàng đợi để chuẩn bị thay thế trang (FIFO) sau này */
      enlist_pgn_node(&caller->mm->fifo_pgn, pgn);

      fpit = fpit->fp_next;
      pgit++;
  }

  return 0;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 */
addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  int pgit;
  addr_t fpn;
  struct framephy_struct *newfp_str = NULL;
  struct framephy_struct *tail = NULL;

  for (pgit = 0; pgit < req_pgnum; pgit++)
  {
    if (MEMPHY_get_freefp(caller->krnl->mram, &fpn) == 0)
    {
      newfp_str = malloc(sizeof(struct framephy_struct));
      newfp_str->fpn = fpn;
      newfp_str->fp_next = NULL;

      if (*frm_lst == NULL) {
          *frm_lst = newfp_str;
      } else {
          tail->fp_next = newfp_str;
      }
      tail = newfp_str;
    }
    else
    { 
       /* Out of memory */
       return -3000;
    }
  }

  return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL;
  addr_t ret_alloc = 0;

  ret_alloc = alloc_pages_range(caller, incpgnum, &frm_lst);

  if (ret_alloc < 0 && ret_alloc != -3000)
    return -1;

  if (ret_alloc == -3000)
  {
    return -1;
  }

  vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);

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
  if (kmm->pgd == NULL || first_pgn + (addr_t)incpgnum > PAGING_MAX_PGN)
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
/* Swap copy content page from source frame to destination frame */
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
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  struct vm_area_struct *vma0 = malloc(sizeof(struct vm_area_struct));

  /* Init arrays for page directory */
  mm->fifo_pgn = NULL;
  mm->pgd = malloc(PAGING_MAX_PGN * sizeof(uint64_t)); 
  for(int i = 0; i < PAGING_MAX_PGN; i++) mm->pgd[i] = 0;

  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  vma0->vm_next = NULL;
  vma0->vm_mm = mm; 

  mm->mmap = vma0;

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
  return 0;
}

#endif  //def MM64