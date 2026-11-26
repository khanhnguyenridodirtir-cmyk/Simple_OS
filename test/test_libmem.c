#include "os-cfg.h"
#include "common.h"
#include "mm.h"
#include "mm64.h"
#include "libmem.h"
#include "queue.h"
#include "../include/syscall.h"
#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>
/* Khai bao dung kieu 64-bit addr_t voi src/libmem.c */
int free_pcb_memph(struct pcb_t *caller);
int pg_getpage(struct mm_struct *mm, addr_t pgn, int *fpn, struct pcb_t *caller);
int pg_getval(struct mm_struct *mm, addr_t addr, BYTE *data, struct pcb_t *caller);
int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller);


/*
 * Stub _syscall va bang syscall de link doc lap test_libmem
 */
const char *sys_call_table[] = {
    "sys_ni_syscall",
    "sys_memmap"
};
const int syscall_table_size = 2;

int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs);

int _syscall(struct krnl_t *krnl, uint32_t pid, uint32_t nr, struct sc_regs *regs)
{
    if (nr == 17) {
        return __sys_memmap(krnl, pid, regs);
    }
    return -1;
}

struct lm_test_env {
    struct krnl_t krnl;
    struct memphy_struct mram;
    struct memphy_struct mswp;
    struct memphy_struct *mswp_arr[PAGING_MAX_MMSWP];
    struct mm_struct kmm;
    struct queue_t running_list;
    int mram_ready;
    int mswp_ready;
    int kmm_ready;
};

static void lm_cleanup_mm_structs(struct mm_struct *mm)
{
    if (mm == NULL)
        return;

    while (mm->fifo_pgn != NULL) {
        struct pgn_t *tmp = mm->fifo_pgn;
        mm->fifo_pgn = mm->fifo_pgn->pg_next;
        free(tmp);
    }

    struct vm_area_struct *vma = mm->mmap;
    while (vma != NULL) {
        struct vm_rg_struct *rg = vma->vm_freerg_list;
        while (rg != NULL) {
            struct vm_rg_struct *rtmp = rg;
            rg = rg->rg_next;
            free(rtmp);
        }
        struct vm_area_struct *vtmp = vma;
        vma = vma->vm_next;
        free(vtmp);
    }

#ifdef MM64
    mm_free_page_tables(mm);
#else
    free(mm->pgd);
#endif
}

static void lm_env_init(struct lm_test_env *env, int ram_frames, int swp_frames)
{
    memset(env, 0, sizeof(*env));
    addr_t page_sz = (addr_t)test_memphy_page_size();

    if (ram_frames > 0) {
        env->mram_ready = (test_memphy_init(&env->mram, (addr_t)ram_frames * page_sz, 1) == 0);
    }
    if (swp_frames > 0) {
        env->mswp_ready = (test_memphy_init(&env->mswp, (addr_t)swp_frames * page_sz, 1) == 0);
    }

    env->mswp_arr[0] = env->mswp_ready ? &env->mswp : NULL;
    env->krnl.mram = env->mram_ready ? &env->mram : NULL;
    env->krnl.mswp = env->mswp_arr;
    env->krnl.active_mswp = env->mswp_ready ? &env->mswp : NULL;
    env->krnl.active_mswp_id = 0;
    env->krnl.running_list = &env->running_list;

    init_mm(&env->kmm, NULL);
    env->krnl.mm = &env->kmm;
    env->kmm_ready = 1;
}

static void lm_proc_init(struct lm_test_env *env, struct pcb_t *proc,
                         struct mm_struct *mm, uint32_t pid)
{
    memset(proc, 0, sizeof(*proc));
    memset(mm, 0, sizeof(*mm));
    proc->pid = pid;
    proc->krnl = &env->krnl;
    proc->mm = mm;
    init_mm(mm, proc);

    /* Khoi tao cac thanh ghi ve gia tri khong hop le (addr_t)-1 de tranh nham voi dia chi 0 */
    for (int i = 0; i < MAXREG; i++) {
        proc->regs[i] = (addr_t)-1;
    }

    if (env->running_list.size < MAX_QUEUE_SIZE) {
        env->running_list.proc[env->running_list.size++] = proc;
    }
}

static void lm_proc_destroy(struct pcb_t *proc, struct mm_struct *mm)
{
    free_pcb_memph(proc);
    lm_cleanup_mm_structs(mm);
}

static void lm_env_destroy(struct lm_test_env *env)
{
    if (env->kmm_ready) {
        struct pcb_t dummy = {0};
        dummy.krnl = &env->krnl;
        dummy.mm = &env->kmm;
        for (int pgn = 0; pgn < 512; ++pgn) {
            addr_t pte = pte_get_entry(&env->kmm, (addr_t)pgn, &dummy);
            if (PAGING_PAGE_PRESENT(pte) && env->mram_ready) {
                MEMPHY_put_freefp(&env->mram, PAGING_FPN(pte));
            }
        }
        lm_cleanup_mm_structs(&env->kmm);
    }
    test_memphy_destroy(&env->mram, env->mram_ready);
    test_memphy_destroy(&env->mswp, env->mswp_ready);
}

static int regions_disjoint(addr_t s1, addr_t e1, addr_t s2, addr_t e2)
{
    return (e1 <= s2) || (e2 <= s1);
}

/* LM01: ALLOC kích thước nhỏ (VMA_SMALL) */
static void run_lm01(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 8);
    lm_proc_init(&env, &proc, &mm, 101);

    int ret = liballoc(&proc, 128, 0);
    TEST_ASSERT_EQ(0, ret);
    printf("DEBUG: sizeof(addr_t)=%zu, sizeof(pcb_t)=%zu, regs[0]=%lu, sym0=[%lu,%lu) vmaid=%d\n",
       sizeof(addr_t), sizeof(struct pcb_t),
       (unsigned long)proc.regs[0],
       (unsigned long)mm.symrgtbl[0].rg_start,
       (unsigned long)mm.symrgtbl[0].rg_end,
       mm.symrgtbl[0].vmaid);
    int rgid = -1;
    struct vm_rg_struct *rg = get_symrg_byaddr(&mm, proc.regs[0], &rgid);
    TEST_ASSERT(rg != NULL);
    TEST_ASSERT_EQ(VMA_SMALL, rg->vmaid);

    struct vm_area_struct *vma = get_vma_by_num(&mm, rg->vmaid);
    TEST_ASSERT(vma != NULL);
    TEST_ASSERT_EQ(vma->vm_start, rg->rg_start);
    TEST_ASSERT_EQ(vma->vm_start + 128, rg->rg_end);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM02: ALLOC đúng kích thước một page (voi reg_index != rgid) */
static void run_lm02(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 8);
    lm_proc_init(&env, &proc, &mm, 102);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    int ret = liballoc(&proc, page_sz, 1); /* Cap vao reg 1 -> rgid dong la 0 */
    TEST_ASSERT_EQ(0, ret);

    int rgid = -1;
    struct vm_rg_struct *rg = get_symrg_byaddr(&mm, proc.regs[1], &rgid);
    TEST_ASSERT(rg != NULL);
    TEST_ASSERT_EQ(0, rgid);

    struct vm_area_struct *vma = get_vma_by_num(&mm, rg->vmaid);
    TEST_ASSERT(vma != NULL);
    TEST_ASSERT_EQ(vma->vm_start, rg->rg_start);
    TEST_ASSERT_EQ(vma->vm_start + page_sz, rg->rg_end);
    TEST_ASSERT_EQ(vma->vm_start + page_sz, vma->sbrk);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM03: ALLOC lớn hơn một page */
static void run_lm03(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 8);
    lm_proc_init(&env, &proc, &mm, 103);

    addr_t req_sz = 6000; /* > 4096 -> can 2 pages (8192 bytes) */
    int ret = liballoc(&proc, req_sz, 2);
    TEST_ASSERT_EQ(0, ret);

    int rgid = -1;
    struct vm_rg_struct *rg = get_symrg_byaddr(&mm, proc.regs[2], &rgid);
    TEST_ASSERT(rg != NULL);

    struct vm_area_struct *vma = get_vma_by_num(&mm, rg->vmaid);
    TEST_ASSERT(vma != NULL);
    TEST_ASSERT_EQ(vma->vm_start, rg->rg_start);
    TEST_ASSERT_EQ(vma->vm_start + req_sz, rg->rg_end);
    TEST_ASSERT_EQ(vma->vm_start + 2 * test_memphy_page_size(), vma->sbrk);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM04: ALLOC nhiều lần - Các vùng không chồng lấn */
static void run_lm04(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 8);
    lm_proc_init(&env, &proc, &mm, 104);

    TEST_ASSERT_EQ(0, liballoc(&proc, 500, 0));
    TEST_ASSERT_EQ(0, liballoc(&proc, 1000, 1));
    TEST_ASSERT_EQ(0, liballoc(&proc, 2000, 2));

    int id0 = -1, id1 = -1, id2 = -1;
    struct vm_rg_struct *r0 = get_symrg_byaddr(&mm, proc.regs[0], &id0);
    struct vm_rg_struct *r1 = get_symrg_byaddr(&mm, proc.regs[1], &id1);
    struct vm_rg_struct *r2 = get_symrg_byaddr(&mm, proc.regs[2], &id2);
    TEST_ASSERT(r0 != NULL && r1 != NULL && r2 != NULL);

    TEST_ASSERT_EQ(500, r0->rg_end - r0->rg_start);
    TEST_ASSERT_EQ(1000, r1->rg_end - r1->rg_start);
    TEST_ASSERT_EQ(2000, r2->rg_end - r2->rg_start);

    TEST_ASSERT(regions_disjoint(r0->rg_start, r0->rg_end, r1->rg_start, r1->rg_end));
    TEST_ASSERT(regions_disjoint(r1->rg_start, r1->rg_end, r2->rg_start, r2->rg_end));
    TEST_ASSERT(regions_disjoint(r0->rg_start, r0->rg_end, r2->rg_start, r2->rg_end));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM05: ALLOC khi hết RAM */
static void run_lm05(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 2, 4);
    lm_proc_init(&env, &proc, &mm, 105);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 3 * page_sz, 0));
    TEST_ASSERT_EQ(-1, liballoc(&proc, 5 * page_sz, 1));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM06: ALLOC kích thước 0 */
static void run_lm06(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 106);

    TEST_ASSERT_EQ(-1, liballoc(&proc, 0, 0));
    TEST_ASSERT_EQ(0, mm.symrgtbl[0].rg_start);
    TEST_ASSERT_EQ(0, mm.symrgtbl[0].rg_end);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM07: ALLOC vượt khả năng virtual/physical memory */
static void run_lm07(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 107);

    addr_t huge_sz = 1024ULL * 1024ULL * 1024ULL;
    TEST_ASSERT_EQ(-1, liballoc(&proc, huge_sz, 0));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM08: FREE region hợp lệ */
static void run_lm08(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 108);

    TEST_ASSERT_EQ(0, liballoc(&proc, 512, 0));
    int rgid = -1;
    struct vm_rg_struct *rg = get_symrg_byaddr(&mm, proc.regs[0], &rgid);
    TEST_ASSERT(rg != NULL);

    int allocated_vmaid = rg->vmaid;
    addr_t exp_start = rg->rg_start;
    addr_t exp_end = rg->rg_end;

    TEST_ASSERT_EQ(0, libfree(&proc, 0));
    TEST_ASSERT_EQ(0, mm.symrgtbl[rgid].rg_start);
    TEST_ASSERT_EQ(0, mm.symrgtbl[rgid].rg_end);
    TEST_ASSERT_EQ(-1, mm.symrgtbl[rgid].vmaid);

    struct vm_area_struct *vma = get_vma_by_num(&mm, allocated_vmaid);
    TEST_ASSERT(vma != NULL);

    int found = 0;
    struct vm_rg_struct *it = vma->vm_freerg_list;
    while (it != NULL) {
        if (it->rg_start == exp_start && it->rg_end == exp_end) {
            found = 1;
            break;
        }
        it = it->rg_next;
    }
    TEST_ASSERT_EQ(1, found);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM09: FREE hai lần (Double free) và không free nhầm vùng mới ở địa chỉ 0 */
static void run_lm09(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 109);

    TEST_ASSERT_EQ(0, liballoc(&proc, 256, 3));
    TEST_ASSERT_EQ(0, libfree(&proc, 3));
    TEST_ASSERT_EQ(-1, libfree(&proc, 3));

    /* Cap phat vung moi vao reg 4 (lay lai dia chi 0), goi lai free(3) phai bao loi -1 */
    TEST_ASSERT_EQ(0, liballoc(&proc, 128, 4));
    TEST_ASSERT_EQ(-1, libfree(&proc, 3));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM10: FREE thanh ghi chưa cấp phát hoặc ngoài phạm vi */
static void run_lm10(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 110);

    TEST_ASSERT_EQ(-1, libfree(&proc, 5));
    TEST_ASSERT_EQ(-1, libfree(&proc, MAXREG + 1));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM11: ALLOC sau FREE - Tái sử dụng vùng đã thu hồi */
static void run_lm11(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 111);

    TEST_ASSERT_EQ(0, liballoc(&proc, 256, 0));
    int id0 = -1;
    struct vm_rg_struct *r0 = get_symrg_byaddr(&mm, proc.regs[0], &id0);
    TEST_ASSERT(r0 != NULL);
    int vmaid0 = r0->vmaid;
    addr_t orig_start = r0->rg_start;

    struct vm_area_struct *vma = get_vma_by_num(&mm, vmaid0);
    TEST_ASSERT(vma != NULL);
    addr_t sbrk_before = vma->sbrk;

    TEST_ASSERT_EQ(0, libfree(&proc, 0));

    TEST_ASSERT_EQ(0, liballoc(&proc, 128, 1));
    int id1 = -1;
    struct vm_rg_struct *r1 = get_symrg_byaddr(&mm, proc.regs[1], &id1);
    TEST_ASSERT(r1 != NULL);
    TEST_ASSERT_EQ(vmaid0, r1->vmaid);
    TEST_ASSERT_EQ(orig_start, r1->rg_start);
    TEST_ASSERT_EQ(orig_start + 128, r1->rg_end);
    TEST_ASSERT_EQ(sbrk_before, vma->sbrk);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM12: WRITE rồi READ cùng offset */
static void run_lm12(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 112);

    TEST_ASSERT_EQ(0, liballoc(&proc, 256, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x5A, 0, 42));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc, 0, 42, &dst_reg));
    TEST_ASSERT_EQ(0x5A, proc.regs[dst_reg]);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM13: WRITE/READ nhiều offset */
static void run_lm13(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 113);

    TEST_ASSERT_EQ(0, liballoc(&proc, 512, 0));
    for (int i = 0; i < 64; ++i) {
        BYTE val = (BYTE)((i * 7 + 13) & 0xFF);
        TEST_ASSERT_EQ(0, libwrite(&proc, val, 0, (addr_t)(i * 8)));
    }
    uint32_t dst_reg = 7;
    for (int i = 0; i < 64; ++i) {
        uint32_t expected = (uint32_t)((i * 7 + 13) & 0xFF);
        TEST_ASSERT_EQ(0, libread(&proc, 0, (addr_t)(i * 8), &dst_reg));
        TEST_ASSERT_EQ(expected, proc.regs[dst_reg]);
    }

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM14: WRITE/READ tại cuối region */
static void run_lm14(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 114);

    TEST_ASSERT_EQ(0, liballoc(&proc, 300, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xE7, 0, 299));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc, 0, 299, &dst_reg));
    TEST_ASSERT_EQ(0xE7, proc.regs[dst_reg]);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM15: READ vượt region */
static void run_lm15(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 115);

    TEST_ASSERT_EQ(0, liballoc(&proc, 200, 0));
    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(-1, libread(&proc, 0, 200, &dst_reg));
    TEST_ASSERT_EQ(-1, libread(&proc, 0, 500, &dst_reg));
    TEST_ASSERT_EQ(-1, libread(&proc, 1, 0, &dst_reg));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM16: WRITE vượt region */
static void run_lm16(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 116);

    TEST_ASSERT_EQ(0, liballoc(&proc, 100, 0));
    TEST_ASSERT_EQ(0, liballoc(&proc, 100, 1));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x11, 1, 0));

    TEST_ASSERT_EQ(-1, libwrite(&proc, (BYTE)0x99, 0, 100));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc, 1, 0, &dst_reg));
    TEST_ASSERT_EQ(0x11, proc.regs[dst_reg]);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM17: READ/WRITE qua hai page (4095 -> 4096) */
static void run_lm17(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 4);
    lm_proc_init(&env, &proc, &mm, 117);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 2 * page_sz, 0));

    addr_t cross_start = page_sz - 8;
    for (int i = 0; i < 16; ++i) {
        BYTE b = (BYTE)(0xA0 + i);
        TEST_ASSERT_EQ(0, libwrite(&proc, b, 0, cross_start + (addr_t)i));
    }

    uint32_t dst_reg = 7;
    for (int i = 0; i < 16; ++i) {
        uint32_t expected = (uint32_t)(0xA0 + i);
        TEST_ASSERT_EQ(0, libread(&proc, 0, cross_start + (addr_t)i, &dst_reg));
        TEST_ASSERT_EQ(expected, proc.regs[dst_reg]);
    }

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM18: Đọc page chưa present (Demand paging trong pg_getpage) */
static void run_lm18(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 118);

    TEST_ASSERT_EQ(0, PAGING_PAGE_PRESENT(pte_get_entry(&mm, 5, &proc)));
    int fpn = -1;
    TEST_ASSERT_EQ(0, pg_getpage(&mm, 5, &fpn, &proc));
    TEST_ASSERT(fpn >= 0 && fpn < 4);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&mm, 5, &proc)) != 0);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM19: SWAP OUT rồi SWAP IN - Bảo toàn dữ liệu toàn vẹn */
static void run_lm19(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 2, 4);
    lm_proc_init(&env, &proc, &mm, 119);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 4 * page_sz, 0));

    for (int p = 0; p < 4; ++p) {
        addr_t base = (addr_t)p * page_sz;
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x10 + p), 0, base));
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x80 + p), 0, base + page_sz - 1));
    }

    uint32_t reg_head = 6, reg_tail = 7;
    for (int p = 0; p < 4; ++p) {
        addr_t base = (addr_t)p * page_sz;
        TEST_ASSERT_EQ(0, libread(&proc, 0, base, &reg_head));
        TEST_ASSERT_EQ(0, libread(&proc, 0, base + page_sz - 1, &reg_tail));
        TEST_ASSERT_EQ((uint64_t)(0x10 + p), proc.regs[reg_head]);
        TEST_ASSERT_EQ((uint64_t)(0x80 + p), proc.regs[reg_tail]);
    }

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM20: KMALLOC vùng nhớ liên tục */
static void run_lm20(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 4);
    lm_proc_init(&env, &proc, &mm, 120);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, (uint32_t)(2 * page_sz), 0));
    TEST_ASSERT_EQ(0, env.kmm.symrgtbl[0].rg_start);
    TEST_ASSERT_EQ(2 * page_sz, env.kmm.symrgtbl[0].rg_end);

    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 0, &proc)) != 0);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 1, &proc)) != 0);
    TEST_ASSERT(env.kmm.fifo_pgn == NULL);

    /* libfree should find this address in Kernel MM, clear the register and
     * return the virtual region so a later kernel allocation can reuse it. */
    TEST_ASSERT_EQ(0, libfree(&proc, 0));
    TEST_ASSERT_EQ((addr_t)-1, proc.regs[0]);
    TEST_ASSERT_EQ(0, env.kmm.symrgtbl[0].rg_start);
    TEST_ASSERT_EQ(0, env.kmm.symrgtbl[0].rg_end);
    TEST_ASSERT_EQ(-1, libfree(&proc, 0));

    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, (uint32_t)(2 * page_sz), 1));
    TEST_ASSERT_EQ(0, proc.regs[1]);
    TEST_ASSERT_EQ(2 * page_sz, env.kmm.symrgtbl[0].rg_end);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 0, &proc)) != 0);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 1, &proc)) != 0);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM21: COPY FROM USER và COPY TO USER */
static void run_lm21(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 4);
    lm_proc_init(&env, &proc, &mm, 121);

    TEST_ASSERT_EQ(0, liballoc(&proc, 256, 0));
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, 256, 0));

    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x30 + i), 0, (addr_t)(10 + i)));
    }

    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, 0, 10, 16));

    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xFF, 0, (addr_t)(10 + i)));
    }

    TEST_ASSERT_EQ(0, libkmem_copy_to_user(&proc, 0, 0, 10, 16));

    uint32_t dst_reg = 7;
    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQ(0, libread(&proc, 0, (addr_t)(10 + i), &dst_reg));
        TEST_ASSERT_EQ((uint32_t)(0x30 + i), proc.regs[dst_reg]);
    }

    TEST_ASSERT_EQ(-1, libkmem_copy_from_user(&proc, 0, 0, 250, 16));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM22: FREE toàn bộ memory của process */
static void run_lm22(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 6, 4);
    lm_proc_init(&env, &proc, &mm, 122);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 4 * page_sz, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x77, 0, 0));

    TEST_ASSERT_EQ(0, free_pcb_memph(&proc));
    TEST_ASSERT(mm.fifo_pgn == NULL);
    TEST_ASSERT(env.mram.used_fp_list == NULL);
    TEST_ASSERT(env.mswp.used_fp_list == NULL);
    verify_memphy_invariants(&env.mram, 6);
    verify_memphy_invariants(&env.mswp, 4);

    lm_cleanup_mm_structs(&mm);
    lm_env_destroy(&env);
}

/* LM23: Phân tuyến 3 VMA (SMALL / MEDIUM / LARGE) theo kích thước macOS layout */
static void run_lm23(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 64, 16);
    lm_proc_init(&env, &proc, &mm, 123);

    addr_t sz_small = SMALL_MAX_SIZE;          /* VMA_SMALL */
    addr_t sz_med   =  4096;   /* VMA_MEDIUM */
    addr_t sz_large = MEDIUM_MAX_SIZE + 4096;  /* VMA_LARGE */

    TEST_ASSERT_EQ(0, liballoc(&proc, sz_small, 0));
    TEST_ASSERT_EQ(0, liballoc(&proc, sz_med,   1));
    TEST_ASSERT_EQ(0, liballoc(&proc, sz_large, 2));

    int id_s = -1, id_m = -1, id_l = -1;
    struct vm_rg_struct *rg_s = get_symrg_byaddr(&mm, proc.regs[0], &id_s);
    struct vm_rg_struct *rg_m = get_symrg_byaddr(&mm, proc.regs[1], &id_m);
    struct vm_rg_struct *rg_l = get_symrg_byaddr(&mm, proc.regs[2], &id_l);

    TEST_ASSERT(rg_s != NULL && rg_m != NULL && rg_l != NULL);
    TEST_ASSERT_EQ(VMA_SMALL,  rg_s->vmaid);
    TEST_ASSERT_EQ(VMA_MEDIUM, rg_m->vmaid);
    TEST_ASSERT_EQ(VMA_LARGE,  rg_l->vmaid);

    /* Ghi va doc kiem tra bang phan trang 5 cap o dia chi cao 2^55 va 2^56 */
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xAA, 1, 100));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xBB, 2, 200));

    uint32_t dst_reg = 5;
    TEST_ASSERT_EQ(0, libread(&proc, 1, 100, &dst_reg));
    TEST_ASSERT_EQ(0xAA, proc.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc, 2, 200, &dst_reg));
    TEST_ASSERT_EQ(0xBB, proc.regs[dst_reg]);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM24: DMA memcpy bất đồng bộ + kiểm tra chặn hazard (RAW/WAW/WAR) + ngắt hoàn tất */
static void run_lm24(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;
    struct code_seg_t code;
    struct inst_t insts[4];

    lm_env_init(&env, 8, 4);
    lm_proc_init(&env, &proc, &mm, 124);

    /* Cap phat 2 vung nho 2 trang (8192B) de test DMA cat qua ranh gioi trang 4KB */
    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 2 * page_sz, 1));
    TEST_ASSERT_EQ(0, liballoc(&proc, 2 * page_sz, 2));

    /* Nap lenh DMA copy 6000 byte (2 chuyen DMA: 4096B + 1904B) */
    TEST_ASSERT_EQ(0, libmem_dma_memcpy(&proc, 1, 2, 0, 6000));
    TEST_ASSERT_EQ(2, libmem_dma_inflight(proc.pid));

    /* Gia lap chuong trinh sap chay cac lenh ke tiep de test hazard */
    memset(&code, 0, sizeof(code));
    code.size = 4;
    code.text = insts;
    proc.code = &code;

    /* Lenh 0: READ tu reg 2 (vung dich DMA) -> phai bi chan (RAW hazard) */
    insts[0].opcode = READ; insts[0].arg_0 = 2; insts[0].arg_1 = 0; insts[0].arg_2 = 5;
    proc.pc = 0;
    TEST_ASSERT_EQ(1, libmem_dma_blocked(&proc));

    /* Lenh 1: READ tu reg 1 (vung nguon DMA) -> duoc phep doc song song */
    insts[1].opcode = READ; insts[1].arg_0 = 1; insts[1].arg_1 = 0; insts[1].arg_2 = 5;
    proc.pc = 1;
    TEST_ASSERT_EQ(0, libmem_dma_blocked(&proc));

    /* Lenh 2: WRITE vao reg 1 (vung nguon DMA) -> phai bi chan (WAR hazard) */
    insts[2].opcode = WRITE; insts[2].arg_0 = 0x11; insts[2].arg_1 = 1; insts[2].arg_2 = 0;
    proc.pc = 2;
    TEST_ASSERT_EQ(1, libmem_dma_blocked(&proc));

    /* Nhan 2 ngat hoan tat DMA -> het job in-flight -> het bi chan */
    uint32_t c_pid, c_src, c_dst, c_len;
    TEST_ASSERT_EQ(0, libmem_dma_complete(&c_pid, &c_src, &c_dst, &c_len));
    TEST_ASSERT_EQ(0, libmem_dma_complete(&c_pid, &c_src, &c_dst, &c_len));
    TEST_ASSERT_EQ(0, libmem_dma_inflight(proc.pid));

    proc.pc = 0;
    TEST_ASSERT_EQ(0, libmem_dma_blocked(&proc));

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

#define RUN_TEST(id, description, function) do { \
    test_begin((id), (description)); \
    function(); \
    test_end(); \
} while (0)

int main(void)
{
    RUN_TEST("LM01", "ALLOC small memory size", run_lm01);
    RUN_TEST("LM02", "ALLOC exact one page size", run_lm02);
    RUN_TEST("LM03", "ALLOC larger than one page", run_lm03);
    RUN_TEST("LM04", "Multiple ALLOCs without region overlap", run_lm04);
    RUN_TEST("LM05", "ALLOC when RAM is exhausted", run_lm05);
    RUN_TEST("LM06", "Reject ALLOC with size = 0", run_lm06);
    RUN_TEST("LM07", "Reject ALLOC exceeding virtual memory capacity", run_lm07);
    RUN_TEST("LM08", "FREE valid region and return to free list", run_lm08);
    RUN_TEST("LM09", "Reject double FREE on same region", run_lm09);
    RUN_TEST("LM10", "Reject FREE on unallocated / invalid region", run_lm10);
    RUN_TEST("LM11", "Reuse freed region on subsequent ALLOC", run_lm11);
    RUN_TEST("LM12", "WRITE then READ at the same offset", run_lm12);
    RUN_TEST("LM13", "WRITE and READ across multiple offsets", run_lm13);
    RUN_TEST("LM14", "WRITE and READ at exact end of region", run_lm14);
    RUN_TEST("LM15", "Reject READ out-of-bounds of region", run_lm15);
    RUN_TEST("LM16", "Reject WRITE out-of-bounds without corrupting neighbors", run_lm16);
    RUN_TEST("LM17", "READ and WRITE across two pages (4095 -> 4096)", run_lm17);
    RUN_TEST("LM18", "Access non-present page via pg_getpage fault path", run_lm18);
    RUN_TEST("LM19", "SWAP OUT then SWAP IN preserves full page data", run_lm19);
    RUN_TEST("LM20", "KMALLOC contiguous kernel memory pages", run_lm20);
    RUN_TEST("LM21", "COPY FROM USER and COPY TO USER data integrity", run_lm21);
    RUN_TEST("LM22", "FREE all process memory and verify invariants", run_lm22);
    RUN_TEST("LM23", "Route SMALL / MEDIUM / LARGE VMAs via 5-level paging", run_lm23);
    RUN_TEST("LM24", "DMA multi-page split, RAW/WAR hazard blocking, and IRQ completion", run_lm24);

    test_summary();
    return 0;
}
