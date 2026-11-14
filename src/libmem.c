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
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

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
      caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mram == NULL)
    return -1;

  uint64_t pte = pte_get_entry(mm, pgn, caller);

  if (!PAGING_PAGE_PRESENT(pte))
  { /* Page is not online, make it actively living */
    
    addr_t vicpgn;
    uint64_t vicpte;
    addr_t vicfpn, swpfpn;
    addr_t tgtfpn; // Khung trang dich se chua du lieu cua trang pgn

    /* Buoc 1: Uu tien xin frame trong truc tiep tu RAM truoc */
    if (MEMPHY_get_freefp(caller->krnl->mram, &tgtfpn) == -1)
    {
      /* RAM da het frame trong -> Tim trang nan nhan (Victim) theo FIFO */
      if (find_victim_page(mm, &vicpgn) == -1)
      {
        // Xin ko dc thi out
        return -1;
      }

      /* Lay so hieu frame vat ly cua trang nan nhan */
      vicpte = pte_get_entry(mm, vicpgn, caller);
      vicfpn = PAGING_FPN(vicpte);

      /* Xin mot frame trong ben vung nho SWAP */
      if (caller->krnl->active_mswp == NULL ||
          MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == -1)
          {
           /* Khoi phuc lai victim vao danh sach FIFO do het SWAP */
            enlist_pgn_node(&mm->fifo_pgn, vicpgn);
            return -1;
          }

      /* Copy du lieu trang nan nhan tu RAM (vicfpn) sang SWAP (swpfpn) qua syscall 17 */
      struct sc_regs regs;
      regs.a1 = SYSMEM_SWP_OP;
      regs.a2 = vicfpn;
      regs.a3 = swpfpn;
      regs.a4 = 0;
      if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
      {
        MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
        enlist_pgn_node(&mm->fifo_pgn, vicpgn);
        return -1;
      }

      /* Cap nhat bang trang cho nan nhan (danh dau da bi day ra SWAP) */
      if (pte_set_swap(caller, vicpgn, 0, swpfpn) < 0)
      {
        MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
        enlist_pgn_node(&mm->fifo_pgn, vicpgn);
        return -1;
      }

      /* Chiem lay khung trang RAM cua nan nhan cho trang hien tai */
      tgtfpn = vicfpn;
    }

    /* Buoc 2: Neu trang pgn truoc do tung bi day ra SWAP, nap nguoc lai vao tgtfpn */
    if (pte & PAGING_PTE_SWAPPED_MASK)
    {
      swpfpn = PAGING_SWP(pte);

      /* Copy du lieu tu SWAP (swpfpn) nguoc ve RAM (tgtfpn) */
     struct sc_regs regs;
      regs.a1 = SYSMEM_SWP_OP;
      regs.a2 = tgtfpn; // Phai dung tgtfpn!
      regs.a3 = swpfpn;
      regs.a4 = 1; // Chieu SWAP -> RAM
      if (_syscall(caller->krnl, caller->pid, 17, &regs) < 0)
      {
        MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
        return -1;
      }

      /* Tra lai frame SWAP vua doc xong vao danh sach trong */
      if (MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn) < 0)
      {
        return -1;
      }
    }

    /* Buoc 3: Cap nhat bang trang va dua trang pgn vao danh sach FIFO */
    if (pte_set_fpn(mm, pgn, tgtfpn, caller) < 0)
    {
      MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
      return -1;
    }

    if (enlist_pgn_node(&mm->fifo_pgn, pgn) < 0)
    {
      return -1;
    }
  }

  *fpn = PAGING_FPN(pte_get_entry(mm, pgn, caller));

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

  addr_t phyaddr = ((addr_t)fpn << PAGING_ADDR_OFFST_LOBIT) + off;

   /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */
  struct sc_regs regs;
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

  int phyaddr = ((addr_t)fpn << PAGING_ADDR_OFFST_LOBIT) + off;

  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */
  struct sc_regs regs;
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
//addr_t  addr;
//int val = ...

  /* 
   * TODO: provide OS kmem allocation validation
   */

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

  return 1;
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

  return 0;
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

  return 0;
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

   return 0;
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

  return 0;
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