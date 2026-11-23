#include "test_helpers.h"
#include "libmem.h"
#include "mm64.h"
#include "queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MM_TEST_FRAMES 16
#define MM_TEST_SWP_FRAMES 16

/* Khai bao prototype cac ham trong mm-vm.c / libmem.c chua co trong header */
int free_pcb_memph(struct pcb_t *caller);
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz);
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend);

struct mm_test_env {
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

    mm_free_page_tables(mm);
}

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

    init_mm(&env->kmm, NULL);
    env->krnl.mm = &env->kmm;
    env->kmm_ready = 1;
}

static void mm_env_destroy(struct mm_test_env *env)
{
    if (env->kmm_ready) {
        struct pcb_t dummy = {0};
        dummy.krnl = &env->krnl;
        dummy.mm = &env->kmm;
        free_pcb_memph(&dummy);
        mm_proc_cleanup_tables(&env->kmm);
    }
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

    if (krnl != NULL && krnl->running_list != NULL &&
        krnl->running_list->size < MAX_QUEUE_SIZE) {
        krnl->running_list->proc[krnl->running_list->size++] = proc;
    }
}

/* MM01: Khởi tạo MM cho User (3 VMA) và Kernel (1 VMA) */
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
    TEST_ASSERT(mm.fifo_pgn == NULL);

    /* Kiem tra du 3 VMA: VMA_SMALL [0, 2^55), VMA_MEDIUM [2^55, 2^56), VMA_LARGE [2^56, 2^57) */
    struct vm_area_struct *v0 = get_vma_by_num(&mm, VMA_SMALL);
    struct vm_area_struct *v1 = get_vma_by_num(&mm, VMA_MEDIUM);
    struct vm_area_struct *v2 = get_vma_by_num(&mm, VMA_LARGE);
    TEST_ASSERT(v0 != NULL && v1 != NULL && v2 != NULL);
    TEST_ASSERT_EQ(0ULL, v0->vm_start);
    TEST_ASSERT_EQ(1ULL << 55, v0->vm_end);
    TEST_ASSERT_EQ(v0->vm_start, v0->sbrk);
    TEST_ASSERT_EQ(1ULL << 55, v1->vm_start);
    TEST_ASSERT_EQ(1ULL << 56, v1->vm_end);
    TEST_ASSERT_EQ(1ULL << 56, v2->vm_start);
    TEST_ASSERT_EQ(1ULL << 57, v2->vm_end);

    /* Kernel MM (caller == NULL) chi co 1 VMA duy nhat id = 0 */
    TEST_ASSERT(env.kmm.mmap != NULL);
    TEST_ASSERT_EQ(0, env.kmm.mmap->vm_id);
    TEST_ASSERT(env.kmm.mmap->vm_next == NULL);

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

    TEST_ASSERT(mm.pgd != NULL);
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, 0, &proc));

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
    addr_t addr = 3ULL * PAGING64_PAGESZ;

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

/* MM10: Đọc lại PTE thông qua API */
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

/* MM16: Mapping khi RAM và SWAP đều không đủ -> rollback nguyên vẹn frame */
static void run_mm16(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 2, 1);
    mm_proc_init(&proc, &mm, &env.krnl, 16);

    struct vm_rg_struct ret_rg = {0};
    addr_t ret = vm_map_ram(&proc, 0, 5ULL * PAGING64_PAGESZ, 0, 5, &ret_rg);
    TEST_ASSERT_EQ(-1, (int)ret);

    TEST_ASSERT_EQ(0, PAGING_PAGE_PRESENT(pte_get_entry(&mm, 4, &proc)));
    verify_memphy_invariants(&env.mram, 2);
    verify_memphy_invariants(&env.mswp, 1);
    TEST_ASSERT(env.mram.used_fp_list == NULL);
    TEST_ASSERT(env.mswp.used_fp_list == NULL);

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

    mm_env_init(&env, 3, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 18);

    struct vm_rg_struct ret_rg = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc, 0, 5ULL * PAGING64_PAGESZ, 0, 5, &ret_rg));

    int ret = free_pcb_memph(&proc);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT(mm.fifo_pgn == NULL);

    verify_memphy_invariants(&env.mram, 3);
    verify_memphy_invariants(&env.mswp, 4);
    TEST_ASSERT(env.mram.used_fp_list == NULL);
    TEST_ASSERT(env.mswp.used_fp_list == NULL);

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

    TEST_ASSERT_EQ(-1, pte_set_fpn(NULL, 0, 1, NULL));
    TEST_ASSERT_EQ(-1, pte_set_swap(NULL, 0, 0, 1));
    TEST_ASSERT_EQ(0, pte_get_entry(NULL, 0, NULL));

    struct vm_rg_struct krg = {0};
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

static void check_independent_pages(addr_t pgn_a, addr_t pgn_b, uint32_t pid)
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

    pte_a = pte_get_entry(&mm, pgn_a, &proc);
    TEST_ASSERT_EQ(2, PAGING_FPN(pte_a));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM21 - MM24: Cach ly cac cap PMD / PUD / P4D / PGD */
static void run_mm21(void) { check_independent_pages(5, (1ULL << 9)  | 5, 21); }
static void run_mm22(void) { check_independent_pages(5, (1ULL << 18) | 5, 22); }
static void run_mm23(void) { check_independent_pages(5, (1ULL << 27) | 5, 23); }
static void run_mm24(void) { check_independent_pages(5, (1ULL << 36) | 5, 24); }

/* MM25: Nhiều nhánh ở cả 5 cấp cùng tồn tại */
static void run_mm25(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;
    const addr_t pages[] = {
        5,
        (1ULL << 9)  | 5,
        (1ULL << 18) | 5,
        (1ULL << 27) | 5,
        (1ULL << 36) | 5
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

/* MM26: Giải phóng cây bảng trang 5 cấp */
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

    TEST_ASSERT_EQ(0, mm_free_page_tables(&mm));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM27: Từ chối địa chỉ vượt không gian 57-bit */
static void run_mm27(void)
{
    addr_t pgd = 0, p4d = 0, pud = 0, pmd = 0, pt = 0;
    TEST_ASSERT_EQ(-1, get_pd_from_address((addr_t)1ULL << 57,
                                           &pgd, &p4d, &pud, &pmd, &pt));
    TEST_ASSERT_EQ(-1, get_pd_from_pagenum(PAGING64_MAX_PGN,
                                           &pgd, &p4d, &pud, &pmd, &pt));
    TEST_ASSERT_EQ((addr_t)1ULL << 45, PAGING64_MAX_PGN);

    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;
    mm_env_init(&env, 2, 1);
    mm_proc_init(&proc, &mm, &env.krnl, 27);
    TEST_ASSERT_EQ(-1, pte_set_fpn(&mm, PAGING64_MAX_PGN, 0, &proc));
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, PAGING64_MAX_PGN, &proc));
    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM28 (MỚI): Kiểm tra PENDING_SWP_FLAG trong 1 lần vm_map_ram vượt số frame RAM */
static void run_mm28(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    /* 2 frame RAM, 4 frame SWAP -> xin 4 trang trong 1 lan goi: 2 trang dau bi day sang SWAP, 2 trang sau nam tren RAM */
    mm_env_init(&env, 2, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 28);

    struct vm_rg_struct ret_rg = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc, 0, 4ULL * PAGING64_PAGESZ, 0, 4, &ret_rg));

    int present_cnt = 0, swapped_cnt = 0;
    for (addr_t pgn = 0; pgn < 4; ++pgn) {
        addr_t pte = pte_get_entry(&mm, pgn, &proc);
        if (PAGING_PAGE_PRESENT(pte)) present_cnt++;
        else if (pte & PAGING_PTE_SWAPPED_MASK) swapped_cnt++;
    }
    TEST_ASSERT_EQ(2, present_cnt);
    TEST_ASSERT_EQ(2, swapped_cnt);

    free_pcb_memph(&proc);
    verify_memphy_invariants(&env.mram, 2);
    verify_memphy_invariants(&env.mswp, 4);
    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM29 (MỚI): Global Page Replacement - Process B thu hồi frame RAM của Process A khi hết RAM */
static void run_mm29(void)
{
    struct mm_test_env env;
    struct pcb_t proc_a, proc_b;
    struct mm_struct mm_a, mm_b;

    /* Tong RAM chi co 2 frame, SWAP co 4 frame */
    mm_env_init(&env, 2, 4);
    mm_proc_init(&proc_a, &mm_a, &env.krnl, 291);
    mm_proc_init(&proc_b, &mm_b, &env.krnl, 292);

    /* Process A chiem tron 2 frame RAM */
    struct vm_rg_struct rg_a = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc_a, 0, 2ULL * PAGING64_PAGESZ, 0, 2, &rg_a));
    TEST_ASSERT(env.mram.free_fp_list == NULL);

    /* Process B xin 1 trang -> buoc phai chon victim tu Process A day ra SWAP */
    struct vm_rg_struct rg_b = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc_b, 0, PAGING64_PAGESZ, 0, 1, &rg_b));

    addr_t pte_b0 = pte_get_entry(&mm_b, 0, &proc_b);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_b0) != 0);

    /* Kiem tra Process A co 1 trang bi chuyen sang SWAPPED */
    int a_swapped = 0;
    for (addr_t pgn = 0; pgn < 2; ++pgn) {
        addr_t pte = pte_get_entry(&mm_a, pgn, &proc_a);
        if (!PAGING_PAGE_PRESENT(pte) && (pte & PAGING_PTE_SWAPPED_MASK))
            a_swapped++;
    }
    TEST_ASSERT_EQ(1, a_swapped);

    free_pcb_memph(&proc_a);
    free_pcb_memph(&proc_b);
    verify_memphy_invariants(&env.mram, 2);
    verify_memphy_invariants(&env.mswp, 4);
    mm_proc_cleanup_tables(&mm_a);
    mm_proc_cleanup_tables(&mm_b);
    mm_env_destroy(&env);
}

/* MM30 (MỚI): Kiểm tra các hàm VMA trong mm-vm.c (get_vma_by_num, overlap, get_vm_area_node_at_brk) */
static void run_mm30(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 30);

    /* get_vma_by_num phai tra NULL cho ID khong ton tai, khong lay nham VMA ke tiep */
    TEST_ASSERT(get_vma_by_num(&mm, -1) == NULL);
    TEST_ASSERT(get_vma_by_num(&mm, 99) == NULL);
    TEST_ASSERT(get_vma_by_num(&mm, VMA_MEDIUM) != NULL);

    /* validate_overlap_vm_area: nam gon trong VMA_MEDIUM hop le, tran sang VMA_LARGE bi tu choi */
    addr_t med_start = 1ULL << 55;
    addr_t lrg_start = 1ULL << 56;
    TEST_ASSERT_EQ(0, validate_overlap_vm_area(&proc, VMA_MEDIUM, med_start, med_start + 8192));
    TEST_ASSERT_EQ(-1, validate_overlap_vm_area(&proc, VMA_MEDIUM, med_start, lrg_start + 4096));

    /* get_vm_area_node_at_brk: tu choi kich thuoc khong boi so page, chap nhan boi so page */
    TEST_ASSERT(get_vm_area_node_at_brk(&proc, VMA_SMALL, 100) == NULL);
    struct vm_rg_struct *brk_rg = get_vm_area_node_at_brk(&proc, VMA_SMALL, 2 * PAGING64_PAGESZ);
    TEST_ASSERT(brk_rg != NULL);
    if (brk_rg != NULL) {
        TEST_ASSERT_EQ(0, brk_rg->rg_start);
        TEST_ASSERT_EQ(2 * PAGING64_PAGESZ, brk_rg->rg_end);
        TEST_ASSERT_EQ(VMA_SMALL, brk_rg->vmaid);
        free(brk_rg);
    }

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM31 (MỚI): inc_vma_limit mở rộng sbrk nhưng giữ cố định biên vm_end */
static void run_mm31(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 8, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 31);

    struct vm_area_struct *vma_med = get_vma_by_num(&mm, VMA_MEDIUM);
    TEST_ASSERT(vma_med != NULL);
    addr_t orig_end = vma_med->vm_end;
    addr_t orig_sbrk = vma_med->sbrk;

    /* Xin 5000 byte -> lam tron len 2 trang (8192 byte) */
    TEST_ASSERT_EQ(0, inc_vma_limit(&proc, VMA_MEDIUM, 5000));
    TEST_ASSERT_EQ(orig_sbrk + 2 * PAGING64_PAGESZ, vma_med->sbrk);
    TEST_ASSERT_EQ(orig_end, vma_med->vm_end);

    /* Danh sach vm_freerg_list phai nhan duoc vung [orig_sbrk, orig_sbrk + 8192) */
    TEST_ASSERT(vma_med->vm_freerg_list != NULL);
    TEST_ASSERT_EQ(orig_sbrk, vma_med->vm_freerg_list->rg_start);
    TEST_ASSERT_EQ(orig_sbrk + 2 * PAGING64_PAGESZ, vma_med->vm_freerg_list->rg_end);

    /* Co tinh mo rong vuot qua vm_end cua VMA_MEDIUM -> phai bi tu choi */
    TEST_ASSERT_EQ(-1, inc_vma_limit(&proc, VMA_MEDIUM, (1ULL << 55)));

    free_pcb_memph(&proc);
    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM32 (MỚI): vm_map_kernel & Rollback khi thiếu RAM (không dùng SWAP / FIFO) */
static void run_mm32(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    /* RAM chi co 3 frame */
    mm_env_init(&env, 3, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 32);

    struct vm_rg_struct krg = {0};
    /* Xin 4 trang cho Kernel khi RAM chi co 3 frame -> phai that bai va rollback du 3 frame RAM */
    TEST_ASSERT_EQ(-1, (int)vm_map_kernel(&proc, 0, 0, 0, 4, &krg));
    verify_memphy_invariants(&env.mram, 3);
    TEST_ASSERT(env.mram.used_fp_list == NULL);

    /* Xin 2 trang cho Kernel -> thanh cong, khong dua vao fifo_pgn */
    TEST_ASSERT_EQ(0, (int)vm_map_kernel(&proc, 0, 0, 0, 2, &krg));
    TEST_ASSERT_EQ(0, krg.rg_start);
    TEST_ASSERT_EQ(2 * PAGING64_PAGESZ, krg.rg_end);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 0, &proc)) != 0);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_get_entry(&env.kmm, 1, &proc)) != 0);
    TEST_ASSERT(env.kmm.fifo_pgn == NULL);

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);
}

/* MM33 (MỚI): vmap_pgd_memset gán mẫu 0xdeadbeef và kiểm tra RAM < 1 trang (test 1K) */
static void run_mm33(void)
{
    struct mm_test_env env;
    struct pcb_t proc;
    struct mm_struct mm;

    mm_env_init(&env, 4, 4);
    mm_proc_init(&proc, &mm, &env.krnl, 33);

    addr_t base = 4ULL * PAGING64_PAGESZ;
    TEST_ASSERT_EQ(0, vmap_pgd_memset(&proc, base, 3));
    TEST_ASSERT_EQ(0xdeadbeefULL, pte_get_entry(&mm, 4, &proc));
    TEST_ASSERT_EQ(0xdeadbeefULL, pte_get_entry(&mm, 5, &proc));
    TEST_ASSERT_EQ(0xdeadbeefULL, pte_get_entry(&mm, 6, &proc));

    mm_proc_cleanup_tables(&mm);
    mm_env_destroy(&env);

    /* Mo phong cau hinh RAM < 4KB (test 1K: 1024 byte RAM, 4 frame SWAP) */
    struct mm_test_env env_1k;
    struct pcb_t proc_1k;
    struct mm_struct mm_1k;
    memset(&env_1k, 0, sizeof(env_1k));
    env_1k.mram_ready = (test_memphy_init(&env_1k.mram, 1024, 1) == 0);
    env_1k.mswp_ready = (test_memphy_init(&env_1k.mswp, 4 * PAGING64_PAGESZ, 1) == 0);
    env_1k.mswp_arr[0] = &env_1k.mswp;
    env_1k.krnl.mram = &env_1k.mram;
    env_1k.krnl.mswp = env_1k.mswp_arr;
    env_1k.krnl.active_mswp = &env_1k.mswp;
    env_1k.krnl.running_list = &env_1k.running_list;

    mm_proc_init(&proc_1k, &mm_1k, &env_1k.krnl, 331);
    struct vm_rg_struct rg_1k = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc_1k, 0, 2 * PAGING64_PAGESZ, 0, 2, &rg_1k));
    TEST_ASSERT((pte_get_entry(&mm_1k, 0, &proc_1k) & PAGING_PTE_SWAPPED_MASK) != 0);
    TEST_ASSERT((pte_get_entry(&mm_1k, 1, &proc_1k) & PAGING_PTE_SWAPPED_MASK) != 0);

    free_pcb_memph(&proc_1k);
    mm_proc_cleanup_tables(&mm_1k);
    test_memphy_destroy(&env_1k.mram, env_1k.mram_ready);
    test_memphy_destroy(&env_1k.mswp, env_1k.mswp_ready);
}

#define RUN_TEST(id, description, function) do { \
    test_begin((id), (description)); \
    function(); \
    test_end(); \
} while (0)

int main(void)
{
    RUN_TEST("MM01", "Initialize MM structure (3 User VMAs + 1 Kernel VMA)", run_mm01);
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
    RUN_TEST("MM16", "Reject mapping when RAM and SWAP are exhausted and rollback", run_mm16);
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
    RUN_TEST("MM27", "Reject virtual addresses outside the 57-bit address space", run_mm27);
    RUN_TEST("MM28", "Single vm_map_ram exceeding free RAM spills excess pages to SWAP", run_mm28);
    RUN_TEST("MM29", "Global page replacement evicts victim from another running process", run_mm29);
    RUN_TEST("MM30", "Verify mm-vm VMA lookup, overlap checks, and brk node helpers", run_mm30);
    RUN_TEST("MM31", "Extend sbrk via inc_vma_limit while preserving reserved vm_end", run_mm31);
    RUN_TEST("MM32", "Map kernel pages via vm_map_kernel and rollback on insufficient RAM", run_mm32);
    RUN_TEST("MM33", "Verify vmap_pgd_memset pattern and sub-page RAM (<4KB) fallback", run_mm33);

    test_summary();
    return 0;
}