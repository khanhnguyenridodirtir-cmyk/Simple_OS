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
#include "../include/syscall.h"
#include "libmem.h"
#include "dma.h"
#include "dpi_rtl.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "queue.h"
#include "sched.h"
#include <limits.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;
/*Khu vu dinh nghia struct helper*/
//#define DMADEBUG
#ifdef MM64
#define MAXPG PAGING64_MAX_PGN
#else
#define MAXPG PAGING_MAX_PGN
#endif

struct victim_query {
    struct mm_struct *kernel_mm;
    addr_t pgn;
};
/*Ket thuc khu vu dinh nghia struct helper*/

/*Bat dau khu vu khai bao ham helper*/

// prototype tu che de giai phong bo nho
int free_pcb_memph(struct pcb_t *caller);
static int is_rg_dma_busy(uint32_t pid, int rgid);
static int pg_getpage_locked(struct mm_struct *mm, addr_t pgn, int *fpn,
                             struct pcb_t *caller);
static int pg_getval_locked(struct mm_struct *mm, addr_t addr, BYTE *data,
                            struct pcb_t *caller);
static int pg_setval_locked(struct mm_struct *mm, addr_t addr, BYTE value,
                            struct pcb_t *caller);
int find_victim_page_locked(struct mm_struct *mm, addr_t *retpgn);

/* Lock interface for VM entry points implemented in other modules. */
void libmem_mmvm_lock(void)
{
    pthread_mutex_lock(&mmvm_lock);
}

void libmem_mmvm_unlock(void)
{
    pthread_mutex_unlock(&mmvm_lock);
}

/*Ket thuc khu vuc khai bao ham helper*/

static int running_proc_has_victim(struct pcb_t *proc, void *arg)
{
    struct victim_query *query = arg;

    if (proc->mm == NULL || proc->mm == query->kernel_mm)
        return 0;

    return find_victim_page_locked(proc->mm, &query->pgn) == 0;
}

/* Cac ham tim kiem region va area */
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

int find_free_symrg(struct mm_struct *mm)
{
    if (mm == NULL)
        return -1;

    for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; i++) {
        if (mm->symrgtbl[i].vmaid < 0 &&
            mm->symrgtbl[i].rg_start == 0 &&
            mm->symrgtbl[i].rg_end == 0) {
            return i;
        }
    }

    return -1;
}

// Helper nay tim symrg theo dia chi cap tu thanh ghi
struct vm_rg_struct *get_symrg_byaddr(
    struct mm_struct *mm,
    addr_t addr,
    int *rgid
) {
    if (mm == NULL || rgid == NULL)
        return NULL;
    
    for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; i++) {
        struct vm_rg_struct *rg = &mm->symrgtbl[i];

        if (rg->vmaid >= 0 &&
            rg->rg_start == addr &&
            rg->rg_end > rg->rg_start) {
            *rgid = i;
            return rg;
        }
      }
   
    return NULL;
}

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
// Dua mot region moi vao free list
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt, int vmaid)
{
  // B1  : validate du lieu
  if (mm == NULL || mm->mmap == NULL || rg_elmt == NULL)
    return -1;

  // B2 xac dinh tinh hop le cua region duoc dua vao
  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  // B3 : lay con con tro vm_freerg_list va xet hop le cua con tro
  struct vm_area_struct* currentvma =  get_vma_by_num(mm, vmaid);
  if(currentvma==NULL){
    return -1;
  }
  struct vm_rg_struct *rg_node  = currentvma->vm_freerg_list;

  // B4  : gan vung moi vao dau danh sach freerg_list
  rg_elmt->rg_next = rg_node;

  /* Enlist the new region */
  currentvma->vm_freerg_list = rg_elmt;

  return 0;
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
  if(caller==NULL||caller->mm==NULL || alloc_addr==NULL || size==0 ) 
    return -1;

  
  pthread_mutex_lock(&mmvm_lock);
  rgid = find_free_symrg(caller->mm);
  if(rgid <0 ||rgid >= PAGING_MAX_SYMTBL_SZ){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
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
    caller->mm->symrgtbl[rgid].vmaid = vmaid;
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  // TH2 xet lay ko du thi 
  
  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/
  struct sc_regs regs={0};
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
  regs.a3 = size;
  
  /* The syscall handler takes mmvm_lock while extending the VMA. Release
   * our lock first so that the handler does not recursively lock the mutex. */
  pthread_mutex_unlock(&mmvm_lock);
  int syscall_ret = _syscall(caller->krnl, caller->pid, 17, &regs);
  pthread_mutex_lock(&mmvm_lock);
  if (syscall_ret < 0) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  
  /*Successful increase limit */
  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0) {
    caller->mm->symrgtbl[rgid].vmaid=vmaid;
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


   /* TODO: Manage the collect freed region to freerg_list */

   // B1_b  : Lay region ra  va xac dinh tinh hop le cua no
   struct vm_area_struct *cur_vma =
       get_vma_by_num(caller->mm, vmaid);

   if (cur_vma == NULL) {
  
     return -1;
   }

  
   struct vm_rg_struct *rgnode =
       get_symrg_byid(caller->mm, rgid);

  if (rgnode == NULL ||
    rgnode->vmaid != vmaid || 
    rgnode->vmaid < 0||
    rgnode->rg_start >= rgnode->rg_end) {
    
    return -1;
  }


   // B2: tao vung moi - xem xet co tao thanh cong thi tiep tuc
   struct vm_rg_struct *freerg_node =
       malloc(sizeof(struct vm_rg_struct));

   if (freerg_node == NULL) {
     return -1;
   }

   // B3 : Gan thong so khoi tao cho region moi va gan cai region do vao free list
   freerg_node->vmaid = vmaid;
   freerg_node->rg_start = rgnode->rg_start;
   freerg_node->rg_end = rgnode->rg_end;
   freerg_node->rg_next = NULL;

   /*enlist the obsoleted memory region */
   // B3.1 Neu ma gan dc thi di tiep ko thi ta dua moi thu ve ban dau free cai region moi tao
   if (enlist_vm_freerg_list(caller->mm, freerg_node,vmaid) < 0) {
     free(freerg_node);
     return -1;
   }

   // B3.2: Dua gia tri cua region nay trong region table ve lai gia tri goc
   rgnode->rg_start = 0;
   rgnode->rg_end = 0;
   rgnode->vmaid = -1;
   rgnode->rg_next = NULL;
   
  return 0;
}
/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  if (reg_index >= MAXREG || proc == NULL || proc->mm == NULL) return -1;
  addr_t  addr;
  
  int vmaid = select_vmaid(size);



  int val = __alloc(proc, vmaid, -1, size, &addr);
 
  if (val == -1)
  {
    return -1;
  }
  proc->regs[reg_index] = addr;
  
#ifdef IODUMP
  if (proc->krnl != NULL && proc->krnl->mram != NULL)
    MEMPHY_dump(proc->krnl->mram);
  printf("[ALLOC] PID=%d reg=%d -> rg_start=%lu, rg_end=%lu\n",
         proc->pid, reg_index, (unsigned long)addr, (unsigned long)(addr + size));

#endif
#ifdef PAGETBL_DUMP
    print_pgtbl(proc, 0, -1); // print max TBL
#endif
  /* By default using vmaid = 0 */
  return val;
}

int select_vmaid(addr_t size)
{
    if (size <= SMALL_MAX_SIZE)
        return VMA_SMALL;

    if (size <= MEDIUM_MAX_SIZE)
        return VMA_MEDIUM;

     return VMA_LARGE;

}


/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
    if (proc == NULL || proc->mm == NULL)
        return -1;

    if (reg_index >= MAXREG)
        return -1;
    pthread_mutex_lock(&mmvm_lock);
    addr_t addr = proc->regs[reg_index];

   
    int rgid = -1;

    struct vm_rg_struct *rgnode =
        get_symrg_byaddr(proc->mm, addr, &rgid);

    if (rgnode == NULL ||is_rg_dma_busy(proc->pid, rgid) ){
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }
    int actual_vmaid = rgnode->vmaid;
    
    int val = __free(proc, actual_vmaid, rgid);

    if (val < 0)
    {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
    proc->regs[reg_index] = (addr_t)-1;
    pthread_mutex_unlock(&mmvm_lock);
    return 0;
}
/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */



static int pg_getpage_locked(struct mm_struct *mm, addr_t pgn, int *fpn,
                             struct pcb_t *caller)
{
    /* B1: Validate input */
    if (mm == NULL || fpn == NULL || caller == NULL ||
        caller->mm == NULL || caller->krnl == NULL ||
        caller->krnl->mram == NULL || pgn >= MAXPG ||
        mm->pgd == NULL)
        return -1;

    int is_kernel_mm = (mm == caller->krnl->mm);
    uint64_t pte = pte_get_entry(mm, pgn, caller);

    /* B2: Page already present in RAM */
    if (PAGING_PAGE_PRESENT(pte)) {
        *fpn = PAGING_FPN(pte);
        return 0;
    }

    /* B3: Handle Kernel Memory */
    if (is_kernel_mm) {
        addr_t kernel_fpn;

        if (pte & PAGING_PTE_SWAPPED_MASK)
            return -1;

        if (MEMPHY_get_freefp(caller->krnl->mram, &kernel_fpn) < 0)
            return -1;

        if (pte_set_fpn(mm, pgn, kernel_fpn, caller) < 0) {
            MEMPHY_put_freefp(caller->krnl->mram, kernel_fpn);
            return -1;
        }

        *fpn = (int)kernel_fpn;
        return 0;
    }

    /* B4: Get a free RAM frame */
    addr_t vicpgn, vicfpn, swpfpn, tgtfpn;
    uint64_t vicpte;

    if (MEMPHY_get_freefp(caller->krnl->mram, &tgtfpn) < 0) {
        struct pcb_t *vic_proc = caller;

        /* Find victim in current process first */
        if (find_victim_page_locked(mm, &vicpgn) < 0) {
            vic_proc = NULL;

            /* Global replacement */
           
          struct victim_query query = {
                .kernel_mm = caller->krnl->mm
              };

          vic_proc = sched_find_running_proc_in(caller->krnl->running_list,
                                                running_proc_has_victim,
                                                &query);
          if (vic_proc != NULL)
              vicpgn = query.pgn;
        }
        if (vic_proc == NULL)
         return -1;
        
        vicpte = pte_get_entry(vic_proc->mm, vicpgn, vic_proc);
        if (!PAGING_PAGE_PRESENT(vicpte)) {
            enlist_pgn_node(&vic_proc->mm->fifo_pgn, vicpgn);
            return -1;
        }

        vicfpn = PAGING_FPN(vicpte);

        /* Get a free SWAP frame */
        if (caller->krnl->active_mswp == NULL ||
            MEMPHY_get_freefp(caller->krnl->active_mswp,
                              &swpfpn) < 0) {
            enlist_pgn_node(&vic_proc->mm->fifo_pgn, vicpgn);
            return -1;
        }

        /* Copy victim page: RAM -> SWAP */
        struct sc_regs swap_regs = {0};
        swap_regs.a1 = SYSMEM_SWP_OP;
        swap_regs.a2 = vicfpn;
        swap_regs.a3 = swpfpn;
        swap_regs.a4 = 0;

        if (_syscall(caller->krnl, caller->pid, 17,
                     &swap_regs) < 0) {
            MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
            enlist_pgn_node(&vic_proc->mm->fifo_pgn, vicpgn);
            return -1;
        }

        if (pte_set_swap(vic_proc, vicpgn, 0, swpfpn) < 0) {
            MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
            enlist_pgn_node(&vic_proc->mm->fifo_pgn, vicpgn);
            return -1;
        }

        tgtfpn = vicfpn;
    }

    /* B5: Restore target page from SWAP if necessary */
    if (pte & PAGING_PTE_SWAPPED_MASK) {
        swpfpn = PAGING_SWP(pte);

        if (caller->krnl->active_mswp == NULL) {
            MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
            return -1;
        }

        struct sc_regs swap_regs = {0};
        swap_regs.a1 = SYSMEM_SWP_OP;
        swap_regs.a2 = tgtfpn;
        swap_regs.a3 = swpfpn;
        swap_regs.a4 = 1;

        if (_syscall(caller->krnl, caller->pid, 17,
                     &swap_regs) < 0) {
            MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
            return -1;
        }
    }

    /* B6: Update PTE and FIFO */
    if (pte_set_fpn(mm, pgn, tgtfpn, caller) < 0) {
        MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
        return -1;
    }

    if (enlist_pgn_node(&mm->fifo_pgn, pgn) < 0) {
        pte_set_entry(mm, pgn, pte, caller);
        MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
        return -1;
    }

    /* B7: Release old SWAP frame after successful restoration */
    if (pte & PAGING_PTE_SWAPPED_MASK) {
        MEMPHY_put_freefp(caller->krnl->active_mswp, swpfpn);
    }

    *fpn = (int)tgtfpn;
    return 0;
}

/* Public entry point for callers that do not already hold mmvm_lock. */
int pg_getpage(struct mm_struct *mm, addr_t pgn, int *fpn,
               struct pcb_t *caller)
{
    libmem_mmvm_lock();
    int ret = pg_getpage_locked(mm, pgn, fpn, caller);
    libmem_mmvm_unlock();
    return ret;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
static int pg_getval_locked(struct mm_struct *mm, addr_t addr, BYTE *data,
                            struct pcb_t *caller)
{
  if (mm == NULL || data == NULL || caller == NULL || caller->krnl == NULL)
    return -1;
    // Phan tich dia chi thanh page num va offset de tim

 #ifdef MM64
  addr_t pgn = addr / PAGING64_PAGESZ;   // Hoặc (addr >> PAGING64_ADDR_PT_SHIFT)
  int off = addr % PAGING64_PAGESZ;   // Hoặc (addr & 0xFFF)
#else
  addr_t pgn = PAGING_PGN(addr);
  int off = PAGING_OFFST(addr);
#endif
  int fpn;
  // lay frame co trang la pgn
  if (pg_getpage_locked(mm, pgn, &fpn, caller) != 0)
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

int pg_getval(struct mm_struct *mm, addr_t addr, BYTE *data,
              struct pcb_t *caller)
{
  libmem_mmvm_lock();
  int ret = pg_getval_locked(mm, addr, data, caller);
  libmem_mmvm_unlock();
  return ret;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
static int pg_setval_locked(struct mm_struct *mm, addr_t addr, BYTE value,
                            struct pcb_t *caller)
{
  //B1 :  Xac dinh input hop le
  if (mm == NULL || caller == NULL || caller->krnl == NULL)
    return -1;
  // B2 : phan tich dia chi thanh pgn va offset
#ifdef MM64
  addr_t pgn = addr / PAGING64_PAGESZ;   // Hoặc (addr >> PAGING64_ADDR_PT_SHIFT)
  int off = addr % PAGING64_PAGESZ;   // Hoặc (addr & 0xFFF)
#else
  addr_t pgn = PAGING_PGN(addr);
  int off = PAGING_OFFST(addr);
#endif
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage_locked(mm, pgn, &fpn, caller) != 0)
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

int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value,
              struct pcb_t *caller)
{
  libmem_mmvm_lock();
  int ret = pg_setval_locked(mm, addr, value, caller);
  libmem_mmvm_unlock();
  return ret;
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
  
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* TODO Invalid memory identify */
 if (currg == NULL || cur_vma == NULL ||currg->vmaid != vmaid||
    (currg->rg_start == 0 && currg->rg_end == 0) ||
    currg->rg_start >= currg->rg_end ||
    offset >= currg->rg_end - currg->rg_start){
    
    return -1;
  }
  if(pg_getval_locked(caller->mm, currg->rg_start + offset, data, caller)< 0){
    
    return -1;
  }
 
  return 0;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
  pthread_mutex_lock(&mmvm_lock);
  if (proc == NULL || proc->mm == NULL ||
        destination == NULL ||
        source >= MAXREG || *destination >= MAXREG)
    { 
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }
    BYTE data;

    // B1: Lay dia chi region tu thanh ghi nguon
    addr_t source_addr = (addr_t)proc->regs[source];

    // B2: Tim region va rgid theo dia chi
    int rgid;
    struct vm_rg_struct *rgnode =
        get_symrg_byaddr(proc->mm, source_addr, &rgid);

    if (rgnode == NULL || rgnode->rg_start >= rgnode->rg_end){
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
    // B3: Lay VMA ID cua region
    int vmaid = rgnode->vmaid;

    if (vmaid < 0){
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
  int val = __read(proc, vmaid, rgid, offset, &data);
  if(val<0){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  // gan gia tri vao data

  proc->regs[*destination] = (uint8_t)data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
  printf("[VERIFY READ] PID=%d reg=%d offset=%ld -> val=%d\n", proc->pid, source, offset, data);

#endif
  #ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
  pthread_mutex_unlock(&mmvm_lock);
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
  
  
  // Note: Consistent with __read, using caller->mm for User space
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  if (currg == NULL || cur_vma == NULL ||currg->vmaid != vmaid||
    (currg->rg_start == 0 && currg->rg_end == 0) ||
    currg->rg_start >= currg->rg_end ||
    offset >= currg->rg_end - currg->rg_start) /* Invalid memory identify */
  {
    
    return -1;
  }

  if (pg_setval_locked(caller->mm,
              currg->rg_start + offset,
              value,
              caller) < 0)
  {
    
    return -1;
  }

  return 0;
}

/*libwrite - PAGING-based write a region memory */
/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  pthread_mutex_lock(&mmvm_lock);
  if (proc == NULL || proc->mm == NULL ||
      destination >= MAXREG){
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
  }
  // B1: Lay dia chi region tu thanh ghi dich
  addr_t destination_addr = (addr_t)proc->regs[destination];

  // B2: Tim region va rgid theo dia chi
  int rgid;
  struct vm_rg_struct *rgnode =
      get_symrg_byaddr(proc->mm, destination_addr, &rgid);

  if (rgnode == NULL ||
      rgnode->rg_start >= rgnode->rg_end)
    {
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }
  // B3: Lay VMA ID cua region
  int vmaid = rgnode->vmaid;

  if (vmaid < 0){
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  // B4: Ghi du lieu vao region
  int val = __write(proc, vmaid, rgid, offset, data);
  if (val == -1)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
  printf("[VERIFY Write] PID=%d reg=%d offset=%ld -> val=%d\n", proc->pid, destination, offset, data);
#endif
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
  pthread_mutex_unlock(&mmvm_lock);
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
  if  (newrg == NULL || size <= 0)
    return -1;
  struct vm_area_struct *vma = get_vma_by_num(mm, vmaid);
  if (vma == NULL)
    return -1;

  // B2: Tim first-fit region vung ma no khop voi size dang muon cap trong free list
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
    region->vmaid != vmaid ||
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
   pthread_mutex_lock(&kmm_lock);
   pthread_mutex_lock(&mmvm_lock);
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
      caller->krnl->mram == NULL || size == 0 ||
      reg_index >= MAXREG){
        pthread_mutex_unlock(&mmvm_lock);
        pthread_mutex_unlock(&kmm_lock);
        return -1;
  }

  /*
   * TODO: provide OS kmem allocation validation
   */
  // B2: Chuyen yeu cau den bo cap phat Kernel Memory
  addr_t alloc_addr = 0;
  int rgid =find_free_symrg(caller->krnl->mm);
  if(rgid==-1){
    pthread_mutex_unlock(&mmvm_lock);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  if (__kmalloc(caller, 0, rgid, size, &alloc_addr) == (addr_t)-1){
    pthread_mutex_unlock(&mmvm_lock);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  caller->regs[reg_index] = alloc_addr;
  // B3: Giu cung quy uoc voi liballoc: thanh cong tra ve 0
  pthread_mutex_unlock(&mmvm_lock);
  pthread_mutex_unlock(&kmm_lock);
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

 

  // B2: Lay Kernel MM, VMA va symbol entry tu cau truc cua OS
  struct mm_struct *kmm = caller->krnl->mm;
  struct vm_area_struct *kvma = get_vma_by_num(kmm, vmaid);
  struct vm_rg_struct *symrg = get_symrg_byid(kmm, rgid);
  if (kvma == NULL || symrg == NULL ||
      symrg->rg_start != 0 || symrg->rg_end != 0)
  {
   
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
    addr_t page_count;
    addr_t aligned_size;
    addr_t map_start = kvma->sbrk;

    if (page_size == 0 || size > (addr_t)-1 - (page_size - 1))
      return -1;

    page_count = size / page_size;
  if (size % page_size != 0)
      page_count++;

  if (page_count == 0 || page_count > (addr_t)INT_MAX ||
      page_count > (addr_t)-1 / page_size)
      return -1;

    aligned_size = page_count * page_size;

    // B4.1: Kiem tra gioi han page table truoc khi cap frame
   addr_t start_pgn = map_start / page_size;

    if (map_start % page_size != 0 ||
      start_pgn >= MAXPG ||
      page_count > MAXPG - start_pgn)
        return -1;

    // B4.2: Map Kernel pages vao RAM; ham nay khong dung SWAP/FIFO
    struct vm_rg_struct *newrg = malloc(sizeof(struct vm_rg_struct));
    if (newrg == NULL)
    {
      
      return -1;
    }

    if (vm_map_kernel(caller, kvma->vm_start, kvma->vm_end,
                      map_start, (int)page_count, newrg) == (addr_t)-1)
    {
      free(newrg);
      
      return -1;
    }

    // B4.3: Cap nhat break va danh sach vung trong cua Kernel VMA
    
    
    newrg->rg_next = NULL;
    if (enlist_vm_rg_node(&kvma->vm_freerg_list, newrg) < 0)
    {
      free(newrg);
      return -1;
    }

    // B5: Lay vung vua mo rong de cap cho request hien tai
    if (get_free_vmrg_area_mm(kmm, vmaid, size, &alloc_rg) < 0)
    {
      
      return -1;
    }
     kvma->sbrk += aligned_size;
     kvma->vm_end = kvma->sbrk;
  }
 
  // B6: Luu vung da cap vao Kernel symbol table va tra dia chi ao
  symrg->vmaid = vmaid;
  symrg->rg_start = alloc_rg.rg_start;
  symrg->rg_end = alloc_rg.rg_end;
  *alloc_addr = alloc_rg.rg_start;


  return 0;
}


/*========================DMA HELPER=====================*/
/* ============================================================================
 * [SYNC-DMA] Tom tat cac bo sung dong bo hoa cho module DMA:
 * 1. Pin trang (is_page_dma_pinned): Luu them mm & khoang dia chi [start, end)
 *    vao dma_jobs[] de find_victim_page() khong chon nham frame dang DMA de swap-out.
 * 2. Chan hazard DMA-DMA (libmem_dma_blocked): Bo sung case DMA_MEMCPY va khoa
 *    mmvm_lock khi tra symrgtbl (reg_to_rgid) de tranh xung dot voi alloc/free.
 * 3. Tranh deadlock khi nap lenh (dma_submit_lock): Tach rieng khoa thu tu nap
 *    lenh voi dma_job_lock, nha dma_job_lock truoc khi goi do_dma_start().
 * 4. An toan thu hoi bo nho (free_pcb_memph): Chan giai phong frame RAM neu
 *    process van con job DMA dang chay (libmem_dma_inflight > 0).
 * ============================================================================ */
static int reg_to_rgid(struct pcb_t *proc, uint32_t reg_idx)
{
    if (proc == NULL || proc->mm == NULL || reg_idx >= MAXREG)
        return -1;
  
    addr_t addr = (addr_t)proc->regs[reg_idx];
    int rgid = -1;
    if (get_symrg_byaddr(proc->mm, addr, &rgid) == NULL)
        return -1;

    return rgid;
}

/* ===================================================================
 * Bang job DMA dang bay (in-flight) cua cac process
 * -------------------------------------------------------------------
 * CPU phat lenh DMA roi chay tiep (bat dong bo). Nhung region nguon va
 * dich van dang bi mach doc/ghi, nen process KHONG duoc:
 *   - doc/ghi/free region DICH truoc khi DMA xong  (giong hazard RAW/WAW)
 *   - ghi/free region NGUON truoc khi DMA xong     (giong hazard WAR)
 *   - ket thuc va tra frame cho he thong khi con DMA dang ghi vao do
 * Ta chi chan DUNG process do (dua no ve hang doi), CPU van chay process
 * khac. Job duoc go khoi bang trong interrupt handler (os.c) khi DMA xong.
 *
 * Mach chi co mot kenh, yeu cau vao hang FIFO, nen job xong theo dung
 * thu tu nap. Vi the bang cung la FIFO: nap o duoi, xong lay o dau.
 * =================================================================== */
#define DMA_JOB_MAX 32          /* >= hang doi 16 + 1 job dang chay */

struct dma_job {
  uint32_t pid;
  struct mm_struct *mm;
  uint32_t src_rg, dst_rg;
  addr_t src_start, src_end;
  addr_t dst_start, dst_end;
  uint32_t len;
};

static struct dma_job  dma_jobs[DMA_JOB_MAX];
static int             dma_job_head = 0, dma_job_count = 0;
static pthread_mutex_t dma_job_lock = PTHREAD_MUTEX_INITIALIZER;

/* [SYNC-DMA] Khoa rieng cho luong nap lenh DMA de khong giu dma_job_lock khi goi do_dma_start() */
static pthread_mutex_t dma_submit_lock = PTHREAD_MUTEX_INITIALIZER;

/* [SYNC-DMA] Kiem tra trang pgn cua mm co dang nam trong vung src/dst cua DMA dang chay khong.
 * Dung cho find_victim_page() de khong day nham frame dang DMA ra SWAP. */
static int is_page_dma_pinned(struct mm_struct *mm, addr_t pgn)
{
  if (mm == NULL) return 0;
#ifdef MM64
  addr_t pagesz = PAGING64_PAGESZ;
#else
  addr_t pagesz = PAGING_PAGESZ;
#endif
  addr_t pg_start = pgn * pagesz;
  addr_t pg_end   = pg_start + pagesz;
  int pinned = 0;

  pthread_mutex_lock(&dma_job_lock);
  for (int i = 0; i < dma_job_count; i++) {
    struct dma_job *j = &dma_jobs[(dma_job_head + i) % DMA_JOB_MAX];
    if (j->mm != mm) continue;
    if ((pg_start < j->src_end && pg_end > j->src_start) ||
        (pg_start < j->dst_end && pg_end > j->dst_start)) {
      pinned = 1;
      break;
    }
  }
  pthread_mutex_unlock(&dma_job_lock);
  return pinned;
}
/* so job DMA chua xong cua mot process */
int libmem_dma_inflight(uint32_t pid)
{
  int n = 0;
  pthread_mutex_lock(&dma_job_lock);
  for (int i = 0; i < dma_job_count; i++)
    if (dma_jobs[(dma_job_head + i) % DMA_JOB_MAX].pid == pid)
      n++;
  pthread_mutex_unlock(&dma_job_lock);
  return n;
}
/* [SYNC-DMA] Kiem tra mot rgid cu the cua pid co dang nam trong hang doi DMA khong */
static int is_rg_dma_busy(uint32_t pid, int rgid)
{
  if (rgid < 0) return 0;
  int busy = 0;
  pthread_mutex_lock(&dma_job_lock);
  for (int i = 0; i < dma_job_count; i++) {
    struct dma_job *j = &dma_jobs[(dma_job_head + i) % DMA_JOB_MAX];
    if (j->pid == pid &&
        ((uint32_t)rgid == j->src_rg || (uint32_t)rgid == j->dst_rg)) {
      busy = 1;
      break;
    }
  }
  pthread_mutex_unlock(&dma_job_lock);
  return busy;
}
/* lenh sap chay co dung vao region dang co DMA khong?
 *   rd = region lenh se doc, wr = region lenh se ghi/free (-1 = khong co) */
static int dma_conflict(uint32_t pid, int rd, int wr)
{
  for (int i = 0; i < dma_job_count; i++) {
    struct dma_job *j = &dma_jobs[(dma_job_head + i) % DMA_JOB_MAX];
    if (j->pid != pid) continue;
    /* region dich: doc hay ghi deu phai doi */
    if ((rd >= 0 && (uint32_t) rd == j->dst_rg) ||
        (wr >= 0 && (uint32_t) wr == j->dst_rg) ||
        /* region nguon: doc thi duoc, ghi/free thi phai doi */
        (wr >= 0 && (uint32_t) wr == j->src_rg))
      return 1;
  }
  return 0;
}

/*
 * libmem_dma_blocked - process co phai doi DMA truoc lenh ke tiep khong
 * Goi tu cpu_routine() TRUOC run(). Tra 1 = chua duoc chay: CPU dua
 * process sang trang thai cho (waiting) va chay process khac.
 */
int libmem_dma_blocked(struct pcb_t *proc)
{
  // B1 : validate du lieu  (note :goc)
  if (proc == NULL || proc->code == NULL)
    return 0;


  int blocked = 0;

   int rd = -1, wr = -1;
   /* [SYNC-DMA] Khoa mmvm_lock vi reg_to_rgid() doc symrgtbl co the xung dot voi alloc/free */
  pthread_mutex_lock(&mmvm_lock);
  if(proc->pc < proc->code->size) {
    struct inst_t *ins = &proc->code->text[proc->pc];
    switch (ins->opcode) {
    case READ:           rd = reg_to_rgid(proc, ins->arg_0); break;
    case WRITE:          wr = reg_to_rgid(proc, ins->arg_1); break;
    case FREE:           wr = reg_to_rgid(proc, ins->arg_0); break;
    case DMA_MEMCPY:     /* doi ten dung theo enum trong common.h */
      rd = reg_to_rgid(proc, ins->arg_0);
      wr = reg_to_rgid(proc, ins->arg_1);
      break;
    case ALLOC:          break; 
#ifdef MM_PAGING
    case COPY_FROM_USER: rd = (int) ins->arg_0; break; /* Lenh syscall kernel vẫn truyền thẳng rgid */
    case COPY_TO_USER:   wr = (int) ins->arg_1; break;
#endif
    default: break;
    }
   
  }
  pthread_mutex_unlock(&mmvm_lock);
   pthread_mutex_lock(&dma_job_lock);
  if (proc->pc >= proc->code->size) {
    /* Het lenh. Khong bao gio run() qua cuoi code: tra 1 de CPU khong
     * chay. Neu con DMA thi process phai cho; neu het DMA thi vong sau
     * cpu_routine() se cho no ket thuc binh thuong. */
    blocked = 1;
  } else{
     blocked = dma_conflict(proc->pid, rd, wr);
  }
  pthread_mutex_unlock(&dma_job_lock);
  return blocked;
}

/*
 * libmem_dma_complete - interrupt handler goi khi DMA bao xong
 * Go job dau hang (job xong theo thu tu FIFO) va tra thong tin ve.
 * tra 0 neu co job, -1 neu bang rong (ngat gia)
 */
int libmem_dma_complete(uint32_t *pid, uint32_t *src_rg, uint32_t *dst_rg,
                        uint32_t *len)
{
  int rc = -1;
  pthread_mutex_lock(&dma_job_lock);
  if (dma_job_count > 0) {
    struct dma_job *j = &dma_jobs[dma_job_head];
    *pid = j->pid; *src_rg = j->src_rg; *dst_rg = j->dst_rg; *len = j->len;
    dma_job_head = (dma_job_head + 1) % DMA_JOB_MAX;
    dma_job_count--;
    rc = 0;
  }
  pthread_mutex_unlock(&dma_job_lock);
  return rc;
}

/* dma_xlat - dich mot dia chi ao sang dia chi vat ly cho DMA
 *
 * DMA la thiet bi tren bus, no khong di qua MMU. Nen truoc khi dua
 * dia chi cho no, phan mem phai tu dich xong.
 *
 * LUU Y khong dung PAGING64_ADDR_OFFST_MASK: mm64.h:57 dinh nghia no
 * bang PAGING_ADDR_OFFST_HIBIT (ban 32 bit, = 7) thay vi
 * PAGING64_ADDR_OFFST_HIBIT (= 11), nen mask ra 0xFF thay vi 0xFFF.
 * Tinh tay tu PAGING64_PAGESZ cho chac, va bao TV2 sua header.
 */
static int dma_xlat(struct pcb_t *caller, addr_t vaddr, uint64_t *phyaddr)
{
  int fpn = 0;
  addr_t pgn = vaddr / PAGING64_PAGESZ;
  addr_t off = vaddr % PAGING64_PAGESZ;

  if (pg_getpage_locked(caller->mm, (addr_t) pgn, &fpn, caller) != 0)
    return -1;                       /* trang khong hop le */

  *phyaddr = (uint64_t) fpn * PAGING64_PAGESZ + off;
  return 0;
}
/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller - process goi
 *@src: source region - Vung nho nguon (se dc copy)
 *@dst: destination region - Vung nho dich cua du lieu copy qua
 *@offset: memory offset  
 *@size: copied data size
 */
// Day la interface cho dma va don gian la nhet con viec vao hang doi de thuc hien
int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  uint64_t sphy = 0, dphy = 0;

  /* [SUA 1] Kiem tra them caller->krnl->mram != NULL de dam bao an toan khi copy phan duoi le */
  if (caller == NULL || caller->mm == NULL || caller->krnl == NULL ||
      caller->krnl->mram == NULL || size == 0)
    return -1;

  /* --- 1. Thanh ghi (reg_index) -> dia chi ao -> region & rgid -------
   * Theo dac ta lenh "dma_memcpy 1 2 0 4": src=1, dst=2 la chi so thanh ghi
   * giu dia chi dau cua vung nho. Ta tra nguoc qua symrgtbl de lay dung
   * struct region va rgid thuc te (phuc vu kiem tra bien va dma_conflict). */
  if (src >= MAXREG || dst >= MAXREG)
    return -1;

  /* [SUA 2] Khoa mmvm_lock khi doc symrgtbl va dich bang trang de tranh race condition voi page fault/swap */
  pthread_mutex_lock(&mmvm_lock);

  addr_t src_addr = (addr_t)caller->regs[src];
  addr_t dst_addr = (addr_t)caller->regs[dst];
  int sourcergid = -1, destinationrgid = -1;

  struct vm_rg_struct *sourceRegion = get_symrg_byaddr(caller->mm, src_addr, &sourcergid);
  struct vm_rg_struct *destinyRegion = get_symrg_byaddr(caller->mm, dst_addr, &destinationrgid);
  if (sourceRegion == NULL || destinyRegion == NULL || sourcergid < 0 || destinationrgid < 0) {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* --- 1b. Kiem tra region da cap phat va [offset, offset+size) hop le --- */
  if (validate_mem_region(caller->mm, sourceRegion->vmaid,
                        sourcergid, offset, size) < 0 ||
      validate_mem_region(caller->mm, destinyRegion->vmaid,
                        destinationrgid, offset, size) < 0) {
#ifdef DMADEBUG
    printf("libmem_dma_memcpy: vung source hoac destination chua cap phat");
#endif
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* --- 2, 3, 4. Cat nho theo ranh gioi trang 4KB, dich dia chi & nap DMA ---
   * Cac trang ao lien ke co the nam o cac frame vat ly khong lien ke trong RAM.
   * Vi vay phai chia nho khoi copy tai moi ranh gioi PAGING64_PAGESZ (4096B),
   * dich dia chi vat ly cho tung doan roi nap bat dong bo vao hang doi DMA. */
  addr_t cur_src = sourceRegion->rg_start + offset;
  addr_t cur_dst = destinyRegion->rg_start + offset;
  uint32_t remaining = size;

  while (remaining > 0) {
    uint32_t src_page_rem = (uint32_t)(PAGING64_PAGESZ - (cur_src % PAGING64_PAGESZ));
    uint32_t dst_page_rem = (uint32_t)(PAGING64_PAGESZ - (cur_dst % PAGING64_PAGESZ));

    uint32_t chunk = remaining;
    if (src_page_rem < chunk) chunk = src_page_rem;
    if (dst_page_rem < chunk) chunk = dst_page_rem;

    /* Dich dia chi ao -> vat ly cho trang hien tai */
    if (dma_xlat(caller, cur_src, &sphy) != 0 ||
        dma_xlat(caller, cur_dst, &dphy) != 0) {
#ifdef DMADEBUG
      printf("libmem_dma_memcpy: chua dich duoc dia chi tai vaddr src=%lu dst=%lu\n",
             (unsigned long)cur_src, (unsigned long)cur_dst);
#endif
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
    }

    /* [SUA 3] Phan cung RTL yeu cau source, destination va length deu chia het cho 4.
     * - Neu sphy va dphy cung do lech mod 4: copy `lead` byte dau bang CPU de can thang
     *   bien 4-byte, phan giua day qua DMA phan cung, phan duoi le copy bang CPU.
     * - Neu sphy va dphy lech pha khac nhau: fallback copy theo byte bang CPU. */
    uint32_t src_mis = (uint32_t)(sphy & 3ULL);
    uint32_t dst_mis = (uint32_t)(dphy & 3ULL);
    uint32_t lead_bytes = 0;
    uint32_t aligned_len = 0;
    uint32_t tail_bytes = 0;

    if (src_mis == dst_mis) {
      lead_bytes = (4U - src_mis) & 3U;
      if (lead_bytes > chunk) lead_bytes = chunk;
      uint32_t after_lead = chunk - lead_bytes;
      aligned_len = after_lead & ~3U;
      tail_bytes  = after_lead & 3U;
    } else {
      lead_bytes = chunk;
    }

    /* Copy phan dau le (neu co) de can bien 4-byte */
    for (uint32_t b = 0; b < lead_bytes; b++) {
      BYTE val = 0;
      if (MEMPHY_read(caller->krnl->mram, sphy + b, &val) < 0 ||
          MEMPHY_write(caller->krnl->mram, dphy + b, val) < 0) {
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
      }
    }

    if (aligned_len > 0) {
      uint64_t dma_sphy = sphy + lead_bytes;
      uint64_t dma_dphy = dphy + lead_bytes;
      addr_t   dma_vsrc = cur_src + lead_bytes;
      addr_t   dma_vdst = cur_dst + lead_bytes;

      /* [SYNC-DMA] Giu thu tu FIFO bang dma_submit_lock, nha dma_job_lock & mmvm_lock
       * truoc khi goi do_dma_start() de tranh deadlock */
      pthread_mutex_lock(&dma_submit_lock);

      pthread_mutex_lock(&dma_job_lock);
      if (dma_job_count >= DMA_JOB_MAX) {
        pthread_mutex_unlock(&dma_job_lock);
        pthread_mutex_unlock(&dma_submit_lock);
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
      }
      int tail = (dma_job_head + dma_job_count) % DMA_JOB_MAX;
      dma_jobs[tail].pid       = caller->pid;
      dma_jobs[tail].mm        = caller->mm;
      dma_jobs[tail].src_rg    = (uint32_t)sourcergid;
      dma_jobs[tail].dst_rg    = (uint32_t)destinationrgid;
      dma_jobs[tail].src_start = dma_vsrc;
      dma_jobs[tail].src_end   = dma_vsrc + aligned_len;
      dma_jobs[tail].dst_start = dma_vdst;
      dma_jobs[tail].dst_end   = dma_vdst + aligned_len;
      dma_jobs[tail].len       = aligned_len;
      dma_job_count++;
      pthread_mutex_unlock(&dma_job_lock);
      pthread_mutex_unlock(&mmvm_lock);

      int rc = do_dma_start(caller, dma_sphy, dma_dphy, (uint64_t)aligned_len);
      if (rc >= 0) {
#ifdef DMADEBUG
        printf("[Libmem_dma_memcpy] Return code: %d, Dua cong viec vao hang doi, RegionId Source: %d, RegionId Destination: %d\n",
               rc, sourcergid, destinationrgid);
#endif
      } else {
        pthread_mutex_lock(&dma_job_lock);
        if (dma_job_count > 0) dma_job_count--;
        pthread_mutex_unlock(&dma_job_lock);
      }
      pthread_mutex_unlock(&dma_submit_lock);

      if (rc < 0) {
#ifdef DMADEBUG
        printf("libmem_dma_memcpy: offset ngoai mien hoac hang doi DMA day\n");
#endif
        return -1;
      }

#ifdef DMADEBUG
      printf("\tDMA: PID %u %s reg%u(rg%d) -> reg%u(rg%d), %u byte (dau %u, DMA %u, duoi %u), CPU chay tiep\n",
             caller->pid, rc == 0 ? "nap lenh" : "kenh ban, XEP HANG",
             src, sourcergid, dst, destinationrgid, chunk, lead_bytes, aligned_len, tail_bytes);
#endif

      pthread_mutex_lock(&mmvm_lock);
    }

    /* Copy phan duoi le 1..3 byte cuoi cung (neu co) */
    uint32_t tail_off = lead_bytes + aligned_len;
    for (uint32_t b = 0; b < tail_bytes; b++) {
      BYTE val = 0;
      if (MEMPHY_read(caller->krnl->mram, sphy + tail_off + b, &val) < 0 ||
          MEMPHY_write(caller->krnl->mram, dphy + tail_off + b, val) < 0) {
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
      }
    }

    cur_src += chunk;
    cur_dst += chunk;
    remaining -= chunk;
  }

  pthread_mutex_unlock(&mmvm_lock);
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
  pthread_mutex_lock(&kmm_lock);
  pthread_mutex_lock(&mmvm_lock);
  // B1: Kiem tra hai region va toan bo khoang copy truoc khi ghi du lieu
  if (caller == NULL || caller->mm == NULL ||
      caller->krnl == NULL || caller->krnl->mm == NULL ||
      size == 0 || source >= 10 || destination >= 10) {
    pthread_mutex_unlock(&mmvm_lock);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  int SourceRgid=-1 ;
  int destinationRgid =-1;
  addr_t src_addr = caller->regs[source];
  addr_t dst_addr = caller->regs[destination];
 
  struct vm_rg_struct *user_region= get_symrg_byaddr(caller->mm, src_addr, &SourceRgid);
  struct vm_rg_struct *kernel_region =get_symrg_byaddr(caller->krnl->mm,dst_addr,&destinationRgid);
  

  if (user_region == NULL || kernel_region == NULL ||SourceRgid==-1 || destinationRgid==-1){
    pthread_mutex_unlock(&mmvm_lock);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
    // B2: Lay VMA ID tu tung region
  int user_vmaid = user_region->vmaid;
  int kernel_vmaid = kernel_region->vmaid;

  if (user_vmaid < 0 || kernel_vmaid < 0){
    pthread_mutex_unlock(&mmvm_lock);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  
  }
 
   // B3: Kiem tra toan bo khoang copy truoc khi ghi
  if (validate_mem_region(caller->mm, user_vmaid,
                          SourceRgid, offset, size) < 0 ||
      validate_mem_region(caller->krnl->mm, kernel_vmaid,
                          destinationRgid, offset, size) < 0){
      pthread_mutex_unlock(&mmvm_lock);
      pthread_mutex_unlock(&kmm_lock);
      return -1;
  }
  // B4: Copy tung byte tu User MM sang Kernel MM
  for (uint32_t i = 0; i < size; i++)
  {
    BYTE data;

    if (__read_user_mem(caller, user_vmaid,
                        SourceRgid, (addr_t)offset + i, &data) < 0 ||
        __write_kernel_mem(caller, kernel_vmaid,
                           destinationRgid, (addr_t)offset + i, data) < 0){
      pthread_mutex_unlock(&mmvm_lock);
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  }
   pthread_mutex_unlock(&mmvm_lock);
   pthread_mutex_unlock(&kmm_lock);
   
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
  pthread_mutex_lock(&kmm_lock);
  pthread_mutex_lock(&mmvm_lock);
  if (caller == NULL || caller->mm == NULL ||
    caller->krnl == NULL || caller->krnl->mm == NULL ||
    size == 0 || source >= 10 || destination >= 10) {
      pthread_mutex_unlock(&mmvm_lock);
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  // B1: Lay region tu Kernel MM va User MM
  // Lấy địa chỉ ảo thực tế từ thanh ghi của tiến trình
  addr_t src_addr = caller->regs[source];
  addr_t dst_addr = caller->regs[destination];

  int SourceRgid = -1;
  int destinationRgid = -1;
  struct vm_rg_struct *kernel_region = get_symrg_byaddr(caller->krnl->mm, src_addr, &SourceRgid);
  struct vm_rg_struct *user_region = get_symrg_byaddr(caller->mm, dst_addr, &destinationRgid);

  if (kernel_region == NULL || user_region == NULL||SourceRgid==-1||destinationRgid==-1){
    pthread_mutex_unlock(&mmvm_lock);
    pthread_mutex_unlock(&kmm_lock);
    return -1;
  }
  // B2: Lay VMA ID tu tung region
  int kernel_vmaid = kernel_region->vmaid;
  int user_vmaid = user_region->vmaid;

  if (kernel_vmaid < 0 || user_vmaid < 0)
    {
      pthread_mutex_unlock(&mmvm_lock);
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  // B3: Kiem tra toan bo khoang copy truoc khi ghi
  if (validate_mem_region(caller->krnl->mm, kernel_vmaid,
                          SourceRgid, offset, size) < 0 ||
      validate_mem_region(caller->mm, user_vmaid,
                        destinationRgid, offset, size) < 0){
      pthread_mutex_unlock(&mmvm_lock);
      pthread_mutex_unlock(&kmm_lock);
      return -1;
  }
  // B4: Copy tung byte tu Kernel MM sang User MM
  for (uint32_t i = 0; i < size; i++)
  {
    BYTE data;

    if (__read_kernel_mem(caller, kernel_vmaid,
                          SourceRgid, (addr_t)offset + i, &data) < 0 ||
        __write_user_mem(caller, user_vmaid,
                         destinationRgid, (addr_t)offset + i, data) < 0){
      pthread_mutex_unlock(&mmvm_lock);
      pthread_mutex_unlock(&kmm_lock);
      return -1;
    }
  }
  pthread_mutex_unlock(&mmvm_lock);
  pthread_mutex_unlock(&kmm_lock);
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

  
  struct mm_struct *kmm = caller->krnl->mm;
  struct vm_rg_struct *region = get_symrg_byid(kmm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(kmm, vmaid);

  // B2: Xac nhan region da cap phat va offset nam trong bien
  if (region == NULL || vma == NULL || region->vmaid != vmaid||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
  
    return -1;
  }

  // B3: Doc byte tu Kernel page table
  int ret = pg_getval_locked(kmm, (addr_t)(region->rg_start + offset), data, caller);
  
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

  
  struct mm_struct *kmm = caller->krnl->mm;
  struct vm_rg_struct *region = get_symrg_byid(kmm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(kmm, vmaid);

  // B2: Xac nhan region da cap phat va offset hop le
  if (region == NULL || vma == NULL ||region->vmaid != vmaid||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
    
    return -1;
  }

  // B3: Ghi byte vao Kernel page table
  int ret = pg_setval_locked(kmm, (addr_t)(region->rg_start + offset), value, caller);
  
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

  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(caller->mm, vmaid);

  // B2: Xac nhan region da cap phat va offset nam trong bien
  if (region == NULL || vma == NULL || region->vmaid!=vmaid ||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
    
    return -1;
  }

  // B3: Doc byte tu User page table
  int ret = pg_getval_locked(caller->mm, (addr_t)(region->rg_start + offset), data, caller);
  
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

  
  struct vm_rg_struct *region = get_symrg_byid(caller->mm, rgid);
  struct vm_area_struct *vma = get_vma_by_num(caller->mm, vmaid);

  // B2: Xac nhan region da cap phat va offset hop le
  if (region == NULL || vma == NULL ||region->vmaid!=vmaid ||
      (region->rg_start == 0 && region->rg_end == 0) ||
      region->rg_start >= region->rg_end || offset >= region->rg_end - region->rg_start)
  {
    
    return -1;
  }

  // B3: Ghi byte vao User page table
  int ret = pg_setval_locked(caller->mm, (addr_t)(region->rg_start + offset), value, caller);
  
  return ret;
}

#ifdef MM64
/* Walk every leaf PT in the 5-level tree and return its RAM/SWAP frames.
  Giai phong page table frame - giai phong cac page tren page table.
*/
static void release_pgtbl_frames(addr_t *table, int level,
                                 struct memphy_struct *mram,
                                 struct memphy_struct *mswp)
{
  //B1: Xac nhan du lieu co dung chuan ko
  if (table == NULL) return;

  if (level == 1) {
    for (size_t i = 0; i < 512; ++i) {
      addr_t pte = table[i];
      if (pte == 0) continue;
      // Co page va dang o ram
      if (PAGING_PAGE_PRESENT(pte))
        MEMPHY_put_freefp(mram, PAGING_FPN(pte));
      else if ((pte & PAGING_PTE_SWAPPED_MASK) && mswp != NULL)
      // No o swap
        MEMPHY_put_freefp(mswp, PAGING_SWP(pte));

      table[i] = 0;
    }
    return;
  }

  for (size_t i = 0; i < 512; ++i) {
    if (table[i] != 0) {
      // Goi de quy de giai phong
      addr_t *child = (addr_t *)(uintptr_t)table[i];
      release_pgtbl_frames(child, level - 1, mram, mswp);
      
    }
  }
}
#endif

/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  // B1 Xac thu du lieu
  if (caller == NULL || caller->krnl == NULL)
    return -1;

  //B1.1 : Lay du lieu can su dung nhu mm, ram swap.
  struct mm_struct *mm = caller->mm;
  struct memphy_struct *mram = caller->krnl->mram;
  struct memphy_struct *mswp = caller->krnl->active_mswp;

  //B1.2 Xac thuc
  if (mm == NULL || mram == NULL)
    return -1;

  pthread_mutex_lock(&mmvm_lock);

  /*B1.3  [SYNC-MM] Khong thu hoi frame RAM khi process van con job DMA dang chay */
  if (libmem_dma_inflight(caller->pid) > 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

#ifdef MM64
  /* Duyet toan bo cay PGD -> P4D -> PUD -> PMD -> PT.
   * Cach nay xu ly ca cac mapping duoc tao truc tiep boi vm_map_ram(),
   * khong phu thuoc mapping co nam trong VMA hay khong.
   */
  release_pgtbl_frames(mm->pgd, 5, mram, mswp);
#else
  // B2: Duyet qua tat ca cac VMA (co che phan trang 32-bit cu)
  struct vm_area_struct *vma = mm->mmap;

  while (vma != NULL)
  {
    addr_t start_pgn = vma->vm_start >> PAGING_ADDR_PGN_LOBIT;
    addr_t end_pgn = (vma->vm_end + PAGING_PAGESZ - 1) >> PAGING_ADDR_PGN_LOBIT;

    for (addr_t pgn = start_pgn; pgn < end_pgn; ++pgn)
    {
      addr_t pte = pte_get_entry(mm, pgn, caller);

      if (pte == 0)
        continue;

      if (PAGING_PAGE_PRESENT(pte))
        MEMPHY_put_freefp(mram, PAGING_FPN(pte));
      else if ((pte & PAGING_PTE_SWAPPED_MASK) && mswp != NULL)
        MEMPHY_put_freefp(mswp, PAGING_SWP(pte));

      pte_set_entry(mm, pgn, 0, caller);
    }

    vma = vma->vm_next;
  }
#endif

  // B3 : Giai phong nhung pcb dang quan ly
  struct pgn_t *pg = mm->fifo_pgn;

  while (pg != NULL)
  {
    struct pgn_t *tmp = pg;
    pg = pg->pg_next;
    free(tmp);
  }

  mm->fifo_pgn = NULL;

  pthread_mutex_unlock(&mmvm_lock);

  return 0;
}
/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page_locked(struct mm_struct *mm, addr_t *retpgn)
{
  // B1 : Kiem tra dau vao va danh sach fifo_pgn
  if (mm == NULL || retpgn == NULL || mm->fifo_pgn == NULL)
  {
    return -1;
  }

  /* TODO: Implement the theorical mechanism to find the victim page */
  // B2 : Duyet tim trang cu nhat (nam gan cuoi danh sach nhat) dang PRESENT trong RAM
  struct pgn_t *curr = mm->fifo_pgn;
  struct pgn_t *prev = NULL;
  struct pgn_t *vic = NULL;
  struct pgn_t *vic_prev = NULL;

  while (curr != NULL)
  {
    addr_t pte = pte_get_entry(mm, curr->pgn, NULL);
    /* [SYNC-MM] Khong chon trang dang truyen DMA lam victim de tranh bi swap-out */
    if (PAGING_PAGE_PRESENT(pte) && !is_page_dma_pinned(mm, curr->pgn))
    {
      vic = curr;
      vic_prev = prev;
    }
    prev = curr;
    curr = curr->pg_next;
  }

  // B3 : Neu khong co trang nao dang PRESENT trong RAM thi tra ve -1
  if (vic == NULL)
  {
    return -1;
  }

  // B4 : Lay so hieu trang nan nhan va go node khoi danh sach fifo_pgn
  *retpgn = vic->pgn;

  if (vic_prev != NULL) {
    vic_prev->pg_next = vic->pg_next;
  } else {
    mm->fifo_pgn = vic->pg_next;
  }

  free(vic);

  return 0;
}

/* Safe public entry point for callers outside the locked VM paths. */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  libmem_mmvm_lock();
  int ret = find_victim_page_locked(mm, retpgn);
  libmem_mmvm_unlock();
  return ret;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
// Ghi chu : don gian khi lay thi ta chi lay thong tin, lay xong ma cai khoi region do ma het vi xoa khoi free_list
// Nguoc lai voi neu ma tra ve free_list thi tao cai moi.
// Yeu cau lay 1 vung (region) co kich thuoc size thuoc area co id la vmaid gan no vao newrg
// Neu duoc tra ve 0
// else ve -1 
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, uint64_t size, struct vm_rg_struct *newrg)
{
  // B1 : xac nhan input
  if(caller==NULL || caller->mm ==NULL||newrg==NULL||size==0){
    return -1;

  }
  // B2  lay khu vuc nho va con tro cua vung free (ranh roi) de dung
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  
  struct vm_rg_struct *rgit = ( (cur_vma ==NULL)? NULL:cur_vma->vm_freerg_list );

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = 0;


  // B3 Tim xem co vung nho nao co size phu hop ko
  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start <= rgit->rg_end &&
    size <= rgit->rg_end - rgit->rg_start)
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
      { 
        /*Use up all space, remove current node */
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

  if (newrg->rg_end == 0) // new region not found
    return -1;

  return 0;
}

// #endif
