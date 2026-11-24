#include "test_helpers.h"
#include "libmem.h"
#include "mm64.h"
#include "queue.h"
#include "syscall.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Khai bao cac ham he thong can thiet */
int libsyscall(struct pcb_t *caller, uint32_t syscall_idx, arg_t a1, arg_t a2, arg_t a3);
int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs);
int free_pcb_memph(struct pcb_t *caller);
struct vm_rg_struct *get_symrg_byaddr(struct mm_struct *mm, addr_t addr, int *rgid);
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size,
            addr_t *alloc_addr);

/* Bang syscall khop voi syscall.tbl */
const char *sys_call_table[] = {
    "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall",
    "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall",
    "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall",
    "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall", "sys_ni_syscall",
    "sys_ni_syscall", "sys_memmap"
};
const int syscall_table_size = 18;

int _syscall(struct krnl_t *krnl, uint32_t pid, uint32_t nr, struct sc_regs *regs)
{
    if (krnl == NULL || regs == NULL || nr >= (uint32_t)syscall_table_size)
        return -1;
    if (nr == 17)
        return __sys_memmap(krnl, pid, regs);
    return -1;
}

struct it_test_env {
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

static void it_cleanup_mm_structs(struct mm_struct *mm)
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

    mm_free_page_tables(mm);
}

static void it_env_init(struct it_test_env *env, int ram_frames, int swp_frames)
{
    memset(env, 0, sizeof(*env));
    addr_t page_sz = (addr_t)test_memphy_page_size();

    if (ram_frames > 0)
        env->mram_ready = (test_memphy_init(&env->mram, (addr_t)ram_frames * page_sz, 1) == 0);
    if (swp_frames > 0)
        env->mswp_ready = (test_memphy_init(&env->mswp, (addr_t)swp_frames * page_sz, 1) == 0);

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

static void it_proc_init(struct it_test_env *env, struct pcb_t *proc,
                         struct mm_struct *mm, uint32_t pid)
{
    memset(proc, 0, sizeof(*proc));
    memset(mm, 0, sizeof(*mm));
    proc->pid = pid;
    proc->krnl = &env->krnl;
    proc->mm = mm;
    init_mm(mm, proc);

    for (int i = 0; i < MAXREG; i++) {
        proc->regs[i] = (addr_t)-1;
    }

    if (env->running_list.size < MAX_QUEUE_SIZE) {
        env->running_list.proc[env->running_list.size++] = proc;
    }
}

static void it_proc_destroy(struct pcb_t *proc, struct mm_struct *mm)
{
    free_pcb_memph(proc);
    it_cleanup_mm_structs(mm);
}

static void it_env_destroy(struct it_test_env *env)
{
    if (env->kmm_ready) {
        struct pcb_t dummy = {0};
        dummy.krnl = &env->krnl;
        dummy.mm = &env->kmm;
        free_pcb_memph(&dummy);
        it_cleanup_mm_structs(&env->kmm);
    }
    test_memphy_destroy(&env->mram, env->mram_ready);
    test_memphy_destroy(&env->mswp, env->mswp_ready);
}

/* IT01: Process A ALLOC va WRITE qua full stack (User -> Syscall 17 -> VMA -> MM64 -> RAM) */
static void run_it01(void)
{
    struct it_test_env env;
    struct pcb_t proc_a;
    struct mm_struct mm_a;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc_a, &mm_a, 2001);

    TEST_ASSERT_EQ(0, liballoc(&proc_a, 512, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)0x55, 0, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)0xAA, 0, 511));

    addr_t pgn = proc_a.regs[0] >> PAGING64_ADDR_PT_SHIFT;
    addr_t pte = pte_get_entry(&mm_a, pgn, &proc_a);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);


    addr_t fpn = PAGING_FPN(pte);
    addr_t page_sz = (addr_t)test_memphy_page_size();
    BYTE phy_first = 0, phy_last = 0;
    MEMPHY_read(&env.mram, fpn * page_sz + 0, &phy_first);
    MEMPHY_read(&env.mram, fpn * page_sz + 511, &phy_last);
    TEST_ASSERT_EQ((BYTE)0x55, phy_first);
    TEST_ASSERT_EQ((BYTE)0xAA, phy_last);

    it_proc_destroy(&proc_a, &mm_a);
    it_env_destroy(&env);
}

/* IT02: Process A READ lai du lieu da ghi qua thanh ghi dich rieng biet */
static void run_it02(void)
{
    struct it_test_env env;
    struct pcb_t proc_a;
    struct mm_struct mm_a;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc_a, &mm_a, 2002);

    TEST_ASSERT_EQ(0, liballoc(&proc_a, 1024, 0));
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)(0x10 + i), 0, (addr_t)(i * 16)));
    }
    uint32_t dst_reg = 7;
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libread(&proc_a, 0, (addr_t)(i * 16), &dst_reg));
        TEST_ASSERT_EQ((uint64_t)(0x10 + i), proc_a.regs[dst_reg]);
    }

    it_proc_destroy(&proc_a, &mm_a);
    it_env_destroy(&env);
}

/* IT03: Process B truy cap cung virtual address - cach ly khong gian dia chi */
static void run_it03(void)
{
    struct it_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc_a, &mm_a, 20031);
    it_proc_init(&env, &proc_b, &mm_b, 20032);

    TEST_ASSERT_EQ(0, liballoc(&proc_a, 256, 0));
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 256, 0));
    TEST_ASSERT_EQ(proc_a.regs[0], proc_b.regs[0]);

    TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)0x33, 0, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0x77, 0, 10));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc_a, 0, 10, &dst_reg));
    TEST_ASSERT_EQ(0x33, proc_a.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc_b, 0, 10, &dst_reg));
    TEST_ASSERT_EQ(0x77, proc_b.regs[dst_reg]);

    it_proc_destroy(&proc_a, &mm_a);
    it_proc_destroy(&proc_b, &mm_b);
    it_env_destroy(&env);
}

/* IT04: Hai process cung ALLOC - khong bao gio cap trung physical frame */
static void run_it04(void)
{
    struct it_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc_a, &mm_a, 20041);
    it_proc_init(&env, &proc_b, &mm_b, 20042);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 3 * page_sz, 0));
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 3 * page_sz, 0));

    addr_t start_pgn_a = proc_a.regs[0] >> PAGING64_ADDR_PT_SHIFT;
    addr_t start_pgn_b = proc_b.regs[0] >> PAGING64_ADDR_PT_SHIFT;

    int seen[8] = {0};
    for (int p = 0; p < 3; ++p) {
        addr_t pte_a = pte_get_entry(&mm_a, start_pgn_a + (addr_t)p, &proc_a);
        addr_t pte_b = pte_get_entry(&mm_b, start_pgn_b + (addr_t)p, &proc_b);
        TEST_ASSERT(PAGING_PAGE_PRESENT(pte_a) != 0);
        TEST_ASSERT(PAGING_PAGE_PRESENT(pte_b) != 0);

        addr_t fa = PAGING_FPN(pte_a);
        addr_t fb = PAGING_FPN(pte_b);
        TEST_ASSERT(fa != fb);
        TEST_ASSERT_EQ(0, seen[fa]);
        seen[fa] = 1;
        TEST_ASSERT_EQ(0, seen[fb]);
        seen[fb] = 1;
    }

    it_proc_destroy(&proc_a, &mm_a);
    it_proc_destroy(&proc_b, &mm_b);
    it_env_destroy(&env);
}

/* IT05: Process A FREE, Process B ALLOC - tai su dung frame vat ly da thu hoi */
static void run_it05(void)
{
    struct it_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    it_env_init(&env, 2, 0);
    it_proc_init(&env, &proc_a, &mm_a, 20051);
    it_proc_init(&env, &proc_b, &mm_b, 20052);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 2 * page_sz, 0));
    TEST_ASSERT_EQ(-1, liballoc(&proc_b, page_sz, 0));

    TEST_ASSERT_EQ(0, free_pcb_memph(&proc_a));
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 2 * page_sz, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0x6C, 0, 0));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc_b, 0, 0, &dst_reg));
    TEST_ASSERT_EQ(0x6C, proc_b.regs[dst_reg]);

    it_cleanup_mm_structs(&mm_a);
    it_proc_destroy(&proc_b, &mm_b);
    it_env_destroy(&env);
}

/* IT06: RAM nho, buoc phat sinh SWAP OUT */
static void run_it06(void)
{
    struct it_test_env env;
    struct pcb_t proc_a;
    struct mm_struct mm_a;

    it_env_init(&env, 2, 4);
    it_proc_init(&env, &proc_a, &mm_a, 2006);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 5 * page_sz, 0));

    for (int p = 0; p < 5; ++p) {
        addr_t base = (addr_t)p * page_sz;
        TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)(0x20 + p), 0, base));
        TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)(0x90 + p), 0, base + page_sz - 1));
    }

    addr_t start_pgn = proc_a.regs[0] >> PAGING64_ADDR_PT_SHIFT;
    int swapped_count = 0;
    for (int p = 0; p < 5; ++p) {
        addr_t pte = pte_get_entry(&mm_a, start_pgn + (addr_t)p, &proc_a);
        if (!PAGING_PAGE_PRESENT(pte) && (pte & PAGING_PTE_SWAPPED_MASK)) {
            swapped_count++;
        }
    }
    TEST_ASSERT(swapped_count >= 3);

    it_proc_destroy(&proc_a, &mm_a);
    it_env_destroy(&env);
}

/* IT07: SWAP IN khoi phuc nguyen ven du lieu trang bi day ra SWAP */
static void run_it07(void)
{
    struct it_test_env env;
    struct pcb_t proc_a;
    struct mm_struct mm_a;

    it_env_init(&env, 2, 4);
    it_proc_init(&env, &proc_a, &mm_a, 2007);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 5 * page_sz, 0));

    for (int p = 0; p < 5; ++p) {
        addr_t base = (addr_t)p * page_sz;
        TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)(0x20 + p), 0, base));
        TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)(0x90 + p), 0, base + page_sz - 1));
    }

    uint32_t reg_head = 6, reg_tail = 7;
    for (int p = 0; p < 5; ++p) {
        addr_t base = (addr_t)p * page_sz;
        TEST_ASSERT_EQ(0, libread(&proc_a, 0, base, &reg_head));
        TEST_ASSERT_EQ(0, libread(&proc_a, 0, base + page_sz - 1, &reg_tail));
        TEST_ASSERT_EQ((uint64_t)(0x20 + p), proc_a.regs[reg_head]);
        TEST_ASSERT_EQ((uint64_t)(0x90 + p), proc_a.regs[reg_tail]);
    }

    it_proc_destroy(&proc_a, &mm_a);
    it_env_destroy(&env);
}

/* IT08: ALLOC nhieu vung lien tuc - khong chong lan va giu nguyen du lieu tung vung */
static void run_it08(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2008);

    for (int r = 0; r < 6; ++r) {
        TEST_ASSERT_EQ(0, liballoc(&proc, 512, (uint32_t)r));
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x40 + r), (uint32_t)r, 0));
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0xA0 + r), (uint32_t)r, 511));
    }

    for (int r = 0; r < 5; ++r) {
        int id0 = -1, id1 = -1;
        struct vm_rg_struct *rg0 = get_symrg_byaddr(&mm, proc.regs[r], &id0);
        struct vm_rg_struct *rg1 = get_symrg_byaddr(&mm, proc.regs[r + 1], &id1);
        TEST_ASSERT(rg0 != NULL && rg1 != NULL);
        if (rg0 && rg1 && rg0->vmaid == rg1->vmaid) {
            TEST_ASSERT(rg0->rg_end <= rg1->rg_start);
        }
    }

    uint32_t reg_v0 = 8, reg_v1 = 9;
    for (int r = 0; r < 6; ++r) {
        TEST_ASSERT_EQ(0, libread(&proc, (uint32_t)r, 0, &reg_v0));
        TEST_ASSERT_EQ(0, libread(&proc, (uint32_t)r, 511, &reg_v1));
        TEST_ASSERT_EQ((uint64_t)(0x40 + r), proc.regs[reg_v0]);
        TEST_ASSERT_EQ((uint64_t)(0xA0 + r), proc.regs[reg_v1]);
    }

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT09: FREE xen ke cac region (phan manh) roi cap phat lai */
static void run_it09(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2009);

    for (int r = 0; r < 4; ++r) {
        TEST_ASSERT_EQ(0, liballoc(&proc, 512, (uint32_t)r));
    }
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xB1, 1, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xB3, 3, 10));

    TEST_ASSERT_EQ(0, libfree(&proc, 0));
    TEST_ASSERT_EQ(0, libfree(&proc, 2));

    TEST_ASSERT_EQ(0, liballoc(&proc, 256, 4));
    TEST_ASSERT_EQ(0, liballoc(&proc, 512, 5));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xE4, 4, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xE5, 5, 10));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc, 1, 10, &dst_reg));
    TEST_ASSERT_EQ(0xB1, proc.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc, 3, 10, &dst_reg));
    TEST_ASSERT_EQ(0xB3, proc.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc, 4, 10, &dst_reg));
    TEST_ASSERT_EQ(0xE4, proc.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc, 5, 10, &dst_reg));
    TEST_ASSERT_EQ(0xE5, proc.regs[dst_reg]);

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT10: Tu choi User READ vao vung nho Kernel */
static void run_it10(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2010);

    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, 256, 5));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(-1, libread(&proc, 5, 0, &dst_reg));

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT11: Tu choi User WRITE vao vung nho Kernel, du lieu Kernel duoc bao toan */
static void run_it11(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2011);

    /* Use the disjoint MEDIUM user VMA so its address cannot alias kernel VA 0. */
    addr_t user_addr = 0;
    TEST_ASSERT_EQ(0, __alloc(&proc, VMA_MEDIUM, -1, 64, &user_addr));
    proc.regs[0] = user_addr;
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, 64, 5));
    int kernel_rgid = -1;
    TEST_ASSERT(get_symrg_byaddr(env.kmm.mmap->vm_mm, proc.regs[5],
                                 &kernel_rgid) != NULL);
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x4B, 0, 0));
    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, (uint32_t)kernel_rgid, 0, 1));

    TEST_ASSERT_EQ(-1, libwrite(&proc, (BYTE)0xFF, 5, 0));

    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x00, 0, 0));
    TEST_ASSERT_EQ(0, libkmem_copy_to_user(&proc, (uint32_t)kernel_rgid, 0, 0, 1));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc, 0, 0, &dst_reg));
    TEST_ASSERT_EQ(0x4B, proc.regs[dst_reg]);

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT12: COPY FROM USER vat ngang ranh gioi nhieu trang */
static void run_it12(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2012);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 2 * page_sz, 0));
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, (uint32_t)(2 * page_sz), 1));
    int kernel_rgid = -1;
    TEST_ASSERT(get_symrg_byaddr(env.kmm.mmap->vm_mm, proc.regs[1],
                                 &kernel_rgid) != NULL);

    addr_t start = page_sz - 16;
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x50 + i), 0, start + (addr_t)i));
    }

    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, (uint32_t)kernel_rgid, start, 32));

    addr_t kpte1 = pte_get_entry(&env.kmm, 1, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(kpte1) != 0);
    BYTE kbyte_after_boundary = 0;
    MEMPHY_read(&env.mram, PAGING_FPN(kpte1) * page_sz + 0, &kbyte_after_boundary);
    TEST_ASSERT_EQ((BYTE)(0x50 + 16), kbyte_after_boundary);

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT13: COPY TO USER vat ngang ranh gioi nhieu trang */
static void run_it13(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2013);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 2 * page_sz, 0));
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, (uint32_t)(2 * page_sz), 1));
    int kernel_rgid = -1;
    TEST_ASSERT(get_symrg_byaddr(env.kmm.mmap->vm_mm, proc.regs[1],
                                 &kernel_rgid) != NULL);

    addr_t start = page_sz - 16;
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x70 + i), 0, start + (addr_t)i));
    }
    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, (uint32_t)kernel_rgid, start, 32));

    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xEE, 0, start + (addr_t)i));
    }

    TEST_ASSERT_EQ(0, libkmem_copy_to_user(&proc, (uint32_t)kernel_rgid, 0, start, 32));

    uint32_t dst_reg = 7;
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libread(&proc, 0, start + (addr_t)i, &dst_reg));
        TEST_ASSERT_EQ((uint64_t)(0x70 + i), proc.regs[dst_reg]);
    }

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT14: Nhieu process cung cap phat, doc/ghi va thu hoi dong thoi */
struct it14_worker_arg {
    struct pcb_t *proc;
    BYTE seed;
    int status;
};

static void *it14_worker(void *arg)
{
    struct it14_worker_arg *w = (struct it14_worker_arg *)arg;
    uint32_t dst_reg = 7;
    for (int round = 0; round < 3; ++round) {
        if (liballoc(w->proc, 1024, 0) < 0) {
            w->status = -1;
            return NULL;
        }
        for (int i = 0; i < 16; ++i) {
            if (libwrite(w->proc, (BYTE)(w->seed + round + i), 0, (addr_t)(i * 32)) < 0) {
                w->status = -2;
                return NULL;
            }
        }
        for (int i = 0; i < 16; ++i) {
            if (libread(w->proc, 0, (addr_t)(i * 32), &dst_reg) < 0 ||
                w->proc->regs[dst_reg] != (uint64_t)(BYTE)(w->seed + round + i)) {
                w->status = -3;
                return NULL;
            }
        }
        if (libfree(w->proc, 0) < 0) {
            w->status = -4;
            return NULL;
        }
    }
    w->status = 0;
    return NULL;
}

static void run_it14(void)
{
    struct it_test_env env;
    struct pcb_t procs[4];
    struct mm_struct mms[4];
    pthread_t tids[4];
    struct it14_worker_arg args[4];

    it_env_init(&env, 16, 8);
    for (int i = 0; i < 4; ++i) {
        it_proc_init(&env, &procs[i], &mms[i], (uint32_t)(20140 + i));
        args[i].proc = &procs[i];
        args[i].seed = (BYTE)(0x10 * (i + 1));
        args[i].status = -1;
        TEST_ASSERT_EQ(0, pthread_create(&tids[i], NULL, it14_worker, &args[i]));
    }

    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQ(0, pthread_join(tids[i], NULL));
        TEST_ASSERT_EQ(0, args[i].status);
    }

    for (int i = 0; i < 4; ++i) {
        it_proc_destroy(&procs[i], &mms[i]);
    }
    verify_memphy_invariants(&env.mram, 16);
    verify_memphy_invariants(&env.mswp, 8);
    it_env_destroy(&env);
}

/* IT15: Ket thuc process A sau khi cap phat -> tai nguyen A duoc thu hoi, B van hoat dong binh thuong */
static void run_it15(void)
{
    struct it_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    it_env_init(&env, 4, 4);
    it_proc_init(&env, &proc_a, &mm_a, 20151);
    it_proc_init(&env, &proc_b, &mm_b, 20152);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 3 * page_sz, 0));
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 2 * page_sz, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)0xAA, 0, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0xBB, 0, page_sz + 12));

    TEST_ASSERT_EQ(0, free_pcb_memph(&proc_a));
    TEST_ASSERT(mm_a.fifo_pgn == NULL);

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc_b, 0, page_sz + 12, &dst_reg));
    TEST_ASSERT_EQ(0xBB, proc_b.regs[dst_reg]);

    TEST_ASSERT_EQ(0, liballoc(&proc_b, 2 * page_sz, 1));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0xCC, 1, 0));

    it_cleanup_mm_structs(&mm_a);
    it_proc_destroy(&proc_b, &mm_b);
    verify_memphy_invariants(&env.mram, 4);
    verify_memphy_invariants(&env.mswp, 4);
    it_env_destroy(&env);
}

/* IT16 (MỚI): Global Page Replacement end-to-end giua 2 tien trinh (SWAP OUT A -> B dung RAM -> SWAP IN A) */
static void run_it16(void)
{
    struct it_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    /* Tong RAM chi co 2 frame, SWAP co 4 frame */
    it_env_init(&env, 2, 4);
    it_proc_init(&env, &proc_a, &mm_a, 20161);
    it_proc_init(&env, &proc_b, &mm_b, 20162);

    addr_t page_sz = (addr_t)test_memphy_page_size();

    /* Process A chiem 2 frame RAM va ghi du lieu dac trung */
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 2 * page_sz, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)0xA1, 0, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)0xA2, 0, page_sz + 20));

    /* Process B xin 2 trang -> ep toan bo trang cua A sang SWAP va chiem RAM */
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 2 * page_sz, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0xB1, 0, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0xB2, 0, page_sz + 20));

    /* Ca A lan B doc lai du lieu -> kich hoat SWAP IN qua lai nhung du lieu phai nguyen ven */
    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc_a, 0, 10, &dst_reg));
    TEST_ASSERT_EQ(0xA1, proc_a.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc_a, 0, page_sz + 20, &dst_reg));
    TEST_ASSERT_EQ(0xA2, proc_a.regs[dst_reg]);

    TEST_ASSERT_EQ(0, libread(&proc_b, 0, 10, &dst_reg));
    TEST_ASSERT_EQ(0xB1, proc_b.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc_b, 0, page_sz + 20, &dst_reg));
    TEST_ASSERT_EQ(0xB2, proc_b.regs[dst_reg]);

    it_proc_destroy(&proc_a, &mm_a);
    it_proc_destroy(&proc_b, &mm_b);
    verify_memphy_invariants(&env.mram, 2);
    verify_memphy_invariants(&env.mswp, 4);
    it_env_destroy(&env);
}

/* IT17 (MỚI): Tich hop 3 VMA (SMALL / MEDIUM / LARGE) tren bang phan trang 5 cap & Syscall 17 */
static void run_it17(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 32, 8);
    it_proc_init(&env, &proc, &mm, 2017);

    /* Cap phat 3 vung o 3 dai kich thuoc khac nhau */
    TEST_ASSERT_EQ(0, liballoc(&proc, SMALL_MAX_SIZE, 0));           /* VMA_SMALL */
    TEST_ASSERT_EQ(0, liballoc(&proc, MEDIUM_MAX_SIZE, 1));          /* VMA_MEDIUM */
    TEST_ASSERT_EQ(0, liballoc(&proc, MEDIUM_MAX_SIZE + 4096, 2));   /* VMA_LARGE (2 pages) */

    /* Ghi va doc xuyen ranh gioi trang tren VMA_LARGE */
    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x11, 0, SMALL_MAX_SIZE - 1));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x22, 1, MEDIUM_MAX_SIZE - 1));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x33, 2, page_sz - 1));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x44, 2, page_sz));

    uint32_t dst_reg = 7;
    TEST_ASSERT_EQ(0, libread(&proc, 0, SMALL_MAX_SIZE - 1, &dst_reg));
    TEST_ASSERT_EQ(0x11, proc.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc, 1, MEDIUM_MAX_SIZE - 1, &dst_reg));
    TEST_ASSERT_EQ(0x22, proc.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc, 2, page_sz - 1, &dst_reg));
    TEST_ASSERT_EQ(0x33, proc.regs[dst_reg]);
    TEST_ASSERT_EQ(0, libread(&proc, 2, page_sz, &dst_reg));
    TEST_ASSERT_EQ(0x44, proc.regs[dst_reg]);

    /* Free VMA_MEDIUM khong anh huong den VMA_SMALL va VMA_LARGE */
    TEST_ASSERT_EQ(0, libfree(&proc, 1));
    TEST_ASSERT_EQ(-1, libread(&proc, 1, 0, &dst_reg));
    TEST_ASSERT_EQ(0, libread(&proc, 2, page_sz, &dst_reg));
    TEST_ASSERT_EQ(0x44, proc.regs[dst_reg]);

    it_proc_destroy(&proc, &mm);
    verify_memphy_invariants(&env.mram, 32);
    it_env_destroy(&env);
}

/* IT18 (MỚI): Tich hop DMA memcpy bat dong bo qua phan trang 5 cap + kiem tra chan hazard */
static void run_it18(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;
    struct code_seg_t code;
    struct inst_t insts[2];

    it_env_init(&env, 16, 4);
    it_proc_init(&env, &proc, &mm, 2018);

    /* Cap phat 2 vung LARGE (2 trang = 8192B) de test DMA dich dia chi vat ly qua 2 trang */
    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 2 * page_sz, 1));
    TEST_ASSERT_EQ(0, liballoc(&proc, 2 * page_sz, 2));

    /* Nap du lieu mau vao vung nguon (reg 1) o ca trang 0 va trang 1 */
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x5A, 1, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xA5, 1, page_sz + 100));

    /* Phat lenh DMA copy 6000 byte (tu dong cat thanh 2 goi 4096B + 1904B) */
    TEST_ASSERT_EQ(0, libmem_dma_memcpy(&proc, 1, 2, 0, 6000));
    TEST_ASSERT_EQ(2, libmem_dma_inflight(proc.pid));

    /* Gia lap CPU sap thuc thi lenh READ vao reg 2 -> phai bi chan (RAW hazard) */
    memset(&code, 0, sizeof(code));
    code.size = 2;
    code.text = insts;
    insts[0].opcode = READ; insts[0].arg_0 = 2; insts[0].arg_1 = 0; insts[0].arg_2 = 5;
    insts[1].opcode = FREE; insts[1].arg_0 = 1;
    proc.code = &code;
    proc.pc = 0;
    TEST_ASSERT_EQ(1, libmem_dma_blocked(&proc));

    /* Lenh FREE reg 1 (vung nguon) cung phai bi chan (WAR hazard) */
    proc.pc = 1;
    TEST_ASSERT_EQ(1, libmem_dma_blocked(&proc));

    /* Hoan tat 2 ngat DMA -> CPU duoc mo khoa */
    uint32_t c_pid, c_src, c_dst, c_len;
    TEST_ASSERT_EQ(0, libmem_dma_complete(&c_pid, &c_src, &c_dst, &c_len));
    TEST_ASSERT_EQ(0, libmem_dma_complete(&c_pid, &c_src, &c_dst, &c_len));
    TEST_ASSERT_EQ(0, libmem_dma_inflight(proc.pid));
    TEST_ASSERT_EQ(0, libmem_dma_blocked(&proc));

    it_proc_destroy(&proc, &mm);
    verify_memphy_invariants(&env.mram, 16);
    it_env_destroy(&env);
}
static void run_it19(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;
    const addr_t page_size = PAGING64_PAGESZ;
    const addr_t pgn_a = 0x12345ULL;
    const addr_t pgn_b = (1ULL << 36) | 0x12345ULL;
    const addr_t va_a = pgn_a * page_size;
    const addr_t va_b = pgn_b * page_size;

    it_env_init(&env, 4, 2);
    it_proc_init(&env, &proc, &mm, 5001);

    struct vm_rg_struct rg_a = {0}, rg_b = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc, va_a, va_a + page_size, va_a, 1, &rg_a));
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc, va_b, va_b + page_size, va_b, 1, &rg_b));

    addr_t pte_a = pte_get_entry(&mm, pgn_a, &proc);
    addr_t pte_b = pte_get_entry(&mm, pgn_b, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_a));
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_b));
    TEST_ASSERT_NEQ(PAGING_FPN(pte_a), PAGING_FPN(pte_b));

    BYTE read_a = 0, read_b = 0;
    TEST_ASSERT_EQ(0, MEMPHY_write(&env.mram, PAGING_FPN(pte_a) * page_size + 17, (BYTE)0x3C));
    TEST_ASSERT_EQ(0, MEMPHY_write(&env.mram, PAGING_FPN(pte_b) * page_size + 17, (BYTE)0xC3));
    TEST_ASSERT_EQ(0, MEMPHY_read(&env.mram, PAGING_FPN(pte_a) * page_size + 17, &read_a));
    TEST_ASSERT_EQ(0, MEMPHY_read(&env.mram, PAGING_FPN(pte_b) * page_size + 17, &read_b));
    TEST_ASSERT_EQ((BYTE)0x3C, read_a);
    TEST_ASSERT_EQ((BYTE)0xC3, read_b);

    /* Thu hoi frame bang cach duyet cay 5 cap du VMA sbrk khong chua 2 trang nay */
    TEST_ASSERT_EQ(0, free_pcb_memph(&proc));
    TEST_ASSERT(env.mram.used_fp_list == NULL);
    TEST_ASSERT(env.mswp.used_fp_list == NULL);
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, pgn_a, &proc));
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, pgn_b, &proc));
    verify_memphy_invariants(&env.mram, 4);
    verify_memphy_invariants(&env.mswp, 2);

    it_cleanup_mm_structs(&mm);
    it_env_destroy(&env);
}
#define RUN_TEST(id, description, function) do { \
    test_begin((id), (description)); \
    function(); \
    test_end(); \
} while (0)

int main(void)
{
    RUN_TEST("IT01", "Process A ALLOC and WRITE end-to-end", run_it01);
    RUN_TEST("IT02", "Process A READ back written data", run_it02);
    RUN_TEST("IT03", "Process B accesses same virtual address with isolation", run_it03);
    RUN_TEST("IT04", "Two processes ALLOC without physical frame collision", run_it04);
    RUN_TEST("IT05", "Process A FREE then Process B ALLOC reuses frames", run_it05);
    RUN_TEST("IT06", "Small RAM forces SWAP OUT without data loss", run_it06);
    RUN_TEST("IT07", "SWAP IN restores original page data accurately", run_it07);
    RUN_TEST("IT08", "Multiple sequential ALLOCs without region overlap", run_it08);
    RUN_TEST("IT09", "Interleaved FREE and re-ALLOC under fragmentation", run_it09);
    RUN_TEST("IT10", "Reject User READ on Kernel memory region", run_it10);
    RUN_TEST("IT11", "Reject User WRITE on Kernel memory region", run_it11);
    RUN_TEST("IT12", "COPY FROM USER across multi-page boundary", run_it12);
    RUN_TEST("IT13", "COPY TO USER across multi-page boundary", run_it13);
    RUN_TEST("IT14", "Concurrent multi-process ALLOC/WRITE/READ/FREE", run_it14);
    RUN_TEST("IT15", "Process termination reclaims resources while peer continues", run_it15);
    RUN_TEST("IT16", "Global page replacement end-to-end across two processes", run_it16);
    RUN_TEST("IT17", "Multi-VMA (SMALL/MEDIUM/LARGE) 5-level paging lifecycle", run_it17);
    RUN_TEST("IT18", "End-to-end DMA multi-page translation and hazard blocking", run_it18);
    RUN_TEST("IT19", "End-to-end mapping and reclaim across separate PGD branches", run_it19);
    test_summary();
    return 0;
}
