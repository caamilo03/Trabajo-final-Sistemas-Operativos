#include "unity.h"
#include "perf.h"
#include "perf_internal.h"

void setUp(void) {}
void tearDown(void) {}

static void test_mem_read_nonzero(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    mem_snapshot_t snap;
    perf_status_t st = sampler_mem_read(&cfg, &snap);
    TEST_ASSERT_EQUAL_INT(PERF_OK, st);
    TEST_ASSERT_GREATER_THAN(0, (int)snap.rss_kb);
}

static void test_mem_peak_ge_rss(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    mem_snapshot_t snap;
    sampler_mem_read(&cfg, &snap);
    TEST_ASSERT_GREATER_OR_EQUAL(snap.rss_kb, snap.peak_kb);
}

static void test_mem_bad_path(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    cfg.proc_root = "/nonexistent_path_xyz";
    mem_snapshot_t snap;
    perf_status_t st = sampler_mem_read(&cfg, &snap);
    TEST_ASSERT_EQUAL_INT(PERF_ERR_IO, st);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mem_read_nonzero);
    RUN_TEST(test_mem_peak_ge_rss);
    RUN_TEST(test_mem_bad_path);
    return UNITY_END();
}
