#define _POSIX_C_SOURCE 200809L
#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define MB          (1024UL * 1024UL)
#define BLOCK_SIZE  (64UL * 1024UL)   /* 64 KiB */

static int write_file(perf_ctx_t *ctx, const char *path, size_t total_mb)
{
    (void)ctx;  /* usado por PERF_PROBE; silencia warning si está deshabilitado */
    char *buf = malloc(BLOCK_SIZE);
    if (!buf) return -1;
    memset(buf, 0xAB, BLOCK_SIZE);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); return -1; }

    size_t written = 0;
    size_t target  = total_mb * MB;

    while (written < target) {
        size_t chunk = BLOCK_SIZE;
        if (written + chunk > target) chunk = target - written;

        PERF_PROBE(ctx, "write_seq") {
            ssize_t r = write(fd, buf, chunk);
            if (r > 0) written += (size_t)r;
        }
    }
    fsync(fd);
    close(fd);
    free(buf);
    return 0;
}

static uint64_t read_sequential(perf_ctx_t *ctx, const char *path)
{
    (void)ctx;
    char *buf = malloc(BLOCK_SIZE);
    if (!buf) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { free(buf); return 0; }

    uint64_t total = 0;
    ssize_t r;
    while ((r = read(fd, buf, BLOCK_SIZE)) > 0) {
        PERF_PROBE(ctx, "read_seq") {
            for (ssize_t i = 0; i < r; i += 4096)
                total += (uint8_t)buf[i];
        }
    }
    close(fd);
    free(buf);
    return total;
}

static uint64_t read_random(perf_ctx_t *ctx, const char *path,
                              off_t file_size, int nreads)
{
    (void)ctx;
    char *buf = malloc(BLOCK_SIZE);
    if (!buf) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { free(buf); return 0; }

    uint64_t total = 0;
    for (int i = 0; i < nreads; i++) {
        off_t max_off = file_size > (off_t)BLOCK_SIZE
                      ? file_size - (off_t)BLOCK_SIZE : 0;
        off_t offset  = max_off > 0 ? (off_t)((uint64_t)rand() % (uint64_t)max_off) : 0;
        /* Alinear al bloque */
        offset = (offset / (off_t)BLOCK_SIZE) * (off_t)BLOCK_SIZE;

        PERF_PROBE(ctx, "read_rand") {
            lseek(fd, offset, SEEK_SET);
            ssize_t r = read(fd, buf, BLOCK_SIZE);
            if (r > 0)
                for (ssize_t j = 0; j < r; j += 512)
                    total += (uint8_t)buf[j];
        }
    }
    close(fd);
    free(buf);
    return total;
}

int main(int argc, char *argv[])
{
    int    duration_s = 10;
    size_t file_mb    = 32;
    int    intensity  = 1;
    const char *tmpfile = "/tmp/perf_io_stress.bin";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i+1 < argc)
            duration_s = atoi(argv[++i]);
        else if (strcmp(argv[i], "--intensity") == 0 && i+1 < argc)
            intensity = atoi(argv[++i]);
        else if (strcmp(argv[i], "--tmpfile") == 0 && i+1 < argc)
            tmpfile = argv[++i];
    }
    if (intensity >= 2) file_mb = 128;

    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    perf_ctx_t *ctx = perf_init(&cfg);
    if (!ctx) { fprintf(stderr, "perf_init falló\n"); return 1; }

#ifndef PERF_DISABLE_SAMPLING
    perf_start_recording(ctx);
#endif

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    t_end.tv_sec += duration_s;

    uint64_t iters = 0, checksum = 0;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > t_end.tv_sec ||
            (now.tv_sec == t_end.tv_sec && now.tv_nsec >= t_end.tv_nsec))
            break;

        write_file(ctx, tmpfile, file_mb);

        struct stat st;
        off_t fsize = (stat(tmpfile, &st) == 0) ? st.st_size : (off_t)(file_mb * MB);

        checksum += read_sequential(ctx, tmpfile);
        checksum += read_random(ctx, tmpfile, fsize, 64);
        iters++;
    }

    unlink(tmpfile);

    perf_series_t series = {0};
#ifndef PERF_DISABLE_SAMPLING
    perf_stop_recording(ctx, &series);
#endif

    printf("workload,io_stress\n");
    printf("iters,%llu\n",     (unsigned long long)iters);
    printf("checksum,%llu\n",  (unsigned long long)checksum);
    printf("samples,%zu\n",    series.count);

    if (series.count > 0) {
        uint64_t rb = 0, wb = 0;
        for (size_t i = 0; i < series.count; i++) {
            rb = series.samples[i].io_read_bytes;
            wb = series.samples[i].io_write_bytes;
        }
        printf("io_read_bytes_final,%llu\n",  (unsigned long long)rb);
        printf("io_write_bytes_final,%llu\n", (unsigned long long)wb);
    }

    size_t np = 0;
    perf_probe_report(ctx, NULL, &np);
    if (np > 0) {
        perf_probe_stats_t *ps = malloc(np * sizeof(*ps));
        if (ps) {
            perf_probe_report(ctx, ps, &np);
            for (size_t i = 0; i < np; i++) {
                printf("probe_%s_avg_us,%.2f\n", ps[i].name, ps[i].elapsed_us_avg);
                printf("probe_%s_p99_us,%.2f\n", ps[i].name, ps[i].elapsed_us_p99);
            }
            free(ps);
        }
    }
    perf_series_free(&series);
    perf_shutdown(ctx);
    return 0;
}
