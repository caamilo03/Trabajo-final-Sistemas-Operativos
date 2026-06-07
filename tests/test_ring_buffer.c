#include "unity.h"
#include "perf_internal.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_ring_push_drain(void)
{
    ring_buffer_t rb;
    ring_init(&rb);

    perf_sample_t s;
    memset(&s, 0, sizeof(s));
    s.mem_rss_kb = 1234;

    ring_push(&rb, &s);

    perf_sample_t dst[4];
    size_t n = ring_drain(&rb, dst, 4);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(1234, dst[0].mem_rss_kb);
}

static void test_ring_empty_drain(void)
{
    ring_buffer_t rb;
    ring_init(&rb);

    perf_sample_t dst[4];
    size_t n = ring_drain(&rb, dst, 4);
    TEST_ASSERT_EQUAL_UINT(0, n);
}

static void test_ring_overflow_keeps_newest(void)
{
    ring_buffer_t rb;
    ring_init(&rb);

    perf_sample_t s;
    memset(&s, 0, sizeof(s));

    /* Llenar más del capacity: el ring descarta las más antiguas */
    for (size_t i = 0; i < RING_CAPACITY + 10; i++) {
        s.mem_rss_kb = (uint64_t)i;
        ring_push(&rb, &s);
    }

    perf_sample_t dst[RING_CAPACITY];
    size_t n = ring_drain(&rb, dst, RING_CAPACITY);
    /* El último valor debe ser el más reciente */
    TEST_ASSERT_EQUAL_UINT64(RING_CAPACITY + 10 - 1, dst[n - 1].mem_rss_kb);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ring_push_drain);
    RUN_TEST(test_ring_empty_drain);
    RUN_TEST(test_ring_overflow_keeps_newest);
    return UNITY_END();
}
