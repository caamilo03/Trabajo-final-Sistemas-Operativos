#include "unity.h"
#include "perf.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_sample_to_json_contains_keys(void)
{
    perf_sample_t s;
    memset(&s, 0, sizeof(s));
    s.cpu_percent = 12.5;
    s.mem_rss_kb  = 4096;

    char buf[4096];
    int n = perf_sample_to_json(&s, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "cpu_pct"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "mem_rss_kb"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "12.50"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "4096"));
}

static void test_sample_to_json_buffer_too_small(void)
{
    perf_sample_t s;
    memset(&s, 0, sizeof(s));
    char buf[4];
    int n = perf_sample_to_json(&s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-1, n);
}

static void test_series_to_json_empty(void)
{
    perf_series_t series = {.samples = NULL, .count = 0, .capacity = 0};
    char buf[64];
    int n = perf_series_to_json(&series, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "[]"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sample_to_json_contains_keys);
    RUN_TEST(test_sample_to_json_buffer_too_small);
    RUN_TEST(test_series_to_json_empty);
    return UNITY_END();
}
