#include "test_helpers.h"

#include <stdlib.h>
#include <string.h>

#define TEST_MAX_CASES 64
#define TEST_ID_LEN 32
#define TEST_DESC_LEN 160

struct test_result {
    char id[TEST_ID_LEN];
    char description[TEST_DESC_LEN];
    unsigned int assertions;
    unsigned int failures;
};

static struct test_result g_results[TEST_MAX_CASES];
static unsigned int g_test_count;
static int g_current_index = -1;
static unsigned int g_total_assertions;
static unsigned int g_total_failures;

void test_begin(const char *test_id, const char *description)
{
    if (g_test_count >= TEST_MAX_CASES) {
        fprintf(stderr, "[TEST RUNNER] Too many test cases (limit=%d)\n",
                TEST_MAX_CASES);
        exit(EXIT_FAILURE);
    }

    g_current_index = (int)g_test_count++;
    struct test_result *result = &g_results[g_current_index];
    memset(result, 0, sizeof(*result));
    snprintf(result->id, sizeof(result->id), "%s",
             test_id != NULL ? test_id : "UNKNOWN");
    snprintf(result->description, sizeof(result->description), "%s",
             description != NULL ? description : "");

    printf("\n[%s] %s\n", result->id, result->description);
}

int test_current_failed(void)
{
    if (g_current_index < 0)
        return 1;
    return g_results[g_current_index].failures != 0;
}

void test_record_assertion(int passed,
                           const char *expression,
                           const char *file,
                           int line,
                           long long expected,
                           long long actual,
                           int has_values)
{
    if (g_current_index < 0) {
        fprintf(stderr, "[TEST RUNNER] Assertion outside a test case: %s\n",
                expression != NULL ? expression : "(unknown)");
        g_total_failures++;
        return;
    }

    struct test_result *result = &g_results[g_current_index];
    result->assertions++;
    g_total_assertions++;

    if (passed) {
        printf("  [PASS] %s\n", expression);
        return;
    }

    result->failures++;
    g_total_failures++;
    if (has_values) {
        printf("  [FAIL] %s | expected=%lld, actual=%lld | %s:%d\n",
               expression, expected, actual, file, line);
    } else {
        printf("  [FAIL] %s | expected=true, actual=false | %s:%d\n",
               expression, file, line);
    }
}

void test_end(void)
{
    if (g_current_index < 0)
        return;

    struct test_result *result = &g_results[g_current_index];
    printf("  => %s (%u assertions, %u failures)\n",
           result->failures == 0 ? "PASS" : "FAIL",
           result->assertions, result->failures);
    g_current_index = -1;
}

void test_summary(void)
{
    unsigned int passed = 0;
    unsigned int failed = 0;

    printf("\n================ TEST SUMMARY ================\n");
    printf("%-8s | %-7s | %-10s | %s\n", "TEST ID", "RESULT",
           "ASSERTIONS", "DESCRIPTION");
    printf("---------------------------------------------------------------\n");

    for (unsigned int i = 0; i < g_test_count; ++i) {
        const struct test_result *result = &g_results[i];
        const char *status = result->failures == 0 ? "PASS" : "FAIL";
        printf("%-8s | %-7s | %-10u | %s\n", result->id, status,
               result->assertions, result->description);
        if (result->failures == 0)
            passed++;
        else
            failed++;
    }

    printf("---------------------------------------------------------------\n");
    printf("TEST CASES: %u total | %u PASS | %u FAIL\n",
           g_test_count, passed, failed);
    printf("ASSERTIONS: %u total | %u FAIL\n",
           g_total_assertions, g_total_failures);
    printf("OVERALL: %s\n", (failed == 0 && g_total_failures == 0)
                                  ? "PASS" : "FAIL");
    printf("===============================================================\n");
}

int test_memphy_init(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
    if (mp == NULL)
        return -1;
    memset(mp, 0, sizeof(*mp));
    return init_memphy(mp, max_size, randomflg);
}

void test_memphy_destroy(struct memphy_struct *mp, int initialized)
{
    if (mp == NULL || !initialized)
        return;
    (void)MEMPHY_destroy(mp);
}

/*
 * This verifier intentionally uses the actual linked-list fields exposed by
 * os-mm.h. The traversal limit also makes a cycle detectable without looping
 * forever. A frame number must occur exactly once across both lists.
 */
int verify_memphy_invariants(struct memphy_struct *mp,
                             int expected_total_frames)
{
    int ok = 1;
    int free_count = 0;
    int used_count = 0;
    int page_size = test_memphy_page_size();
    unsigned char *seen = NULL;

    if (mp == NULL || expected_total_frames < 0 || page_size <= 0) {
        TEST_ASSERT(0 && "verify_memphy_invariants received invalid arguments");
        return 0;
    }

    if (expected_total_frames > 0) {
        seen = (unsigned char *)calloc((size_t)expected_total_frames,
                                       sizeof(*seen));
        if (seen == NULL) {
            TEST_ASSERT(0 && "unable to allocate invariant bitmap");
            return 0;
        }
    }

    struct framephy_struct *lists[2] = {
        mp->free_fp_list,
        mp->used_fp_list
    };
    const char *list_names[2] = {"free_fp_list", "used_fp_list"};
    int *counts[2] = {&free_count, &used_count};

    for (int list_index = 0; list_index < 2; ++list_index) {
        struct framephy_struct *node = lists[list_index];
        int traversed = 0;

        while (node != NULL) {
            if (traversed >= expected_total_frames) {
                printf("  [INVARIANT] Cycle or excess nodes detected in %s\n",
                       list_names[list_index]);
                ok = 0;
                break;
            }

            if (node->fpn >= (addr_t)expected_total_frames) {
                printf("  [INVARIANT] Out-of-range frame %llu in %s\n",
                       (unsigned long long)node->fpn,
                       list_names[list_index]);
                ok = 0;
            } else if (seen[node->fpn] != 0) {
                printf("  [INVARIANT] Duplicate frame %llu (same-list or cross-list)\n",
                       (unsigned long long)node->fpn);
                ok = 0;
            } else {
                seen[node->fpn] = (unsigned char)(list_index + 1);
            }

            ++(*counts[list_index]);
            ++traversed;
            node = node->fp_next;
        }
    }

    int accounted = free_count + used_count;
    int missing = 0;
    for (int i = 0; i < expected_total_frames; ++i) {
        if (seen[i] == 0)
            missing++;
    }

    if (accounted != expected_total_frames) {
        printf("  [INVARIANT] Frame accounting mismatch: total=%d, free=%d, used=%d\n",
               expected_total_frames, free_count, used_count);
        ok = 0;
    }
    if (missing != 0) {
        printf("  [INVARIANT] %d frame(s) are missing from both lists\n", missing);
        ok = 0;
    }

    /* For a successfully initialized MEMPHY, maxsz is the byte capacity. */
    if (mp->storage != NULL && expected_total_frames > 0) {
        int actual_frames = mp->maxsz / page_size;
        if (actual_frames != expected_total_frames) {
            printf("  [INVARIANT] Capacity mismatch: maxsz=%d, page_size=%d, frames=%d, expected=%d\n",
                   mp->maxsz, page_size, actual_frames, expected_total_frames);
            ok = 0;
        }
    }

    TEST_ASSERT_EQ(1, ok);
    TEST_ASSERT_EQ(expected_total_frames, accounted);
    TEST_ASSERT_EQ(0, missing);
    free(seen);
    return ok;
}
int do_dma_start(struct pcb_t *caller, uint64_t source,
                 uint64_t destination, uint64_t length)
{
    (void)caller;
    (void)source;
    (void)destination;
    (void)length;
    return 0;
}