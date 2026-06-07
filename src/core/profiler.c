#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE              /* necesario para RUSAGE_THREAD */
#include "perf_internal.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/resource.h>

/* ── Tabla de sondas ───────────────────────────────────────────────────── */

void probe_table_init(probe_table_t *t)
{
    memset(t, 0, sizeof(*t));
    pthread_mutex_init(&t->global_mu, NULL);
    for (size_t i = 0; i < PROBE_MAX_ENTRIES; i++)
        pthread_mutex_init(&t->entries[i].mu, NULL);
}

probe_entry_t *probe_table_get_or_create(probe_table_t *t, const char *name)
{
    pthread_mutex_lock(&t->global_mu);

    /* Busca entrada existente */
    for (size_t i = 0; i < t->count; i++) {
        if (strncmp(t->entries[i].name, name, sizeof(t->entries[i].name) - 1) == 0) {
            pthread_mutex_unlock(&t->global_mu);
            return &t->entries[i];
        }
    }

    /* Crea nueva */
    if (t->count >= PROBE_MAX_ENTRIES) {
        pthread_mutex_unlock(&t->global_mu);
        return NULL;
    }
    probe_entry_t *e = &t->entries[t->count++];
    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->min_us    = 1e18;
    e->max_us    = 0.0;
    e->res_count = 0;

    /* Semilla derivada del nombre + dirección para que cada entrada tenga
     * un RNG independiente (rand_r es thread-safe; rand() no lo es). */
    unsigned seed = (unsigned)(uintptr_t)e;
    for (const char *p = name; *p; p++) seed = seed * 31u + (unsigned)*p;
    e->rng_state = seed ? seed : 1;

    pthread_mutex_unlock(&t->global_mu);
    return e;
}

/* Reservoir sampling de Vitter (Algorithm R) para percentiles online.
 * Llamado bajo e->mu, por lo que rng_state está protegido por el mutex. */
static void reservoir_add(probe_entry_t *e, double val)
{
    if (e->res_count < RESERVOIR_SIZE) {
        e->reservoir[e->res_count++] = val;
    } else {
        /* Reemplaza aleatoriamente con probabilidad RESERVOIR_SIZE / count.
         * count ya está incrementado, así que count >= RESERVOIR_SIZE+1. */
        unsigned r = (unsigned)rand_r(&e->rng_state);
        size_t j = (size_t)r % (size_t)e->count;
        if (j < RESERVOIR_SIZE)
            e->reservoir[j] = val;
    }
}

void probe_entry_record(probe_entry_t *e,
                         double elapsed_us,
                         double cpu_us,
                         double mem_delta_kb)
{
    pthread_mutex_lock(&e->mu);

    e->count++;
    e->sum_us    += elapsed_us;
    e->sum2_us   += elapsed_us * elapsed_us;
    e->cpu_sum_us += cpu_us;
    e->mem_sum_kb += mem_delta_kb;
    if (elapsed_us < e->min_us) e->min_us = elapsed_us;
    if (elapsed_us > e->max_us) e->max_us = elapsed_us;

    reservoir_add(e, elapsed_us);

    pthread_mutex_unlock(&e->mu);
}

/* Comparador para qsort */
static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

void probe_entry_percentile(probe_entry_t *e,
                              double *p50, double *p95, double *p99)
{
    pthread_mutex_lock(&e->mu);

    if (e->res_count == 0) {
        *p50 = *p95 = *p99 = 0.0;
        pthread_mutex_unlock(&e->mu);
        return;
    }

    /* Copia temporal para ordenar sin modificar el reservoir */
    double *tmp = malloc(e->res_count * sizeof(double));
    if (!tmp) {
        *p50 = *p95 = *p99 = 0.0;
        pthread_mutex_unlock(&e->mu);
        return;
    }
    memcpy(tmp, e->reservoir, e->res_count * sizeof(double));
    pthread_mutex_unlock(&e->mu);

    qsort(tmp, e->res_count, sizeof(double), cmp_double);

    size_t n = e->res_count;
    *p50 = tmp[(size_t)(0.50 * (double)(n - 1))];
    *p95 = tmp[(size_t)(0.95 * (double)(n - 1))];
    *p99 = tmp[(size_t)(0.99 * (double)(n - 1))];
    free(tmp);
}

/* ── API pública de profiling ──────────────────────────────────────────── */

perf_probe_t perf_probe_begin(perf_ctx_t *ctx, const char *name)
{
    perf_probe_t p;
    memset(&p, 0, sizeof(p));
    p.ctx  = ctx;
    p.name = name;

    /* Sampling desactivado en runtime: marcamos la sonda como deshabilitada
     * y retornamos de inmediato. El cuerpo del bloque PERF_PROBE igual se
     * ejecuta una vez; perf_probe_end no registrará nada. Esto da una línea
     * base "instrumentada pero inerte" con el MISMO binario. */
    if (!ctx || !ctx->sampling_enabled) {
        p.disabled = 1;
        return p;
    }

    clock_gettime(CLOCK_MONOTONIC_RAW, &p.t0);

    /* Solo getrusage para tiempo CPU del hilo. Evitamos perf_sample()
     * (que abre 4-5 archivos /proc) para que el costo por PROBE sea
     * del orden de µs y no de decenas de µs. La memoria delta se lee
     * con un sampler_mem_read ligero (un solo archivo). */
    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    p.utime0_us = (uint64_t)(ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec);
    p.stime0_us = (uint64_t)(ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec);

    mem_snapshot_t m;
    if (sampler_mem_read(&ctx->cfg, &m) == PERF_OK)
        p.mem0_kb = m.rss_kb;

    return p;
}

void perf_probe_end(perf_probe_t *p)
{
    if (!p || !p->ctx || !p->name || p->disabled) return;

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);

    double elapsed_us = (double)(t1.tv_sec  - p->t0.tv_sec)  * 1e6
                      + (double)(t1.tv_nsec - p->t0.tv_nsec) * 1e-3;

    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    uint64_t utime1 = (uint64_t)(ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec);
    uint64_t stime1 = (uint64_t)(ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec);
    double cpu_us = (double)((utime1 + stime1) - (p->utime0_us + p->stime0_us));

    double mem_delta_kb = 0.0;
    mem_snapshot_t m;
    if (sampler_mem_read(&p->ctx->cfg, &m) == PERF_OK)
        mem_delta_kb = (double)m.rss_kb - (double)p->mem0_kb;

    probe_entry_t *entry = probe_table_get_or_create(&p->ctx->probes, p->name);
    if (entry)
        probe_entry_record(entry, elapsed_us, cpu_us, mem_delta_kb);

    /* Limpiar para que el cleanup de __attribute__((cleanup)) sea idempotente */
    p->ctx  = NULL;
    p->name = NULL;
}

perf_status_t perf_probe_report(perf_ctx_t *ctx,
                                 perf_probe_stats_t *buf,
                                 size_t *n)
{
    if (!n) return PERF_ERR_INVAL;

    probe_table_t *t = &ctx->probes;
    pthread_mutex_lock(&t->global_mu);
    size_t total = t->count;
    pthread_mutex_unlock(&t->global_mu);

    if (!buf) { *n = total; return PERF_OK; }
    if (*n < total) { *n = total; return PERF_ERR_INVAL; }

    for (size_t i = 0; i < total; i++) {
        probe_entry_t *e  = &t->entries[i];
        perf_probe_stats_t *s = &buf[i];

        pthread_mutex_lock(&e->mu);
        strncpy(s->name, e->name, sizeof(s->name) - 1);
        s->count           = e->count;
        s->elapsed_us_avg  = e->count ? e->sum_us / (double)e->count : 0.0;
        s->elapsed_us_min  = e->min_us < 1e17 ? e->min_us : 0.0;
        s->elapsed_us_max  = e->max_us;
        s->cpu_us_avg      = e->count ? e->cpu_sum_us / (double)e->count : 0.0;
        s->mem_delta_kb_avg = e->count ? e->mem_sum_kb / (double)e->count : 0.0;
        pthread_mutex_unlock(&e->mu);

        probe_entry_percentile(e,
                                &s->elapsed_us_p50,
                                &s->elapsed_us_p95,
                                &s->elapsed_us_p99);
    }
    *n = total;
    return PERF_OK;
}

void perf_probe_reset(perf_ctx_t *ctx)
{
    probe_table_t *t = &ctx->probes;
    pthread_mutex_lock(&t->global_mu);
    for (size_t i = 0; i < t->count; i++) {
        probe_entry_t *e = &t->entries[i];
        pthread_mutex_lock(&e->mu);
        e->count      = 0;
        e->sum_us     = 0;
        e->sum2_us    = 0;
        e->min_us     = 1e18;
        e->max_us     = 0;
        e->cpu_sum_us = 0;
        e->mem_sum_kb = 0;
        e->res_count  = 0;
        pthread_mutex_unlock(&e->mu);
    }
    pthread_mutex_unlock(&t->global_mu);
}
