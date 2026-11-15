#include "test_helpers.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TEST_FRAME_COUNT 8
#define TEST_THREAD_COUNT 8
#define TEST_THREAD_ITERATIONS 1000

static addr_t test_capacity_bytes(int frame_count)
{
    return (addr_t)frame_count * (addr_t)test_memphy_page_size();
}

static void run_mp01(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(4), 1);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT(mp.storage != NULL);
    TEST_ASSERT_EQ(4, mp.maxsz / test_memphy_page_size());
    verify_memphy_invariants(&mp, 4);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp02(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, (addr_t)0, 1);
    TEST_ASSERT_EQ(-1, ret);
    TEST_ASSERT(mp.storage == NULL);
    verify_memphy_invariants(&mp, 0);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp03(void)
{
    struct memphy_struct mp;
    /* addr_t is uint64_t in this build: -1 converts to UINT64_MAX.
       malloc(UINT64_MAX) must fail; this tests an extreme allocation size,
       not a signed-negative value reaching init_memphy(). */
    int ret = test_memphy_init(&mp, (addr_t)-1, 1);
    TEST_ASSERT_EQ(-1, ret);
    TEST_ASSERT(mp.storage == NULL);
    verify_memphy_invariants(&mp, 0);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp04(void)
{
    struct memphy_struct mp;
    /* A nonzero capacity smaller than one 4-KiB page cannot form a frame. */
    int ret = test_memphy_init(&mp,
                               (addr_t)(test_memphy_page_size() - 1), 1);
    TEST_ASSERT_EQ(-1, ret);
    TEST_ASSERT(mp.storage == NULL);
    verify_memphy_invariants(&mp, 0);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp05(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(4), 1);
    TEST_ASSERT_EQ(0, ret);
    addr_t fpn = (addr_t)-1;
    TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &fpn));
    TEST_ASSERT_EQ(0, fpn);
    verify_memphy_invariants(&mp, 4);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp06(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(4), 1);
    TEST_ASSERT_EQ(0, ret);
    for (int i = 0; i < 4; ++i) {
        addr_t fpn = (addr_t)-1;
        TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &fpn));
        TEST_ASSERT(fpn < 4);
    }
    TEST_ASSERT(mp.free_fp_list == NULL);
    verify_memphy_invariants(&mp, 4);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp07(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(2), 1);
    TEST_ASSERT_EQ(0, ret);
    addr_t fpn;
    TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &fpn));
    TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &fpn));
    TEST_ASSERT_EQ(-1, MEMPHY_get_freefp(&mp, &fpn));
    verify_memphy_invariants(&mp, 2);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp08(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(TEST_FRAME_COUNT), 1);
    TEST_ASSERT_EQ(0, ret);
    int seen[TEST_FRAME_COUNT] = {0};
    for (int i = 0; i < TEST_FRAME_COUNT; ++i) {
        addr_t fpn = (addr_t)-1;
        TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &fpn));
        TEST_ASSERT(fpn < TEST_FRAME_COUNT);
        if (fpn < TEST_FRAME_COUNT) {
            TEST_ASSERT_EQ(0, seen[fpn]);
            seen[fpn]++;
        }
    }
    verify_memphy_invariants(&mp, TEST_FRAME_COUNT);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp09(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(4), 1);
    TEST_ASSERT_EQ(0, ret);
    addr_t fpn;
    TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &fpn));
    TEST_ASSERT_EQ(0, MEMPHY_put_freefp(&mp, fpn));
    verify_memphy_invariants(&mp, 4);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp10(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(4), 1);
    TEST_ASSERT_EQ(0, ret);
    addr_t fpn;
    TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &fpn));
    TEST_ASSERT_EQ(0, MEMPHY_put_freefp(&mp, fpn));
    TEST_ASSERT_EQ(-1, MEMPHY_put_freefp(&mp, fpn));
    verify_memphy_invariants(&mp, 4);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp11(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(4), 1);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT_EQ(-1, MEMPHY_put_freefp(&mp, 1));
    verify_memphy_invariants(&mp, 4);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp12(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(4), 1);
    TEST_ASSERT_EQ(0, ret);
    TEST_ASSERT_EQ(-1, MEMPHY_put_freefp(&mp, 4));
    TEST_ASSERT_EQ(-1, MEMPHY_put_freefp(&mp, (addr_t)-1));
    verify_memphy_invariants(&mp, 4);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp13(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(3), 1);
    TEST_ASSERT_EQ(0, ret);
    addr_t first;
    addr_t second;
    TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &first));
    TEST_ASSERT_EQ(0, MEMPHY_put_freefp(&mp, first));
    TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &second));
    TEST_ASSERT_EQ(first, second);
    verify_memphy_invariants(&mp, 3);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp14(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(2), 1);
    TEST_ASSERT_EQ(0, ret);
    BYTE value = 0;
    addr_t last = (addr_t)mp.maxsz - 1;
    TEST_ASSERT_EQ(0, MEMPHY_write(&mp, 0, (BYTE)0x35));
    TEST_ASSERT_EQ(0, MEMPHY_read(&mp, 0, &value));
    TEST_ASSERT_EQ(0x35, (unsigned char)value);
    TEST_ASSERT_EQ(0, MEMPHY_write(&mp, last, (BYTE)0x6A));
    TEST_ASSERT_EQ(0, MEMPHY_read(&mp, last, &value));
    TEST_ASSERT_EQ(0x6A, (unsigned char)value);
    verify_memphy_invariants(&mp, 2);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp15(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(2), 1);
    TEST_ASSERT_EQ(0, ret);
    BYTE value = 0;
    addr_t out_of_bounds = (addr_t)mp.maxsz;
    TEST_ASSERT_EQ(-1, MEMPHY_read(&mp, out_of_bounds, &value));
    TEST_ASSERT_EQ(-1, MEMPHY_write(&mp, out_of_bounds, (BYTE)0x7F));
    TEST_ASSERT_EQ(-1, MEMPHY_read(&mp, 0, NULL));
    TEST_ASSERT_EQ(-1, MEMPHY_write(NULL, 0, (BYTE)0x01));
    verify_memphy_invariants(&mp, 2);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp16(void)
{
    struct memphy_struct mp;
    /* randomflg=0 selects the sequential-device path. */
    int ret = test_memphy_init(&mp, test_capacity_bytes(2), 0);
    TEST_ASSERT_EQ(0, ret);
    BYTE value = 0;
    TEST_ASSERT_EQ(0, MEMPHY_write(&mp, 17, (BYTE)0x52));
    TEST_ASSERT_EQ(0, MEMPHY_read(&mp, 17, &value));
    TEST_ASSERT_EQ(0x52, (unsigned char)value);
    TEST_ASSERT_EQ(-1, MEMPHY_read(&mp, (addr_t)mp.maxsz, &value));
    verify_memphy_invariants(&mp, 2);
    test_memphy_destroy(&mp, ret == 0);
}

static void run_mp17(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(5), 1);
    TEST_ASSERT_EQ(0, ret);
    for (int round = 0; round < 100; ++round) {
        addr_t frames[5];
        for (int i = 0; i < 5; ++i)
            TEST_ASSERT_EQ(0, MEMPHY_get_freefp(&mp, &frames[i]));
        TEST_ASSERT_EQ(-1, MEMPHY_get_freefp(&mp, &frames[0]));
        for (int i = 4; i >= 0; --i)
            TEST_ASSERT_EQ(0, MEMPHY_put_freefp(&mp, frames[i]));
        TEST_ASSERT_EQ(1, verify_memphy_invariants(&mp, 5));
    }
    test_memphy_destroy(&mp, ret == 0);
}

struct worker_context {
    struct memphy_struct *mp;
    atomic_int *occupied;
    atomic_int *errors;
    int frame_count;
};

static void *mp18_worker(void *argument)
{
    struct worker_context *ctx = (struct worker_context *)argument;

    for (int iteration = 0; iteration < TEST_THREAD_ITERATIONS; ++iteration) {
        addr_t fpn = 0;
        int ret = -1;
        int retries = 0;
        while ((ret = MEMPHY_get_freefp(ctx->mp, &fpn)) == -1 &&
               retries++ < 1000000) {
            sched_yield(); /* Another worker may temporarily own every frame. */
        }

        if (ret != 0 || fpn >= (addr_t)ctx->frame_count) {
            atomic_fetch_add(ctx->errors, 1);
            break;
        }

        if (atomic_exchange(&ctx->occupied[fpn], 1) != 0)
            atomic_fetch_add(ctx->errors, 1); /* Same frame handed out twice. */

        sched_yield();
        atomic_store(&ctx->occupied[fpn], 0);

        if (MEMPHY_put_freefp(ctx->mp, fpn) != 0)
            atomic_fetch_add(ctx->errors, 1);
    }
    return NULL;
}

static void run_mp18(void)
{
    struct memphy_struct mp;
    int ret = test_memphy_init(&mp, test_capacity_bytes(TEST_FRAME_COUNT), 1);
    TEST_ASSERT_EQ(0, ret);

    pthread_t threads[TEST_THREAD_COUNT];
    int thread_created[TEST_THREAD_COUNT] = {0};
    atomic_int occupied[TEST_FRAME_COUNT];
    atomic_int errors;
    atomic_init(&errors, 0);
    for (int i = 0; i < TEST_FRAME_COUNT; ++i)
        atomic_init(&occupied[i], 0);

    struct worker_context context = {
        .mp = &mp,
        .occupied = occupied,
        .errors = &errors,
        .frame_count = TEST_FRAME_COUNT
    };

    for (int i = 0; i < TEST_THREAD_COUNT; ++i) {
        int thread_ret = pthread_create(&threads[i], NULL, mp18_worker,
                                        &context);
        TEST_ASSERT_EQ(0, thread_ret);
        thread_created[i] = (thread_ret == 0);
    }
    for (int i = 0; i < TEST_THREAD_COUNT; ++i) {
        if (thread_created[i])
            TEST_ASSERT_EQ(0, pthread_join(threads[i], NULL));
    }

    TEST_ASSERT_EQ(0, atomic_load(&errors));
    for (int i = 0; i < TEST_FRAME_COUNT; ++i)
        TEST_ASSERT_EQ(0, atomic_load(&occupied[i]));
    verify_memphy_invariants(&mp, TEST_FRAME_COUNT);
    test_memphy_destroy(&mp, ret == 0);
}

#define RUN_TEST(id, description, function) do { \
    test_begin((id), (description)); \
    function(); \
    test_end(); \
} while (0)

int main(void)
{
    RUN_TEST("MP01", "Initialize MEMPHY with valid capacity", run_mp01);
    RUN_TEST("MP02", "Reject max_size = 0", run_mp02);
    RUN_TEST("MP03", "Reject (addr_t)-1 / UINT64_MAX allocation size", run_mp03);
    RUN_TEST("MP04", "Reject capacity smaller than one page", run_mp04);
    RUN_TEST("MP05", "Allocate one frame", run_mp05);
    RUN_TEST("MP06", "Allocate all frames", run_mp06);
    RUN_TEST("MP07", "Reject allocation after exhausting frames", run_mp07);
    RUN_TEST("MP08", "Ensure allocated frame numbers are unique", run_mp08);
    RUN_TEST("MP09", "Return an allocated frame", run_mp09);
    RUN_TEST("MP10", "Reject double-free", run_mp10);
    RUN_TEST("MP11", "Reject returning a frame that was never allocated", run_mp11);
    RUN_TEST("MP12", "Reject out-of-range frame numbers", run_mp12);
    RUN_TEST("MP13", "Reuse a returned frame", run_mp13);
    RUN_TEST("MP14", "Read/write first and last valid byte", run_mp14);
    RUN_TEST("MP15", "Reject out-of-bounds and invalid read/write arguments", run_mp15);
    RUN_TEST("MP16", "Read/write through sequential-device mode", run_mp16);
    RUN_TEST("MP17", "Repeated allocate/free cycles preserve invariants", run_mp17);
    RUN_TEST("MP18", "Concurrent allocation/free with pthread workers", run_mp18);

    test_summary();
    return 0;
}
