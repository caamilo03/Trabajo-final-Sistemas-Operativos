/**
 * container_monitor.c — Monitor de contenedores Docker desde el host
 *
 * Lee métricas de CPU, memoria y E/S de un contenedor por nombre/ID
 * directamente desde cgroup v2 del host, sin necesidad de instrumentar
 * el contenedor ni instalar nada dentro de él.
 *
 * Uso:
 *   container_monitor --name <container_name> [--interval-ms <n>]
 *                     [--duration <s>] [--output <csv|json|human>]
 *                     [--out-file <path>]
 *
 * Diferencia respecto a "docker stats":
 *   - Acceso directo a cgroup v2 (sin overhead del Docker daemon)
 *   - Exporta métricas de profiling si el contenedor usa libperfanalyzer
 *   - Puede correr como proceso sin privilegios (solo lectura de cgroup)
 *   - Salida CSV lista para analyze.py
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

/* ── Tipos ──────────────────────────────────────────────────────────────── */

typedef struct {
    char     container_name[256];
    char     container_id[128];
    char     cgroup_path[512];
    unsigned interval_ms;
    int      duration_s;         /* -1 = indefinido */
    enum { FMT_HUMAN, FMT_CSV, FMT_JSON } output_fmt;
    char     out_file[512];
} monitor_config_t;

typedef struct {
    struct timespec ts;
    uint64_t cpu_usage_us;       /* cpu.stat usage_usec (acumulado) */
    uint64_t mem_current;        /* memory.current (bytes) */
    uint64_t mem_peak;           /* memory.peak (bytes) */
    uint64_t io_rbytes;          /* io.stat rbytes (sum) */
    uint64_t io_wbytes;          /* io.stat wbytes (sum) */
    double   cpu_pct;            /* calculado entre dos muestras */
} cg_sample_t;

static volatile int g_stop = 0;

/* ── Señales ────────────────────────────────────────────────────────────── */

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ── Lectura de archivos cgroup v2 ──────────────────────────────────────── */

static int read_u64(const char *cg, const char *file, uint64_t *out)
{
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", cg, file);
    FILE *f = fopen(path, "r");
    if (!f) { *out = 0; return -1; }
    unsigned long long v = 0;
    int r = fscanf(f, "%llu", &v);
    fclose(f);
    if (r != 1) { *out = 0; return -1; }
    *out = (uint64_t)v;
    return 0;
}

static int read_cpu_stat(const char *cg, uint64_t *usage_us)
{
    char path[640];
    snprintf(path, sizeof(path), "%s/cpu.stat", cg);
    FILE *f = fopen(path, "r");
    if (!f) { *usage_us = 0; return -1; }
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;
        if (sscanf(line, "usage_usec %llu", &v) == 1) {
            *usage_us = (uint64_t)v;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static int read_io_stat(const char *cg, uint64_t *rbytes, uint64_t *wbytes)
{
    char path[640];
    snprintf(path, sizeof(path), "%s/io.stat", cg);
    FILE *f = fopen(path, "r");
    if (!f) { *rbytes = 0; *wbytes = 0; return -1; }
    char line[512];
    *rbytes = 0; *wbytes = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p;
        unsigned long long rb = 0, wb = 0;
        if ((p = strstr(line, "rbytes=")) != NULL) sscanf(p, "rbytes=%llu", &rb);
        if ((p = strstr(line, "wbytes=")) != NULL) sscanf(p, "wbytes=%llu", &wb);
        *rbytes += rb;
        *wbytes += wb;
    }
    fclose(f);
    return 0;
}

/* ── Búsqueda del cgroup del contenedor ─────────────────────────────────── */

/*
 * Docker coloca los contenedores en paths como:
 *   /sys/fs/cgroup/system.slice/docker-<full_id>.scope      (systemd cgroup driver)
 *   /sys/fs/cgroup/docker/<full_id>                          (cgroupfs driver)
 *
 * Esta función busca recursivamente en /sys/fs/cgroup el directorio que
 * coincide con el nombre o ID del contenedor.
 */
static int find_cgroup_by_id(const char *base, const char *id_prefix,
                               char *out, size_t outsz)
{
    DIR *d = opendir(base);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* ¿Contiene el ID del contenedor en su nombre? */
        if (strstr(ent->d_name, id_prefix) != NULL) {
            snprintf(out, outsz, "%s/%s", base, ent->d_name);
            closedir(d);
            return 0;
        }

        /* Buscar recursivamente en subdirectorios */
        if (ent->d_type == DT_DIR || ent->d_type == DT_UNKNOWN) {
            char subpath[512];
            snprintf(subpath, sizeof(subpath), "%s/%s", base, ent->d_name);
            struct stat st;
            if (stat(subpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (find_cgroup_by_id(subpath, id_prefix, out, outsz) == 0) {
                    closedir(d);
                    return 0;
                }
            }
        }
    }
    closedir(d);
    return -1;
}

/*
 * Obtiene el ID completo del contenedor usando `docker inspect`.
 * Si docker no está disponible, usa name directamente como prefijo de búsqueda.
 */
static int resolve_container_id(const char *name, char *id_out, size_t id_sz)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "docker inspect --format '{{.Id}}' %s 2>/dev/null", name);

    FILE *p = popen(cmd, "r");
    if (!p) {
        strncpy(id_out, name, id_sz - 1);
        id_out[id_sz - 1] = '\0';
        return 0;
    }
    char buf[256] = {0};
    if (fgets(buf, sizeof(buf), p)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' ||
                            buf[len-1] == '\'' || buf[len-1] == ' '))
            buf[--len] = '\0';
        if (len > 0) {
            strncpy(id_out, buf, id_sz - 1);
            id_out[id_sz - 1] = '\0';
            pclose(p);
            return 0;
        }
    }
    pclose(p);
    strncpy(id_out, name, id_sz - 1);
    id_out[id_sz - 1] = '\0';
    return 0;
}

static int find_cgroup_path(monitor_config_t *cfg)
{
    const char *cgroup_root = "/sys/fs/cgroup";

    /* Usar primero los 12 caracteres del ID (prefijo corto) */
    char id_prefix[16];
    strncpy(id_prefix, cfg->container_id, 12);
    id_prefix[12] = '\0';

    if (find_cgroup_by_id(cgroup_root, id_prefix,
                           cfg->cgroup_path, sizeof(cfg->cgroup_path)) == 0)
        return 0;

    /* Fallback: buscar por nombre */
    if (find_cgroup_by_id(cgroup_root, cfg->container_name,
                           cfg->cgroup_path, sizeof(cfg->cgroup_path)) == 0)
        return 0;

    return -1;
}

/* ── Muestreo ────────────────────────────────────────────────────────────── */

static int take_sample(const monitor_config_t *cfg, cg_sample_t *s)
{
    clock_gettime(CLOCK_REALTIME, &s->ts);
    read_cpu_stat(cfg->cgroup_path, &s->cpu_usage_us);
    read_u64(cfg->cgroup_path, "memory.current", &s->mem_current);
    read_u64(cfg->cgroup_path, "memory.peak",    &s->mem_peak);
    read_io_stat(cfg->cgroup_path, &s->io_rbytes, &s->io_wbytes);
    s->cpu_pct = -1.0;
    return 0;
}

static double calc_cpu_pct(const cg_sample_t *prev, const cg_sample_t *curr)
{
    double elapsed_us = (double)(curr->ts.tv_sec  - prev->ts.tv_sec)  * 1e6
                      + (double)(curr->ts.tv_nsec - prev->ts.tv_nsec) * 1e-3;
    if (elapsed_us <= 0.0) return -1.0;
    uint64_t delta_us = curr->cpu_usage_us - prev->cpu_usage_us;
    return (double)delta_us / elapsed_us * 100.0;
}

/* ── Salida ──────────────────────────────────────────────────────────────── */

static void print_csv_header(FILE *out)
{
    fprintf(out,
        "ts_sec,container,cpu_pct,mem_current_kb,mem_peak_kb,"
        "io_rbytes,io_wbytes\n");
}

static void print_sample(FILE *out, const monitor_config_t *cfg,
                          const cg_sample_t *s)
{
    switch (cfg->output_fmt) {

    case FMT_CSV:
        fprintf(out, "%lld,%s,%.2f,%llu,%llu,%llu,%llu\n",
            (long long)s->ts.tv_sec,
            cfg->container_name,
            s->cpu_pct,
            (unsigned long long)(s->mem_current / 1024),
            (unsigned long long)(s->mem_peak    / 1024),
            (unsigned long long)s->io_rbytes,
            (unsigned long long)s->io_wbytes);
        break;

    case FMT_JSON:
        fprintf(out,
            "{\"ts\":%lld,\"container\":\"%s\","
            "\"cpu_pct\":%.2f,"
            "\"mem_current_kb\":%llu,\"mem_peak_kb\":%llu,"
            "\"io_rbytes\":%llu,\"io_wbytes\":%llu}\n",
            (long long)s->ts.tv_sec,
            cfg->container_name,
            s->cpu_pct,
            (unsigned long long)(s->mem_current / 1024),
            (unsigned long long)(s->mem_peak    / 1024),
            (unsigned long long)s->io_rbytes,
            (unsigned long long)s->io_wbytes);
        break;

    case FMT_HUMAN:
    default: {
        char tbuf[32];
        struct tm *tm = localtime(&s->ts.tv_sec);
        strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
        fprintf(out,
            "[%s] %s | CPU: %5.1f%%  "
            "Mem: %6llu MB / peak %6llu MB  "
            "IO R: %7llu MB  W: %7llu MB\n",
            tbuf,
            cfg->container_name,
            s->cpu_pct,
            (unsigned long long)(s->mem_current / (1024*1024)),
            (unsigned long long)(s->mem_peak    / (1024*1024)),
            (unsigned long long)(s->io_rbytes   / (1024*1024)),
            (unsigned long long)(s->io_wbytes   / (1024*1024)));
        break;
    }
    }
    fflush(out);
}

/* ── Uso ─────────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "container_monitor — Monitorea contenedores Docker desde el host vía cgroup v2\n\n"
        "Uso: %s --name <nombre> [opciones]\n\n"
        "Opciones:\n"
        "  --name       <s>  Nombre o ID del contenedor (requerido)\n"
        "  --interval   <n>  Intervalo de muestreo en ms (defecto: 500)\n"
        "  --duration   <n>  Duración en segundos; -1 = indefinido (defecto: -1)\n"
        "  --output     <s>  Formato: human | csv | json (defecto: human)\n"
        "  --out-file   <s>  Archivo de salida (defecto: stdout)\n"
        "  --cgroup-root<s>  Raíz de cgroup (defecto: /sys/fs/cgroup)\n\n"
        "Ejemplo:\n"
        "  %s --name perf_baseline --interval 200 --output csv \\\n"
        "         --out-file results/baseline.csv --duration 30\n\n"
        "Diferencia vs 'docker stats':\n"
        "  - Acceso directo a cgroup v2, sin overhead del Docker daemon\n"
        "  - Salida CSV compatible con experiments/analyze.py\n"
        "  - Funciona sin permisos de root (solo lectura de /sys/fs/cgroup)\n",
        prog, prog);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    monitor_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.interval_ms = 500;
    cfg.duration_s  = -1;
    cfg.output_fmt  = FMT_HUMAN;

    const char *cgroup_root = "/sys/fs/cgroup";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i+1 < argc) {
            strncpy(cfg.container_name, argv[++i], sizeof(cfg.container_name)-1);
        } else if (strcmp(argv[i], "--interval") == 0 && i+1 < argc) {
            cfg.interval_ms = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc) {
            cfg.duration_s = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--output") == 0 && i+1 < argc) {
            i++;
            if      (strcmp(argv[i], "csv")   == 0) cfg.output_fmt = FMT_CSV;
            else if (strcmp(argv[i], "json")  == 0) cfg.output_fmt = FMT_JSON;
            else                                      cfg.output_fmt = FMT_HUMAN;
        } else if (strcmp(argv[i], "--out-file") == 0 && i+1 < argc) {
            strncpy(cfg.out_file, argv[++i], sizeof(cfg.out_file)-1);
        } else if (strcmp(argv[i], "--cgroup-root") == 0 && i+1 < argc) {
            cgroup_root = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Argumento desconocido: %s\n\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (cfg.container_name[0] == '\0') {
        usage(argv[0]);
        return 1;
    }

    /* Resolver ID del contenedor */
    fprintf(stderr, "Resolviendo contenedor '%s'...\n", cfg.container_name);
    resolve_container_id(cfg.container_name,
                          cfg.container_id, sizeof(cfg.container_id));
    fprintf(stderr, "ID: %.16s...\n", cfg.container_id);

    /* Encontrar cgroup path */
    if (find_cgroup_path(&cfg) != 0) {
        /* Intentar con la raíz custom */
        char id_prefix[16];
        strncpy(id_prefix, cfg.container_id, 12);
        id_prefix[12] = '\0';
        if (find_cgroup_by_id(cgroup_root, id_prefix,
                               cfg.cgroup_path, sizeof(cfg.cgroup_path)) != 0) {
            fprintf(stderr,
                "ERROR: No se encontró el cgroup del contenedor '%s'.\n"
                "  Verifica que el contenedor esté corriendo y que cgroup v2 esté activo.\n"
                "  stat -fc %%T /sys/fs/cgroup  →  debe decir cgroup2fs\n",
                cfg.container_name);
            return 1;
        }
    }
    fprintf(stderr, "cgroup: %s\n", cfg.cgroup_path);

    /* Archivo de salida */
    FILE *out = stdout;
    if (cfg.out_file[0] != '\0') {
        out = fopen(cfg.out_file, "w");
        if (!out) {
            perror("fopen");
            return 1;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (cfg.output_fmt == FMT_CSV) print_csv_header(out);

    if (cfg.output_fmt == FMT_HUMAN) {
        fprintf(stderr,
            "Monitoreando '%s' cada %u ms. Ctrl+C para detener.\n"
            "─────────────────────────────────────────────────────\n",
            cfg.container_name, cfg.interval_ms);
    }

    struct timespec interval = {
        .tv_sec  = (time_t)(cfg.interval_ms / 1000),
        .tv_nsec = (long)((cfg.interval_ms % 1000) * 1000000L),
    };

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    if (cfg.duration_s > 0)
        t_end.tv_sec += cfg.duration_s;

    cg_sample_t prev, curr;
    take_sample(&cfg, &prev);
    nanosleep(&interval, NULL);

    uint64_t sample_count = 0;
    double   cpu_sum = 0.0;
    uint64_t mem_peak_global = 0;

    while (!g_stop) {
        if (cfg.duration_s > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec >= t_end.tv_sec) break;
        }

        take_sample(&cfg, &curr);
        curr.cpu_pct = calc_cpu_pct(&prev, &curr);
        print_sample(out, &cfg, &curr);

        cpu_sum += (curr.cpu_pct >= 0.0 ? curr.cpu_pct : 0.0);
        if (curr.mem_peak > mem_peak_global)
            mem_peak_global = curr.mem_peak;
        sample_count++;

        prev = curr;
        nanosleep(&interval, NULL);
    }

    /* Resumen final */
    if (cfg.output_fmt == FMT_HUMAN && sample_count > 0) {
        fprintf(stderr,
            "─────────────────────────────────────────────────────\n"
            "Resumen (%s): %llu muestras\n"
            "  CPU promedio : %.2f%%\n"
            "  Mem peak     : %llu MB\n",
            cfg.container_name,
            (unsigned long long)sample_count,
            cpu_sum / (double)sample_count,
            (unsigned long long)(mem_peak_global / (1024*1024)));
    }

    if (out != stdout) fclose(out);
    return 0;
}
