#define _POSIX_C_SOURCE 200809L
#include "perf_internal.h"

#include <stdio.h>
#include <string.h>

/*
 * Lee /proc/self/io.
 * Campos relevantes: read_bytes, write_bytes, syscr (read ops), syscw (write ops).
 *
 * Nota: este archivo requiere CAP_SYS_PTRACE en algunos kernels o que
 * el proceso sea propietario. Dentro de un contenedor normalmente funciona.
 */
perf_status_t sampler_io_read(const perf_config_t *cfg,
                               io_snapshot_t *snap)
{
    const char *proc_root = cfg->proc_root ? cfg->proc_root : "/proc";
    char path[256];
    if (cfg->pid == 0)
        snprintf(path, sizeof(path), "%s/self/io", proc_root);
    else
        snprintf(path, sizeof(path), "%s/%d/io", proc_root, cfg->pid);

    FILE *f = fopen(path, "r");
    if (!f) return PERF_ERR_IO;

    snap->read_bytes  = 0;
    snap->write_bytes = 0;
    snap->read_ops    = 0;
    snap->write_ops   = 0;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f) && found < 4) {
        unsigned long long val;
        if (sscanf(line, "read_bytes: %llu",  &val) == 1) {
            snap->read_bytes  = val; found++;
        } else if (sscanf(line, "write_bytes: %llu", &val) == 1) {
            snap->write_bytes = val; found++;
        } else if (sscanf(line, "syscr: %llu", &val) == 1) {
            snap->read_ops    = val; found++;
        } else if (sscanf(line, "syscw: %llu", &val) == 1) {
            snap->write_ops   = val; found++;
        }
    }
    fclose(f);

    return (found > 0) ? PERF_OK : PERF_ERR_IO;
}
