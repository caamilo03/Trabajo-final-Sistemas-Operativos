#define _POSIX_C_SOURCE 200809L
#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 8080
#define RECV_BUF     (1024 * 512)

static void usage(const char *prog)
{
    fprintf(stderr,
        "perfctl — cliente CLI para perfanalyzer\n\n"
        "Uso: %s [--host H] [--port P] <comando>\n\n"
        "Comandos:\n"
        "  health            Consulta liveness del servidor\n"
        "  snapshot          Muestra puntual CPU/Mem/IO\n"
        "  record start      Inicia grabación\n"
        "  record stop       Detiene grabación y muestra la serie\n"
        "  probe begin <fn>  Inicia profiling de función <fn>\n"
        "  probe end   <fn>  Finaliza profiling de función <fn>\n"
        "  probe reset       Resetea contadores de sondas\n"
        "  probes            Estadísticas de todas las sondas\n"
        "  local snapshot    Toma muestra directamente (sin servidor)\n"
        "\n"
        "Opciones:\n"
        "  --host <H>  Host del servidor (defecto: " DEFAULT_HOST ")\n"
        "  --port <P>  Puerto (defecto: %d)\n",
        prog, DEFAULT_PORT);
}

/* ── HTTP client mínimo ──────────────────────────────────────────────────── */

static int tcp_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int http_request(const char *host, int port,
                         const char *method, const char *path,
                         const char *body, char **resp_body)
{
    int fd = tcp_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "Error: no se puede conectar a %s:%d\n", host, port);
        return -1;
    }

    size_t blen = body ? strlen(body) : 0;
    char req[1024];
    int rlen = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, path, host, port, blen);

    send(fd, req, (size_t)rlen, 0);
    if (blen > 0) send(fd, body, blen, 0);

    char *rbuf = malloc(RECV_BUF);
    if (!rbuf) { close(fd); return -1; }

    size_t total = 0;
    ssize_t got;
    while ((got = recv(fd, rbuf + total, RECV_BUF - total - 1, 0)) > 0)
        total += (size_t)got;
    rbuf[total] = '\0';
    close(fd);

    /* Saltar cabeceras HTTP */
    char *body_start = strstr(rbuf, "\r\n\r\n");
    if (body_start) body_start += 4;
    else body_start = rbuf;

    *resp_body = strdup(body_start);
    free(rbuf);
    return 0;
}

static void print_resp(const char *host, int port,
                        const char *method, const char *path,
                        const char *body)
{
    char *resp = NULL;
    if (http_request(host, port, method, path, body, &resp) != 0) return;
    printf("%s\n", resp);
    free(resp);
}

/* ── Modo local (sin servidor) ───────────────────────────────────────────── */

static void local_snapshot(void)
{
    perf_config_t cfg = PERF_CONFIG_DEFAULT;
    perf_ctx_t *ctx = perf_init(&cfg);
    if (!ctx) { fprintf(stderr, "Error: perf_init falló\n"); return; }

    perf_sample_t s;
    /* Dos muestras para tener delta de CPU */
    perf_sample(ctx, &s);
    struct timespec ts = {0, 200000000L};
    nanosleep(&ts, NULL);
    perf_sample(ctx, &s);

    char buf[4096];
    perf_sample_to_json(&s, buf, sizeof(buf));
    printf("%s\n", buf);
    perf_shutdown(ctx);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    int i = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else {
            break;
        }
    }

    if (i >= argc) { usage(argv[0]); return 1; }
    const char *cmd = argv[i++];

    if (strcmp(cmd, "health") == 0) {
        print_resp(host, port, "GET", "/health", NULL);
    } else if (strcmp(cmd, "snapshot") == 0) {
        print_resp(host, port, "GET", "/snapshot", NULL);
    } else if (strcmp(cmd, "record") == 0 && i < argc) {
        const char *sub = argv[i++];
        if (strcmp(sub, "start") == 0)
            print_resp(host, port, "POST", "/recording/start", "{}");
        else if (strcmp(sub, "stop") == 0)
            print_resp(host, port, "POST", "/recording/stop", "{}");
        else { fprintf(stderr, "Subcomando desconocido: %s\n", sub); return 1; }
    } else if (strcmp(cmd, "probe") == 0 && i < argc) {
        const char *sub = argv[i++];
        if (strcmp(sub, "begin") == 0 && i < argc) {
            char body[256];
            snprintf(body, sizeof(body), "{\"name\":\"%s\"}", argv[i++]);
            print_resp(host, port, "POST", "/probe/begin", body);
        } else if (strcmp(sub, "end") == 0 && i < argc) {
            char body[256];
            snprintf(body, sizeof(body), "{\"name\":\"%s\"}", argv[i++]);
            print_resp(host, port, "POST", "/probe/end", body);
        } else if (strcmp(sub, "reset") == 0) {
            print_resp(host, port, "POST", "/probe/reset", "{}");
        } else {
            fprintf(stderr, "Uso: perfctl probe begin|end <nombre> | reset\n");
            return 1;
        }
    } else if (strcmp(cmd, "probes") == 0) {
        print_resp(host, port, "GET", "/probes", NULL);
    } else if (strcmp(cmd, "local") == 0 && i < argc) {
        if (strcmp(argv[i], "snapshot") == 0) local_snapshot();
        else { fprintf(stderr, "Subcomando local desconocido\n"); return 1; }
    } else {
        usage(argv[0]);
        return 1;
    }

    return 0;
}
