#define _POSIX_C_SOURCE 200809L
#include "routes.h"
#include "http.h"
#include "perf.h"
#include "dashboard.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#define JSON_BUF_SIZE (4 * 1024 * 1024)   /* 4 MiB para series largas (4096 muestras × ~600 B) */

/* ── Tabla de sesiones de probe activas ─────────────────────────────────────
 * Cuando un cliente HTTP llama /probe/begin almacenamos el probe_t en una
 * tabla indexada por nombre. /probe/end busca por nombre y cierra el probe,
 * registrando la duración en la tabla de sondas del ctx. */
#define MAX_ACTIVE_PROBES 64

typedef struct {
    char         name[128];
    perf_probe_t probe;
    int          in_use;
} active_probe_t;

static active_probe_t g_active[MAX_ACTIVE_PROBES];
static pthread_mutex_t g_active_mu = PTHREAD_MUTEX_INITIALIZER;

static active_probe_t *active_find(const char *name)
{
    for (size_t i = 0; i < MAX_ACTIVE_PROBES; i++)
        if (g_active[i].in_use && strcmp(g_active[i].name, name) == 0)
            return &g_active[i];
    return NULL;
}

static active_probe_t *active_alloc(const char *name)
{
    for (size_t i = 0; i < MAX_ACTIVE_PROBES; i++) {
        if (!g_active[i].in_use) {
            g_active[i].in_use = 1;
            strncpy(g_active[i].name, name, sizeof(g_active[i].name) - 1);
            g_active[i].name[sizeof(g_active[i].name) - 1] = '\0';
            return &g_active[i];
        }
    }
    return NULL;
}

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

/* Extrae el campo "name" de un cuerpo JSON simple: {"name":"<valor>"} */
static int extract_json_name(const char *body, char *out, size_t outsz)
{
    const char *p = strstr(body, "\"name\"");
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1)
        out[i++] = *p++;
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* ── POST /probe/begin ───────────────────────────────────────────────────── */
static void handle_probe_begin(perf_ctx_t *ctx,
                                const http_request_t *req, int fd)
{
    char name[128];
    if (extract_json_name(req->body, name, sizeof(name)) != 0) {
        http_send_error(fd, 400, "missing 'name' in body");
        return;
    }

    pthread_mutex_lock(&g_active_mu);
    if (active_find(name)) {
        pthread_mutex_unlock(&g_active_mu);
        http_send_error(fd, 409, "probe already active");
        return;
    }
    active_probe_t *slot = active_alloc(name);
    if (!slot) {
        pthread_mutex_unlock(&g_active_mu);
        http_send_error(fd, 500, "too many active probes");
        return;
    }
    slot->probe = perf_probe_begin(ctx, slot->name);
    pthread_mutex_unlock(&g_active_mu);

    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"probe_name\":\"%s\",\"status\":\"started\"}", name);
    http_send_json(fd, 202, resp);
}

/* ── POST /probe/end ─────────────────────────────────────────────────────── */
static void handle_probe_end(perf_ctx_t *ctx,
                              const http_request_t *req, int fd)
{
    (void)ctx;
    char name[128];
    if (extract_json_name(req->body, name, sizeof(name)) != 0) {
        http_send_error(fd, 400, "missing 'name' in body");
        return;
    }

    pthread_mutex_lock(&g_active_mu);
    active_probe_t *slot = active_find(name);
    if (!slot) {
        pthread_mutex_unlock(&g_active_mu);
        http_send_error(fd, 404, "probe not active");
        return;
    }
    perf_probe_end(&slot->probe);
    slot->in_use = 0;
    pthread_mutex_unlock(&g_active_mu);

    http_send_json(fd, 200, "{\"status\":\"recorded\"}");
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

/* ── GET /dashboard ──────────────────────────────────────────────────────── */
static void handle_dashboard(int fd)
{
    const char *html = dashboard_html();
    http_response_t resp = {
        .status   = 200,
        .body     = (char *)html,
        .body_len = strlen(html),
    };
    strncpy(resp.content_type, "text/html; charset=utf-8",
            sizeof(resp.content_type) - 1);
    http_send(fd, &resp);
}

/* ── Dispatcher ──────────────────────────────────────────────────────────── */

void routes_dispatch(perf_ctx_t *ctx, const http_request_t *req, int fd)
{
    const char *path = req->path;

    if (req->method == HTTP_GET &&
        (strcmp(path, "/dashboard") == 0 || strcmp(path, "/") == 0)) {
        handle_dashboard(fd);
    } else if (req->method == HTTP_GET && strcmp(path, "/health") == 0) {
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
