#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdint.h>
#include <stdio.h>
#include "mm.h"
#include "mm64.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Test runner / reporting helpers. */
void test_begin(const char *test_id, const char *description);
void test_end(void);
void test_summary(void);
int test_current_failed(void);

void test_record_assertion(int passed,
                           const char *expression,
                           const char *file,
                           int line,
                           long long expected,
                           long long actual,
                           int has_values);

#define TEST_ASSERT(condition) do { \
    int _test_actual = !!(condition); \
    test_record_assertion(_test_actual, #condition, __FILE__, __LINE__, 1LL, \
                          (long long)_test_actual, 0); \
} while (0)

#define TEST_ASSERT_EQ(expected, actual) do { \
    long long _test_expected = (long long)(expected); \
    long long _test_actual = (long long)(actual); \
    test_record_assertion(_test_expected == _test_actual, #expected " == " #actual, \
                          __FILE__, __LINE__, _test_expected, _test_actual, 1); \
} while (0)

#define TEST_ASSERT_NEQ(expected, actual) do { \
    long long _test_expected = (long long)(expected); \
    long long _test_actual = (long long)(actual); \
    test_record_assertion(_test_expected != _test_actual, #expected " != " #actual, \
                          __FILE__, __LINE__, _test_expected, _test_actual, 1); \
} while (0)

/*
 * Initialize a MEMPHY object. The object is zeroed before calling the real
 * init_memphy() API. Returns the API return value unchanged.
 */
int test_memphy_init(struct memphy_struct *mp, addr_t max_size, int randomflg);

/* Destroy an object only when init_memphy() completed successfully. */
void test_memphy_destroy(struct memphy_struct *mp, int initialized);

/* Check list integrity, frame range, uniqueness, and total frame accounting. */
int verify_memphy_invariants(struct memphy_struct *mp,
                             int expected_total_frames);

static inline int test_memphy_page_size(void)
{
#ifdef MM64
    return PAGING64_PAGESZ;
#else
    return PAGING_PAGESZ;
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* TEST_HELPERS_H */
