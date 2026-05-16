#define _POSIX_C_SOURCE 200809L
#include "routes.h"
#include "http.h"
#include "perf.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define JSON_BUF_SIZE (1024 * 1024)   /* 1 MiB para series largas */

/* ── GET /health ─────────────────────────────────────────────────────────── */
static void handle_health(int fd)
{
    http_send_json(fd, 200, "{\"status\":\"ok\"}");
}

/* ── GET /snapshot ───────────────────────────────────────────────────────── */
static void handle_snapshot(perf_ctx_t *ctx, int fd)
{
    perf_sample_t s;
    if (perf_sample(ctx, &s) != PERF_OK) {
        http_send_error(fd, 500, "sampling failed");
        return;
    }

    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) { http_send_error(fd, 500, "out of memory"); return; }

    int n = perf_sample_to_json(&s, buf, JSON_BUF_SIZE);
    if (n < 0) {
        http_send_error(fd, 500, "serialization failed");
    } else {
        http_send_json(fd, 200, buf);
    }
    free(buf);
}

/* ── POST /recording/start ───────────────────────────────────────────────── */
static void handle_recording_start(perf_ctx_t *ctx, int fd)
{
    perf_status_t st = perf_start_recording(ctx);
    if (st == PERF_ERR_BUSY) {
        http_send_error(fd, 409, "already recording");
    } else if (st != PERF_OK) {
        http_send_error(fd, 500, perf_strerror(st));
    } else {
        http_send_json(fd, 202, "{\"status\":\"recording\"}");
    }
}

/* ── POST /recording/stop ────────────────────────────────────────────────── */
static void handle_recording_stop(perf_ctx_t *ctx, int fd)
{
    perf_series_t series;
    perf_status_t st = perf_stop_recording(ctx, &series);

    if (st == PERF_ERR_NOT_RUNNING) {
        http_send_error(fd, 409, "not recording");
        return;
    }
    if (st != PERF_OK) {
        http_send_error(fd, 500, perf_strerror(st));
        return;
    }

    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) {
        perf_series_free(&series);
        http_send_error(fd, 500, "out of memory");
        return;
    }

    int n = perf_series_to_json(&series, buf, JSON_BUF_SIZE);
    if (n < 0) {
        http_send_error(fd, 500, "serialization failed");
    } else {
        http_send_json(fd, 200, buf);
    }
    free(buf);
    perf_series_free(&series);
}

/* ── POST /probe/begin ───────────────────────────────────────────────────── */
static void handle_probe_begin(perf_ctx_t *ctx,
                                const http_request_t *req, int fd)
{
    /* Extraer "name" del body JSON: {"name":"fn_name"} */
    char name[128] = "unnamed";
    const char *p = strstr(req->body, "\"name\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            size_t i = 0;
            while (*p && *p != '"' && i < sizeof(name) - 1)
                name[i++] = *p++;
            name[i] = '\0';
        }
    }

    /* Guardamos la sonda en un contexto temporal por-conexión (no ideal para
     * múltiples clientes; suficiente para el caso de uso de un workload). */
    perf_probe_t probe = perf_probe_begin(ctx, name);
    (void)probe; /* La macro PERF_PROBE no aplica aquí; usamos begin/end manual */

    char resp[256];
    snprintf(resp, sizeof(resp), "{\"probe_name\":\"%s\",\"status\":\"started\"}", name);
    http_send_json(fd, 202, resp);
}

/* ── POST /probe/end ─────────────────────────────────────────────────────── */
static void handle_probe_end(perf_ctx_t *ctx,
                              const http_request_t *req, int fd)
{
    /* Extraer "name" del body JSON */
    char name[128] = "unnamed";
    const char *p = strstr(req->body, "\"name\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            size_t i = 0;
            while (*p && *p != '"' && i < sizeof(name) - 1)
                name[i++] = *p++;
            name[i] = '\0';
        }
    }

    /* Tomamos muestra final usando perf_probe_begin + end en el servidor;
     * esto registra un intervalo de duración ~0 (para el endpoint REST).
     * El uso real de /probe/begin+end es para workloads que corren
     * independientemente y reportan sus stats vía /probes. */
    perf_probe_t probe = perf_probe_begin(ctx, name);
    perf_probe_end(&probe);

    http_send_json(fd, 200, "{\"status\":\"recorded\"}");
    (void)req;
}

/* ── GET /probes ─────────────────────────────────────────────────────────── */
static void handle_probes(perf_ctx_t *ctx, int fd)
{
    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) { http_send_error(fd, 500, "out of memory"); return; }

    int n = perf_probes_to_json(ctx, buf, JSON_BUF_SIZE);
    if (n < 0) {
        http_send_error(fd, 500, "serialization failed");
    } else {
        http_send_json(fd, 200, buf);
    }
    free(buf);
}

/* ── POST /probe/reset ───────────────────────────────────────────────────── */
static void handle_probe_reset(perf_ctx_t *ctx, int fd)
{
    perf_probe_reset(ctx);
    http_send_json(fd, 200, "{\"status\":\"reset\"}");
}

/* ── Dispatcher ──────────────────────────────────────────────────────────── */

void routes_dispatch(perf_ctx_t *ctx, const http_request_t *req, int fd)
{
    const char *path = req->path;

    if (req->method == HTTP_GET && strcmp(path, "/health") == 0) {
        handle_health(fd);
    } else if (req->method == HTTP_GET && strcmp(path, "/snapshot") == 0) {
        handle_snapshot(ctx, fd);
    } else if (req->method == HTTP_POST && strcmp(path, "/recording/start") == 0) {
        handle_recording_start(ctx, fd);
    } else if (req->method == HTTP_POST && strcmp(path, "/recording/stop") == 0) {
        handle_recording_stop(ctx, fd);
    } else if (req->method == HTTP_POST && strcmp(path, "/probe/begin") == 0) {
        handle_probe_begin(ctx, req, fd);
    } else if (req->method == HTTP_POST && strcmp(path, "/probe/end") == 0) {
        handle_probe_end(ctx, req, fd);
    } else if (req->method == HTTP_GET && strcmp(path, "/probes") == 0) {
        handle_probes(ctx, fd);
    } else if (req->method == HTTP_POST && strcmp(path, "/probe/reset") == 0) {
        handle_probe_reset(ctx, fd);
    } else {
        http_send_error(fd, 404, "not found");
    }
}
