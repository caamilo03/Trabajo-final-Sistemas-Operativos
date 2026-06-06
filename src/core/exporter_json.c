#define _POSIX_C_SOURCE 200809L
#include "perf_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

/* Helper: escribe en buf+offset si hay espacio; retorna bytes escritos */
static int jw(char *buf, size_t bufsz, size_t off, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int jw(char *buf, size_t bufsz, size_t off, const char *fmt, ...)
{
    if (off >= bufsz) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, bufsz - off, fmt, ap);
    va_end(ap);
    return (n < 0) ? 0 : n;
}

int perf_sample_to_json(const perf_sample_t *s, char *buf, size_t bufsz)
{
    size_t off = 0;
    off += (size_t)jw(buf, bufsz, off,
        "{"
        "\"ts_sec\":%lld,"
        "\"ts_nsec\":%ld,"
        "\"cpu_pct\":%.2f,"
        "\"cpu_sys_pct\":%.2f,"
        "\"utime_us\":%llu,"
        "\"stime_us\":%llu,"
        "\"mem_rss_kb\":%llu,"
        "\"mem_vms_kb\":%llu,"
        "\"mem_peak_kb\":%llu,"
        "\"io_read_bytes\":%llu,"
        "\"io_write_bytes\":%llu,"
        "\"io_read_ops\":%llu,"
        "\"io_write_ops\":%llu,"
        "\"cg_cpu_usage_us\":%llu,"
        "\"cg_mem_current\":%llu,"
        "\"cg_mem_peak\":%llu,"
        "\"cg_io_rbytes\":%llu,"
        "\"cg_io_wbytes\":%llu"
        "}",
        (long long)s->timestamp.tv_sec,
        s->timestamp.tv_nsec,
        s->cpu_percent,
        s->cpu_system_pct,
        (unsigned long long)s->utime_us,
        (unsigned long long)s->stime_us,
        (unsigned long long)s->mem_rss_kb,
        (unsigned long long)s->mem_vms_kb,
        (unsigned long long)s->mem_peak_kb,
        (unsigned long long)s->io_read_bytes,
        (unsigned long long)s->io_write_bytes,
        (unsigned long long)s->io_read_ops,
        (unsigned long long)s->io_write_ops,
        (unsigned long long)s->cg_cpu_usage_us,
        (unsigned long long)s->cg_mem_current,
        (unsigned long long)s->cg_mem_peak,
        (unsigned long long)s->cg_io_rbytes,
        (unsigned long long)s->cg_io_wbytes
    );

    if (off >= bufsz) return -1;
    return (int)off;
}

int perf_series_to_json(const perf_series_t *s, char *buf, size_t bufsz)
{
    size_t off = 0;
    off += (size_t)jw(buf, bufsz, off, "[");
    for (size_t i = 0; i < s->count; i++) {
        if (i > 0)
            off += (size_t)jw(buf, bufsz, off, ",");
        int n = perf_sample_to_json(&s->samples[i],
                                     buf + off, bufsz - off);
        if (n < 0) return -1;
        off += (size_t)n;
    }
    off += (size_t)jw(buf, bufsz, off, "]");
    if (off >= bufsz) return -1;
    return (int)off;
}

int perf_probes_to_json(perf_ctx_t *ctx, char *buf, size_t bufsz)
{
    size_t n = 0;
    perf_probe_report(ctx, NULL, &n);
    if (n == 0) {
        int r = snprintf(buf, bufsz, "[]");
        return (r < 0 || (size_t)r >= bufsz) ? -1 : r;
    }

    perf_probe_stats_t *stats = malloc(n * sizeof(*stats));
    if (!stats) return -1;

    perf_probe_report(ctx, stats, &n);

    size_t off = 0;
    off += (size_t)jw(buf, bufsz, off, "[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0)
            off += (size_t)jw(buf, bufsz, off, ",");
        perf_probe_stats_t *ps = &stats[i];
        off += (size_t)jw(buf, bufsz, off,
            "{"
            "\"name\":\"%s\","
            "\"count\":%llu,"
            "\"elapsed_us_avg\":%.2f,"
            "\"elapsed_us_min\":%.2f,"
            "\"elapsed_us_max\":%.2f,"
            "\"elapsed_us_p50\":%.2f,"
            "\"elapsed_us_p95\":%.2f,"
            "\"elapsed_us_p99\":%.2f,"
            "\"mem_delta_kb_avg\":%.2f,"
            "\"cpu_us_avg\":%.2f"
            "}",
            ps->name,
            (unsigned long long)ps->count,
            ps->elapsed_us_avg,
            ps->elapsed_us_min,
            ps->elapsed_us_max,
            ps->elapsed_us_p50,
            ps->elapsed_us_p95,
            ps->elapsed_us_p99,
            ps->mem_delta_kb_avg,
            ps->cpu_us_avg
        );
    }
    off += (size_t)jw(buf, bufsz, off, "]");
    free(stats);
    if (off >= bufsz) return -1;
    return (int)off;
}
