#include "test_helpers.h"
#include "libmem.h"
#include "queue.h"
#include "syscall.h"

#include <stdlib.h>
#include <string.h>

/* This test exercises vm_map_ram/free_pcb_memph without executing syscalls. */
int _syscall(struct krnl_t *krnl, uint32_t pid, uint32_t nr,
             struct sc_regs *regs)
{
    (void)krnl;
    (void)pid;
    (void)nr;
    (void)regs;
    return -1;
}

static void cleanup_mm(struct mm_struct *mm)
{
    if (mm == NULL) return;

    while (mm->fifo_pgn != NULL) {
        struct pgn_t *next = mm->fifo_pgn->pg_next;
        free(mm->fifo_pgn);
        mm->fifo_pgn = next;
    }

    struct vm_area_struct *vma = mm->mmap;
    while (vma != NULL) {
        struct vm_area_struct *next_vma = vma->vm_next;
        struct vm_rg_struct *rg = vma->vm_freerg_list;
        while (rg != NULL) {
            struct vm_rg_struct *next_rg = rg->rg_next;
            free(rg);
            rg = next_rg;
        }
        free(vma);
        vma = next_vma;
    }

    mm_free_page_tables(mm);
}

static void run_it5l01(void)
{
    struct krnl_t krnl;
    struct memphy_struct mram, mswp;
    struct memphy_struct *mswp_arr[PAGING_MAX_MMSWP] = {0};
    struct queue_t running_list = {0};
    struct mm_struct mm;
    struct pcb_t proc;
    const int ram_frames = 4;
    const int swap_frames = 2;
    const addr_t page_size = PAGING64_PAGESZ;
    const addr_t pgn_a = 0x12345ULL;
    const addr_t pgn_b = (1ULL << 36) | 0x12345ULL;
    const addr_t va_a = pgn_a * page_size;
    const addr_t va_b = pgn_b * page_size;
    int ram_ready = 0, swap_ready = 0;

    memset(&krnl, 0, sizeof(krnl));
    memset(&mm, 0, sizeof(mm));
    memset(&proc, 0, sizeof(proc));

    ram_ready = (test_memphy_init(&mram, ram_frames * page_size, 1) == 0);
    swap_ready = (test_memphy_init(&mswp, swap_frames * page_size, 1) == 0);
    TEST_ASSERT(ram_ready);
    TEST_ASSERT(swap_ready);
    if (!ram_ready || !swap_ready) goto cleanup_devices;

    mswp_arr[0] = &mswp;
    krnl.mram = &mram;
    krnl.mswp = mswp_arr;
    krnl.active_mswp = &mswp;
    krnl.active_mswp_id = 0;
    krnl.running_list = &running_list;

    proc.pid = 5001;
    proc.krnl = &krnl;
    proc.mm = &mm;
    TEST_ASSERT_EQ(0, init_mm(&mm, &proc));

    /* Same PT offset, but different PGD index: two independent tree branches. */
    struct vm_rg_struct rg_a = {0}, rg_b = {0};
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc, va_a, va_a + page_size,
                                     va_a, 1, &rg_a));
    TEST_ASSERT_EQ(0, (int)vm_map_ram(&proc, va_b, va_b + page_size,
                                     va_b, 1, &rg_b));

    addr_t pte_a = pte_get_entry(&mm, pgn_a, &proc);
    addr_t pte_b = pte_get_entry(&mm, pgn_b, &proc);
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_a));
    TEST_ASSERT(PAGING_PAGE_PRESENT(pte_b));
    TEST_ASSERT_NEQ(PAGING_FPN(pte_a), PAGING_FPN(pte_b));

    /* Verify the virtual pages lead to independent physical frames. */
    BYTE value_a = (BYTE)0x3C, value_b = (BYTE)0xC3;
    BYTE read_a = 0, read_b = 0;
    TEST_ASSERT_EQ(0, MEMPHY_write(&mram,
                    PAGING_FPN(pte_a) * page_size + 17, value_a));
    TEST_ASSERT_EQ(0, MEMPHY_write(&mram,
                    PAGING_FPN(pte_b) * page_size + 17, value_b));
    TEST_ASSERT_EQ(0, MEMPHY_read(&mram,
                    PAGING_FPN(pte_a) * page_size + 17, &read_a));
    TEST_ASSERT_EQ(0, MEMPHY_read(&mram,
                    PAGING_FPN(pte_b) * page_size + 17, &read_b));
    TEST_ASSERT_EQ(value_a, read_a);
    TEST_ASSERT_EQ(value_b, read_b);

    /* The VMA list does not describe these direct vm_map_ram mappings.
     * free_pcb_memph must therefore walk every leaf PT in the 5-level tree.
     */
    TEST_ASSERT_EQ(0, free_pcb_memph(&proc));
    TEST_ASSERT(mram.used_fp_list == NULL);
    TEST_ASSERT(mswp.used_fp_list == NULL);
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, pgn_a, &proc));
    TEST_ASSERT_EQ(0, pte_get_entry(&mm, pgn_b, &proc));
    verify_memphy_invariants(&mram, ram_frames);
    verify_memphy_invariants(&mswp, swap_frames);

    cleanup_mm(&mm);

cleanup_devices:
    test_memphy_destroy(&mram, ram_ready);
    test_memphy_destroy(&mswp, swap_ready);
}

int main(void)
{
    test_begin("IT5L01", "End-to-end mapping and reclaim across separate PGD branches");
    run_it5l01();
    test_end();
    test_summary();
    return 0;
}
