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

/*get_vma_by_num - get vm area by numID */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *pvma = mm->mmap;

  if (mm->mmap == NULL || pvma == NULL)
    return NULL;

  int vmait = pvma->vm_id;

  while (vmait < vmaid)
  {
    if (pvma == NULL)
      return NULL;

    pvma = pvma->vm_next;
    if(pvma != NULL) vmait = pvma->vm_id;
  }

  return pvma;
}

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
    __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
    return 0;
}

/*get_vm_area_node - get vm area for a number of pages */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct vm_rg_struct * newrg = NULL;
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  struct vm_area_struct *cur_vma = get_vma_by_num(mm, vmaid);

  if (cur_vma == NULL)
    return NULL;

  newrg = malloc(sizeof(struct vm_rg_struct));
  if (newrg == NULL)
    return NULL;

  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = cur_vma->sbrk + alignedsz;
  newrg->rg_next = NULL;

  return newrg;
}

/*validate_overlap_vm_area */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (mm == NULL)
    return -1;
    
  struct vm_area_struct *vma = mm->mmap;

  if (vmastart >= vmaend)
  {
    return -1;
  }

  if (vma == NULL)
  {
    return -1;
  }

  struct vm_area_struct *cur_area = get_vma_by_num(mm, vmaid);
  if (cur_area == NULL)
  {
    return -1;
  }

  while (vma != NULL)
  {
    if (vma != cur_area && OVERLAP(vmastart, vmaend, vma->vm_start, vma->vm_end))
    {
      return -1;
    }
    vma = vma->vm_next;
  }

  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (mm == NULL)
    return -1;

  struct vm_area_struct *cur_vma = get_vma_by_num(mm, vmaid);
  if (cur_vma == NULL)
    return -1;

  /* Align size to page boundary */
  addr_t aligned_inc_sz = PAGING64_PAGE_ALIGNSZ(inc_sz);
  int incpgnum = aligned_inc_sz / PAGING64_PAGESZ;
  addr_t old_sbrk = cur_vma->sbrk;
  addr_t new_sbrk = old_sbrk + aligned_inc_sz;

  /* Validate overlap */
  if (validate_overlap_vm_area(caller, vmaid, old_sbrk, new_sbrk) < 0)
    return -1;

  /* Expand boundary if needed */
  if (new_sbrk > cur_vma->vm_end)
  {
    cur_vma->vm_end = new_sbrk;
  }

  /* Cấp khung vật lý và ánh xạ PTE ngay khi mở rộng vùng nhớ */
  struct vm_rg_struct ret_rg;
if (vm_map_ram(caller, cur_vma->vm_start, cur_vma->vm_end, old_sbrk, incpgnum, &ret_rg) == (addr_t)-1)
  {
    cur_vma->vm_end = old_sbrk;
    return -1;
  }

  /* Update sbrk */
 cur_vma->sbrk = new_sbrk;
  if (new_sbrk / PAGING64_PAGESZ > 512)
  return -1;
  struct vm_rg_struct *new_freerg = init_vm_rg(old_sbrk, new_sbrk);
  if (new_freerg == NULL)
    return -1;
  enlist_vm_rg_node(&cur_vma->vm_freerg_list, new_freerg);

  return 0;
}

// #endif
