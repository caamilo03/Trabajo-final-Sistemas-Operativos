#define _POSIX_C_SOURCE 200809L
#include "perf_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

/* ── Mensajes de error ─────────────────────────────────────────────────── */

const char *perf_strerror(perf_status_t s)
{
    switch (s) {
    case PERF_OK:              return "OK";
    case PERF_ERR_NOMEM:       return "out of memory";
    case PERF_ERR_IO:          return "I/O error reading /proc";
    case PERF_ERR_INVAL:       return "invalid argument";
    case PERF_ERR_BUSY:        return "already recording";
    case PERF_ERR_NOT_RUNNING: return "not recording";
    case PERF_ERR_CGROUP:      return "cgroup v2 not available";
    default:                   return "unknown error";
    }
}

/* ── Ciclo de vida ─────────────────────────────────────────────────────── */

perf_ctx_t *perf_init(const perf_config_t *cfg)
{
    perf_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    if (cfg)
        ctx->cfg = *cfg;
    else {
        perf_config_t def = PERF_CONFIG_DEFAULT;
        ctx->cfg = def;
    }

    if (ctx->cfg.sample_period_ms < 10)
        ctx->cfg.sample_period_ms = 10;

    ctx->sampling_enabled = ctx->cfg.disable_sampling ? 0 : 1;

    ring_init(&ctx->ring);
    probe_table_init(&ctx->probes);
    pthread_mutex_init(&ctx->sample_mu, NULL);

    /* Intentar detectar cgroup v2 */
    perf_source_t src = ctx->cfg.source;
    if (src == PERF_SRC_CGROUP || src == PERF_SRC_AUTO) {
        if (sampler_cgroup_detect_path(&ctx->cfg,
                                        ctx->cg_path,
                                        sizeof(ctx->cg_path)) == PERF_OK) {
            ctx->has_cgroup = 1;
        } else if (src == PERF_SRC_CGROUP) {
            /* Requerido y no disponible */
            free(ctx);
            return NULL;
        }
    }

    return ctx;
}

void perf_shutdown(perf_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->sampler_running) {
        ctx->sampler_running = 0;
        pthread_join(ctx->sampler_thread, NULL);
    }
    pthread_mutex_destroy(&ctx->sample_mu);
    free(ctx);
}

/* ── Muestra puntual ───────────────────────────────────────────────────── */

perf_status_t perf_sample(perf_ctx_t *ctx, perf_sample_t *out)
{
    if (!ctx || !out) return PERF_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    clock_gettime(CLOCK_REALTIME, &out->timestamp);

    cpu_snapshot_t cpu_snap;
    perf_status_t st = sampler_cpu_read(&ctx->cfg, &cpu_snap);
    if (st == PERF_OK) {
        out->utime_us = cpu_snap.utime_ticks * 1000000ULL / cpu_snap.ticks_per_sec;
        out->stime_us = cpu_snap.stime_ticks * 1000000ULL / cpu_snap.ticks_per_sec;

        pthread_mutex_lock(&ctx->sample_mu);
        if (ctx->prev_cpu_valid) {
            out->cpu_percent    = sampler_cpu_percent(&ctx->prev_cpu, &cpu_snap);
            out->cpu_system_pct = sampler_cpu_system_percent(&ctx->prev_cpu, &cpu_snap);
        }
        ctx->prev_cpu       = cpu_snap;
        ctx->prev_cpu_valid = 1;
        pthread_mutex_unlock(&ctx->sample_mu);
    }

    mem_snapshot_t mem_snap;
    if (sampler_mem_read(&ctx->cfg, &mem_snap) == PERF_OK) {
        out->mem_rss_kb  = mem_snap.rss_kb;
        out->mem_vms_kb  = mem_snap.vms_kb;
        out->mem_peak_kb = mem_snap.peak_kb;
    }

    io_snapshot_t io_snap;
    if (sampler_io_read(&ctx->cfg, &io_snap) == PERF_OK) {
        out->io_read_bytes  = io_snap.read_bytes;
        out->io_write_bytes = io_snap.write_bytes;
        out->io_read_ops    = io_snap.read_ops;
        out->io_write_ops   = io_snap.write_ops;
    }

    if (ctx->has_cgroup) {
        cgroup_snapshot_t cg;
        if (sampler_cgroup_read(&ctx->cfg, ctx->cg_path, &cg) == PERF_OK) {
            out->cg_cpu_usage_us = cg.cpu_usage_us;
            out->cg_mem_current  = cg.mem_current;
            out->cg_mem_peak     = cg.mem_peak;
            out->cg_io_rbytes    = cg.io_rbytes;
            out->cg_io_wbytes    = cg.io_wbytes;
        }
    }

    return PERF_OK;
}

/* ── Sampler de fondo ──────────────────────────────────────────────────── */

static void *sampler_thread_fn(void *arg)
{
    perf_ctx_t *ctx = (perf_ctx_t *)arg;
    struct timespec sleep_ts = {
        .tv_sec  = (time_t)(ctx->cfg.sample_period_ms / 1000),
        .tv_nsec = (long)((ctx->cfg.sample_period_ms % 1000) * 1000000L),
    };

    while (ctx->sampler_running) {
        perf_sample_t s;
        if (perf_sample(ctx, &s) == PERF_OK)
            ring_push(&ctx->ring, &s);
        nanosleep(&sleep_ts, NULL);
    }
    return NULL;
}

perf_status_t perf_start_recording(perf_ctx_t *ctx)
{
    if (!ctx) return PERF_ERR_INVAL;
    /* Si el sampling está desactivado en runtime, no arrancamos el hilo.
     * Se devuelve OK para que el código del workload no necesite ramas. */
    if (!ctx->sampling_enabled) return PERF_OK;
    if (ctx->sampler_running) return PERF_ERR_BUSY;

    ctx->sampler_running = 1;
    if (pthread_create(&ctx->sampler_thread, NULL, sampler_thread_fn, ctx) != 0) {
        ctx->sampler_running = 0;
        return PERF_ERR_IO;
    }
    return PERF_OK;
}

perf_status_t perf_stop_recording(perf_ctx_t *ctx, perf_series_t *out)
{
    if (!ctx) return PERF_ERR_INVAL;
    /* Sampling desactivado: serie vacía, sin error (simetría con start). */
    if (!ctx->sampling_enabled) {
        if (out) { out->samples = NULL; out->count = 0; out->capacity = 0; }
        return PERF_OK;
    }
    if (!ctx->sampler_running) return PERF_ERR_NOT_RUNNING;

    ctx->sampler_running = 0;
    pthread_join(ctx->sampler_thread, NULL);

    if (!out) return PERF_OK;

    /* Drena el ring buffer hacia una serie dinámica */
    out->capacity = RING_CAPACITY;
    out->samples  = malloc(out->capacity * sizeof(*out->samples));
    if (!out->samples) return PERF_ERR_NOMEM;

    out->count = ring_drain(&ctx->ring, out->samples, out->capacity);
    return PERF_OK;
}

void perf_series_free(perf_series_t *s)
{
    if (s) {
        free(s->samples);
        s->samples  = NULL;
        s->count    = 0;
        s->capacity = 0;
    }
}
