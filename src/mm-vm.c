/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

//#ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "string.h"
#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <limits.h>

extern void libmem_mmvm_lock(void);
extern void libmem_mmvm_unlock(void);
extern addr_t vm_map_ram_locked(struct pcb_t *caller, addr_t astart,
                                addr_t aend, addr_t mapstart,
                                int incpgnum,
                                struct vm_rg_struct *ret_rg);

/*
 * get_vma_by_num - find a VMA by its exact ID.
 *
 * Do not assume that the list is contiguous or sorted by ID. In particular,
 * returning the next larger ID for a missing ID can make an allocation land
 * in the wrong SMALL/MEDIUM/LARGE area.
 */
struct vm_area_struct *get_vma_by_num_locked(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *vma;

  if (mm == NULL || vmaid < 0)
    return NULL;

  for (vma = mm->mmap; vma != NULL; vma = vma->vm_next) {
    if (vma->vm_id == (unsigned long)vmaid)
      return vma;
  }

  return NULL;
}

struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  libmem_mmvm_lock();
  struct vm_area_struct *ret = get_vma_by_num_locked(mm, vmaid);
  libmem_mmvm_unlock();
  return ret;
}

/*
 * __mm_swap_page - Thực hiện sao chép dữ liệu một trang vật lý từ RAM ra vùng nhớ SWAP
 * @caller: Tiến trình yêu cầu hoán đổi trang
 * @vicfpn: Frame Page Number (khung trang nạn nhân) trên RAM chính (mram)
 * @swpfpn: Frame Page Number đích trên thiết bị SWAP (active_mswp)
 * Return: 0 nếu thành công
 */
int __mm_swap_page_locked(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL || caller->krnl->active_mswp == NULL)
    return -1;

  struct memphy_struct *mram = caller->krnl->mram;
  struct memphy_struct *mswp = caller->krnl->active_mswp;
  if (vicfpn >= mram->maxsz / PAGING64_PAGESZ ||
      swpfpn >= mswp->maxsz / PAGING64_PAGESZ)
    return -1;
  
  return __swap_cp_page(mram, vicfpn, mswp, swpfpn);
}

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
  libmem_mmvm_lock();
  int ret = __mm_swap_page_locked(caller, vicfpn, swpfpn);
  libmem_mmvm_unlock();
  return ret;
}

/*get_vm_area_node - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
static struct vm_rg_struct *get_vm_area_node_at_brk_locked(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct mm_struct *mm;
  struct vm_area_struct *vma;
  struct vm_rg_struct *newrg;

  mm = (caller != NULL && caller->mm != NULL)
          ? caller->mm
          : ((caller != NULL && caller->krnl != NULL)
                  ? caller->krnl->mm : NULL);

  if (mm == NULL || alignedsz == 0 ||
      (alignedsz % PAGING64_PAGESZ) != 0)
    return NULL;

  vma = get_vma_by_num_locked(mm, vmaid);
  if (vma == NULL || vma->sbrk < vma->vm_start ||
      vma->sbrk > vma->vm_end ||
      (vma->sbrk % PAGING64_PAGESZ) != 0 ||
      alignedsz > vma->vm_end - vma->sbrk)
    return NULL;

  newrg = init_vm_rg(vma->sbrk, vma->sbrk + alignedsz);
  if (newrg == NULL)
    return NULL;

  newrg->vmaid = vmaid;
  return newrg;
}

struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  libmem_mmvm_lock();
  struct vm_rg_struct *ret = get_vm_area_node_at_brk_locked(caller, vmaid, alignedsz);
  libmem_mmvm_unlock();
  return ret;
}
/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
static int validate_overlap_vm_area_locked(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct mm_struct *mm;
  struct vm_area_struct *cur_vma;
  struct vm_area_struct *vma;

  mm = (caller != NULL && caller->mm != NULL)
          ? caller->mm
          : ((caller != NULL && caller->krnl != NULL)
                  ? caller->krnl->mm : NULL);

  if (mm == NULL || vmastart >= vmaend)
    return -1;

  cur_vma = get_vma_by_num_locked(mm, vmaid);
  if (cur_vma == NULL ||
      cur_vma->vm_start > cur_vma->vm_end ||
      vmastart < cur_vma->vm_start ||
      vmaend > cur_vma->vm_end)
    return -1;

  for (vma = mm->mmap; vma != NULL; vma = vma->vm_next) {
    if (vma == cur_vma)
      continue;

    if (vma->vm_start > vma->vm_end)
      return -1;

    /* Half-open interval overlap: [a,b) intersects [c,d). */
    if (vmastart < vma->vm_end && vma->vm_start < vmaend)
      return -1;
  }

  return 0;
}

int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  libmem_mmvm_lock();
  int ret = validate_overlap_vm_area_locked(caller, vmaid, vmastart, vmaend);
  libmem_mmvm_unlock();
  return ret;
}

/* Round a non-zero byte count up to a page multiple without integer overflow. */
static int align_size_to_page(addr_t size, addr_t *aligned_size)
{
  const addr_t page_size = (addr_t)PAGING64_PAGESZ;
  const addr_t max_addr = (addr_t)-1;
  addr_t remainder;
  addr_t padding;

  if (aligned_size == NULL || size == 0 || page_size == 0)
    return -1;

  remainder = size % page_size;
  padding = (remainder == 0) ? 0 : page_size - remainder;

  if (size > max_addr - padding)
    return -1;

  *aligned_size = size + padding;
  return 0;
}

/*
 * inc_vma_limit - map additional pages at the current VMA break.
 *
 * vm_end is the exclusive, pre-reserved upper bound of the VMA. It must not
 * be moved when an allocation grows; only sbrk advances. This is essential
 * for keeping the SMALL/MEDIUM/LARGE address areas disjoint.
 */
static int inc_vma_limit_locked(struct pcb_t *caller, int vmaid,
                                addr_t inc_sz)
{
  struct mm_struct *mm;
  struct vm_area_struct *vma;
  struct vm_rg_struct *new_freerg;
  struct vm_rg_struct mapped_rg;
  addr_t aligned_inc_sz;
  addr_t old_sbrk;
  addr_t new_sbrk;
  addr_t page_count;
  addr_t map_result;

  mm = (caller != NULL && caller->mm != NULL)
           ? caller->mm
           : ((caller != NULL && caller->krnl != NULL)
                  ? caller->krnl->mm : NULL);
  if (mm == NULL || caller == NULL || caller->krnl == NULL ||
      caller->krnl->mram == NULL || align_size_to_page(inc_sz, &aligned_inc_sz) < 0)
    return -1;

  vma = get_vma_by_num_locked(mm, vmaid);
  if (vma == NULL)
    return -1;

  old_sbrk = vma->sbrk;
  if (old_sbrk < vma->vm_start || old_sbrk > vma->vm_end ||
      (old_sbrk % PAGING64_PAGESZ) != 0 ||
      aligned_inc_sz > vma->vm_end - old_sbrk)
    return -1;

  new_sbrk = old_sbrk + aligned_inc_sz;
  if (validate_overlap_vm_area_locked(caller, vmaid, old_sbrk, new_sbrk) < 0)
    return -1;

  page_count = aligned_inc_sz / PAGING64_PAGESZ;
  if (page_count == 0 || page_count > (addr_t)INT_MAX)
    return -1;

  /* Allocate metadata before mapping so an allocation failure cannot leave
   * mapped pages without a corresponding free-region node. */
  new_freerg = init_vm_rg(old_sbrk, new_sbrk);
  if (new_freerg == NULL)
    return -1;
  new_freerg->vmaid = vmaid;

  map_result = vm_map_ram_locked(caller, vma->vm_start, vma->vm_end,
                                 old_sbrk, (int)page_count, &mapped_rg);
  if (map_result != 0) {
    free(new_freerg);
    return -1;
  }

  /* The VMA's reserved bounds stay unchanged; only the committed break moves. */
  vma->sbrk = new_sbrk;
  if (enlist_vm_rg_node(&vma->vm_freerg_list, new_freerg) < 0) {
    /* With valid non-NULL arguments enlist_vm_rg_node cannot fail. Keep a
     * defensive error path in case its implementation changes later. */
    vma->sbrk = old_sbrk;
    free(new_freerg);
    return -1;
  }

  return 0;
}

/* Public interface for syscall and other callers. The complete VMA growth
 * transaction, including frame allocation and victim replacement, is locked. */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  libmem_mmvm_lock();
  int ret = inc_vma_limit_locked(caller, vmaid, inc_sz);
  libmem_mmvm_unlock();
  return ret;
}

// #endif
