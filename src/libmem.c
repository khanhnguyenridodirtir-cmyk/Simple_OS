/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
// Dua mot region moi vao free list
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  // B1  : validate du lieu
  if (mm == NULL || mm->mmap == NULL || rg_elmt == NULL)
    return -1;

  // B2 xac dinh tinh hop le cua region duoc dua vao
  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  // B3 : lay con con tro vm_freerg_list va xet hop le cua con tro
  struct vm_rg_struct *rg_node = mm->mmap->vm_freerg_list;

  // B4  : gan vung moi vao dau danh sach freerg_list
  rg_elmt->rg_next = rg_node;

  /* Enlist the new region */
  mm->mmap->vm_freerg_list = rg_elmt;

  return 0;
}
/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  // B1: Kiem tra page table va chi so symbol
  if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

  // B2: Tra ve entry tu symbol table
  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */

int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  //B1 : Kiem tra 
  if(caller==NULL||caller->mm==NULL || alloc_addr==NULL || size==0 || rgid <0 ||rgid >= PAGING_MAX_SYMTBL_SZ) 
    return -1;

  
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  // Lay con tro vung nho vma truoc de chuan bi lay freerg
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if(cur_vma == NULL)
  {
    // Neu ko tim thay vung nho nay thi out 
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  // Kiem tra symbol table slot da duoc su dung chua
  if (caller->mm->symrgtbl[rgid].rg_start != 0 ||
      caller->mm->symrgtbl[rgid].rg_end != 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // lay vung nho neu ma lay duoc thi gan va tra ve
  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;

    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  // TH2 xet lay ko du thi 
  
  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
  regs.a3 = size;
  
  if( _syscall(caller->krnl, caller->pid, 17, &regs) <0 ){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  
  /*Successful increase limit */
  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0) {
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
    *alloc_addr = rgnode.rg_start;
    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  pthread_mutex_unlock(&mmvm_lock);
  return -1;
}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
   // B1_a: Validate input
   if (caller == NULL || caller->mm == NULL) {
     return -1;
   }

   if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ) {
     return -1;
   }

   pthread_mutex_lock(&mmvm_lock);

   /* TODO: Manage the collect freed region to freerg_list */

   // B1_b  : Lay region ra  va xac dinh tinh hop le cua no
   struct vm_area_struct *cur_vma =
       get_vma_by_num(caller->mm, vmaid);

   if (cur_vma == NULL) {
     pthread_mutex_unlock(&mmvm_lock);
     return -1;
   }

   // enlist_vm_freerg_list hien tai chi quan ly VMA dau tien
   if (cur_vma != caller->mm->mmap) {
     pthread_mutex_unlock(&mmvm_lock);
     return -1;
   }

   struct vm_rg_struct *rgnode =
       get_symrg_byid(caller->mm, rgid);

   if (rgnode == NULL ||
       (rgnode->rg_start == 0 && rgnode->rg_end == 0) ||
       rgnode->rg_start >= rgnode->rg_end) {
     pthread_mutex_unlock(&mmvm_lock);
     return -1;
   }

   // B2: tao vung moi - xem xet co tao thanh cong thi tiep tuc
   struct vm_rg_struct *freerg_node =
       malloc(sizeof(struct vm_rg_struct));

   if (freerg_node == NULL) {
     pthread_mutex_unlock(&mmvm_lock);
     return -1;
   }

   // B3 :
   freerg_node->rg_start = rgnode->rg_start;
   freerg_node->rg_end = rgnode->rg_end;
   freerg_node->rg_next = NULL;

   /*enlist the obsoleted memory region */
   if (enlist_vm_freerg_list(caller->mm, freerg_node) < 0) {
     free(freerg_node);
     pthread_mutex_unlock(&mmvm_lock);
     return -1;
   }

   // B3: Reset symbol table entry
   rgnode->rg_start = rgnode->rg_end = 0;
   rgnode->rg_next = NULL;

   pthread_mutex_unlock(&mmvm_lock);
   return 0;
}
/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t  addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
    print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  /* By default using vmaid = 0 */
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val == -1)
  {
    return -1;
  }
printf("%s:%d\n",__func__,__LINE__);
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0;//val;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */

int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller)
{
  // B1: Xac dinh tinh hop le cua input
  if (mm == NULL || fpn == NULL || caller == NULL ||
      caller->mm == NULL || caller->krnl == NULL || caller->krnl->mram == NULL ||
      pgn < 0 || pgn >= PAGING_MAX_PGN || mm->pgd == NULL)
    return -1;

  // B2: Xac dinh page table dang thao tac la Kernel hay User
  int is_kernel_mm = (mm == caller->krnl->mm);
  uint64_t pte = pte_get_entry(mm, pgn, caller);

  // B3: Neu trang da nam trong RAM thi tra ve frame hien tai
  if (PAGING_PAGE_PRESENT(pte))
  {
    *fpn = PAGING_FPN(pte);
    return 0;
  }

  // B4: Kernel Memory khong duoc phep SWAP theo phuong an A
  if (is_kernel_mm)
  {
    addr_t kernel_fpn;

    // B4.1: Kernel chi duoc lay frame RAM con trong
    if (pte & PAGING_PTE_SWAPPED_MASK)
      return -1;

    if (MEMPHY_get_freefp(caller->krnl->mram, &kernel_fpn) < 0)
      return -1;

    // B4.2: Gan frame vao page table Kernel, khong dua vao FIFO User
    if (pte_set_fpn(mm, pgn, kernel_fpn, caller) < 0)
    {
      MEMPHY_put_freefp(caller->krnl->mram, kernel_fpn);
      return -1;
    }

    *fpn = (int)kernel_fpn;
    return 0;
  }

  // B5: User Memory - uu tien lay frame RAM trong
  addr_t vicpgn;
  uint64_t vicpte;
  addr_t vicfpn, swpfpn;
  addr_t tgtfpn; // Khung trang dich se chua du lieu cua trang pgn

  /* Buoc 1: Uu tien xin frame trong truc tiep tu RAM truoc */
  if (MEMPHY_get_freefp(caller->krnl->mram, &tgtfpn) < 0)
  {
    /* RAM da het frame trong -> Tim trang nan nhan (Victim) theo FIFO */
    // B5.1: RAM het frame - chon victim theo FIFO cua User MM
    if (find_victim_page(mm, &vicpgn) < 0)
      return -1;

    /* Lay so hieu frame vat ly cua trang nan nhan */
    vicpte = pte_get_entry(mm, vicpgn, caller);
    if (!PAGING_PAGE_PRESENT(vicpte))
    {
      enlist_pgn_node(&mm->fifo_pgn, vicpgn);
      return -1;
    }
    vicfpn = PAGING_FPN(vicpte);

    /* Xin mot frame trong ben vung nho SWAP */
    // B5.2: Xin frame SWAP de luu victim
    if (caller->krnl->active_mswp == NULL ||
        MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) < 0)
    {
      enlist_pgn_node(&mm->fifo_pgn, vicpgn);
      return -1;
    }

    /* Copy du lieu trang nan nhan tu RAM (vicfpn) sang SWAP (swpfpn) qua syscall 17 */
    // B5.3: Copy RAM -> SWAP bang syscall 17
    struct sc_regs swap_regs = {0};
    swap_regs.a1 = SYSMEM_SWP_OP;
    swap_regs.a2 = vicfpn;
    swap_regs.a3 = swpfpn;
    swap_regs.a4 = 0;
    if (_syscall(caller->krnl, caller->pid, 17, &swap_regs) < 0)
    {
      MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
      enlist_pgn_node(&mm->fifo_pgn, vicpgn);
      return -1;
    }

    /* Cap nhat bang trang cho nan nhan (danh dau da bi day ra SWAP) */
    // B5.4: Cap nhat PTE victim thanh trang da swap
    if (pte_set_swap(caller, vicpgn, 0, swpfpn) < 0)
    {
      MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
      enlist_pgn_node(&mm->fifo_pgn, vicpgn);
      return -1;
    }

    /* Chiem lay khung trang RAM cua nan nhan cho trang hien tai */
    // B5.5: Tai su dung frame RAM cua victim cho trang dich
    tgtfpn = vicfpn;
  }

  /* Buoc 2: Neu trang pgn truoc do tung bi day ra SWAP, nap nguoc lai vao tgtfpn */
  // B6: Neu trang dich dang nam trong SWAP thi nap lai vao RAM
  if (pte & PAGING_PTE_SWAPPED_MASK)
  {
    swpfpn = PAGING_SWP(pte);
    if (caller->krnl->active_mswp == NULL)
    {
      MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
      return -1;
    }

    /* Copy du lieu tu SWAP (swpfpn) nguoc ve RAM (tgtfpn) */
    struct sc_regs swap_regs = {0};
    swap_regs.a1 = SYSMEM_SWP_OP;
    swap_regs.a2 = tgtfpn; // Phai dung tgtfpn!
    swap_regs.a3 = swpfpn;
    swap_regs.a4 = 1; // Chieu SWAP -> RAM
    if (_syscall(caller->krnl, caller->pid, 17, &swap_regs) < 0)
    {
      MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
      return -1;
    }

    /* Tra lai frame SWAP vua doc xong vao danh sach trong */
    // B6.1: Giai phong frame SWAP da nap xong
    if (MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn) < 0)
    {
      MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
      return -1;
    }
  }

  /* Buoc 3: Cap nhat bang trang va dua trang pgn vao danh sach FIFO */
  // B7: Cap nhat PTE User va dua trang vao FIFO
  if (pte_set_fpn(mm, pgn, tgtfpn, caller) < 0)
  {
    MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
    return -1;
  }

  if (enlist_pgn_node(&mm->fifo_pgn, pgn) < 0)
  {
    MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
    pte_set_entry(mm, pgn, 0, caller);
    return -1;
  }

  *fpn = (int)tgtfpn;
  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, int addr, BYTE *data, struct pcb_t *caller)
{
  if (mm == NULL || data == NULL || caller == NULL || caller->krnl == NULL)
    return -1;
    // Phan tich dia chi thanh page num va offset de tim
  int pgn = PAGING_PGN(addr);
  int off = PAGING_OFFST(addr);
  int fpn;
  // lay frame co trang la pgn
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

#ifdef MM64
  addr_t phyaddr = (addr_t)fpn * PAGING64_PAGESZ + off;
#else
  addr_t phyaddr = (addr_t)fpn * PAGING_PAGESZ + off;
#endif

   /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */
  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;

  if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
    return -1;

  *data = (BYTE)regs.a3;

  return 0;
 

}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, int addr, BYTE value, struct pcb_t *caller)
{
  //B1 :  Xac dinh input hop le
  if (mm == NULL || caller == NULL || caller->krnl == NULL)
    return -1;
  // B2 : phan tich dia chi thanh pgn va offset
  int pgn = PAGING_PGN(addr);
  int off = PAGING_OFFST(addr);
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

#ifdef MM64
  addr_t phyaddr = (addr_t)fpn * PAGING64_PAGESZ + off;
#else
  addr_t phyaddr = (addr_t)fpn * PAGING_PAGESZ + off;
#endif

  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */
  struct sc_regs regs = {0};
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = value;

  if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
    return -1;
  return 0;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  if( caller==NULL || caller->mm==NULL) return -1;
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* TODO Invalid memory identify */
  if (currg == NULL || cur_vma == NULL){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  if(pg_getval(caller->mm, currg->rg_start + offset, data, caller)< 0){
    pthread_mutex_unlock(&mmvm_lock);  
    return -1;
  }
  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
    if (proc == NULL || destination == NULL)
        return -1;
  BYTE data;
  printf("%s:%d\n",__func__,__LINE__);
  // Doc gia tri tai vi tri source + offset va gan vao data
  int val = __read(proc, 0, source, offset, &data);
  if(val<0){
    return -1;
  }
  // gan gia tri vao data
  *destination = data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  if(caller==NULL||caller->mm==NULL) return -1;
  pthread_mutex_lock(&mmvm_lock);
  
  // Note: Consistent with __read, using caller->mm for User space
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (currg == NULL || cur_vma == NULL) /* Invalid memory identify */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  if (pg_setval(caller->mm,
              currg->rg_start + offset,
              value,
              caller) < 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  if (proc == NULL)
    return -1;
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}


/*
 * Kernel/User region helpers used by the kmem and copy routines.
 * These helpers keep the existing public function prototypes unchanged.
 */
static int get_free_vmrg_area_mm(struct mm_struct *mm, int vmaid, addr_t size,
                                 struct vm_rg_struct *newrg)
{
  // B1: Kiem tra dau vao va VMA
  if (mm == NULL || newrg == NULL || size == 0)
    return -1;

  struct vm_area_struct *vma = get_vma_by_num(mm, vmaid);
  if (vma == NULL)
    return -1;

  // B2: Tim first-fit region trong free list
  struct vm_rg_struct *node = vma->vm_freerg_list;
  while (node != NULL)
  {
    if (node->rg_start <= node->rg_end &&
        size <= node->rg_end - node->rg_start)
    {
      // B3: Ghi nhan vung cap phat
      newrg->rg_start = node->rg_start;
      newrg->rg_end = node->rg_start + size;

      // B4: Cap nhat free list sau khi cat vung da cap
      if (newrg->rg_end < node->rg_end)
        node->rg_start = newrg->rg_end;
      else
      {
        struct vm_rg_struct *next = node->rg_next;
        if (next != NULL)
        {
          node->rg_start = next->rg_start;
          node->rg_end = next->rg_end;
          node->rg_next = next->rg_next;
          free(next);
        }
        else
        {
          node->rg_start = node->rg_end;
          node->rg_next = NULL;
        }
      }
      newrg->rg_next = NULL;
      return 0;
    }
    node = node->rg_next;
  }

  return -1;
}

static int validate_mem_region(struct mm_struct *mm, int vmaid, int rgid,
                               addr_t offset, addr_t size)
{
  // B1: Kiem tra symbol va VMA
  if (mm == NULL || size == 0)
    return -1;
  struct vm_rg_struct *region = get_symrg_byid(mm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(mm, vmaid);
  if (region == NULL || vma == NULL ||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end)
    return -1;

  // B2: Kiem tra toan bo khoang [offset, offset + size)
  addr_t region_size = region->rg_end - region->rg_start;
  if (offset > region_size || size > region_size - offset)
    return -1;

  return 0;
}

/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /*
   * TODO: provide OS level management
   *       and forward the request to helper
   */
  // B1: Kiem tra tien dieu kien cap phat Kernel Memory
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      caller->krnl->mram == NULL || size == 0 ||
      reg_index >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  /*
   * TODO: provide OS kmem allocation validation
   */
  // B2: Chuyen yeu cau den bo cap phat Kernel Memory
  addr_t alloc_addr = 0;
  if (__kmalloc(caller, 0, (int)reg_index, size, &alloc_addr) == (addr_t)-1)
    return -1;

  // B3: Giu cung quy uoc voi liballoc: thanh cong tra ve 0
  return 0;
}


/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /* TODO: provide OS kernel memory allocation
   *       update krnl_pgd for OS kernel level management */

  //struct krnl_t *krnl = caller->krnl;
  //krnl->symrgtbl...
  //krnl->krnl_pgd ...

  // B1: Kiem tra dau vao va Kernel MM
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      caller->krnl->mram == NULL || alloc_addr == NULL || size == 0 ||
      rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return -1;

  pthread_mutex_lock(&kmm_lock);

  // B2: Lay Kernel MM, VMA va symbol entry tu cau truc cua OS
  struct mm_struct *kmm = caller->krnl->mm;
  struct vm_area_struct *kvma = get_vma_by_num(kmm, vmaid);
  struct vm_rg_struct *symrg = get_symrg_byid(kmm, rgid);
  if (kvma == NULL || symrg == NULL ||
      symrg->rg_start != 0 || symrg->rg_end != 0)
  {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // B3: Thu lay vung trong da duoc cap truoc do
  struct vm_rg_struct alloc_rg;
  if (get_free_vmrg_area_mm(kmm, vmaid, size, &alloc_rg) < 0)
  {
    // B4: Neu khong du, mo rong Kernel VMA theo boi so cua kich thuoc page
#ifdef MM64
    addr_t page_size = PAGING64_PAGESZ;
#else
    addr_t page_size = PAGING_PAGESZ;
#endif
    addr_t page_count = (size + page_size - 1) / page_size;
    addr_t aligned_size = page_count * page_size;
    addr_t map_start = kvma->sbrk;

    // B4.1: Kiem tra gioi han page table truoc khi cap frame
    if (map_start % page_size != 0 ||
        map_start / page_size + page_count > PAGING_MAX_PGN)
    {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }

    // B4.2: Map Kernel pages vao RAM; ham nay khong dung SWAP/FIFO
    struct vm_rg_struct *newrg = malloc(sizeof(struct vm_rg_struct));
    if (newrg == NULL)
    {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }

    if (vm_map_kernel(caller, kvma->vm_start, kvma->vm_end,
                      map_start, (int)page_count, newrg) == (addr_t)-1)
    {
      free(newrg);
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }

    // B4.3: Cap nhat break va danh sach vung trong cua Kernel VMA
    kvma->sbrk += aligned_size;
    kvma->vm_end = kvma->sbrk;
    newrg->rg_next = NULL;
    if (enlist_vm_rg_node(&kvma->vm_freerg_list, newrg) < 0)
    {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }

    // B5: Lay vung vua mo rong de cap cho request hien tai
    if (get_free_vmrg_area_mm(kmm, vmaid, size, &alloc_rg) < 0)
    {
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  }

  // B6: Luu vung da cap vao Kernel symbol table va tra dia chi ao
  symrg->rg_start = alloc_rg.rg_start;
  symrg->rg_end = alloc_rg.rg_end;
  *alloc_addr = alloc_rg.rg_start;

  pthread_mutex_unlock(&kmm_lock);
  return 0;
}


/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  /* TODO: provide DMA mempy */

  //struct krnl_t *krnl = caller->krnl;

  /*
   *  TODO: provide asynchronous dma memcpy
   *        handling the dma_done_callback
   */
  //do_dma(caller, (uint64_t) src, (uint64_t) dst, (uint64_t) size);
  //krnl->...
  //krnl->...

  return 0;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_user_mem(...)
  //__write_kernel_mem(...);

  // B1: Kiem tra hai region va toan bo khoang copy truoc khi ghi du lieu
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mm == NULL || size == 0)
    return -1;

  if (validate_mem_region(caller->mm, 0, (int)source, offset, size) < 0 ||
      validate_mem_region(caller->krnl->mm, 0, (int)destination, offset, size) < 0)
    return -1;

  // B2: Copy tung byte tu User MM sang Kernel MM
  for (uint32_t i = 0; i < size; i++)
  {
    BYTE data;
    if (__read_user_mem(caller, 0, (int)source, offset + i, &data) < 0 ||
        __write_kernel_mem(caller, 0, (int)destination, offset + i, data) < 0)
      return -1;
  }

  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_kernel_mem(...)
  //__write_user_mem(...);

  // B1: Kiem tra hai region va toan bo khoang copy truoc khi ghi du lieu
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mm == NULL || size == 0)
    return -1;

  if (validate_mem_region(caller->krnl->mm, 0, (int)source, offset, size) < 0 ||
      validate_mem_region(caller->mm, 0, (int)destination, offset, size) < 0)
    return -1;

  // B2: Copy tung byte tu Kernel MM sang User MM
  for (uint32_t i = 0; i < size; i++)
  {
    BYTE data;
    if (__read_kernel_mem(caller, 0, (int)source, offset + i, &data) < 0 ||
        __write_user_mem(caller, 0, (int)destination, offset + i, data) < 0)
      return -1;
  }

  return 0;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

  // B1: Kiem tra Kernel MM va con tro du lieu
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL || data == NULL)
    return -1;

  pthread_mutex_lock(&kmm_lock);
  struct mm_struct *kmm = caller->krnl->mm;
  struct vm_rg_struct *region = get_symrg_byid(kmm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(kmm, vmaid);

  // B2: Xac nhan region da cap phat va offset nam trong bien
  if (region == NULL || vma == NULL ||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // B3: Doc byte tu Kernel page table
  int ret = pg_getval(kmm, (int)(region->rg_start + offset), data, caller);
  pthread_mutex_unlock(&kmm_lock);
  return ret;
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

  // B1: Kiem tra Kernel MM
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL)
    return -1;

  pthread_mutex_lock(&kmm_lock);
  struct mm_struct *kmm = caller->krnl->mm;
  struct vm_rg_struct *region = get_symrg_byid(kmm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(kmm, vmaid);

  // B2: Xac nhan region da cap phat va offset hop le
  if (region == NULL || vma == NULL ||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }

  // B3: Ghi byte vao Kernel page table
  int ret = pg_setval(kmm, (int)(region->rg_start + offset), value, caller);
  pthread_mutex_unlock(&kmm_lock);
  return ret;
}


/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

  // B1: Kiem tra User MM va con tro du lieu
  if (caller == NULL || caller->mm == NULL || data == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(caller->mm, vmaid);

  // B2: Xac nhan region da cap phat va offset nam trong bien
  if (region == NULL || vma == NULL ||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // B3: Doc byte tu User page table
  int ret = pg_getval(caller->mm, (int)(region->rg_start + offset), data, caller);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

  // B1: Kiem tra User MM
  if (caller == NULL || caller->mm == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(caller->mm, vmaid);

  // B2: Xac nhan region da cap phat va offset hop le
  if (region == NULL || vma == NULL ||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  // B3: Ghi byte vao User page table
  int ret = pg_setval(caller->mm, (int)(region->rg_start + offset), value, caller);
  pthread_mutex_unlock(&mmvm_lock);
  return ret;
}


/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;

  for (pagenum = 0; pagenum < PAGING_MAX_PGN; pagenum++)
  {
    pte = caller->mm->pgd[pagenum];

    if (PAGING_PAGE_PRESENT(pte))
    {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else
    {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;

  /* TODO: Implement the theorical mechanism to find the victim page */
  if (!pg)
  {
    return -1;
  }
  struct pgn_t *prev = NULL;
  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;
  
  if (prev) {
     prev->pg_next = NULL;
  } else {
     mm->fifo_pgn = NULL;
  }

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
// Yeu cau lay 1 vung (region) co kich thuoc size thuoc area co id la vmaid gan no vao newrg
// Neu duoc tra ve 0
// else ve -1 
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  // B1 : xac nhan input
  if(caller==NULL || caller->mm ==NULL){
    return -1;
  }
  // B2  lay khu vuc nho va con tro cua vung free (ranh roi) de dung
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  
  struct vm_rg_struct *rgit = ( (cur_vma ==NULL)? NULL:cur_vma->vm_freerg_list );

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;

  // B3 Tim xem co vung nho nao co size phu hop ko
  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      // Lay 
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {  // Neu ma vung lay ma con du thi doi con tro bat dau qua 
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        // Th2 lay vua dung thi xoa khoi free list cai region do
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        // Th2a : Lay dung khi co nhieu hon 1 node
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                
          /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif