#define _POSIX_C_SOURCE 200809L
#include "perf_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * Lee /proc/self/status buscando VmRSS, VmSize y VmPeak.
 * Formato de línea:  "VmRSS:    1234 kB\n"
 */
perf_status_t sampler_mem_read(const perf_config_t *cfg,
                                mem_snapshot_t *snap)
{
    const char *proc_root = cfg->proc_root ? cfg->proc_root : "/proc";
    char path[256];
    if (cfg->pid == 0)
        snprintf(path, sizeof(path), "%s/self/status", proc_root);
    else
        snprintf(path, sizeof(path), "%s/%d/status", proc_root, cfg->pid);

    FILE *f = fopen(path, "r");
    if (!f) return PERF_ERR_IO;

    snap->rss_kb  = 0;
    snap->vms_kb  = 0;
    snap->peak_kb = 0;

    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f) && found < 3) {
        unsigned long long val;
        if (sscanf(line, "VmRSS: %llu", &val) == 1) {
            snap->rss_kb = val; found++;
        } else if (sscanf(line, "VmSize: %llu", &val) == 1) {
            snap->vms_kb = val; found++;
        } else if (sscanf(line, "VmPeak: %llu", &val) == 1) {
            snap->peak_kb = val; found++;
        }
    }
    fclose(f);

    return (found > 0) ? PERF_OK : PERF_ERR_IO;
}
