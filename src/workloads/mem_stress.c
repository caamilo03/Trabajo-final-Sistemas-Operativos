#define _POSIX_C_SOURCE 200809L
#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE_SIZE     4096
#define MB            (1024UL * 1024UL)

/* Fuerza la lectura de todas las páginas para evitar lazy allocation */
static void touch_pages(volatile char *p, size_t sz)
{
    for (size_t i = 0; i < sz; i += PAGE_SIZE)
        p[i] = (char)(i & 0xFF);
}

/* Stride access para estresar la caché */
static uint64_t stride_walk(const char *buf, size_t sz, size_t stride)
{
    uint64_t sum = 0;
    for (size_t i = 0; i < sz; i += stride)
        sum += (uint8_t)buf[i];
    return sum;
}

int main(int argc, char *argv[])
{
    int    duration_s = 10;
    size_t alloc_mb   = 64;   /* low intensity */
    int    intensity  = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i+1 < argc)
            duration_s = atoi(argv[++i]);
        else if (strcmp(argv[i], "--intensity") == 0 && i+1 < argc)
            intensity = atoi(argv[++i]);
    }

    if (intensity >= 2) alloc_mb = 256;

    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    perf_ctx_t *ctx = perf_init(&cfg);
    if (!ctx) { fprintf(stderr, "perf_init falló\n"); return 1; }

    perf_start_recording(ctx);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    t_end.tv_sec += duration_s;

    uint64_t alloc_iters = 0, walk_iters = 0;
    uint64_t checksum = 0;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > t_end.tv_sec ||
            (now.tv_sec == t_end.tv_sec && now.tv_nsec >= t_end.tv_nsec))
            break;

        /* Alloc + touch + free */
        PERF_PROBE(ctx, "alloc_touch_free") {
            char *buf = malloc(alloc_mb * MB);
            if (buf) {
                touch_pages((volatile char *)buf, alloc_mb * MB);
                alloc_iters++;
                free(buf);
            }
        }

        /* Stride walk sobre buffer estático */
        PERF_PROBE(ctx, "stride_walk") {
            char *sbuf = malloc(alloc_mb * MB);
            if (sbuf) {
                touch_pages((volatile char *)sbuf, alloc_mb * MB);
                checksum += stride_walk(sbuf, alloc_mb * MB, 64);
                checksum += stride_walk(sbuf, alloc_mb * MB, PAGE_SIZE);
                walk_iters++;
                free(sbuf);
            }
        }
    }

    perf_series_t series;
    perf_stop_recording(ctx, &series);

    printf("workload,mem_stress\n");
    printf("alloc_iters,%llu\n", (unsigned long long)alloc_iters);
    printf("walk_iters,%llu\n",  (unsigned long long)walk_iters);
    printf("checksum,%llu\n",    (unsigned long long)checksum);
    printf("samples,%zu\n", series.count);

    if (series.count > 0) {
        uint64_t rss_sum = 0;
        for (size_t i = 0; i < series.count; i++)
            rss_sum += series.samples[i].mem_rss_kb;
        printf("mem_rss_avg_kb,%.0f\n",
               (double)rss_sum / (double)series.count);
    }

    size_t np = 0;
    perf_probe_report(ctx, NULL, &np);
    perf_probe_stats_t *ps = malloc(np * sizeof(*ps));
    perf_probe_report(ctx, ps, &np);
    for (size_t i = 0; i < np; i++) {
        printf("probe_%s_avg_us,%.2f\n", ps[i].name, ps[i].elapsed_us_avg);
        printf("probe_%s_p99_us,%.2f\n", ps[i].name, ps[i].elapsed_us_p99);
    }
    free(ps);
    perf_series_free(&series);
    perf_shutdown(ctx);
    return 0;
}
