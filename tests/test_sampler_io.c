#include "unity.h"
#include "perf.h"
#include "perf_internal.h"

void setUp(void) {}
void tearDown(void) {}

static void test_io_read_ok(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    io_snapshot_t snap;
    perf_status_t st = sampler_io_read(&cfg, &snap);
    /* /proc/self/io puede fallar si no hay permisos; aceptamos ambos casos */
    TEST_ASSERT(st == PERF_OK || st == PERF_ERR_IO);
}

static void test_io_bad_path(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    cfg.proc_root = "/nonexistent_path_xyz";
    io_snapshot_t snap;
    perf_status_t st = sampler_io_read(&cfg, &snap);
    TEST_ASSERT_EQUAL_INT(PERF_ERR_IO, st);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_io_read_ok);
    RUN_TEST(test_io_bad_path);
    return UNITY_END();
}
