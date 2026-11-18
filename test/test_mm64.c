#include "test_helpers.h"
#include "libmem.h"
#include "queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Implement this in mm64.c and declare it in mm.h.
 * It must recursively free all dynamically allocated child tables.
 */
extern int mm_free_page_tables(struct mm_struct *mm);

#define MM_TEST_FRAMES 16
#define MM_TEST_SWP_FRAMES 16

struct mm_test_env {
    struct krnl_t krnl;
    struct memphy_struct mram;
    struct memphy_struct mswp;
    struct memphy_struct *mswp_arr[PAGING_MAX_MMSWP];
    struct queue_t running_list;
    int mram_ready;
    int mswp_ready;
};

static void mm_env_init(struct mm_test_env *env, int ram_frames, int swp_frames)
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
}

static void mm_env_destroy(struct mm_test_env *env)
{
    test_memphy_destroy(&env->mram, env->mram_ready);
    test_memphy_destroy(&env->mswp, env->mswp_ready);
}

static void mm_proc_init(struct pcb_t *proc, struct mm_struct *mm,
                         struct krnl_t *krnl, uint32_t pid)
{
    memset(proc, 0, sizeof(*proc));
    memset(mm, 0, sizeof(*mm));
    proc->pid = pid;
    proc->krnl = krnl;
    proc->mm = mm;
    init_mm(mm, proc);
}

static void mm_proc_cleanup_tables(struct mm_struct *mm)
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

    /* Do not free p4d/pud/pmd/pt as flat arrays.
     * The 5-level implementation owns a tree of dynamically allocated tables.
     */
    mm_free_page_tables(mm);
}

/* MM01: Khởi tạo MM */
static void run_mm01(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 4, 4);
    memset(&proc, 0, sizeof(proc));
    memset(&mm, 0, sizeof(mm));
    proc.pid = 1;
    proc.krnl = &env.krnl;
    proc.mm = &mm;

    int ret = init_mm(&mm, &proc);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT(mm.mmap != NULL);
    TEST_ASSERT_EQ(0, mm.mmap->vm_id);
    TEST_ASSERT_EQ(0, mm.mmap->vm_start);
    TEST_ASSERT_EQ(0, mm.mmap->sbrk);
    TEST_ASSERT(mm.fifo_pgn == NULL);

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM02: Khởi tạo root và tạo nhánh bảng trang khi cần */
static void run_mm02(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 4, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 2);

    /* Root PGD must exist after init_mm(). */
    TEST_ASSERT(mm.pgd != NULL);

    /* An unmapped page must return an empty PTE. */
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, 0, &proc));

    /* Mapping a page must create the required child-table path. */
    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, 0, 3, &proc));
    addr_t pte = pte_get_entry(&mm, 0, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);
    TEST_ASSERT_EQ(3, PAGING_FPN(pte));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM03: Giải mã địa chỉ 64-bit */
static void run_mm03(void)
{
    addr_t pgd = 0, p4d = 0, pud = 0, pmd = 0, pt = 0;
    /* Đặt các chỉ số phân biệt ở từng cấp 9-bit:
       PGD=0x15, P4D=0x2A, PUD=0x3F, PMD=0x105, PT=0x1FE, offset=0xABC */
    addr_t addr = ((addr_t)0x15ULL << 48) |
                  ((addr_t)0x2AULL << 39) |
                  ((addr_t)0x3FULL << 30) |
                  ((addr_t)0x105ULL << 21) |
                  ((addr_t)0x1FEULL << 12) |
                  (addr_t)0xABCULL;

    int ret = get_pd_from_address(addr, &pgd, &p4d, &pud, &pmd, &pt);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT_EQ(0x15, pgd);
    TEST_ASSERT_EQ(0x2A, p4d);
    TEST_ASSERT_EQ(0x3F, pud);
    TEST_ASSERT_EQ(0x105, pmd);
    TEST_ASSERT_EQ(0x1FE, pt);
    TEST_ASSERT_EQ(0xABC, PAGING64_ADDR_OFFST(addr));
}

/* MM04: Giải mã page number */
static void run_mm04(void)
{
    addr_t pgd = 0, p4d = 0, pud = 0, pmd = 0, pt = 0;
    addr_t pgn = ((addr_t)0x07ULL << 36) |
                 ((addr_t)0x11ULL << 27) |
                 ((addr_t)0x22ULL << 18) |
                 ((addr_t)0x33ULL << 9) |
                 (addr_t)0x44ULL;

    int ret = get_pd_from_pagenum(pgn, &pgd, &p4d, &pud, &pmd, &pt);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT_EQ(0x07, pgd);
    TEST_ASSERT_EQ(0x11, p4d);
    TEST_ASSERT_EQ(0x22, pud);
    TEST_ASSERT_EQ(0x33, pmd);
    TEST_ASSERT_EQ(0x44, pt);
}

/* MM05: Địa chỉ tại offset 0 */
static void run_mm05(void)
{
    addr_t pgd = 0, p4d = 0, pud = 0, pmd = 0, pt = 0;
    addr_t addr = 3ULL * PAGING64_PAGESZ; /* Đầu page 3 */

    TEST_ASSERT_EQ(0, PAGING64_ADDR_OFFST(addr));
    TEST_ASSERT_EQ(0, get_pd_from_address(addr, &pgd, &p4d, &pud, &pmd, &pt));
    TEST_ASSERT_EQ(3, pt);
}

/* MM06: Địa chỉ cuối page (offset = 4095) */
static void run_mm06(void)
{
    addr_t pgd = 0, p4d = 0, pud = 0, pmd = 0, pt = 0;
    addr_t addr = 3ULL * PAGING64_PAGESZ + (PAGING64_PAGESZ - 1);

    TEST_ASSERT_EQ(4095, PAGING64_ADDR_OFFST(addr));
    TEST_ASSERT_EQ(0, get_pd_from_address(addr, &pgd, &p4d, &pud, &pmd, &pt));
    TEST_ASSERT_EQ(3, pt);
}

/* MM07: Địa chỉ chuyển sang page tiếp theo */
static void run_mm07(void)
{
    addr_t pgd1 = 0, p4d1 = 0, pud1 = 0, pmd1 = 0, pt1 = 0;
    addr_t pgd2 = 0, p4d2 = 0, pud2 = 0, pmd2 = 0, pt2 = 0;

    addr_t end_of_pg3 = 3ULL * PAGING64_PAGESZ + 4095ULL;
    addr_t start_of_pg4 = end_of_pg3 + 1ULL;

    get_pd_from_address(end_of_pg3, &pgd1, &p4d1, &pud1, &pmd1, &pt1);
    get_pd_from_address(start_of_pg4, &pgd2, &p4d2, &pud2, &pmd2, &pt2);

    TEST_ASSERT_EQ(3, pt1);
    TEST_ASSERT_EQ(4, pt2);
    TEST_ASSERT_EQ(0, PAGING64_ADDR_OFFST(start_of_pg4));
}

/* MM08: Page table entry chưa present */
static void run_mm08(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 4, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 8);

    addr_t pte = pte_get_entry(&mm, 5, &proc);
    TEST_ASSERT_EQ(0, PAGING_PAGE_PRESENT(pte));
    TEST_ASSERT_EQ(0, (pte & PAGING_PTE_SWAPPED_MASK));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM09: Ghi PTE với FPN hợp lệ */
static void run_mm09(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 9);

    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, 4, 6, &proc));
    addr_t pte = pte_get_entry(&mm, 4, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);
    TEST_ASSERT_EQ(0, (pte & PAGING_PTE_SWAPPED_MASK));
    TEST_ASSERT_EQ(6, PAGING_FPN(pte));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM10: Đọc lại PTE thông qua API, không phụ thuộc layout nội bộ */
static void run_mm10(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 10);

    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, 12, 3, &proc));
    addr_t pte_via_api = pte_get_entry(&mm, 12, &proc);

    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_via_api) != 0);
    TEST_ASSERT_EQ(0, (pte_via_api & PAGING_PTE_SWAPPED_MASK));
    TEST_ASSERT_EQ(3, PAGING_FPN(pte_via_api));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM11: Cập nhật PTE nhiều lần (overwrite & chuyển đổi trạng thái) */
static void run_mm11(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 11);

    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, 2, 1, &proc));
    TEST_ASSERT_EQ(1, PAGING_FPN(pte_get_entry(&mm, 2, &proc)));

    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, 2, 7, &proc));
    addr_t pte = pte_get_entry(&mm, 2, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);
    TEST_ASSERT_EQ(7, PAGING_FPN(pte));

    /* Chuyển sang SWAP rồi nạp ngược lại FPN mới */
    TEST_ASSERT_EQ(0, pte_set_swap(&proc, 2, 0, 3));
    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, 2, 5, &proc));
    pte = pte_get_entry(&mm, 2, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);
    TEST_ASSERT_EQ(0, (pte & PAGING_PTE_SWAPPED_MASK));
    TEST_ASSERT_EQ(5, PAGING_FPN(pte));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM12: Ghi trạng thái SWAP */
static void run_mm12(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 8);
    mm_proc_init(&proc, &mm, &env.krnl, 12);

    pte_set_fpn(&mm, 6, 2, &proc);
    TEST_ASSERT_EQ(0, pte_set_swap(&proc, 6, 1, 5));

    addr_t pte = pte_get_entry(&mm, 6, &proc);
    TEST_ASSERT_EQ(0, PAGING_PAGE_PRESENT(pte));
    TEST_ASSERT((pte & PAGING_PTE_SWAPPED_MASK) != 0);

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM13: Đọc PTE sau khi SWAP (metadata swptyp / swpoff) */
static void run_mm13(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 16);
    mm_proc_init(&proc, &mm, &env.krnl, 13);

    TEST_ASSERT_EQ(0, pte_set_swap(&proc, 9, 2, 14));
    addr_t pte = pte_get_entry(&mm, 9, &proc);

    TEST_ASSERT_EQ(2, PAGING_PTE_SWPTYP(pte));
    TEST_ASSERT_EQ(14, PAGING_PTE_SWP(pte));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM14: Mapping nhiều virtual page qua vmap_page_range() */
static void run_mm14(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 14);

    struct framephy_struct f0 = {.fpn = 1, .fp_next = NULL};
    struct framephy_struct f1 = {.fpn = 4, .fp_next = &f0};
    struct framephy_struct f2 = {.fpn = 7, .fp_next = &f1};
    struct vm_rg_struct ret_rg = {0};

    addr_t start_addr = 2ULL * PAGING64_PAGESZ;
    vmap_page_range(&proc, start_addr, 3, &f2, &ret_rg);

    TEST_ASSERT_EQ(start_addr, ret_rg.rg_start);
    TEST_ASSERT_EQ(start_addr + 3ULL * PAGING64_PAGESZ, ret_rg.rg_end);
    TEST_ASSERT_EQ(7, PAGING_FPN(pte_get_entry(&mm, 2, &proc)));
    TEST_ASSERT_EQ(4, PAGING_FPN(pte_get_entry(&mm, 3, &proc)));
    TEST_ASSERT_EQ(1, PAGING_FPN(pte_get_entry(&mm, 4, &proc)));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM15: Mapping vùng có nhiều page qua vm_map_ram - không trùng physical frame */
static void run_mm15(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, MM_TEST_FRAMES, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 15);

    struct vm_rg_struct ret_rg = {0};
    int req_pages = 6;
    addr_t ret = vm_map_ram(&proc, 0, (addr_t)req_pages * PAGING64_PAGESZ,
                            0, req_pages, &ret_rg);
    TEST_ASSERT_EQ(0, (int)ret);

    int seen_fpn[MM_TEST_FRAMES] = {0};
    for (int pgn = 0; pgn < req_pages; ++pgn) {
        addr_t pte = pte_get_entry(&mm, (addr_t)pgn, &proc);
        TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);
        addr_t fpn = PAGING_FPN(pte);
        TEST_ASSERT(fpn < MM_TEST_FRAMES);
        if (fpn < MM_TEST_FRAMES) {
            TEST_ASSERT_EQ(0, seen_fpn[fpn]);
            seen_fpn[fpn]++;
        }
    }

    verify_memphy_invariants(&env.mram, MM_TEST_FRAMES);
    free_pcb_memph(&proc);
    verify_memphy_invariants(&env.mram, MM_TEST_FRAMES);

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM16: Mapping khi RAM và SWAP đều không đủ */
static void run_mm16(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    /* Chỉ có 2 frame RAM và 1 frame SWAP = tối đa 3 trang, xin 5 trang phải thất bại */
    mm_env_init(&env, 2, 1);
    mm_proc_init(&proc, &mm, &env.krnl, 16);

    struct vm_rg_struct ret_rg = {0};
    addr_t ret = vm_map_ram(&proc, 0, 5ULL * PAGING64_PAGESZ, 0, 5, &ret_rg);
    TEST_ASSERT_EQ(-1, (int)ret);

    /* Đảm bảo page chưa được map bừa và invariant vật lý vẫn bảo toàn */
    TEST_ASSERT_EQ(0, PAGING_PAGE_PRESENT(pte_get_entry(&mm, 4, &proc)));
    verify_memphy_invariants(&env.mram, 2);

    free_pcb_memph(&proc);
    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM17: Hai process có cùng virtual address nhưng cách ly frame vật lý */
static void run_mm17(void)
{
    struct mm_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc_a, &mm_a, &env.krnl, 171);
    mm_proc_init(&proc_b, &mm_b, &env.krnl, 172);

    struct vm_rg_struct rg_a = {0}, rg_b = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc_a, 0, 2ULL * PAGING64_PAGESZ, 0, 2, &rg_a));
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc_b, 0, 2ULL * PAGING64_PAGESZ, 0, 2, &rg_b));

    addr_t fpn_a0 = PAGING_FPN(pte_get_entry(&mm_a, 0, &proc_a));
    addr_t fpn_b0 = PAGING_FPN(pte_get_entry(&mm_b, 0, &proc_b));
    TEST_ASSERT_NEQ(fpn_a0, fpn_b0);

    verify_memphy_invariants(&env.mram, 8);
    free_pcb_memph(&proc_a);
    free_pcb_memph(&proc_b);
    verify_memphy_invariants(&env.mram, 8);

    mm_proc_cleanup_tables(&mm_a);
    mm_proc_cleanup_tables(&mm_b);
    mm_env_destroy(&env);
}

/* MM18: Giải phóng tài nguyên MM (RAM + SWAP frames + PTE + fifo_pgn) */
static void run_mm18(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    /* 3 RAM frames, 4 SWAP frames -> map 5 pages sẽ đẩy 2 page sang SWAP */
    mm_env_init(&env, 3, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 18);

    struct vm_rg_struct ret_rg = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc, 0, 5ULL * PAGING64_PAGESZ, 0, 5, &ret_rg));

    int ret = free_pcb_memph(&proc);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT(mm.fifo_pgn == NULL);

    /* Toàn bộ frame trên cả RAM và SWAP phải quay về free list */
    verify_memphy_invariants(&env.mram, 3);
    verify_memphy_invariants(&env.mswp, 4);
    TEST_ASSERT(env.mram.used_fp_list == NULL);
    TEST_ASSERT(env.mswp.used_fp_list == NULL);

    /* PTE đã được xóa sạch */
    for (int i = 0; i < 5; ++i) {
        TEST_ASSERT_EQ(0, pte_get_entry(&mm, (addr_t)i, &proc));
    }

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM19: Địa chỉ không hợp lệ / vượt giới hạn */
static void run_mm19(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 4, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 19);

    /* Truyền MM NULL phải từ chối an toàn */
    TEST_ASSERT_EQ(-1, pte_set_fpn(NULL, 0, 1, NULL));
    TEST_ASSERT_EQ(-1, pte_set_swap(NULL, 0, 0, 1));
    TEST_ASSERT_EQ(0, pte_get_entry(NULL, 0, NULL));

    /* Địa chỉ không căn chỉnh page trong vm_map_kernel phải bị từ chối */
    struct vm_rg_struct krg = {0};
    env.krnl.mm = &mm;
    TEST_ASSERT_EQ(-1, (int)vm_map_kernel(&proc, 0, 4096, 123, 1, &krg));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM20: Ánh xạ đồng thời từ nhiều process */
struct mm20_worker_arg {
    struct pcb_t *proc;
    int pages;
    int status;
};

static void *mm20_worker(void *arg)
{
    struct mm20_worker_arg *w = (struct mm20_worker_arg *)arg;
    struct vm_rg_struct ret_rg = {0};
    w->status = (int)vm_map_ram(w->proc, 0,
                                (addr_t)w->pages * PAGING64_PAGESZ,
                                0, w->pages, &ret_rg);
    return NULL;
}

static void run_mm20(void)
{
    struct mm_test_env env;
    const int n_procs = 4;
    const int pages_per_proc = 4;
    struct pcb_t procs[4];
    struct mm_struct mms[4];
    pthread_t tids[4];
    struct mm20_worker_arg args[4];

    mm_env_init(&env, n_procs * pages_per_proc, 4);
    for (int i = 0; i < n_procs; ++i) {
        mm_proc_init(&procs[i], &mms[i], &env.krnl, (uint32_t)(200 + i));
        args[i].proc = &procs[i];
        args[i].pages = pages_per_proc;
        args[i].status = -1;
    }

    for (int i = 0; i < n_procs; ++i) {
        TEST_ASSERT_EQ(0, pthread_create(&tids[i], NULL, mm20_worker, &args[i]));
    }
    for (int i = 0; i < n_procs; ++i) {
        TEST_ASSERT_EQ(0, pthread_join(tids[i], NULL));
        TEST_ASSERT_EQ(0, args[i].status);
    }

    /* Kiểm tra không có 2 process nào bị cấp trùng FPN vật lý */
    int total_frames = n_procs * pages_per_proc;
    int *fpn_owners = (int *)calloc((size_t)total_frames, sizeof(int));
    TEST_ASSERT(fpn_owners != NULL);

    for (int i = 0; i < n_procs; ++i) {
        for (int p = 0; p < pages_per_proc; ++p) {
            addr_t pte = pte_get_entry(&mms[i], (addr_t)p, &procs[i]);
            TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);
            addr_t fpn = PAGING_FPN(pte);
            TEST_ASSERT(fpn < (addr_t)total_frames);
            if (fpn < (addr_t)total_frames) {
                TEST_ASSERT_EQ(0, fpn_owners[fpn]);
                fpn_owners[fpn]++;
            }
        }
    }
    free(fpn_owners);

    verify_memphy_invariants(&env.mram, total_frames);

    for (int i = 0; i < n_procs; ++i) {
        free_pcb_memph(&procs[i]);
        mm_proc_cleanup_tables(&mms[i]);
    }
    verify_memphy_invariants(&env.mram, total_frames);
    mm_env_destroy(&env);
}

/* Helper: verify two virtual pages keep independent leaf PTEs. */
static void check_independent_pages(addr_t pgn_a, addr_t pgn_b,
                                    uint32_t pid)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, pid);

    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, pgn_a, 2, &proc));
    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, pgn_b, 6, &proc));

    addr_t pte_a = pte_get_entry(&mm, pgn_a, &proc);
    addr_t pte_b = pte_get_entry(&mm, pgn_b, &proc);

    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_a) != 0);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_b) != 0);
    TEST_ASSERT_EQ(2, PAGING_FPN(pte_a));
    TEST_ASSERT_EQ(6, PAGING_FPN(pte_b));

    /* Re-read A after writing B: B must not overwrite A. */
    pte_a = pte_get_entry(&mm, pgn_a, &proc);
    TEST_ASSERT_EQ(2, PAGING_FPN(pte_a));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM21: Same PT index, different PMD index. */
static void run_mm21(void)
{
    check_independent_pages(5, (1ULL << 9) | 5, 21);
}

/* MM22: Same lower indices, different PUD index. */
static void run_mm22(void)
{
    check_independent_pages(5, (1ULL << 18) | 5, 22);
}

/* MM23: Same lower indices, different P4D index. */
static void run_mm23(void)
{
    check_independent_pages(5, (1ULL << 27) | 5, 23);
}

/* MM24: Same lower indices, different PGD index. */
static void run_mm24(void)
{
    check_independent_pages(5, (1ULL << 36) | 5, 24);
}

/* MM25: Several branches at all five levels coexist in one address space. */
static void run_mm25(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;
    const addr_t pages[] = {
        5,
        (1ULL << 9)  | 5,  /* PMD differs */
        (1ULL << 18) | 5,  /* PUD differs */
        (1ULL << 27) | 5,  /* P4D differs */
        (1ULL << 36) | 5   /* PGD differs */
    };
    const addr_t fpns[] = {2, 3, 4, 5, 6};

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 25);

    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i)
        TEST_ASSERT_EQ(0, pte_set_fpn(&mm, pages[i], fpns[i], &proc));

    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        addr_t pte = pte_get_entry(&mm, pages[i], &proc);
        TEST_ASSERT(PAGING_PAGE_PRESENT(pte) != 0);
        TEST_ASSERT_EQ(fpns[i], PAGING_FPN(pte));
    }

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM26: Hierarchical page-table cleanup releases the whole tree. */
static void run_mm26(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 26);

    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, 5, 2, &proc));
    TEST_ASSERT_EQ(0, pte_set_fpn(&mm, (1ULL << 36) | 5, 6, &proc));

    TEST_ASSERT_EQ(0, mm_free_page_tables(&mm));
    TEST_ASSERT(mm.pgd == NULL);
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, 5, &proc));
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, (1ULL << 36) | 5, &proc));

    /* Cleanup should be safe to call again. */
    TEST_ASSERT_EQ(0, mm_free_page_tables(&mm));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

#define RUN_TEST(id, description, function) do { \
    test_begin((id), (description)); \
    function(); \
    test_end(); \
} while (0)

int main(void)
{
    RUN_TEST("MM01", "Initialize MM structure", run_mm01);
    RUN_TEST("MM02", "Initialize 5-level page tables", run_mm02);
    RUN_TEST("MM03", "Decode 64-bit virtual address into 5 levels + offset", run_mm03);
    RUN_TEST("MM04", "Decode page number into 5 directory indices", run_mm04);
    RUN_TEST("MM05", "Verify page boundary at offset 0", run_mm05);
    RUN_TEST("MM06", "Verify page boundary at offset 4095", run_mm06);
    RUN_TEST("MM07", "Verify transition across page boundary", run_mm07);
    RUN_TEST("MM08", "Detect non-present page table entry", run_mm08);
    RUN_TEST("MM09", "Set PTE with valid physical frame number", run_mm09);
    RUN_TEST("MM10", "Read back PTE entry accurately", run_mm10);
    RUN_TEST("MM11", "Overwrite PTE multiple times and transition RAM/SWAP", run_mm11);
    RUN_TEST("MM12", "Set PTE to SWAPPED state", run_mm12);
    RUN_TEST("MM13", "Read SWAP type and offset from swapped PTE", run_mm13);
    RUN_TEST("MM14", "Map multiple virtual pages via vmap_page_range", run_mm14);
    RUN_TEST("MM15", "Map contiguous virtual range without duplicate frames", run_mm15);
    RUN_TEST("MM16", "Reject mapping when RAM and SWAP are exhausted", run_mm16);
    RUN_TEST("MM17", "Isolate identical virtual addresses across processes", run_mm17);
    RUN_TEST("MM18", "Release all MM resources (RAM, SWAP, PTE, FIFO)", run_mm18);
    RUN_TEST("MM19", "Reject invalid pointers and unaligned kernel mapping", run_mm19);
    RUN_TEST("MM20", "Concurrent page mapping across multiple processes", run_mm20);
    RUN_TEST("MM21", "Same PT index but different PMD index", run_mm21);
    RUN_TEST("MM22", "Same lower indices but different PUD index", run_mm22);
    RUN_TEST("MM23", "Same lower indices but different P4D index", run_mm23);
    RUN_TEST("MM24", "Same lower indices but different PGD index", run_mm24);
    RUN_TEST("MM25", "Multiple branches across all five levels", run_mm25);
    RUN_TEST("MM26", "Free the hierarchical page-table tree", run_mm26);

    test_summary();
    return 0;
}