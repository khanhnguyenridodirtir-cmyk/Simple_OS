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

/*get_vma_by_num - get vm area by numID
 *@mm: memory region
 *@vmaid: ID vm area to alloc memory region
 *
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *pvma = mm->mmap;

  // Kiem tra danh sach vung nho rong
  if (mm->mmap == NULL || pvma == NULL) return NULL;

  int vmait = pvma->vm_id;

  // Duyet tuan tu qua danh sach lien ket cac VMA cho den khi dat ID mong muon
  while (vmait < vmaid)
  {
    if (pvma == NULL)
      return NULL;

    pvma = pvma->vm_next;
    if(pvma != NULL) vmait = pvma->vm_id;
  }

  return pvma;
}

/*
 * __mm_swap_page - Thực hiện sao chép dữ liệu một trang vật lý từ RAM ra vùng nhớ SWAP
 * @caller: Tiến trình yêu cầu hoán đổi trang
 * @vicfpn: Frame Page Number (khung trang nạn nhân) trên RAM chính (mram)
 * @swpfpn: Frame Page Number đích trên thiết bị SWAP (active_mswp)
 * Return: 0 nếu thành công
 */
int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
    __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
    return 0;
}

/*get_vm_area_node - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct vm_rg_struct *newrg = NULL;

  // Lay cau truc mm an toan tu ngu canh User hoac Kernel 
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  struct vm_area_struct *cur_vma = get_vma_by_num(mm, vmaid);

  if (cur_vma == NULL) return NULL;

  // Cap phat dong cho node quan ly vung nho moi
  newrg = malloc(sizeof(struct vm_rg_struct));
  if (newrg == NULL) return NULL;

  // Thiet lap ranh gioi bat dau tu dinh sbrk cu va mo rong them mot doan alignedsz
  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end = cur_vma->sbrk + alignedsz;
  newrg->rg_next = NULL;

  return newrg;
}

/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (mm == NULL) return -1;
    
  struct vm_area_struct *vma = mm->mmap;

  // Kiem tra tinh hop le co ban cua khoang dia chi
  if (vmastart >= vmaend)
  {
    return -1;
  }

  if (vma == NULL)
  {
    return -1;
  }

  // Lay con tro toi VMA hien tai de loai tru chinh no khi duyet kiem tra chong lan
  struct vm_area_struct *cur_area = get_vma_by_num(mm, vmaid);
  if (cur_area == NULL)
  {
    return -1;
  }

  // Duyet toan bo danh sach cac VMA cua tien trinh
  while (vma != NULL)
  {
    // Neu la VMA khac va co giao nhau ve khong gian dia chi -> bao loi
    if (vma != cur_area && OVERLAP(vmastart, vmaend, vma->vm_start, vma->vm_end))
    {
      return -1;
    }
    vma = vma->vm_next;
  }

  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  struct mm_struct *mm = (caller && caller->mm) ? caller->mm : ((caller && caller->krnl) ? caller->krnl->mm : NULL);
  if (mm == NULL) return -1;

  struct vm_area_struct *cur_vma = get_vma_by_num(mm, vmaid);
  if (cur_vma == NULL) return -1;

  // 1. Lam tron kich thuoc xin cap phat len boi so cua kich thuoc trang (Page Size)
  addr_t aligned_inc_sz = PAGING64_PAGE_ALIGNSZ(inc_sz);
  int incpgnum = aligned_inc_sz / PAGING64_PAGESZ;
  addr_t old_sbrk = cur_vma->sbrk;
  addr_t new_sbrk = old_sbrk + aligned_inc_sz;

  // 2. Kiem tra vung mo rong moi [old_sbrk, new_sbrk) co dung cham voi VMA khac khong
  if (validate_overlap_vm_area(caller, vmaid, old_sbrk, new_sbrk) < 0)
    return -1;

  // 3. Kiem tra gioi han so luong trang toi da (512 entries) truoc khi cap phat
  if (new_sbrk / PAGING64_PAGESZ > 512)
    return -1;
  
  // 4. Luu lai gioi han cu de phuc hoi (rollback) neu buoc anh xa RAM ben duoi that bai
  addr_t old_vm_end = cur_vma->vm_end;
  if (new_sbrk > cur_vma->vm_end)
  {
    cur_vma->vm_end = new_sbrk;
  }

  // 5. Cap phat khung trang vat ly (RAM) va anh xa ngay vao bang phan trang (PTE)
  struct vm_rg_struct ret_rg;
  if (vm_map_ram(caller, cur_vma->vm_start, cur_vma->vm_end, old_sbrk, incpgnum, &ret_rg) == (addr_t)-1)
  {
    cur_vma->vm_end = old_vm_end; // Khoi phuc lai vm_end ban dau
    return -1;
  }

  // 6. Cap nhat dinh sbrk moi
  cur_vma->sbrk = new_sbrk;

  // 7. Dong goi vung nho vua duoc mo rong thanh mot free region node 
  // va dua vao danh sach quan ly vung trong
  struct vm_rg_struct *new_freerg = init_vm_rg(old_sbrk, new_sbrk);
  if (new_freerg == NULL)
    return -1;
  enlist_vm_rg_node(&cur_vma->vm_freerg_list, new_freerg);

  return 0;
}

// #endif
