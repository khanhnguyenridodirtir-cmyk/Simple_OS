#include "test_helpers.h"
#include "libmem.h"
#include "queue.h"
#include "syscall.h"

#include <stdlib.h>
#include <string.h>

/* Khai bao ham dinh nghia trong src/libmem.c nhung chua co trong mm.h */
int free_pcb_memph(struct pcb_t *caller);
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller);
int pg_getval(struct mm_struct *mm, int addr, BYTE *data, struct pcb_t *caller);
int pg_setval(struct mm_struct *mm, int addr, BYTE value, struct pcb_t *caller);

/*
 * Cung cap stub _syscall va bang syscall de link doc lap test_libmem
 * ma khong can keo theo Verilator / CPU / Loader.
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

    free(mm->pgd);
    free(mm->p4d);
    free(mm->pud);
    free(mm->pmd);
    free(mm->pt);
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

    /* Dua proc vao running_list de __sys_memmap tim thay caller theo pid */
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
        /* Thu hoi cac frame kernel da map (neu co) */
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

/* LM01: ALLOC kích thước nhỏ */
static void run_lm01(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 8);
    lm_proc_init(&env, &proc, &mm, 101);

    int ret = liballoc(&proc, 128, 0);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT_EQ(0, mm.symrgtbl[0].rg_start);
    TEST_ASSERT_EQ(128, mm.symrgtbl[0].rg_end);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM02: ALLOC đúng kích thước một page */
static void run_lm02(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 8, 8);
    lm_proc_init(&env, &proc, &mm, 102);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    int ret = liballoc(&proc, page_sz, 1);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT_EQ(0, mm.symrgtbl[1].rg_start);
    TEST_ASSERT_EQ(page_sz, mm.symrgtbl[1].rg_end);
    TEST_ASSERT_EQ(page_sz, mm.mmap->sbrk);

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
    TEST_ASSERT_EQ(0, mm.symrgtbl[2].rg_start);
    TEST_ASSERT_EQ(req_sz, mm.symrgtbl[2].rg_end);
    TEST_ASSERT_EQ(2 * test_memphy_page_size(), mm.mmap->sbrk);

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

    TEST_ASSERT(mm.symrgtbl[0].rg_end <= mm.symrgtbl[1].rg_start);
    TEST_ASSERT(mm.symrgtbl[1].rg_end <= mm.symrgtbl[2].rg_start);
    TEST_ASSERT_EQ(3500, mm.symrgtbl[2].rg_end);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM05: ALLOC khi hết RAM */
static void run_lm05(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    /* RAM chỉ có 2 frame (8KB), SWAP có 4 frame -> xin 3 page (12KB) phải tận dụng SWAP thành công */
    lm_env_init(&env, 2, 4);
    lm_proc_init(&env, &proc, &mm, 105);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 3 * page_sz, 0));

    /* Xin tiếp 5 page nữa vượt cả RAM + SWAP -> phải trả về lỗi -1 */
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

/* LM07: ALLOC vượt khả năng virtual memory */
static void run_lm07(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 107);

    /* Xin kích thước khổng lồ vượt quá tài nguyên hệ thống */
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
    TEST_ASSERT_EQ(0, libfree(&proc, 0));
    TEST_ASSERT_EQ(0, mm.symrgtbl[0].rg_start);
    TEST_ASSERT_EQ(0, mm.symrgtbl[0].rg_end);

    /* Vùng [0, 512) phải xuất hiện trong vm_freerg_list */
    int found = 0;
    struct vm_rg_struct *it = mm.mmap->vm_freerg_list;
    while (it != NULL) {
        if (it->rg_start == 0 && it->rg_end == 512) {
            found = 1;
            break;
        }
        it = it->rg_next;
    }
    TEST_ASSERT_EQ(1, found);

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM09: FREE hai lần (Double free) */
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

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM10: FREE địa chỉ / region không tồn tại */
static void run_lm10(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 110);

    TEST_ASSERT_EQ(-1, libfree(&proc, 5));
    TEST_ASSERT_EQ(-1, libfree(&proc, PAGING_MAX_SYMTBL_SZ + 1));

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

    TEST_ASSERT_EQ(0, liballoc(&proc, 1024, 0));
    addr_t sbrk_before = mm.mmap->sbrk;
    TEST_ASSERT_EQ(0, libfree(&proc, 0));

    /* Cấp phát lại 512B phải lấy lại từ [0, 1024) mà không tăng sbrk */
    TEST_ASSERT_EQ(0, liballoc(&proc, 512, 1));
    TEST_ASSERT_EQ(0, mm.symrgtbl[1].rg_start);
    TEST_ASSERT_EQ(512, mm.symrgtbl[1].rg_end);
    TEST_ASSERT_EQ(sbrk_before, mm.mmap->sbrk);

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

    uint32_t out = 0;
    TEST_ASSERT_EQ(0, libread(&proc, 0, 42, &out));
    TEST_ASSERT_EQ(0x5A, out);

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
    for (int i = 0; i < 64; ++i) {
        uint32_t expected = (uint32_t)((i * 7 + 13) & 0xFF);
        uint32_t actual = 0;
        TEST_ASSERT_EQ(0, libread(&proc, 0, (addr_t)(i * 8), &actual));
        TEST_ASSERT_EQ(expected, actual);
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

    uint32_t val = 0;
    TEST_ASSERT_EQ(0, libread(&proc, 0, 299, &val));
    TEST_ASSERT_EQ(0xE7, val);

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
    uint32_t val = 0;
    TEST_ASSERT_EQ(-1, libread(&proc, 0, 200, &val));
    TEST_ASSERT_EQ(-1, libread(&proc, 0, 500, &val));
    TEST_ASSERT_EQ(-1, libread(&proc, 1, 0, &val)); /* Region 1 chưa cấp phát */

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

    /* Ghi vượt biên region 0 (offset 100 chạm vào đầu region 1) phải bị từ chối */
    TEST_ASSERT_EQ(-1, libwrite(&proc, (BYTE)0x99, 0, 100));

    /* Dữ liệu đầu region 1 phải còn nguyên 0x11 */
    uint32_t val = 0;
    TEST_ASSERT_EQ(0, libread(&proc, 1, 0, &val));
    TEST_ASSERT_EQ(0x11, val);

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

    /* Ghi 16 byte vắt ngang ranh giới page 0 (offset 4088..4095) sang page 1 (4096..4103) */
    addr_t cross_start = page_sz - 8;
    for (int i = 0; i < 16; ++i) {
        BYTE b = (BYTE)(0xA0 + i);
        TEST_ASSERT_EQ(0, libwrite(&proc, b, 0, cross_start + (addr_t)i));
    }

    for (int i = 0; i < 16; ++i) {
        uint32_t expected = (uint32_t)(0xA0 + i);
        uint32_t actual = 0;
        TEST_ASSERT_EQ(0, libread(&proc, 0, cross_start + (addr_t)i, &actual));
        TEST_ASSERT_EQ(expected, actual);
    }

    lm_proc_destroy(&proc, &mm);
    lm_env_destroy(&env);
}

/* LM18: Đọc page chưa present (Demand paging / Page fault path trong pg_getpage) */
static void run_lm18(void)
{
    struct lm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    lm_env_init(&env, 4, 4);
    lm_proc_init(&env, &proc, &mm, 118);

    /* Trang 5 chưa present -> pg_getpage phải tự động xin frame RAM và map vào PTE */
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

    /* Giới hạn RAM chỉ có 2 frame (8KB), SWAP có 4 frame -> dùng 4 page (16KB) chắc chắn phải SWAP */
    lm_env_init(&env, 2, 4);
    lm_proc_init(&env, &proc, &mm, 119);

    addr_t page_sz = (addr_t)test_memphy_page_size();
    TEST_ASSERT_EQ(0, liballoc(&proc, 4 * page_sz, 0));

    /* Ghi dữ liệu đặc trưng lên đầu và cuối mỗi trang trong cả 4 trang */
    for (int p = 0; p < 4; ++p) {
        addr_t base = (addr_t)p * page_sz;
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x10 + p), 0, base));
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x80 + p), 0, base + page_sz - 1));
    }

    /* Lúc này trang 0 và trang 1 đã bị đẩy ra SWAP; đọc lại từng trang buộc hệ thống SWAP IN */
    for (int p = 0; p < 4; ++p) {
        addr_t base = (addr_t)p * page_sz;
        uint32_t head_val = 0, tail_val = 0;
        TEST_ASSERT_EQ(0, libread(&proc, 0, base, &head_val));
        TEST_ASSERT_EQ(0, libread(&proc, 0, base + page_sz - 1, &tail_val));
        TEST_ASSERT_EQ((uint32_t)(0x10 + p), head_val);
        TEST_ASSERT_EQ((uint32_t)(0x80 + p), tail_val);
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

    /* Xác minh 2 trang kernel đều nằm trên RAM (PRESENT) và không đưa vào fifo_pgn của user */
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 0, &proc)) != 0);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 1, &proc)) != 0);
    TEST_ASSERT(env.kmm.fifo_pgn == NULL);

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

    /* Cấp phát region 0 bên User và region 0 bên Kernel */
    TEST_ASSERT_EQ(0, liballoc(&proc, 256, 0));
    TEST_ASSERT_EQ(0, libkmem_malloc(&proc, 256, 0));

    /* Ghi chuỗi 16 byte vào User region 0 tại offset 10..25 */
    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)(0x30 + i), 0, (addr_t)(10 + i)));
    }

    /* Copy từ User -> Kernel */
    TEST_ASSERT_EQ(0, libkmem_copy_from_user(&proc, 0, 0, 10, 16));

    /* Sửa dữ liệu bên User thành 0xFF để đảm bảo không đọc nhầm từ User */
    for (int i = 0; i < 16; ++i) {
        TEST_ASSERT_EQ(0, libwrite(&proc, (BYTE)0xFF, 0, (addr_t)(10 + i)));
    }

    /* Copy ngược lại từ Kernel -> User */
    TEST_ASSERT_EQ(0, libkmem_copy_to_user(&proc, 0, 0, 10, 16));

    /* Kiểm tra dữ liệu ban đầu 0x30..0x3F đã quay về User */
    for (int i = 0; i < 16; ++i) {
        uint32_t val = 0;
        TEST_ASSERT_EQ(0, libread(&proc, 0, (addr_t)(10 + i), &val));
        TEST_ASSERT_EQ((uint32_t)(0x30 + i), val);
    }

    /* Kiểm tra chặn copy vượt biên */
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

    test_summary();
    return 0;
}