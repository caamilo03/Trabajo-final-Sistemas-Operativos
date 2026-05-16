#include "unity.h"
#include "perf.h"
#include "perf_internal.h"

#include <time.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static perf_ctx_t *ctx = NULL;

void test_probe_begin_end(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    ctx = perf_init(&cfg);
    TEST_ASSERT_NOT_NULL(ctx);

    {
        PERF_PROBE(ctx, "dummy_work") {
            struct timespec ts = {0, 1000000L}; /* 1 ms */
            nanosleep(&ts, NULL);
        }
    }

    size_t n = 0;
    perf_probe_report(ctx, NULL, &n);
    TEST_ASSERT_EQUAL_UINT(1, n);

    perf_probe_stats_t stats[1];
    n = 1;
    perf_status_t st = perf_probe_report(ctx, stats, &n);
    TEST_ASSERT_EQUAL_INT(PERF_OK, st);
    TEST_ASSERT_EQUAL_UINT(1, stats[0].count);
    TEST_ASSERT_GREATER_THAN(0.0, stats[0].elapsed_us_avg);
    TEST_ASSERT_EQUAL_STRING("dummy_work", stats[0].name);

    perf_shutdown(ctx);
}

void test_probe_reset(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    ctx = perf_init(&cfg);
    TEST_ASSERT_NOT_NULL(ctx);

    PERF_PROBE(ctx, "fn_a") { (void)0; }
    perf_probe_reset(ctx);

    size_t n = 0;
    perf_probe_report(ctx, NULL, &n);
    /* Las entradas siguen existiendo pero con count=0 */
    perf_probe_stats_t stats[1];
    n = 1;
    perf_probe_report(ctx, stats, &n);
    TEST_ASSERT_EQUAL_UINT(0, stats[0].count);

    perf_shutdown(ctx);
}

void test_probe_percentiles(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    ctx = perf_init(&cfg);
    TEST_ASSERT_NOT_NULL(ctx);

    for (int i = 0; i < 100; i++) {
        PERF_PROBE(ctx, "repeated") {
            struct timespec ts = {0, 500000L}; /* 0.5 ms */
            nanosleep(&ts, NULL);
        }
    }

    size_t n = 0;
    perf_probe_report(ctx, NULL, &n);
    perf_probe_stats_t *stats = malloc(n * sizeof(*stats));
    perf_probe_report(ctx, stats, &n);

    TEST_ASSERT_GREATER_THAN(0.0, stats[0].elapsed_us_p50);
    TEST_ASSERT_GREATER_OR_EQUAL(stats[0].elapsed_us_p50, stats[0].elapsed_us_p50);
    TEST_ASSERT_GREATER_OR_EQUAL(stats[0].elapsed_us_p99, stats[0].elapsed_us_p50);

    free(stats);
    perf_shutdown(ctx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_probe_begin_end);
    RUN_TEST(test_probe_reset);
    RUN_TEST(test_probe_percentiles);
    return UNITY_END();
}
