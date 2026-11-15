#include "test_helpers.h"
#include "libmem.h"
#include "queue.h"
#include "syscall.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Khai bao cac ham he thong can thiet */
int libsyscall(struct pcb_t *caller, uint32_t syscall_idx, arg_t a1, arg_t a2, arg_t a3);
int __sys_memmap(struct krnl_t *krnl, uint32_t pid, struct sc_regs *regs);
int free_pcb_memph(struct pcb_t *caller);

/* Bang syscall that khop voi syscall.tbl */
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

    free(mm->pgd);
    free(mm->p4d);
    free(mm->pud);
    free(mm->pmd);
    free(mm->pt);
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
        for (int pgn = 0; pgn < 512; ++pgn) {
            addr_t pte = pte_get_entry(&env->kmm, (addr_t)pgn, &dummy);
            if (PAGING_PAGE_PRESENT(pte) && env->mram_ready) {
                MEMPHY_put_freefp(&env->mram, PAGING_FPN(pte));
            }
        }
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

    /* Kiem tra PTE da map vao RAM va du lieu nam dung frame vat ly */
    addr_t pte = pte_get_entry(&mm_a, 0, &proc_a);
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

/* IT02: Process A READ lai du lieu da ghi */
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
    for (int i = 0; i < 32; ++i) {
        uint32_t val = 0;
        TEST_ASSERT_EQ(0, libread(&proc_a, 0, (addr_t)(i * 16), &val));
        TEST_ASSERT_EQ((uint32_t)(0x10 + i), val);
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

    /* Ca A va B deu cap phat region 0 bat dau tai virtual address 0 */
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 256, 0));
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 256, 0));
    TEST_ASSERT_EQ(mm_a.symrgtbl[0].rg_start, mm_b.symrgtbl[0].rg_start);

    TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)0x33, 0, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0x77, 0, 10));

    uint32_t val_a = 0, val_b = 0;
    TEST_ASSERT_EQ(0, libread(&proc_a, 0, 10, &val_a));
    TEST_ASSERT_EQ(0, libread(&proc_b, 0, 10, &val_b));
    TEST_ASSERT_EQ(0x33, val_a);
    TEST_ASSERT_EQ(0x77, val_b);

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

    int seen[8] = {0};
    for (int p = 0; p < 3; ++p) {
        addr_t pte_a = pte_get_entry(&mm_a, (addr_t)p, &proc_a);
        addr_t pte_b = pte_get_entry(&mm_b, (addr_t)p, &proc_b);
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

    /* RAM chi co 2 frame, SWAP = 0 -> B chi ALLOC duoc neu A thuc su tra frame */
    it_env_init(&env, 2, 0);
    it_proc_init(&env, &proc_a, &mm_a, 20051);
    it_proc_init(&env, &proc_b, &mm_b, 20052);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 2 * page_sz, 0));
    /* Luc nay RAM da het sach -> B xin 1 page phai bi tu choi */
    TEST_ASSERT_EQ(-1, liballoc(&proc_b, page_sz, 0));

    /* Ket thuc / giai phong bo nho vat ly cua A -> B xin lai 2 page phai thanh cong */
    TEST_ASSERT_EQ(0, free_pcb_memph(&proc_a));
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 2 * page_sz, 0));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0x6C, 0, 0));

    uint32_t out = 0;
    TEST_ASSERT_EQ(0, libread(&proc_b, 0, 0, &out));
    TEST_ASSERT_EQ(0x6C, out);

    it_cleanup_mm_structs(&mm_a);
    it_proc_destroy(&proc_b, &mm_b);
    it_env_destroy(&env);
}

/* IT06 & IT07: RAM nho, buoc phat sinh SWAP OUT roi SWAP IN va kiem tra bao toan du lieu */
static void run_it06(void)
{
    struct it_test_env env;
    struct pcb_t proc_a;
    struct mm_struct mm_a;

    /* RAM = 2 frames, SWAP = 4 frames -> cap phat 5 pages ep 3 pages ra SWAP */
    it_env_init(&env, 2, 4);
    it_proc_init(&env, &proc_a, &mm_a, 2006);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc_a, 5 * page_sz, 0));

    /* Ghi mau du lieu duy nhat cho tung trang (0..4) */
    for (int p = 0; p < 5; ++p) {
        addr_t base = (addr_t)p * page_sz;
        TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)(0x20 + p), 0, base));
        TEST_ASSERT_EQ(0, libwrite(&proc_a, (BYTE)(0x90 + p), 0, base + page_sz - 1));
    }

    /* Kiem tra thuc te co trang dang nam o SWAP (khong PRESENT nhung co bit SWAPPED) */
    int swapped_count = 0;
    for (int p = 0; p < 5; ++p) {
        addr_t pte = pte_get_entry(&mm_a, (addr_t)p, &proc_a);
        if (!PAGING_PAGE_PRESENT(pte) && (pte & PAGING_PTE_SWAPPED_MASK)) {
            swapped_count++;
        }
    }
    TEST_ASSERT(swapped_count >= 3);

    it_proc_destroy(&proc_a, &mm_a);
    it_env_destroy(&env);
}

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

    /* Doc nguoc tu trang 0 -> 4 buoc he thong SWAP IN lai toan bo cac trang da bi day ra SWAP */
    for (int p = 0; p < 5; ++p) {
        addr_t base = (addr_t)p * page_sz;
        uint32_t v_head = 0, v_tail = 0;
        TEST_ASSERT_EQ(0, libread(&proc_a, 0, base, &v_head));
        TEST_ASSERT_EQ(0, libread(&proc_a, 0, base + page_sz - 1, &v_tail));
        TEST_ASSERT_EQ((uint32_t)(0x20 + p), v_head);
        TEST_ASSERT_EQ((uint32_t)(0x90 + p), v_tail);
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
        TEST_ASSERT(mm.symrgtbl[r].rg_end <= mm.symrgtbl[r + 1].rg_start);
    }

    for (int r = 0; r < 6; ++r) {
        uint32_t v0 = 0, v1 = 0;
        TEST_ASSERT_EQ(0, libread(&proc, (uint32_t)r, 0, &v0));
        TEST_ASSERT_EQ(0, libread(&proc, (uint32_t)r, 511, &v1));
        TEST_ASSERT_EQ((uint32_t)(0x40 + r), v0);
        TEST_ASSERT_EQ((uint32_t)(0xA0 + r), v1);
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

    /* Cap 4 vung 512B: R0 [0,512), R1 [512,1024), R2 [1024,1536), R3 [1536,2048) */
    for (int r = 0; r < 4; ++r) {
        TEST_ASSERT_EQ(0, liballoc(&proc, 512, (uint32_t)r));
    }
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xB1, 1, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xB3, 3, 10));

    /* Giai phong xen ke R0 va R2 */
    TEST_ASSERT_EQ(0, libfree(&proc, 0));
    TEST_ASSERT_EQ(0, libfree(&proc, 2));

    /* Cap phat lai R4 (256B) va R5 (512B) -> phai lay tu lo trong R0 / R2 ma khong de len R1, R3 */
    TEST_ASSERT_EQ(0, liballoc(&proc, 256, 4));
    TEST_ASSERT_EQ(0, liballoc(&proc, 512, 5));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xE4, 4, 10));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xE5, 5, 10));

    uint32_t v1 = 0, v3 = 0, v4 = 0, v5 = 0;
    TEST_ASSERT_EQ(0, libread(&proc, 1, 10, &v1));
    TEST_ASSERT_EQ(0, libread(&proc, 3, 10, &v3));
    TEST_ASSERT_EQ(0, libread(&proc, 4, 10, &v4));
    TEST_ASSERT_EQ(0, libread(&proc, 5, 10, &v5));
    TEST_ASSERT_EQ(0xB1, v1);
    TEST_ASSERT_EQ(0xB3, v3);
    TEST_ASSERT_EQ(0xE4, v4);
    TEST_ASSERT_EQ(0xE5, v5);

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT10 & IT11: User doc/ghi vao kernel region bi tu choi, bao ve du lieu kernel */
static void run_it10(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2010);

    /* Kernel cap phat region 5 */
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, 256, 5));

    /* User co tinh doc region 5 (thuoc kernel, khong nam trong proc.mm->symrgtbl) -> phai bi tu choi */
    uint32_t leak = 0;
    TEST_ASSERT_EQ(-1, libread(&proc, 5, 0, &leak));

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

static void run_it11(void)
{
    struct it_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    it_env_init(&env, 8, 4);
    it_proc_init(&env, &proc, &mm, 2011);

    /* Cap phat User region 0 va Kernel region 5, nap du lieu mau vao Kernel qua copy_from_user */
    TEST_ASSERT_EQ(0, liballoc(&proc, 64, 0));
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, 64, 5));
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x4B, 0, 0));
    /* Copy tu User (source = 0) -> Kernel (destination = 5) */
    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, 5, 0, 1));

    /* User co tinh ghi de vao region 5 cua Kernel -> bi tu choi */
    TEST_ASSERT_EQ(-1, libwrite(&proc, (BYTE)0xFF, 5, 0));

    /* Xoa du lieu phia User thanh 0x00 roi copy nguoc tu Kernel ve de xac nhan Kernel van con 0x4B */
    TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0x00, 0, 0));
    /* Copy tu Kernel (source = 5) -> User (destination = 0) */
    TEST_ASSERT_EQ(0, libkmem_copy_to_user(&proc, 5, 0, 0, 1));
    uint32_t k_intact = 0;
    TEST_ASSERT_EQ(0, libread(&proc, 0, 0, &k_intact));
    TEST_ASSERT_EQ(0x4B, k_intact);

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

/* IT12 & IT13: COPY FROM USER va COPY TO USER vat ngang ranh gioi nhieu trang */
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

    /* Ghi 32 byte vat ngang ranh gioi page 0 -> page 1 ben User */
    addr_t start = page_sz - 16;
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x50 + i), 0, start + (addr_t)i));
    }

    /* Copy tu User (rg 0) -> Kernel (rg 1) */
    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, 1, start, 32));

    /* Xac minh truc tiep tren khung vat ly cua Kernel */
    addr_t kpte1 = pte_get_entry(&env.kmm, 1, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(kpte1) != 0);
    BYTE kbyte_after_boundary = 0;
    MEMPHY_read(&env.mram, PAGING_FPN(kpte1) * page_sz + 0, &kbyte_after_boundary);
    TEST_ASSERT_EQ((BYTE)(0x50 + 16), kbyte_after_boundary);

    it_proc_destroy(&proc, &mm);
    it_env_destroy(&env);
}

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

    addr_t start = page_sz - 16;
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x70 + i), 0, start + (addr_t)i));
    }
    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, 1, start, 32));
    /* Ghi de rac 0xEE vao User */
    for (int i = 0; i < 32; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xEE, 0, start + (addr_t)i));
    }

    /* COPY TO USER khoi phuc lai toan bo 32 byte vat ngang 2 trang */
   TEST_ASSERT_EQ(0, libkmem_copy_to_user(&proc, 1, 0, start, 32));
    for (int i = 0; i < 32; ++i) {
        uint32_t val = 0;
        TEST_ASSERT_EQ(0, libread(&proc, 0, start + (addr_t)i, &val));
        TEST_ASSERT_EQ((uint32_t)(0x70 + i), val);
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
            uint32_t v = 0;
            if (libread(w->proc, 0, (addr_t)(i * 32), &v) < 0 ||
                v != (uint32_t)(BYTE)(w->seed + round + i)) {
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

    /* Process A ket thuc -> giai phong toan bo tai nguyen cua A */
    TEST_ASSERT_EQ(0, free_pcb_memph(&proc_a));
    TEST_ASSERT(mm_a.fifo_pgn == NULL);

    /* Process B van hoat dong hoan hao va doc dung du lieu cua minh */
    uint32_t vb = 0;
    TEST_ASSERT_EQ(0, libread(&proc_b, 0, page_sz + 12, &vb));
    TEST_ASSERT_EQ(0xBB, vb);

    /* Process B co the xin them trang nho tu cac frame ma A vua tra lai */
    TEST_ASSERT_EQ(0, liballoc(&proc_b, 2 * page_sz, 1));
    TEST_ASSERT_EQ(0, libwrite(&proc_b, (BYTE)0xCC, 1, 0));

    it_cleanup_mm_structs(&mm_a);
    it_proc_destroy(&proc_b, &mm_b);
    verify_memphy_invariants(&env.mram, 4);
    verify_memphy_invariants(&env.mswp, 4);
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

    test_summary();
    return 0;
}