#define _POSIX_C_SOURCE 200809L
#include "perf_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/*
 * Detecta el path del cgroup v2 propio del proceso leyendo /proc/self/cgroup.
 * Formato cgroup v2: "0::<path_relativo>\n"
 * El path absoluto = <cgroup_root> + <path_relativo>
 */
perf_status_t sampler_cgroup_detect_path(const perf_config_t *cfg,
                                          char *out, size_t outsz)
{
    const char *proc_root   = cfg->proc_root   ? cfg->proc_root   : "/proc";
    const char *cgroup_root = cfg->cgroup_root ? cfg->cgroup_root : "/sys/fs/cgroup";

    char cgroup_file[256];
    snprintf(cgroup_file, sizeof(cgroup_file), "%s/self/cgroup", proc_root);

    FILE *f = fopen(cgroup_file, "r");
    if (!f) return PERF_ERR_CGROUP;

    char line[512];
    perf_status_t st = PERF_ERR_CGROUP;
    while (fgets(line, sizeof(line), f)) {
        /* Solo la jerarquía 0 corresponde a cgroup v2 */
        char *rel = NULL;
        if (line[0] == '0' && line[1] == ':' && line[2] == ':') {
            rel = line + 3;
            /* Quitar newline */
            size_t len = strlen(rel);
            if (len > 0 && rel[len - 1] == '\n')
                rel[len - 1] = '\0';

            int n = snprintf(out, outsz, "%s%s", cgroup_root, rel);
            if (n > 0 && (size_t)n < outsz)
                st = PERF_OK;
            break;
        }
    }
    fclose(f);
    return st;
}

/* Lee un archivo de cgroup v2 que contiene un solo uint64 */
static perf_status_t read_cg_u64(const char *cg_path,
                                   const char *filename,
                                   uint64_t *out)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", cg_path, filename);
    FILE *f = fopen(path, "r");
    if (!f) { *out = 0; return PERF_ERR_CGROUP; }
    unsigned long long v = 0;
    int r = fscanf(f, "%llu", &v);
    fclose(f);
    if (r != 1) { *out = 0; return PERF_ERR_CGROUP; }
    *out = (uint64_t)v;
    return PERF_OK;
}

/*
 * Lee cpu.stat de cgroup v2.
 * Líneas relevantes: "usage_usec <n>"
 */
static perf_status_t read_cpu_stat(const char *cg_path, uint64_t *usage_us)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/cpu.stat", cg_path);
    FILE *f = fopen(path, "r");
    if (!f) { *usage_us = 0; return PERF_ERR_CGROUP; }

    char line[128];
    perf_status_t st = PERF_ERR_CGROUP;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;
        if (sscanf(line, "usage_usec %llu", &v) == 1) {
            *usage_us = (uint64_t)v;
            st = PERF_OK;
            break;
        }
    }
    fclose(f);
    return st;
}

/*
 * Lee io.stat de cgroup v2.
 * Formato: "<maj:min> rbytes=N wbytes=N ..."
 * Suma rbytes y wbytes de todos los dispositivos.
 */
static perf_status_t read_io_stat(const char *cg_path,
                                   uint64_t *rbytes, uint64_t *wbytes)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/io.stat", cg_path);
    FILE *f = fopen(path, "r");
    if (!f) { *rbytes = 0; *wbytes = 0; return PERF_ERR_CGROUP; }

    char line[512];
    *rbytes = 0; *wbytes = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long rb = 0, wb = 0;
        /* Buscar rbytes= y wbytes= en la línea */
        char *p;
        if ((p = strstr(line, "rbytes=")) != NULL)
            sscanf(p, "rbytes=%llu", &rb);
        if ((p = strstr(line, "wbytes=")) != NULL)
            sscanf(p, "wbytes=%llu", &wb);
        *rbytes += rb;
        *wbytes += wb;
    }
    fclose(f);
    return PERF_OK;
}

perf_status_t sampler_cgroup_read(const perf_config_t *cfg,
                                   const char *cg_path,
                                   cgroup_snapshot_t *snap)
{
    (void)cfg;
    perf_status_t st;

    st = read_cpu_stat(cg_path, &snap->cpu_usage_us);
    if (st != PERF_OK) return st;

    read_cg_u64(cg_path, "memory.current", &snap->mem_current);
    read_cg_u64(cg_path, "memory.peak",    &snap->mem_peak);
    read_io_stat(cg_path, &snap->io_rbytes, &snap->io_wbytes);

    return PERF_OK;
}
