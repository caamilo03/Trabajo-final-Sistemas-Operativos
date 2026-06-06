#define _POSIX_C_SOURCE 200809L
#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── SHA-256 minimal (sin dependencias externas) ─────────────────────────── */

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROTR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z)   (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z)  (((x)&(y))^((x)&(z))^((y)&(z)))
#define S0(x) (ROTR32(x,2)^ROTR32(x,13)^ROTR32(x,22))
#define S1(x) (ROTR32(x,6)^ROTR32(x,11)^ROTR32(x,25))
#define s0(x) (ROTR32(x,7)^ROTR32(x,18)^((x)>>3))
#define s1(x) (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static void sha256_block(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24)
             | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] << 8)
             | (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; i++)
        w[i] = s1(w[i-2]) + w[i-7] + s0(w[i-15]) + w[i-16];

    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],
             e=h[4],f=h[5],g=h[6],hh=h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t T1 = hh + S1(e) + CH(e,f,g) + K[i] + w[i];
        uint32_t T2 = S0(a) + MAJ(a,b,c);
        hh=g; g=f; f=e; e=d+T1; d=c; c=b; b=a; a=T1+T2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
    h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

static void sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    const uint8_t *p = (const uint8_t *)data;
    size_t rem = len;
    uint8_t block[64];

    while (rem >= 64) {
        sha256_block(h, p);
        p += 64; rem -= 64;
    }
    memcpy(block, p, rem);
    block[rem] = 0x80;
    memset(block + rem + 1, 0, 63 - rem);
    if (rem >= 56) {
        sha256_block(h, block);
        memset(block, 0, 56);
    }
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        block[56 + i] = (uint8_t)(bitlen >> (56 - i * 8));
    sha256_block(h, block);

    for (int i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);
        out[i*4+3] = (uint8_t)(h[i]);
    }
}

/* ── Workload de multiplicación de matrices ──────────────────────────────── */

#define MAT_N 256

static void matmul(double *C, const double *A, const double *B, int n)
{
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) {
            double aik = A[i*n + k];
            for (int j = 0; j < n; j++)
                C[i*n + j] += aik * B[k*n + j];
        }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int duration_s  = 10;
    int intensity   = 1;  /* 1=low, 2=high */

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i+1 < argc)
            duration_s = atoi(argv[++i]);
        else if (strcmp(argv[i], "--intensity") == 0 && i+1 < argc)
            intensity = atoi(argv[++i]);
    }

    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    perf_ctx_t *ctx = perf_init(&cfg);
    if (!ctx) { fprintf(stderr, "perf_init falló\n"); return 1; }

#ifndef PERF_DISABLE_SAMPLING
    perf_start_recording(ctx);
#endif

    int mat_n = (intensity >= 2) ? MAT_N : MAT_N / 2;
    double *A = calloc((size_t)(mat_n * mat_n), sizeof(double));
    double *B = calloc((size_t)(mat_n * mat_n), sizeof(double));
    double *C = calloc((size_t)(mat_n * mat_n), sizeof(double));
    if (!A || !B || !C) { fprintf(stderr, "alloc falló\n"); return 1; }

    for (int i = 0; i < mat_n * mat_n; i++) {
        A[i] = (double)rand() / RAND_MAX;
        B[i] = (double)rand() / RAND_MAX;
    }

    uint8_t hash_in[128];
    uint8_t hash_out[32];
    memset(hash_in, 0xAB, sizeof(hash_in));

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    t_end.tv_sec += duration_s;

    uint64_t matmul_iters = 0, hash_iters = 0;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > t_end.tv_sec ||
            (now.tv_sec == t_end.tv_sec && now.tv_nsec >= t_end.tv_nsec))
            break;

        PERF_PROBE(ctx, "matmul") {
            memset(C, 0, (size_t)(mat_n * mat_n) * sizeof(double));
            matmul(C, A, B, mat_n);
            matmul_iters++;
        }

        PERF_PROBE(ctx, "sha256") {
            for (int k = 0; k < 1000; k++) {
                sha256(hash_in, sizeof(hash_in), hash_out);
                memcpy(hash_in, hash_out, 32);
                hash_iters++;
            }
        }
    }

    perf_series_t series = {0};
#ifndef PERF_DISABLE_SAMPLING
    perf_stop_recording(ctx, &series);
#endif

    /* Imprimir resumen a stdout (CSV) */
    printf("workload,cpu_stress\n");
    printf("matmul_iters,%llu\n", (unsigned long long)matmul_iters);
    printf("sha256_iters,%llu\n", (unsigned long long)hash_iters);
    printf("samples,%zu\n", series.count);

    if (series.count > 0) {
        double cpu_sum = 0;
        for (size_t i = 0; i < series.count; i++)
            cpu_sum += series.samples[i].cpu_percent;
        printf("cpu_pct_avg,%.2f\n", cpu_sum / (double)series.count);
    }

    /* Reporte de sondas (vacío si PERF_DISABLE_SAMPLING) */
    size_t np = 0;
    perf_probe_report(ctx, NULL, &np);
    if (np > 0) {
        perf_probe_stats_t *ps = malloc(np * sizeof(*ps));
        if (ps) {
            perf_probe_report(ctx, ps, &np);
            for (size_t i = 0; i < np; i++) {
                printf("probe_%s_avg_us,%.2f\n", ps[i].name, ps[i].elapsed_us_avg);
                printf("probe_%s_p99_us,%.2f\n", ps[i].name, ps[i].elapsed_us_p99);
                printf("probe_%s_count,%llu\n",  ps[i].name,
                       (unsigned long long)ps[i].count);
            }
            free(ps);
        }
    }
    free(A); free(B); free(C);
    perf_series_free(&series);
    perf_shutdown(ctx);
    return 0;
}
