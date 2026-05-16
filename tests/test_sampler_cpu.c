#include "unity.h"
#include "perf.h"
#include "perf_internal.h"

void setUp(void) {}
void tearDown(void) {}

void test_cpu_read_self(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    cpu_snapshot_t snap;
    perf_status_t st = sampler_cpu_read(&cfg, &snap);
    TEST_ASSERT_EQUAL_INT(PERF_OK, st);
    TEST_ASSERT_GREATER_THAN(0, (int)snap.ticks_per_sec);
}

void test_cpu_percent_negative_on_first(void)
{
    double pct = sampler_cpu_percent(NULL, NULL);
    TEST_ASSERT_LESS_OR_EQUAL(0.0, pct);
}

void test_cpu_percent_range(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    cpu_snapshot_t s1, s2;

    sampler_cpu_read(&cfg, &s1);
    /* Pequeña pausa para que haya delta */
    struct timespec ts = {0, 20000000L}; /* 20 ms */
    nanosleep(&ts, NULL);
    sampler_cpu_read(&cfg, &s2);

    double pct = sampler_cpu_percent(&s1, &s2);
    /* Puede ser -1 si no hubo delta aún, o bien [0, n_cpus*100] */
    TEST_ASSERT_GREATER_OR_EQUAL(-1.1, pct);
    TEST_ASSERT_LESS_OR_EQUAL(100.0 * 256.0, pct); /* máximo razonable */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cpu_read_self);
    RUN_TEST(test_cpu_percent_negative_on_first);
    RUN_TEST(test_cpu_percent_range);
    return UNITY_END();
}
