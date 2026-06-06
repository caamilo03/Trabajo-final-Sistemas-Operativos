/**
 * perf.h — API pública de libperfanalyzer
 *
 * Uso típico:
 *   perf_ctx_t *ctx = perf_init(&cfg);
 *   perf_start_recording(ctx);
 *   // ... workload ...
 *   perf_series_t series;
 *   perf_stop_recording(ctx, &series);
 *   perf_series_free(&series);
 *   perf_shutdown(ctx);
 *
 * Profiling por función:
 *   PERF_PROBE(ctx, "mi_funcion") {
 *       mi_funcion();
 *   }
 */

#ifndef PERF_H
#define PERF_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Códigos de estado ─────────────────────────────────────────────────── */

typedef enum {
    PERF_OK              =  0,
    PERF_ERR_NOMEM       = -1,
    PERF_ERR_IO          = -2,
    PERF_ERR_INVAL       = -3,
    PERF_ERR_BUSY        = -4,
    PERF_ERR_NOT_RUNNING = -5,
    PERF_ERR_CGROUP      = -6,
} perf_status_t;

const char *perf_strerror(perf_status_t s);

/* ── Fuente de datos ───────────────────────────────────────────────────── */

typedef enum {
    PERF_SRC_PROC   = 0x01,  /* /proc/self/...  lectura desde el proceso */
    PERF_SRC_CGROUP = 0x02,  /* /sys/fs/cgroup  lectura cgroup v2        */
    PERF_SRC_AUTO   = 0x03,  /* PROC si no hay cgroup disponible         */
} perf_source_t;

/* ── Configuración ─────────────────────────────────────────────────────── */

typedef struct {
    perf_source_t source;
    unsigned int  sample_period_ms;   /* intervalo entre muestras (≥10)  */
    const char   *proc_root;          /* override de /proc (tests)       */
    const char   *cgroup_root;        /* override de /sys/fs/cgroup      */
    int           pid;                /* 0 = proceso propio              */
} perf_config_t;

#define PERF_CONFIG_DEFAULT { \
    .source           = PERF_SRC_AUTO, \
    .sample_period_ms = 100,           \
    .proc_root        = NULL,          \
    .cgroup_root      = NULL,          \
    .pid              = 0,             \
}

/* ── Tipos de muestra ──────────────────────────────────────────────────── */

typedef struct {
    struct timespec timestamp;

    /* CPU */
    double cpu_percent;       /* % de CPU del proceso en el intervalo   */
    double cpu_system_pct;    /* % de CPU del sistema (global)          */
    uint64_t utime_us;        /* tiempo usuario acumulado (µs)          */
    uint64_t stime_us;        /* tiempo kernel acumulado (µs)           */

    /* Memoria */
    uint64_t mem_rss_kb;      /* RSS actual (kB)                        */
    uint64_t mem_vms_kb;      /* VmSize actual (kB)                     */
    uint64_t mem_peak_kb;     /* VmPeak (kB)                            */

    /* E/S */
    uint64_t io_read_bytes;   /* bytes leídos (acumulado)               */
    uint64_t io_write_bytes;  /* bytes escritos (acumulado)             */
    uint64_t io_read_ops;     /* operaciones de lectura                 */
    uint64_t io_write_ops;    /* operaciones de escritura               */

    /* cgroup v2 (solo si PERF_SRC_CGROUP) */
    uint64_t cg_cpu_usage_us; /* cpu.stat usage_usec                   */
    uint64_t cg_mem_current;  /* memory.current (bytes)                */
    uint64_t cg_mem_peak;     /* memory.peak (bytes)                   */
    uint64_t cg_io_rbytes;    /* io.stat rbytes                        */
    uint64_t cg_io_wbytes;    /* io.stat wbytes                        */
} perf_sample_t;

/* Serie temporal de muestras */
typedef struct {
    perf_sample_t *samples;
    size_t         count;
    size_t         capacity;
} perf_series_t;

void perf_series_free(perf_series_t *s);

/* ── Ciclo de vida del contexto ────────────────────────────────────────── */

typedef struct perf_ctx perf_ctx_t;

/**
 * Inicializa un contexto de medición.
 * @param cfg  Configuración (NULL usa PERF_CONFIG_DEFAULT).
 * @return     Puntero al contexto, o NULL si falla (errno seteado).
 */
perf_ctx_t *perf_init(const perf_config_t *cfg);

/** Libera todos los recursos del contexto. */
void perf_shutdown(perf_ctx_t *ctx);

/* ── Muestreo ──────────────────────────────────────────────────────────── */

/** Toma una muestra puntual y la escribe en *out. */
perf_status_t perf_sample(perf_ctx_t *ctx, perf_sample_t *out);

/** Arranca el sampler en un hilo de fondo. */
perf_status_t perf_start_recording(perf_ctx_t *ctx);

/**
 * Detiene el sampler y entrega la serie acumulada.
 * El llamador debe invocar perf_series_free() cuando termine.
 */
perf_status_t perf_stop_recording(perf_ctx_t *ctx, perf_series_t *out);

/* ── Profiling por función ─────────────────────────────────────────────── */

typedef struct {
    perf_ctx_t      *ctx;
    const char      *name;
    struct timespec  t0;
    uint64_t         utime0_us;
    uint64_t         stime0_us;
    uint64_t         mem0_kb;
} perf_probe_t;

/** Inicia una medición asociada a name. */
perf_probe_t perf_probe_begin(perf_ctx_t *ctx, const char *name);

/** Finaliza la medición y acumula estadísticas. */
void perf_probe_end(perf_probe_t *probe);

typedef struct {
    char     name[128];
    uint64_t count;
    double   elapsed_us_avg;
    double   elapsed_us_min;
    double   elapsed_us_max;
    double   elapsed_us_p50;
    double   elapsed_us_p95;
    double   elapsed_us_p99;
    double   mem_delta_kb_avg;
    double   cpu_us_avg;         /* tiempo CPU (user+sys) por llamada   */
} perf_probe_stats_t;

/**
 * Copia las estadísticas actuales de todas las sondas en buf[0..n-1].
 * Si buf==NULL, solo escribe el número de sondas en *n.
 */
perf_status_t perf_probe_report(perf_ctx_t *ctx,
                                perf_probe_stats_t *buf,
                                size_t *n);

/** Reinicia los contadores de todas las sondas. */
void perf_probe_reset(perf_ctx_t *ctx);

/* ── Macro de azúcar para profiling ───────────────────────────────────── */

/**
 * PERF_PROBE(ctx, "nombre") { cuerpo; }
 *
 * Registra el tiempo y recursos consumidos por el bloque.
 * Usa __attribute__((cleanup)) para llamar perf_probe_end incluso ante
 * returns anticipados dentro del bloque.
 */
static inline void _perf_probe_cleanup(perf_probe_t *p) {
    perf_probe_end(p);
}

#ifdef PERF_DISABLE_SAMPLING
  /* Modo overhead-baseline: la macro se vuelve no-op para medir el costo
   * intrínseco del workload sin instrumentación. */
  #define PERF_PROBE(ctx_, name_) \
      for (int _once_ = 0; _once_ < 1; _once_++)
#else
  #define PERF_PROBE(ctx_, name_) \
      for (perf_probe_t _probe_ \
               __attribute__((cleanup(_perf_probe_cleanup))) \
               = perf_probe_begin((ctx_), (name_)), _done_ = {0}; \
           !_done_.ctx; \
           _done_.ctx = (perf_ctx_t *)1)
#endif

/* ── Exportación JSON ──────────────────────────────────────────────────── */

/**
 * Serializa una muestra a JSON en buf (tamaño bufsz).
 * @return bytes escritos (sin NUL), o -1 si el buffer es insuficiente.
 */
int perf_sample_to_json(const perf_sample_t *s, char *buf, size_t bufsz);

/**
 * Serializa las estadísticas de todas las sondas a JSON.
 * @return bytes escritos (sin NUL), o -1 si el buffer es insuficiente.
 */
int perf_probes_to_json(perf_ctx_t *ctx, char *buf, size_t bufsz);

/**
 * Serializa una serie completa a JSON (array de objetos).
 * @return bytes escritos (sin NUL), o -1 si el buffer es insuficiente.
 */
int perf_series_to_json(const perf_series_t *s, char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif

#endif /* PERF_H */
