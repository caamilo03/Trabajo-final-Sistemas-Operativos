/*
 * perf_internal.h — tipos y funciones internas de libperfanalyzer.
 * No forma parte de la API pública.
 */
#ifndef PERF_INTERNAL_H
#define PERF_INTERNAL_H

#include "perf.h"

#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* ── Snapshots de bajo nivel ──────────────────────────────────────────── */

typedef struct {
    struct timespec ts;
    uint64_t utime_ticks;
    uint64_t stime_ticks;
    uint64_t sys_total_ticks;
    uint64_t sys_idle_ticks;
    uint64_t ticks_per_sec;
} cpu_snapshot_t;

typedef struct {
    uint64_t rss_kb;
    uint64_t vms_kb;
    uint64_t peak_kb;
} mem_snapshot_t;

typedef struct {
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t read_ops;
    uint64_t write_ops;
} io_snapshot_t;

typedef struct {
    uint64_t cpu_usage_us;
    uint64_t mem_current;
    uint64_t mem_peak;
    uint64_t io_rbytes;
    uint64_t io_wbytes;
} cgroup_snapshot_t;

/* ── Samplers ─────────────────────────────────────────────────────────── */

perf_status_t sampler_cpu_read(const perf_config_t *cfg, cpu_snapshot_t *snap);
double        sampler_cpu_percent(const cpu_snapshot_t *prev,
                                  const cpu_snapshot_t *curr);
double        sampler_cpu_system_percent(const cpu_snapshot_t *prev,
                                         const cpu_snapshot_t *curr);

perf_status_t sampler_mem_read(const perf_config_t *cfg, mem_snapshot_t *snap);
perf_status_t sampler_io_read(const perf_config_t *cfg, io_snapshot_t *snap);

perf_status_t sampler_cgroup_detect_path(const perf_config_t *cfg,
                                          char *out, size_t outsz);
perf_status_t sampler_cgroup_read(const perf_config_t *cfg,
                                   const char *cg_path,
                                   cgroup_snapshot_t *snap);

/* ── Ring buffer ──────────────────────────────────────────────────────── */

#define RING_CAPACITY 4096

typedef struct {
    perf_sample_t  slots[RING_CAPACITY];
    volatile size_t head;   /* productor */
    volatile size_t tail;   /* consumidor */
    pthread_mutex_t mu;
} ring_buffer_t;

void          ring_init(ring_buffer_t *rb);
perf_status_t ring_push(ring_buffer_t *rb, const perf_sample_t *s);
size_t        ring_drain(ring_buffer_t *rb,
                          perf_sample_t *dst, size_t max);

/* ── Profiler ─────────────────────────────────────────────────────────── */

#define PROBE_MAX_ENTRIES  256
#define RESERVOIR_SIZE     1024

typedef struct {
    char     name[128];
    uint64_t count;
    double   sum_us;
    double   sum2_us;         /* para varianza online */
    double   min_us;
    double   max_us;
    double   cpu_sum_us;
    double   mem_sum_kb;
    /* reservoir sampling para percentiles */
    double   reservoir[RESERVOIR_SIZE];
    size_t   res_count;
    pthread_mutex_t mu;
} probe_entry_t;

typedef struct {
    probe_entry_t entries[PROBE_MAX_ENTRIES];
    size_t        count;
    pthread_mutex_t global_mu;
} probe_table_t;

void          probe_table_init(probe_table_t *t);
probe_entry_t *probe_table_get_or_create(probe_table_t *t, const char *name);
void          probe_entry_record(probe_entry_t *e,
                                  double elapsed_us,
                                  double cpu_us,
                                  double mem_delta_kb);
void          probe_entry_percentile(probe_entry_t *e,
                                      double *p50, double *p95, double *p99);

/* ── Contexto principal ───────────────────────────────────────────────── */

struct perf_ctx {
    perf_config_t   cfg;
    char            cg_path[512];
    int             has_cgroup;

    /* Sampler de fondo */
    pthread_t       sampler_thread;
    volatile int    sampler_running;
    ring_buffer_t   ring;

    cpu_snapshot_t  prev_cpu;
    int             prev_cpu_valid;

    /* Profiler */
    probe_table_t   probes;
};

#endif /* PERF_INTERNAL_H */
