#include "test_helpers.h"
#include "libmem.h"
#include "queue.h"
#include "../include/syscall.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Khai bao cac ham syscall va handler trong src/ */
int libsyscall(struct pcb_t *caller, uint32_t syscall_idx, arg_t a1, arg_t a2, arg_t a3);
int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs);
int free_pcb_memph(struct pcb_t *caller);
void mm_free_page_tables(struct mm_struct *mm);

/*
 * Dinh nghia bang syscall that (khop voi syscall.tbl cua he thong)
 * de kiem tra ca duong di dispatch _syscall() -> __sys_memmap()
 * ma khong can keo theo Verilator / CPU / Loader.
 */
const char *sys_call_table[] = {
    "sys_ni_syscall",  /* 0 */
    "sys_ni_syscall",  /* 1 */
    "sys_ni_syscall",  /* 2 */
    "sys_ni_syscall",  /* 3 */
    "sys_ni_syscall",  /* 4 */
    "sys_ni_syscall",  /* 5 */
    "sys_ni_syscall",  /* 6 */
    "sys_ni_syscall",  /* 7 */
    "sys_ni_syscall",  /* 8 */
    "sys_ni_syscall",  /* 9 */
    "sys_ni_syscall",  /* 10 */
    "sys_ni_syscall",  /* 11 */
    "sys_ni_syscall",  /* 12 */
    "sys_ni_syscall",  /* 13 */
    "sys_ni_syscall",  /* 14 */
    "sys_ni_syscall",  /* 15 */
    "sys_ni_syscall",  /* 16 */
    "sys_memmap"       /* 17 */
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

struct sc17_test_env {
    struct krnl_t krnl;
    struct memphy_struct mram;
    struct memphy_struct mswp;
    struct memphy_struct *mswp_arr[PAGING_MAX_MMSWP];
    struct queue_t running_list;
    int mram_ready;
    int mswp_ready;
};

static void sc17_cleanup_mm(struct mm_struct *mm)
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

    /* Giai phong toan bo cay bang trang 5 cap cap phat dong */
    mm_free_page_tables(mm);
    mm->pgd = NULL;
}

static void sc17_env_init(struct sc17_test_env *env, int ram_frames, int swp_frames)
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
}

static void sc17_proc_init(struct sc17_test_env *env, struct pcb_t *proc,
                           struct mm_struct *mm, uint32_t pid, int add_to_running)
{
    memset(proc, 0, sizeof(*proc));
    memset(mm, 0, sizeof(*mm));
    proc->pid = pid;
    proc->krnl = &env->krnl;
    proc->mm = mm;
    init_mm(mm, proc);

    if (add_to_running && env->running_list.size < MAX_QUEUE_SIZE) {
        env->running_list.proc[env->running_list.size++] = proc;
    }
}

static void sc17_proc_destroy(struct pcb_t *proc, struct mm_struct *mm)
{
    free_pcb_memph(proc);
    sc17_cleanup_mm(mm);
}

static void sc17_env_destroy(struct sc17_test_env *env)
{
    test_memphy_destroy(&env->mram, env->mram_ready);
    test_memphy_destroy(&env->mswp, env->mswp_ready);
}

/* SC17-01: Goi syscall 17 voi SYSMEM_MAP_OP (ca truc tiep lan qua libsyscall) */
/* SC17-01: Goi syscall 17 voi SYSMEM_MAP_OP (ca truc tiep lan qua libsyscall) */
static void run_sc17_01(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 8, 4);
    sc17_proc_init(&env, &proc, &mm, 1701, 1);

    /* Tang 1: Goi truc tiep handler __sys_memmap */
    struct sc_regs regs = {0};
    regs.a1 = SYSMEM_MAP_OP;
    regs.a2 = 0;
    regs.a3 = 2;
    TEST_ASSERT_EQ(0, __sys_memmap(&env.krnl, proc.pid, &regs));
    TEST_ASSERT_EQ(0xdeadbeefULL, pte_get_entry(&mm, 0, &proc));
    TEST_ASSERT_EQ(0xdeadbeefULL, pte_get_entry(&mm, 1, &proc));

    /* Tang 2: Goi qua giao dien libsyscall(proc, 17, ...) */
    addr_t addr2 = 4ULL * PAGING64_PAGESZ;
    TEST_ASSERT_EQ(0, libsyscall(&proc, 17, SYSMEM_MAP_OP, addr2, 2));
    TEST_ASSERT_EQ(0xdeadbeefULL, pte_get_entry(&mm, 4, &proc));
    TEST_ASSERT_EQ(0xdeadbeefULL, pte_get_entry(&mm, 5, &proc));

    /* Giai phong cay bang trang truoc khi free_pcb_memph vi 0xdeadbeef khong phai FPN that */
    mm_free_page_tables(&mm);
    mm.pgd = NULL;
    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-02: Goi syscall 17 voi SYSMEM_INC_OP */
static void run_sc17_02(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 8, 4);
    sc17_proc_init(&env, &proc, &mm, 1702, 1);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    addr_t initial_vm_end = mm.mmap->vm_end; // Lưu lại giới hạn trên ban đầu (2^55)

    TEST_ASSERT_EQ(0, libsyscall(&proc, 17, SYSMEM_INC_OP, 0, page_sz));
    TEST_ASSERT_EQ(page_sz, mm.mmap->sbrk);
    TEST_ASSERT_EQ(initial_vm_end, mm.mmap->vm_end); // vm_end không đổi sau khi tăng sbrk
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&mm, 0, &proc)) != 0);

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-03: Goi syscall 17 voi SYSMEM_SWP_OP (kiem tra ca 2 chieu RAM <-> SWAP) */
static void run_sc17_03(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 4, 4);
    sc17_proc_init(&env, &proc, &mm, 1703, 1);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    /* Ghi mau du lieu vao frame 1 cua RAM */
    MEMPHY_write(&env.mram, 1 * page_sz + 0, (BYTE)0x42);
    MEMPHY_write(&env.mram, 1 * page_sz + 100, (BYTE)0x99);

    /* Chieu 1: RAM (frame 1) -> SWAP (frame 2) voi a4 = 0 */
    struct sc_regs regs = {0};
    regs.a1 = SYSMEM_SWP_OP;
    regs.a2 = 1; /* vicfpn (RAM) */
    regs.a3 = 2; /* swpfpn (SWAP) */
    regs.a4 = 0;
    TEST_ASSERT_EQ(0, __sys_memmap(&env.krnl, proc.pid, &regs));

    BYTE b0 = 0, b1 = 0;
    MEMPHY_read(&env.mswp, 2 * page_sz + 0, &b0);
    MEMPHY_read(&env.mswp, 2 * page_sz + 100, &b1);
    TEST_ASSERT_EQ((BYTE)0x42, b0);
    TEST_ASSERT_EQ((BYTE)0x99, b1);

    /* Chieu 2: SWAP (frame 2) -> RAM (frame 3) voi a4 = 1 */
    regs.a2 = 3; /* dst RAM frame */
    regs.a3 = 2; /* src SWAP frame */
    regs.a4 = 1;
    TEST_ASSERT_EQ(0, __sys_memmap(&env.krnl, proc.pid, &regs));

    BYTE r0 = 0, r1 = 0;
    MEMPHY_read(&env.mram, 3 * page_sz + 0, &r0);
    MEMPHY_read(&env.mram, 3 * page_sz + 100, &r1);
    TEST_ASSERT_EQ((BYTE)0x42, r0);
    TEST_ASSERT_EQ((BYTE)0x99, r1);
    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-04 & SC17-05: SYSMEM_IO_WRITE va SYSMEM_IO_READ */
static void run_sc17_04(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 4, 4);
    sc17_proc_init(&env, &proc, &mm, 1704, 1);

    MEMPHY_write(&env.mram, 512, (BYTE)0xC3);

    struct sc_regs r_read = {0};
    r_read.a1 = SYSMEM_IO_READ;
    r_read.a2 = 512;
    TEST_ASSERT_EQ(0, __sys_memmap(&env.krnl, proc.pid, &r_read));
    TEST_ASSERT_EQ((BYTE)0xC3, (BYTE)r_read.a3);
 
    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

static void run_sc17_05(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 4, 4);
    sc17_proc_init(&env, &proc, &mm, 1705, 1);

    /* Goi qua libsyscall(proc, 17, SYSMEM_IO_WRITE, phyaddr, val) */
    TEST_ASSERT_EQ(0, libsyscall(&proc, 17, SYSMEM_IO_WRITE, 1024, 0x7E));

    BYTE verify = 0;
    MEMPHY_read(&env.mram, 1024, &verify);
    TEST_ASSERT_EQ(0x7E, verify);

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-06: Truyen operation khong hop le */
static void run_sc17_06(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 4, 4);
    sc17_proc_init(&env, &proc, &mm, 1706, 1);

    TEST_ASSERT_EQ(-1, libsyscall(&proc, 17, 9999, 0, 0));

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-07: Truyen PID khong ton tai trong running_list */
static void run_sc17_07(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 4, 4);
    /* Khong dua proc vao running_list (add_to_running = 0) */
    sc17_proc_init(&env, &proc, &mm, 1707, 0);

    TEST_ASSERT_EQ(-1, libsyscall(&proc, 17, SYSMEM_INC_OP, 0, 4096));
    TEST_ASSERT_EQ(0, mm.mmap->sbrk);

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-08: Truyen PID cua process khac - chi thao tac dung process mang PID do */
static void run_sc17_08(void)
{
    struct sc17_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    sc17_env_init(&env, 8, 4);
    sc17_proc_init(&env, &proc_a, &mm_a, 17081, 1);
    sc17_proc_init(&env, &proc_b, &mm_b, 17082, 1);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    struct sc_regs regs = {0};
    regs.a1 = SYSMEM_INC_OP;
    regs.a2 = 0;
    regs.a3 = page_sz;

    /* Goi syscall voi PID cua proc_b -> chi mm_b tang sbrk, mm_a van bang 0 */
    TEST_ASSERT_EQ(0, __sys_memmap(&env.krnl, proc_b.pid, &regs));
    TEST_ASSERT_EQ(0, mm_a.mmap->sbrk);
    TEST_ASSERT_EQ(page_sz, mm_b.mmap->sbrk);

    sc17_proc_destroy(&proc_a, &mm_a);
    sc17_proc_destroy(&proc_b, &mm_b);
    sc17_env_destroy(&env);
}

/* SC17-09: Truyen tham so khong hop le (NULL krnl/regs, vmaid sai) */
static void run_sc17_09(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 4, 4);
    sc17_proc_init(&env, &proc, &mm, 1709, 1);

    struct sc_regs regs = {0};
    TEST_ASSERT_EQ(-1, __sys_memmap(NULL, proc.pid, &regs));
    TEST_ASSERT_EQ(-1, __sys_memmap(&env.krnl, proc.pid, NULL));

    /* vmaid = 99 khong ton tai */
    TEST_ASSERT_EQ(-1, libsyscall(&proc, 17, SYSMEM_INC_OP, 99, 4096));

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-10: Goi syscall khi MEMPHY het sach frame (RAM + SWAP = 0) */
static void run_sc17_10(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    /* 1 RAM, 1 SWAP -> mo rong 1 page thanh cong, lan 2 xin 2 page phai that bai */
    sc17_env_init(&env, 1, 1);
    sc17_proc_init(&env, &proc, &mm, 1710, 1);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, libsyscall(&proc, 17, SYSMEM_INC_OP, 0, page_sz));
    TEST_ASSERT_EQ(-1, libsyscall(&proc, 17, SYSMEM_INC_OP, 0, 2 * page_sz));
    TEST_ASSERT_EQ(page_sz, mm.mmap->sbrk);

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-11: IO_READ tai dia chi ngoai vung MEMPHY */
static void run_sc17_11(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 2, 2);
    sc17_proc_init(&env, &proc, &mm, 1711, 1);

    addr_t oob_addr = (addr_t)env.mram.maxsz;
    TEST_ASSERT_EQ(-1, libsyscall(&proc, 17, SYSMEM_IO_READ, oob_addr, 0));

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-12: IO_WRITE tai dia chi ngoai vung MEMPHY */
static void run_sc17_12(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 2, 2);
    sc17_proc_init(&env, &proc, &mm, 1712, 1);

    addr_t oob_addr = (addr_t)env.mram.maxsz + 64;
    TEST_ASSERT_EQ(-1, libsyscall(&proc, 17, SYSMEM_IO_WRITE, oob_addr, 0xAB));

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-13: Kiem tra giao tiep qua krnl_t (mram == NULL bi tu choi an toan) */
static void run_sc17_13(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 2, 2);
    sc17_proc_init(&env, &proc, &mm, 1713, 1);

    struct memphy_struct *saved_mram = env.krnl.mram;
    env.krnl.mram = NULL;
    TEST_ASSERT_EQ(-1, libsyscall(&proc, 17, SYSMEM_IO_READ, 0, 0));
    env.krnl.mram = saved_mram;

    TEST_ASSERT_EQ(0, libsyscall(&proc, 17, SYSMEM_IO_WRITE, 0, 0x55));

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-14: Goi syscall lien tiep nhieu lan khong lam sai lech trang thai */
static void run_sc17_14(void)
{
    struct sc17_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    sc17_env_init(&env, 8, 4);
    sc17_proc_init(&env, &proc, &mm, 1714, 1);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQ(0, libsyscall(&proc, 17, SYSMEM_INC_OP, 0, page_sz));
    }
    TEST_ASSERT_EQ(4 * page_sz, mm.mmap->sbrk);

    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQ(0, libsyscall(&proc, 17, SYSMEM_IO_WRITE, (arg_t)i, (arg_t)(0x20 + i)));
    }
    for (int i = 0; i < 16; ++i) {
        struct sc_regs r = {0};
        r.a1 = SYSMEM_IO_READ;
        r.a2 = (arg_t)i;
        TEST_ASSERT_EQ(0, __sys_memmap(&env.krnl, proc.pid, &r));
        TEST_ASSERT_EQ((arg_t)(0x20 + i), r.a3);
    }

    sc17_proc_destroy(&proc, &mm);
    sc17_env_destroy(&env);
}

/* SC17-15: Hai process goi syscall dong thoi */
struct sc17_worker_arg {
    struct pcb_t *proc;
    addr_t base_phyaddr;
    BYTE tag;
    int status;
};

static void *sc17_worker(void *arg)
{
    struct sc17_worker_arg *w = (struct sc17_worker_arg *)arg;
    for (int i = 0; i < 64; ++i) {
        if (libsyscall(w->proc, 17, SYSMEM_IO_WRITE,
                       w->base_phyaddr + (arg_t)i,
                       (arg_t)(w->tag + i)) < 0) {
            w->status = -1;
            return NULL;
        }
    }
    w->status = 0;
    return NULL;
}

static void run_sc17_15(void)
{
    struct sc17_test_env env;
    struct pcb_t proc1, proc2;
    struct mm_struct mm1, mm2;

    sc17_env_init(&env, 8, 4);
    sc17_proc_init(&env, &proc1, &mm1, 17151, 1);
    sc17_proc_init(&env, &proc2, &mm2, 17152, 1);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    struct sc17_worker_arg w1 = {&proc1, 0, 0x10, -1};
    struct sc17_worker_arg w2 = {&proc2, page_sz, 0x80, -1};
    pthread_t t1, t2;

    TEST_ASSERT_EQ(0, pthread_create(&t1, NULL, sc17_worker, &w1));
    TEST_ASSERT_EQ(0, pthread_create(&t2, NULL, sc17_worker, &w2));
    TEST_ASSERT_EQ(0, pthread_join(t1, NULL));
    TEST_ASSERT_EQ(0, pthread_join(t2, NULL));
    TEST_ASSERT_EQ(0, w1.status);
    TEST_ASSERT_EQ(0, w2.status);

    for (int i = 0; i < 64; ++i) {
        BYTE v1 = 0, v2 = 0;
        MEMPHY_read(&env.mram, (addr_t)i, &v1);
        MEMPHY_read(&env.mram, page_sz + (addr_t)i, &v2);
        TEST_ASSERT_EQ((BYTE)(0x10 + i), v1);
        TEST_ASSERT_EQ((BYTE)(0x80 + i), v2);
    }

    sc17_proc_destroy(&proc1, &mm1);
    sc17_proc_destroy(&proc2, &mm2);
    sc17_env_destroy(&env);
}

#define RUN_TEST(id, description, function) do { \
    test_begin((id), (description)); \
    function(); \
    test_end(); \
} while (0)

int main(void)
{
    RUN_TEST("SC17-01", "Invoke syscall 17 with SYSMEM_MAP_OP", run_sc17_01);
    RUN_TEST("SC17-02", "Invoke syscall 17 with SYSMEM_INC_OP", run_sc17_02);
    RUN_TEST("SC17-03", "Invoke syscall 17 with SYSMEM_SWP_OP (both directions)", run_sc17_03);
    RUN_TEST("SC17-04", "Invoke syscall 17 with SYSMEM_IO_READ", run_sc17_04);
    RUN_TEST("SC17-05", "Invoke syscall 17 with SYSMEM_IO_WRITE", run_sc17_05);
    RUN_TEST("SC17-06", "Reject invalid memop code", run_sc17_06);
    RUN_TEST("SC17-07", "Reject non-existent process PID", run_sc17_07);
    RUN_TEST("SC17-08", "Isolate syscall execution by target PID", run_sc17_08);
    RUN_TEST("SC17-09", "Reject invalid parameters (NULL pointers, bad vmaid)", run_sc17_09);
    RUN_TEST("SC17-10", "Reject SYSMEM_INC_OP when MEMPHY frames are exhausted", run_sc17_10);
    RUN_TEST("SC17-11", "Reject SYSMEM_IO_READ out-of-bounds address", run_sc17_11);
    RUN_TEST("SC17-12", "Reject SYSMEM_IO_WRITE out-of-bounds address", run_sc17_12);
    RUN_TEST("SC17-13", "Validate kernel krnl_t interface integrity", run_sc17_13);
    RUN_TEST("SC17-14", "Sequential syscall invocations preserve state", run_sc17_14);
    RUN_TEST("SC17-15", "Concurrent syscalls from two processes", run_sc17_15);

    test_summary();
    return 0;
}