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

  if (mm->mmap == NULL)
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
  struct vm_rg_struct * newrg;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  newrg = malloc(sizeof(struct vm_rg_struct));

  /* Start tại điểm ngắt cấp phát hiện tại (sbrk) */
  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = cur_vma->sbrk + alignedsz;
  newrg->rg_next = NULL;

  return newrg;
}

/*validate_overlap_vm_area */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct vm_area_struct *vma = caller->mm->mmap;

  if (vmastart >= vmaend)
  {
    return -1;
  }

  if (vma == NULL)
  {
    return -1;
  }

  struct vm_area_struct *cur_area = get_vma_by_num(caller->mm, vmaid);
  if (cur_area == NULL)
  {
    return -1;
  }

  while (vma != NULL)
  {
    if (vma != cur_area && OVERLAP(cur_area->vm_start, cur_area->vm_end, vma->vm_start, vma->vm_end))
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
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (!cur_vma) return -1;
  
  /* Căn chỉnh kích thước trang */
  addr_t alignedsz = inc_sz; 
#ifdef MM64
  addr_t num_pages = (alignedsz + PAGING64_PAGESZ - 1) / PAGING64_PAGESZ;
  alignedsz = num_pages * PAGING64_PAGESZ;
#endif

  addr_t vmastart = cur_vma->sbrk;
  addr_t vmaend = vmastart + alignedsz;

  if (validate_overlap_vm_area(caller, vmaid, vmastart, vmaend) < 0)
     return -1;  //Overlap and failed allocation 

  /* Lấy node bộ nhớ ảo mới */
  struct vm_rg_struct *newrg = get_vm_area_node_at_brk(caller, vmaid, alignedsz);

  /* Yêu cầu ánh xạ phần bộ nhớ ảo này xuống RAM thực */
  if (vm_map_ram(caller, cur_vma->vm_start, cur_vma->vm_end, newrg->rg_start, num_pages, newrg) < 0) {
      free(newrg);
      return -1; 
  }

  /* Nới rộng limit (sbrk) */
  cur_vma->sbrk += alignedsz;
  cur_vma->vm_end = cur_vma->sbrk;
  
  /* Đưa vùng mới cấp vào danh sách rảnh (free region list) để hệ thống quản lý biến */
  enlist_vm_rg_node(&cur_vma->vm_freerg_list, newrg);

  return 0;
}

// #endif