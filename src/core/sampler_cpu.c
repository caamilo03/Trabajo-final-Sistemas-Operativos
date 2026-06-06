#define _POSIX_C_SOURCE 200809L
#include "perf_internal.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>

/*
 * Lee /proc/self/stat para obtener utime y stime del proceso (en ticks).
 * Devuelve PERF_OK o PERF_ERR_IO.
 */
static perf_status_t read_proc_self_stat(const char *proc_root,
                                          int pid,
                                          uint64_t *utime_ticks,
                                          uint64_t *stime_ticks)
{
    char path[256];
    if (pid == 0)
        snprintf(path, sizeof(path), "%s/self/stat", proc_root);
    else
        snprintf(path, sizeof(path), "%s/%d/stat", proc_root, pid);

    FILE *f = fopen(path, "r");
    if (!f) return PERF_ERR_IO;

    /* El campo comm (field 2) puede contener espacios y paréntesis.
     * Saltamos hasta el último ')' y luego leemos desde el field 3. */
    char buf[1024];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return PERF_ERR_IO; }
    fclose(f);

    char *p = strrchr(buf, ')');
    if (!p) return PERF_ERR_IO;
    p += 2; /* saltar ') ' */

    /* fields 3..13 que no nos interesan (estado, ppid, pgrp, session,
     * tty_nr, tpgid, flags, minflt, cminflt, majflt, cmajflt).
     * Usamos variables locales unsigned long long para evitar warnings
     * de %llu cuando uint64_t es unsigned long en glibc/x86_64. */
    unsigned long long dummy;
    unsigned long long utime_ull, stime_ull;
    char state;
    int scanned = sscanf(p,
        "%c "                          /* 3: state             */
        "%llu %llu %llu %llu %llu "    /* 4-8                  */
        "%llu %llu %llu %llu %llu "    /* 9-13                 */
        "%llu %llu",                   /* 14: utime, 15: stime */
        &state,
        &dummy, &dummy, &dummy, &dummy, &dummy,
        &dummy, &dummy, &dummy, &dummy, &dummy,
        &utime_ull, &stime_ull);

    if (scanned != 13) return PERF_ERR_IO;
    *utime_ticks = (uint64_t)utime_ull;
    *stime_ticks = (uint64_t)stime_ull;
    return PERF_OK;
}

/*
 * Lee /proc/stat (primera línea: cpu global) para calcular porcentaje
 * de CPU del sistema. Devuelve total de ticks y ticks idle.
 */
static perf_status_t read_proc_stat_global(const char *proc_root,
                                            uint64_t *total,
                                            uint64_t *idle)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/stat", proc_root);
    FILE *f = fopen(path, "r");
    if (!f) return PERF_ERR_IO;

    unsigned long long u, n, s, id, iow, irq, sirq, steal;
    int r = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &n, &s, &id, &iow, &irq, &sirq, &steal);
    fclose(f);
    if (r < 4) return PERF_ERR_IO;

    *idle  = id + iow;
    *total = u + n + s + id + iow + irq + sirq + steal;
    return PERF_OK;
}

perf_status_t sampler_cpu_read(const perf_config_t *cfg,
                                cpu_snapshot_t *snap)
{
    const char *proc_root = cfg->proc_root ? cfg->proc_root : "/proc";

    perf_status_t st = read_proc_self_stat(proc_root, cfg->pid,
                                            &snap->utime_ticks,
                                            &snap->stime_ticks);
    if (st != PERF_OK) return st;

    st = read_proc_stat_global(proc_root,
                                &snap->sys_total_ticks,
                                &snap->sys_idle_ticks);
    if (st != PERF_OK) return st;

    /* Tiempo de reloj monotónico para calcular delta entre muestras */
    clock_gettime(CLOCK_MONOTONIC, &snap->ts);
    snap->ticks_per_sec = (uint64_t)sysconf(_SC_CLK_TCK);
    return PERF_OK;
}

/*
 * Calcula %CPU del proceso entre dos snapshots consecutivos.
 * Retorna -1.0 si el delta es 0 (primera muestra).
 */
double sampler_cpu_percent(const cpu_snapshot_t *prev,
                            const cpu_snapshot_t *curr)
{
    if (!prev || !curr) return -1.0;

    uint64_t proc_delta = (curr->utime_ticks + curr->stime_ticks)
                        - (prev->utime_ticks  + prev->stime_ticks);

    double elapsed_s = (double)(curr->ts.tv_sec  - prev->ts.tv_sec)
                     + (double)(curr->ts.tv_nsec - prev->ts.tv_nsec) * 1e-9;

    if (elapsed_s <= 0.0) return -1.0;

    double tps = (double)curr->ticks_per_sec;
    return (double)proc_delta / tps / elapsed_s * 100.0;
}

double sampler_cpu_system_percent(const cpu_snapshot_t *prev,
                                   const cpu_snapshot_t *curr)
{
    if (!prev || !curr) return -1.0;

    uint64_t total_delta = curr->sys_total_ticks - prev->sys_total_ticks;
    uint64_t idle_delta  = curr->sys_idle_ticks  - prev->sys_idle_ticks;

    if (total_delta == 0) return -1.0;
    return (1.0 - (double)idle_delta / (double)total_delta) * 100.0;
}
