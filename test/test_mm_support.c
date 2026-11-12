#include "syscall.h"

struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  struct vm_area_struct *vma = mm == NULL ? NULL : mm->mmap;
  while (vma != NULL && (int)vma->vm_id != vmaid)
    vma = vma->vm_next;
  return vma;
}

int _syscall(struct krnl_t *krnl, uint32_t pid, uint32_t syscall_num,
             struct sc_regs *regs)
{
  (void)krnl;
  (void)pid;
  (void)syscall_num;
  (void)regs;
  return -1;
}